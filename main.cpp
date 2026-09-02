// main.cpp
//
// Acquisition binary for the Hopkins rocky-shore camera station.
//
// Two threads. The producer, pinned to core 2, triggers both sensors and
// pushes the retrieved frames into a RAM ring buffer. The consumer, pinned to
// core 3, drains that buffer onto the SATA vault. The buffer decouples the
// two so that a transient stall of the drive does not propagate back to the
// sensors as a dropped frame (threshold No.17).
//
// Output layout, one directory per burst:
//
//   <output_root>/<YYYY-MM-DDTHH-MM-SSZ>/
//       manifest.json          written before the first trigger
//       summary.json           written after a clean shutdown
//       cam0_<serial>/000001.raw ...
//       cam1_<serial>/000001.raw ...
//
// The six-digit index is the trigger ordinal, not a write counter. A trigger
// that yields nothing leaves a gap; indices therefore stay aligned across the
// two cameras for the whole burst.

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <pthread.h>
#include <sys/statvfs.h>
#include <sys/timepps.h>
#include <sys/timex.h>
#include <unistd.h>

#include "Spinnaker.h"
#include "SpinGenApi/SpinnakerGenApi.h"

#include "GpioTrigger.h"
#include "RingBuffer.h"
#include "Session.h"

using namespace Spinnaker;
using namespace Spinnaker::GenApi;
using namespace Spinnaker::GenICam;

namespace fs = std::filesystem;

std::atomic<bool>    keep_running{true};
std::atomic<int>     caught_signal{0};

// Handles SIGINT and SIGTERM alike. SIGTERM is not optional: it is what
// systemd sends on stop, and what the kernel sends at shutdown. Handling only
// SIGINT meant that every orderly stop of the future service unit would kill
// the process outright and discard whatever the ring buffer still held, up to
// thirty seconds of acquisition.
//
// The handler does nothing but set two flags. Writing to a stream from a
// signal handler is not async-signal-safe: std::cout is not reentrant, and a
// signal arriving while the consumer holds its internal lock would deadlock
// the process at the exact moment it is being asked to stop cleanly. The
// message is printed by the main thread once it observes the flag.
extern "C" void signal_handler(int signum)
{
    caught_signal.store(signum, std::memory_order_relaxed);
    keep_running.store(false, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// GenICam node readers
// ---------------------------------------------------------------------------
// A node that is absent or unreadable is reported rather than fatal: the
// manifest records what the hardware actually exposes, and a missing optional
// feature must not abort a burst.

static std::string nodeStr(INodeMap& m, const char* name)
{
    try
    {
        CNodePtr node = m.GetNode(name);
        if (!node.IsValid() || !IsAvailable(node) || !IsReadable(node)) return "<unavailable>";
        CValuePtr v = static_cast<CValuePtr>(node);
        if (!v.IsValid()) return "<unavailable>";
        return std::string(v->ToString().c_str());
    }
    catch (...) { return "<unavailable>"; }
}

static int64_t nodeInt(INodeMap& m, const char* name, int64_t fallback)
{
    try
    {
        CIntegerPtr n = m.GetNode(name);
        if (!n.IsValid() || !IsAvailable(n) || !IsReadable(n)) return fallback;
        return n->GetValue();
    }
    catch (...) { return fallback; }
}

static double nodeFloat(INodeMap& m, const char* name, double fallback)
{
    try
    {
        CFloatPtr n = m.GetNode(name);
        if (!n.IsValid() || !IsAvailable(n) || !IsReadable(n)) return fallback;
        return n->GetValue();
    }
    catch (...) { return fallback; }
}


// ---------------------------------------------------------------------------
// GenICam node writers
// ---------------------------------------------------------------------------
// Every setter throws on failure. A camera that silently refuses a setting is
// worse than one that refuses loudly: the burst would run, look healthy, and
// carry radiometry nobody chose.

static void setEnum(INodeMap& m, const char* node, const char* value)
{
    CEnumerationPtr e = m.GetNode(node);
    if (!e.IsValid() || !IsAvailable(e) || !IsWritable(e))
        throw std::runtime_error(std::string("node not writable: ") + node);
    CEnumEntryPtr entry = e->GetEntryByName(value);
    if (!entry.IsValid() || !IsAvailable(entry))
        throw std::runtime_error(std::string("value not available: ") + node + " = " + value);
    e->SetIntValue(entry->GetValue());
}

static void setBool(INodeMap& m, const char* node, bool value)
{
    CBooleanPtr b = m.GetNode(node);
    if (!b.IsValid() || !IsAvailable(b) || !IsWritable(b))
        throw std::runtime_error(std::string("node not writable: ") + node);
    b->SetValue(value);
}

// Clamped to the node's own bounds, which depend on the current state of the
// camera: a value silently rejected out of range would leave the sensor on
// whatever it held before.
static double setFloatClamped(INodeMap& m, const char* node, double value)
{
    CFloatPtr f = m.GetNode(node);
    if (!f.IsValid() || !IsAvailable(f) || !IsWritable(f))
        throw std::runtime_error(std::string("node not writable: ") + node);
    const double lo = f->GetMin();
    const double hi = f->GetMax();
    double v = value;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    if (v != value)
    {
        std::cerr << "[CONFIG] " << node << " clamped from " << value << " to " << v
                  << " (device range " << lo << " to " << hi << ")\n";
    }
    f->SetValue(v);
    return v;
}

static void setInt(INodeMap& m, const char* node, int64_t value)
{
    CIntegerPtr i = m.GetNode(node);
    if (!i.IsValid() || !IsAvailable(i) || !IsWritable(i))
        throw std::runtime_error(std::string("node not writable: ") + node);
    i->SetValue(value);
}

static void execCommand(INodeMap& m, const char* node)
{
    CCommandPtr c = m.GetNode(node);
    if (!c.IsValid() || !IsAvailable(c) || !IsWritable(c))
        throw std::runtime_error(std::string("command not available: ") + node);
    c->Execute();
}

// Optional settings: absent on some models or firmware revisions. Reported,
// never fatal.
static bool trySetBool(INodeMap& m, const char* node, bool value)
{
    try { setBool(m, node, value); return true; }
    catch (...) { std::cerr << "[CONFIG] optional node skipped: " << node << "\n"; return false; }
}

static bool trySetEnum(INodeMap& m, const char* node, const char* value)
{
    try { setEnum(m, node, value); return true; }
    catch (...) { std::cerr << "[CONFIG] optional node skipped: " << node << " = " << value
                            << "\n"; return false; }
}

// ---------------------------------------------------------------------------
// Deterministic camera configuration
// ---------------------------------------------------------------------------
// Applied identically to both sensors, from a known starting point, so that a
// burst acquired next year on a replacement camera is comparable with one
// acquired today. Nothing is inherited from camera flash.

static void configureCamera(CameraPtr pCam, const Config& cfg, CameraInfo& info)
{
    INodeMap& m = pCam->GetNodeMap();

    // A factory user set gives a reproducible baseline. Without it the
    // configuration below sits on top of whatever the last operator, or the
    // last SpinView session, happened to leave in non-volatile memory.
    setEnum(m, "UserSetSelector", "Default");
    execCommand(m, "UserSetLoad");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    info.user_set_loaded = "Default";

    // Geometry and encoding. RAW Bayer 8-bit is the format arbitrated in
    // Appendix E: one byte per photosite, identical payload to monochrome,
    // demosaicing deferred to the laboratory.
    setEnum(m, "PixelFormat", "BayerRG8");
    trySetBool(m, "IspEnable", false);

    // Gamma is a radiometric non-linearity applied to data destined for
    // intensity cross-correlation. Disabled outright.
    trySetBool(m, "GammaEnable", false);

    // Free-running frame rate is meaningless under external triggering and
    // would otherwise cap the achievable exposure.
    trySetBool(m, "AcquisitionFrameRateEnable", false);
    setEnum(m, "AcquisitionMode", "Continuous");

    if (cfg.exposure_auto)
    {
        std::cerr << "[CONFIG] WARNING: automatic exposure, gain and white balance left "
                     "enabled. Luminance will vary frame to frame and the burst is "
                     "radiometrically unusable for OCM.\n";
        trySetEnum(m, "ExposureAuto", "Continuous");
        trySetEnum(m, "GainAuto", "Continuous");
        trySetEnum(m, "BalanceWhiteAuto", "Continuous");
    }
    else
    {
        setEnum(m, "ExposureAuto", "Off");
        setEnum(m, "ExposureMode", "Timed");
        info.exposure_us = setFloatClamped(m, "ExposureTime", cfg.exposure_us);

        setEnum(m, "GainAuto", "Off");
        info.gain_db = setFloatClamped(m, "Gain", cfg.gain_db);

        // Per-channel gains alter luminance after demosaicing, so automatic
        // white balance is a second, slower source of the same corruption.
        trySetEnum(m, "BalanceWhiteAuto", "Off");
    }

    // Triggering. Source is set with the mode off, as the node is locked
    // while triggering is armed.
    setEnum(m, "TriggerSelector", "FrameStart");
    setEnum(m, "TriggerMode", "Off");

    if (cfg.trigger == "software")
    {
        setEnum(m, "TriggerSource", "Software");
    }
        else
    {
        // Line2 is pin 3 of the Hirose HR10, the red wire, and it is a dual
        // function pin: the same conductor can be driven by the camera as a
        // +3.3 V rail capable of 120 mA nominal, under the V3_3Enable node.
        // Left enabled while a Raspberry Pi GPIO pulls the net low, that is a
        // 120 mA source shorted against an RP1 pin rated for nothing of the
        // sort. It is forced off before the line is ever configured as an
        // input, and the state reached is read back into the manifest rather
        // than assumed.
        //
        // Line0 is the opto-isolated input on pin 2, immune to the ground
        // loops expected between a Pi and two cameras five metres away on a
        // coastal roof, but specified for 5 V and therefore marginal when
        // driven at 3.3 V; it is the reason the 470 ohm resistors sit in the
        // bill of materials. The non-isolated return is pin 6, brown, the
        // camera power ground; pin 5 is the opto ground and is deliberately
        // not connected to it.
        const char* line = (cfg.trigger == "line2") ? "Line2" : "Line0";
        setEnum(m, "LineSelector", line);
        trySetBool(m, "V3_3Enable", false);
        trySetEnum(m, "LineMode", "Input");
        setEnum(m, "TriggerSource", line);
        trySetEnum(m, "TriggerActivation", "FallingEdge");
    }

    // Payload is read before chunk data is enabled, so that it measures the
    // image alone. Enabling chunks appends metadata to the transport buffer
    // and inflates PayloadSize; sizing the ring buffer on the inflated figure
    // would write chunk bytes into the archive behind every frame.
    info.payload = nodeInt(m, "PayloadSize", 0);

    // Chunk data. The device frame identifier increments by exactly one per
    // trigger, which turns drop detection from an inference into a
    // measurement: an index advancing by one while the identifier advances by
    // two means a frame was exposed and lost in transport without the
    // application ever seeing it.
    try
    {
        setBool(m, "ChunkModeActive", true);
        setEnum(m, "ChunkSelector", "FrameID");
        setBool(m, "ChunkEnable", true);
        info.chunk_frame_id = true;

        setEnum(m, "ChunkSelector", "Timestamp");
        setBool(m, "ChunkEnable", true);
        info.chunk_timestamp = true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[CONFIG] chunk data unavailable: " << e.what() << "\n";
    }

    info.transport_payload = nodeInt(m, "PayloadSize", 0);

    // Transport buffer pool. The factory value of nine is not a decision; it
    // is simply what the driver defaults to. Sixteen buffers hold eight
    // seconds at 2 Hz, which covers a producer stall without competing with
    // the ring buffer for memory.
    try
    {
        INodeMap& sm = pCam->GetTLStreamNodeMap();
        setEnum(sm, "StreamBufferCountMode", "Manual");
        setInt(sm, "StreamBufferCountManual", cfg.stream_buffers);
        setEnum(sm, "StreamBufferHandlingMode", "OldestFirst");
        info.stream_buffer_count = nodeInt(sm, "StreamBufferCountResult", 0);
    }
    catch (const std::exception& e)
    {
        std::cerr << "[CONFIG] stream buffer configuration failed: " << e.what() << "\n";
    }

    // Arm last.
    setEnum(m, "TriggerMode", "On");

    // Read back everything that goes into the manifest, from the camera and
    // not from the request: what is recorded must be what the sensor holds.
    info.serial             = nodeStr(m, "DeviceSerialNumber");
    info.model              = nodeStr(m, "DeviceModelName");
    info.firmware           = nodeStr(m, "DeviceFirmwareVersion");
    info.pixel_format       = nodeStr(m, "PixelFormat");
    info.shutter_mode       = nodeStr(m, "SensorShutterMode");
    info.exposure_auto      = nodeStr(m, "ExposureAuto");
    info.gain_auto          = nodeStr(m, "GainAuto");
    info.white_balance_auto = nodeStr(m, "BalanceWhiteAuto");
    info.gamma_enable       = nodeStr(m, "GammaEnable");
    info.trigger_source     = nodeStr(m, "TriggerSource");
        if (cfg.trigger != "software")
    {
        std::cout << "[LINE] " << info.serial << " " << nodeStr(m, "LineSelector")
                  << ": mode " << nodeStr(m, "LineMode")
                  << ", 3V3 " << nodeStr(m, "V3_3Enable")
                  << ", status " << nodeStr(m, "LineStatus")
                  << ", activation " << nodeStr(m, "TriggerActivation") << "\n";
    }
    info.width              = nodeInt(m, "Width", 0);
    info.height             = nodeInt(m, "Height", 0);
    info.exposure_us        = nodeFloat(m, "ExposureTime", info.exposure_us);
    info.gain_db            = nodeFloat(m, "Gain", info.gain_db);
}

// ---------------------------------------------------------------------------
// Camera session
// ---------------------------------------------------------------------------
// Owns the camera list and both device handles for the lifetime of one burst.
// A single enumeration pass now serves both the manifest and the acquisition:
// the previous arrangement initialised each sensor twice, once to read its
// payload and once to acquire, costing several seconds and leaving two places
// where configuration could diverge. Release is explicit and happens before
// System::ReleaseInstance, a lingering CameraPtr being the documented cause of
// the Spinnaker -1004 raised at shutdown.

class CameraSession
{
public:
    explicit CameraSession(SystemPtr system) : system_(system) {}
    ~CameraSession() { release(); }

    CameraSession(const CameraSession&)            = delete;
    CameraSession& operator=(const CameraSession&) = delete;

    bool open(const Config& cfg, std::string& err)
    {
        list_ = system_->GetCameras();
        const unsigned int n = list_.GetSize();
        if (n < 2)
        {
            err = "incomplete topology: " + std::to_string(n) + " camera(s), 2 required";
            return false;
        }

        const bool by_serial = !cfg.cam0_serial.empty() && !cfg.cam1_serial.empty();
        if (!by_serial)
        {
            std::cerr << "[WARN] No serial numbers supplied: falling back to USB enumeration "
                         "order, which is not stable across boots. The two fields of view may "
                         "transpose between runs.\n";
        }
        if (by_serial && cfg.cam0_serial == cfg.cam1_serial)
        {
            err = "--cam0-serial and --cam1-serial are identical";
            return false;
        }

        const std::string wanted[2] = {cfg.cam0_serial, cfg.cam1_serial};
        info_.resize(2);

        for (int role = 0; role < 2; ++role)
        {
            if (by_serial)
            {
                try { cams_[role] = list_.GetBySerial(wanted[role]); }
                catch (Spinnaker::Exception&) { cams_[role] = nullptr; }
                if (!cams_[role].IsValid())
                {
                    err = "no camera with serial " + wanted[role] + " is attached";
                    return false;
                }
            }
            else
            {
                cams_[role] = list_.GetByIndex(static_cast<unsigned int>(role));
            }

            info_[role].role = role;

            try
            {
                cams_[role]->Init();
                configureCamera(cams_[role], cfg, info_[role]);
            }
            catch (Spinnaker::Exception& e)
            {
                err = std::string("camera configuration failed: ") + e.what();
                return false;
            }
            catch (const std::exception& e)
            {
                err = std::string("camera configuration failed: ") + e.what();
                return false;
            }

            if (info_[role].payload <= 0)
            {
                err = "camera reports a null payload size";
                return false;
            }
            if (info_[role].serial.empty() || info_[role].serial.find('<') != std::string::npos)
            {
                err = "camera at role " + std::to_string(role) +
                      " does not report a usable serial number";
                return false;
            }
        }

        if (info_[0].payload != info_[1].payload)
        {
            err = "the two cameras report different image payloads (" +
                  std::to_string(info_[0].payload) + " and " +
                  std::to_string(info_[1].payload) + ")";
            return false;
        }
        if (info_[0].pixel_format != info_[1].pixel_format ||
            info_[0].width != info_[1].width || info_[0].height != info_[1].height)
        {
            err = "the two cameras disagree on geometry or pixel format";
            return false;
        }

        open_ = true;
        return true;
    }

    void release()
    {
        // Idempotent: early-return paths call it explicitly, and the
        // destructor calls it again after System::ReleaseInstance has already
        // run.
        if (released_) return;
        released_ = true;

        for (int r = 0; r < 2; ++r)
        {
            if (cams_[r].IsValid())
            {
                try
                {
                    if (cams_[r]->IsInitialized()) cams_[r]->DeInit();
                }
                catch (Spinnaker::Exception& e)
                {
                    std::cerr << "[RELEASE] " << e.what() << "\n";
                }
            }
            cams_[r] = nullptr;
        }
        list_.Clear();
        open_ = false;
    }

    const std::vector<CameraInfo>& info() const { return info_; }
    CameraPtr                      cam(int role) { return cams_[role]; }

private:
    SystemPtr               system_;
    CameraList              list_;
    CameraPtr               cams_[2];
    std::vector<CameraInfo> info_;
    bool                    open_     = false;
    bool                    released_ = false;
};

// ---------------------------------------------------------------------------
// Wall-clock and transport-layer instrumentation
// ---------------------------------------------------------------------------

// CLOCK_REALTIME rather than the monotonic clock: the figure must be
// comparable with the PPS assert timestamps, which the kernel stamps in the
// same domain. It inherits that clock's absolute inaccuracy, which is
// irrelevant here, every quantity derived from it being a difference.
static inline uint64_t nowRealtimeNs()
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

// The driver's own view of the stream. These counters see frames the sensor
// exposed and the transport layer discarded before the application was ever
// offered them, which no count kept above the API can detect.
static void readStreamCounters(CameraPtr pCam, StreamCounters& sc)
{
    try
    {
        INodeMap& sm  = pCam->GetTLStreamNodeMap();
        sc.started    = nodeInt(sm, "StreamStartedFrameCount", -1);
        sc.delivered  = nodeInt(sm, "StreamDeliveredFrameCount", -1);
        sc.lost       = nodeInt(sm, "StreamLostFrameCount", -1);
        sc.incomplete = nodeInt(sm, "StreamIncompleteFrameCount", -1);
        sc.dropped    = nodeInt(sm, "StreamDroppedFrameCount", -1);
    }
    catch (Spinnaker::Exception& e)
    {
        std::cerr << "[TRANSPORT] Counters unreadable: " << e.what() << "\n";
    }
}

// ---------------------------------------------------------------------------
// System clock validity
// ---------------------------------------------------------------------------
// The Raspberry Pi has no battery-backed RTC. At boot the clock is restored
// from a stale saved value and stays wrong until the network is reachable, so
// a burst started too early would be filed under a date weeks in the past.
// Reported and recorded rather than enforced: on the bench an unsynchronised
// clock is a nuisance, in the field it is a scheduling precondition, and that
// belongs to the orchestrator.

static bool clockIsSynchronized()
{
    struct timex tx;
    std::memset(&tx, 0, sizeof(tx));
    const int state = adjtimex(&tx);
    if (state < 0) return false;
    return (state != TIME_ERROR) && ((tx.status & STA_UNSYNC) == 0);
}

// ---------------------------------------------------------------------------
// PPS device resolution
// ---------------------------------------------------------------------------
// Every PPS source is exposed through the same class, and the enumeration
// order is not contractual. On this station /sys/class/pps/pps0 is the GNSS
// timepulse and pps1 the RP1 Ethernet PTP clock, which never asserts, but
// nothing guarantees that ordering survives a reboot. A binary opening
// /dev/pps0 by literal path is therefore one boot away from waiting on a
// device that will never pulse, and the consequence is worse than a failure:
// a fetch timeout does not advance the trigger index, so the burst would not
// abort, it would simply never end.
//
// Resolution takes two steps. The exported name orders the candidates, then
// the hardware confirms that ordering: the assert attribute carries a
// monotonic pulse counter, and a source that does not advance it does not
// pulse, whatever it is called. The second step also catches what the first
// cannot, a correctly named receiver holding no fix and emitting nothing, and
// it catches it before the burst directory exists rather than an hour later.

enum class PpsProbe
{
    Pulsing,
    Silent,
    Unreadable
};

struct PpsCandidate
{
    std::string        node;          // "pps0"
    std::string        name;          // contents of the name attribute
    bool               counter_read = false;
    unsigned long long first        = 0;
    unsigned long long second       = 0;
    PpsProbe           verdict      = PpsProbe::Unreadable;
};

static std::string ppsAttribute(const std::string& node, const char* attr)
{
    std::ifstream f("/sys/class/pps/" + node + "/" + attr);
    std::string   line;
    if (f && std::getline(f, line)) return line;
    return std::string();
}

// The assert attribute reads "<seconds>.<nanoseconds>#<counter>". Only the
// counter is of interest here: it is monotonic and increments once per pulse,
// which makes liveness a comparison rather than an inference.
static bool ppsPulseCounter(const std::string& node, unsigned long long& out)
{
    const std::string line = ppsAttribute(node, "assert");
    const size_t      hash = line.find('#');
    if (hash == std::string::npos || hash + 1 >= line.size()) return false;

    const char* start = line.c_str() + hash + 1;
    char*       end   = nullptr;
    errno             = 0;
    const unsigned long long v = std::strtoull(start, &end, 10);
    if (errno != 0 || end == start) return false;

    out = v;
    return true;
}

// device is in and out. Non-empty on entry it is an operator override, which
// is honoured: it is still probed, but silence downgrades to a warning. The
// guard against an endless burst is the consecutive-timeout ceiling, not this
// probe, and an operator who names a device has a reason.
static bool resolvePpsDevice(std::string& device, std::string& err)
{
    const bool explicit_choice = !device.empty();

    std::vector<PpsCandidate> cands;
    try
    {
        for (const auto& entry : fs::directory_iterator("/sys/class/pps"))
        {
            const std::string node = entry.path().filename().string();
            if (node.rfind("pps", 0) != 0) continue;
            PpsCandidate c;
            c.node = node;
            c.name = ppsAttribute(node, "name");
            cands.push_back(c);
        }
    }
    catch (const fs::filesystem_error& e)
    {
        err = std::string("cannot enumerate /sys/class/pps: ") + e.what();
        return false;
    }

    if (cands.empty())
    {
        err = "no PPS source is registered under /sys/class/pps. Check that "
              "dtoverlay=pps-gpio is present in /boot/firmware/config.txt.";
        return false;
    }

    std::sort(cands.begin(), cands.end(),
              [](const PpsCandidate& a, const PpsCandidate& b) { return a.node < b.node; });

    // One 1.5 s window, every candidate sampled either side of it. A single
    // sleep keeps start-up cost independent of how many sources are present.
    for (PpsCandidate& c : cands) c.counter_read = ppsPulseCounter(c.node, c.first);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    for (PpsCandidate& c : cands)
    {
        unsigned long long later = 0;
        if (c.counter_read && ppsPulseCounter(c.node, later))
        {
            c.second  = later;
            c.verdict = (later != c.first) ? PpsProbe::Pulsing : PpsProbe::Silent;
        }
        else
        {
            c.verdict = PpsProbe::Unreadable;
        }
    }

    for (const PpsCandidate& c : cands)
    {
        std::cout << "[PPS] /dev/" << c.node << " name="
                  << (c.name.empty() ? "<none>" : c.name) << " ";
        switch (c.verdict)
        {
            case PpsProbe::Pulsing:
                std::cout << "pulsing (" << c.first << " -> " << c.second << ")\n";
                break;
            case PpsProbe::Silent:
                std::cout << "silent (counter held at " << c.first << ")\n";
                break;
            case PpsProbe::Unreadable:
                std::cout << "pulse counter unreadable\n";
                break;
        }
    }

    if (explicit_choice)
    {
        const std::string node = fs::path(device).filename().string();
        auto              it   = std::find_if(cands.begin(), cands.end(),
                                              [&](const PpsCandidate& c) { return c.node == node; });
        if (it == cands.end())
        {
            std::cerr << "[PPS] WARNING: " << device << " is not registered under "
                         "/sys/class/pps. Opening it as instructed.\n";
        }
        else if (it->verdict == PpsProbe::Silent)
        {
            std::cerr << "[PPS] WARNING: " << device << " (" << it->name
                      << ") did not pulse during the probe window. Proceeding as "
                         "instructed; the burst will abandon on the timeout ceiling if "
                         "it stays silent.\n";
        }
        std::cout << "[PPS] Using " << device << " as given on the command line.\n";
        return true;
    }

    std::vector<const PpsCandidate*> pulsing, unreadable;
    for (const PpsCandidate& c : cands)
    {
        if (c.verdict == PpsProbe::Pulsing)         pulsing.push_back(&c);
        else if (c.verdict == PpsProbe::Unreadable) unreadable.push_back(&c);
    }

    // A PTP clock exposed through this class is never the intended source, so
    // it loses a tie. It should not be pulsing at all, but preferring against
    // it costs nothing and removes a way to be wrong silently.
    auto preferNonPtp = [](const std::vector<const PpsCandidate*>& v) {
        for (const PpsCandidate* c : v)
            if (c->name.find("ptp") == std::string::npos) return c;
        return v.front();
    };

    if (pulsing.size() == 1)
    {
        device = "/dev/" + pulsing.front()->node;
        std::cout << "[PPS] Selected " << device << " (" << pulsing.front()->name
                  << "), the only source observed pulsing.\n";
        return true;
    }

    if (pulsing.size() > 1)
    {
        const PpsCandidate* pick = preferNonPtp(pulsing);
        device = "/dev/" + pick->node;
        std::cerr << "[PPS] WARNING: " << pulsing.size() << " sources are pulsing; selected "
                  << device << " (" << pick->name
                  << "). Name --pps-device to remove the ambiguity.\n";
        return true;
    }

    // Nothing pulsed. If a counter could not be read the probe is
    // inconclusive rather than negative, and a missing permission must not
    // stop a burst: fall back on the exported name and say so.
    if (!unreadable.empty())
    {
        const PpsCandidate* pick = preferNonPtp(unreadable);
        device = "/dev/" + pick->node;
        std::cerr << "[PPS] WARNING: no pulse counter could be read, so liveness is "
                     "undetermined. Falling back on the exported name and selecting "
                  << device << " (" << pick->name << ").\n";
        return true;
    }

    err = "no PPS source is pulsing: every registered source held its counter over a "
          "1.5 s window. Check that the GNSS antenna has sky view and that the receiver "
          "holds a fix.";
    return false;
}

// ---------------------------------------------------------------------------
// Free space
// ---------------------------------------------------------------------------

static bool checkFreeSpace(const std::string& root, uint64_t required_bytes, std::string& err)
{
    struct statvfs vfs;
    if (statvfs(root.c_str(), &vfs) != 0)
    {
        err = "statvfs failed on " + root + ": " + std::strerror(errno);
        return false;
    }

    const uint64_t available = static_cast<uint64_t>(vfs.f_bavail) *
                               static_cast<uint64_t>(vfs.f_frsize);

    // Five per cent over the computed burst volume, and never less than two
    // gigabytes: filling a journalling filesystem to the last block is a way
    // to lose the burst already written, not merely the one being started.
    const uint64_t margin = std::max<uint64_t>(required_bytes / 20, 2ULL * 1024 * 1024 * 1024);
    const uint64_t needed = required_bytes + margin;

    std::cout << "[SPACE] Required " << (required_bytes / 1e9) << " GB, margin "
              << (margin / 1e9) << " GB, available " << (available / 1e9) << " GB on "
              << root << "\n";

    if (available < needed)
    {
        err = "insufficient free space: " + std::to_string(needed / 1000000000ULL) +
              " GB needed, " + std::to_string(available / 1000000000ULL) + " GB available";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// THREAD 2: CONSUMER
// ---------------------------------------------------------------------------

void consumer_thread(std::shared_ptr<RingBuffer>     buffer,
                     std::shared_ptr<BurstStats>     stats,
                     std::vector<std::string>        cam_dirs)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(3, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0)
    {
        std::cerr << "[CRITICAL] Failed to pin consumer to core 3.\n";
    }

    std::cout << "[THREAD 2] Consumer running on core 3.\n";

    StereoFrame frame;
    char        name[32];

    while (buffer->pop(frame))
    {
        std::snprintf(name, sizeof(name), "%06llu.raw",
                      static_cast<unsigned long long>(frame.index));

        bool ok = true;

        const std::string p0 = cam_dirs[0] + "/" + name;
        std::ofstream out0(p0, std::ios::binary);
        if (out0)
        {
            out0.write(reinterpret_cast<const char*>(frame.cam0_data.data()),
                       static_cast<std::streamsize>(frame.cam0_data.size()));
            out0.close();
            if (!out0) ok = false;
        }
        else
        {
            ok = false;
        }

        const std::string p1 = cam_dirs[1] + "/" + name;
        std::ofstream out1(p1, std::ios::binary);
        if (out1)
        {
            out1.write(reinterpret_cast<const char*>(frame.cam1_data.data()),
                       static_cast<std::streamsize>(frame.cam1_data.size()));
            out1.close();
            if (!out1) ok = false;
        }
        else
        {
            ok = false;
        }

        if (ok)
        {
            stats->frames_written.fetch_add(1);
        }
        else
        {
            stats->write_errors.fetch_add(1);
            stats->recordMissing(frame.index);
            std::cerr << "[I/O] Write failure on index " << frame.index << "\n";
        }
    }

    std::cout << "[THREAD 2] Ring buffer fully drained, consumer stopped.\n";
}

// ---------------------------------------------------------------------------
// THREAD 1: PRODUCER
// ---------------------------------------------------------------------------

void producer_thread(CameraPtr                      cam0,
                     CameraPtr                      cam1,
                     std::shared_ptr<RingBuffer>    buffer,
                     std::shared_ptr<BurstStats>    stats,
                     Config                         cfg,
                     std::vector<CameraInfo>        cams)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(2, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0)
    {
        std::cerr << "[CRITICAL] Failed to pin producer to core 2.\n";
    }

    const uint64_t total_triggers = cfg.total_triggers();
    const int64_t  expected_bytes = cams[0].payload;

    std::cout << "[THREAD 1] Producer running on core 2. Target: " << total_triggers
              << " triggers at " << kSamplingRateHz << " Hz, trigger source " << cfg.trigger
              << ".\n";

    if (cfg.trigger != "software")
    {
        std::cerr << "[THREAD 1] WARNING: hardware triggering selected. This path has never "
                     "been exercised; if no pulse reaches the cameras the loop will time out "
                     "on every retrieval.\n";
    }

    // Reserved up front. A reallocation of several hundred kilobytes in the
    // middle of the acquisition loop is exactly the kind of latency spike the
    // timing campaign is meant to detect, and it would be self-inflicted.
    stats->timing.reserve(total_triggers + 4);

    int pps_fd = open(cfg.pps_device.c_str(), O_RDWR);
    if (pps_fd < 0)
    {
        std::cerr << "[CRITICAL] Cannot open " << cfg.pps_device << ": "
                  << std::strerror(errno) << "\n";
        buffer->shutdown();
        return;
    }

    pps_handle_t pps_handle;
    if (time_pps_create(pps_fd, &pps_handle) < 0)
    {
        std::cerr << "[CRITICAL] time_pps_create failed on " << cfg.pps_device << ": "
                  << std::strerror(errno) << "\n";
        close(pps_fd);
        buffer->shutdown();
        return;
    }

    try
    {
        cam0->BeginAcquisition();
        cam1->BeginAcquisition();

        std::cout << "[THREAD 1] Acquisition loop armed.\n";

        pps_info_t      info;
        struct timespec timeout      = {2, 0};
        uint64_t        next_index   = 1;
        bool            first_frame  = true;
        bool            geometry_bad = false;

        // Device frame identifiers are per-camera free-running counters. They
        // do not reset at BeginAcquisition and the two sensors were observed
        // starting two frames apart, so only the increment within one camera
        // carries meaning; comparing absolute values across sensors does not.
        int64_t  last_fid[2]           = {-1, -1};
        uint64_t gap_reports           = 0;
        int64_t  consecutive_overflows = 0;
        int64_t  consecutive_timeouts  = 0;
        auto     last_beat     = std::chrono::steady_clock::now();
        const auto beat_period = std::chrono::seconds(60);

        auto readChunks = [&](ImagePtr img, int cam, TimingRecord& rec) {
            if (!cams[0].chunk_frame_id) return;
            try
            {
                const ChunkData& cd = img->GetChunkData();
                const int64_t  fid  = cd.GetFrameID();
                const uint64_t dts  = cd.GetTimestamp();

                if (cam == 0) { rec.cam0_frame_id = fid; rec.cam0_dev_ts = dts; }
                else          { rec.cam1_frame_id = fid; rec.cam1_dev_ts = dts; }

                // A gap here is the one loss the application cannot otherwise
                // see: a frame the sensor exposed and the driver discarded
                // before GetNextImage was ever offered it.
                if (last_fid[cam] >= 0)
                {
                    const int64_t delta = fid - last_fid[cam];
                    if (delta > 1)
                    {
                        stats->fid_gaps.fetch_add(static_cast<uint64_t>(delta - 1));
                        if (gap_reports < 20)
                        {
                            ++gap_reports;
                            std::cerr << "[TRANSPORT] cam" << cam << " frame id jumped by "
                                      << delta << " at index " << rec.index << ": "
                                      << (delta - 1) << " frame(s) lost below the API.\n";
                        }
                    }
                }
                last_fid[cam] = fid;
            }
            catch (Spinnaker::Exception&)
            {
                // Chunk data is not always retrievable from an incomplete
                // buffer. Silent: the frame is already accounted for.
            }
        };

        // The pulse generator, in hardware trigger modes only. Requested once
        // and held for the whole burst: opening the chip four times a second,
        // on the one code path whose punctuality is the measurement, would be
        // paying jitter for nothing. Failure here is fatal to the burst rather
        // than degraded to software triggering, which would silently produce a
        // dataset acquired under a configuration the manifest does not
        // describe.
        std::unique_ptr<GpioTrigger> gpio;
        if (cfg.trigger != "software")
        {
            try
            {
                gpio = std::make_unique<GpioTrigger>(
                    cfg.gpio_chip, static_cast<unsigned int>(cfg.gpio_line_cam0),
                    static_cast<unsigned int>(cfg.gpio_line_cam1),
                    static_cast<unsigned int>(cfg.gpio_pulse_us));
                std::cout << "[THREAD 1] GPIO trigger on " << cfg.gpio_chip << " lines "
                          << cfg.gpio_line_cam0 << " and " << cfg.gpio_line_cam1
                          << ", open-drain, " << cfg.gpio_pulse_us << " us pulse.\n";
            }
            catch (const std::exception& e)
            {
                std::cerr << "[CRITICAL] " << e.what() << "\n";
                keep_running = false;
                buffer->shutdown();
                return;
            }
        }

        // One trigger event: fire both sensors, retrieve both frames, and
        // either push the pair or account for its loss. A pair is pushed only
        // if both halves are complete, a half-pair being useless for the
        // stereo product and merely a way to de-align the archive.
        auto fireOnce = [&](uint64_t index, uint64_t ts_ns, uint64_t anchor_ns) {
            stats->triggers_issued.fetch_add(1);

            TimingRecord rec;
            rec.index         = index;
            rec.pps_anchor_ns = anchor_ns;
            rec.planned_ns    = ts_ns;
            rec.trigger_ns    = nowRealtimeNs();

            if (cfg.trigger == "software")
            {
                // Two sequential USB transactions, so the two sensors do not
                // expose at the same instant. The offset is not visible in the
                // device timestamps, which are dominated by the 1.1 ppm drift
                // between the two sensor oscillators.
                cam0->TriggerSoftware.Execute();
                cam1->TriggerSoftware.Execute();
            }
            else if (gpio)
            {
                // One register write drops both lines together, so both
                // sensors see the same falling edge. This is the whole point
                // of the hardware path: two separate writes would reintroduce
                // the offset it exists to remove.
                if (!gpio->pulse()) stats->trigger_errors.fetch_add(1);
            }

            try
            {
                ImagePtr img0 = cam0->GetNextImage(1000);
                ImagePtr img1 = cam1->GetNextImage(1000);
                rec.retrieved_ns = nowRealtimeNs();

                readChunks(img0, 0, rec);
                readChunks(img1, 1, rec);

                if (!img0->IsIncomplete() && !img1->IsIncomplete())
                {
                    // Enabling chunk data inflates the transport payload. The
                    // image itself must still measure what the manifest
                    // claims, or every archived frame would carry trailing
                    // metadata that no reader expects.
                    if (first_frame)
                    {
                        first_frame = false;
                        const size_t got0 = img0->GetImageSize();
                        const size_t got1 = img1->GetImageSize();
                        std::cout << "[THREAD 1] First frame: image size " << got0 << " and "
                                  << got1 << " bytes, expected " << expected_bytes << ".\n";
                        if (static_cast<int64_t>(got0) != expected_bytes ||
                            static_cast<int64_t>(got1) != expected_bytes)
                        {
                            std::cerr << "[CRITICAL] Image size disagrees with the declared "
                                         "payload. Aborting rather than writing frames whose "
                                         "geometry the manifest misstates.\n";
                            geometry_bad = true;
                            keep_running = false;
                        }
                        std::cout << "[THREAD 1] Chunk data: cam0 frame id " << rec.cam0_frame_id
                                  << ", cam1 frame id " << rec.cam1_frame_id << ", device skew "
                                  << (static_cast<double>(rec.cam0_dev_ts) -
                                      static_cast<double>(rec.cam1_dev_ts)) / 1.0e6
                                  << " ms.\n";
                    }

                    if (!geometry_bad)
                    {
                        const bool stored =
                            buffer->push(static_cast<const uint8_t*>(img0->GetData()),
                                         static_cast<const uint8_t*>(img1->GetData()),
                                         ts_ns, index);
                        if (stored)
                        {
                            stats->frames_pushed.fetch_add(1);
                            rec.status         = 0;
                            consecutive_overflows = 0;
                        }
                        else
                        {
                            // The drive has fallen behind by the full depth of
                            // the buffer. The frame is lost, deliberately and
                            // countably, rather than by process death.
                            ++consecutive_overflows;
                            stats->buffer_overflows.fetch_add(1);
                            stats->recordMissing(index);
                            rec.status = 3;
                            std::cerr << "[OVERFLOW] index " << index << ": ring buffer full ("
                                      << buffer->capacity() << " slots), frame dropped ("
                                      << consecutive_overflows << " consecutive)\n";
                            if (consecutive_overflows >= cfg.max_consecutive_overflows)
                            {
                                std::cerr << "[CRITICAL] " << consecutive_overflows
                                          << " consecutive overflows: storage cannot sustain "
                                             "the ingestion rate. Abandoning the burst so that "
                                             "what has been written stays coherent.\n";
                                keep_running = false;
                            }
                        }
                    }
                }
                else
                {
                    stats->incomplete.fetch_add(1);
                    stats->recordMissing(index);
                    rec.status = 1;
                }

                img0 = nullptr;
                img1 = nullptr;
            }
            catch (Spinnaker::Exception& e)
            {
                rec.retrieved_ns = nowRealtimeNs();
                rec.status       = 2;
                stats->retrieval_errors.fetch_add(1);
                stats->recordMissing(index);
                if (keep_running)
                {
                    std::cerr << "[RETRIEVAL] index " << index << ": " << e.what() << "\n";
                }
            }

            stats->timing.push_back(rec);
        };

        while (keep_running && next_index <= total_triggers)
        {
            if (time_pps_fetch(pps_handle, PPS_TSFMT_TSPEC, &info, &timeout) < 0)
            {
                stats->pps_timeouts.fetch_add(1);
                ++consecutive_timeouts;
                if (keep_running)
                {
                    std::cerr << "[WARN] PPS timeout on " << cfg.pps_device << " ("
                              << consecutive_timeouts << " consecutive)\n";
                }
                // A timeout does not advance next_index, so without this
                // ceiling a silent receiver does not fail the burst: it makes
                // it run forever, holding the slot and queueing the next.
                if (consecutive_timeouts >= cfg.max_pps_timeouts)
                {
                    std::cerr << "[CRITICAL] " << consecutive_timeouts
                              << " consecutive PPS timeouts on " << cfg.pps_device
                              << ": no time reference. Abandoning the burst rather than "
                                 "waiting indefinitely.\n";
                    keep_running = false;
                }
                continue;
            }
            consecutive_timeouts = 0;

            const uint64_t t_base_ns =
                (static_cast<uint64_t>(info.assert_timestamp.tv_sec) * 1000000000ULL) +
                static_cast<uint64_t>(info.assert_timestamp.tv_nsec);

            // Frame A, on the second boundary.
            fireOnce(next_index++, t_base_ns, t_base_ns);

            // A burst of five thousand seconds that prints nothing until it
            // ends is a burst whose failure is discovered ninety minutes late.
            const auto now = std::chrono::steady_clock::now();
            if (now - last_beat >= beat_period)
            {
                last_beat = now;
                std::cout << "[BEAT] index " << (next_index - 1) << "/" << total_triggers
                          << ", written " << stats->frames_written.load()
                          << ", buffer " << buffer->occupancy() << "/" << buffer->capacity()
                          << " (peak " << buffer->highWater() << ")"
                          << ", incomplete " << stats->incomplete.load()
                          << ", transport gaps " << stats->fid_gaps.load() << std::endl;
            }

            if (!keep_running || next_index > total_triggers) break;

            // Frame B, at the absolute instant t_pps + 500 ms.
            //
            // The previous implementation slept a fixed 500 ms measured from
            // the moment frame A had been retrieved, which charged the whole
            // retrieval latency to the half-period: measurement gave 559.0 ms
            // for the A to B leg and 441.0 ms for B to A, an 11.8 per cent
            // modulation entering the f-k transforms of the OCM directly.
            //
            // The target is now the instant itself, in the same CLOCK_REALTIME
            // domain as the PPS assert timestamp, and the sleep is whatever
            // remains of it, around 447 ms once the 52.5 ms of two sequential
            // 16.13 MB USB transfers have been paid. clock_nanosleep with
            // TIMER_ABSTIME is used in preference to sleep_until on the steady
            // clock, which would require converting between two clock domains
            // and reintroduce the error it is meant to remove.
            const uint64_t slot_ns = t_base_ns + 500000000ULL;
            struct timespec slot;
            slot.tv_sec  = static_cast<time_t>(slot_ns / 1000000000ULL);
            slot.tv_nsec = static_cast<long>(slot_ns % 1000000000ULL);

            const uint64_t before_sleep = nowRealtimeNs();
            if (before_sleep >= slot_ns)
            {
                // Retrieval overran the half-period. Firing now would produce a
                // frame exposed late and stamped on time, which is a falsified
                // record and worse than a gap: the archive would look intact.
                stats->late_frames.fetch_add(1);
                stats->triggers_issued.fetch_add(1);
                stats->recordMissing(next_index);

                TimingRecord late;
                late.index         = next_index;
                late.pps_anchor_ns = t_base_ns;
                late.planned_ns    = slot_ns;
                late.trigger_ns    = before_sleep;
                late.status        = 4;
                stats->timing.push_back(late);

                std::cerr << "[LATE] index " << next_index << ": slot instant already "
                          << (before_sleep - slot_ns) / 1000 << " us in the past, frame "
                          << "skipped rather than stamped with a time it was not taken at\n";
                ++next_index;
                continue;
            }

            // EINTR is expected: a signal delivered mid-sleep must end it.
            while (clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &slot, nullptr) == EINTR)
            {
                if (!keep_running) break;
            }

            if (!keep_running) break;

            fireOnce(next_index++, slot_ns, t_base_ns);
        }

        // Read before EndAcquisition: the stream node map reports the counters
        // of the acquisition still in progress.
        readStreamCounters(cam0, stats->stream[0]);
        readStreamCounters(cam1, stats->stream[1]);

        cam0->EndAcquisition();
        cam1->EndAcquisition();
    }
    catch (Spinnaker::Exception& e)
    {
        std::cerr << "[PIPELINE] " << e.what() << "\n";
    }

    stats->buffer_high_water.store(buffer->highWater());
    stats->buffer_capacity.store(buffer->capacity());

    time_pps_destroy(pps_handle);
    close(pps_fd);
    buffer->shutdown();
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv)
{
    Config cfg;
    bool   help_requested = false;

    if (!parseArgs(argc, argv, cfg, help_requested))
    {
        return kExitUsage;
    }
    if (help_requested)
    {
        printUsage(argv[0]);
        return kExitOk;
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    const bool clock_ok = clockIsSynchronized();
    if (!clock_ok)
    {
        if (cfg.require_clock_sync)
        {
            std::cerr << "[FATAL] The system clock is not disciplined and "
                         "--require-clock-sync was given. Refusing to start: an archive "
                         "named from a stale date is worse than a missed slot.\n";
            return kExitRefused;
        }
        std::cerr << "[WARN] System clock is not synchronised. The burst directory will be "
                     "named from a possibly stale date; this is recorded in the manifest.\n";
    }

    // Before the SDK is touched, so that a station with no time reference
    // fails in seconds and leaves nothing behind. This also makes --dry-run
    // an answer to the question of whether the receiver is alive.
    {
        std::string pps_err;
        if (!resolvePpsDevice(cfg.pps_device, pps_err))
        {
            std::cerr << "[FATAL] " << pps_err << "\n";
            return kExitRefused;
        }
    }

    SystemPtr system = System::GetInstance();
    int       exit_code = kExitIncomplete;

    {
        // Scoped so that the session releases both cameras before
        // ReleaseInstance is called below.
        CameraSession session(system);
        std::string   err;

        if (!session.open(cfg, err))
        {
            std::cerr << "[FATAL] " << err << "\n";
            session.release();
            system->ReleaseInstance();
            return kExitRefused;
        }

        const std::vector<CameraInfo>& cams = session.info();
        const int64_t  payload  = cams[0].payload;
        const uint64_t required =
            cfg.total_triggers() * static_cast<uint64_t>(payload) * cams.size();

        std::cout << "\n[PLAN] " << cfg.duration_s << " s, " << cfg.total_triggers()
                  << " triggers, " << payload << " B/frame, " << cams.size() << " cameras -> "
                  << (required / 1e9) << " GB\n";
        for (const CameraInfo& c : cams)
        {
            std::cout << "[PLAN] cam" << c.role << " serial " << c.serial << ", " << c.width
                      << "x" << c.height << " " << c.pixel_format << ", " << c.shutter_mode
                      << " shutter\n";
            std::cout << "[PLAN]      exposure " << c.exposure_us << " us (auto "
                      << c.exposure_auto << "), gain " << c.gain_db << " dB (auto "
                      << c.gain_auto << "), white balance " << c.white_balance_auto
                      << ", gamma " << c.gamma_enable << "\n";
            std::cout << "[PLAN]      trigger " << c.trigger_source << ", stream buffers "
                      << c.stream_buffer_count << ", transport payload "
                      << c.transport_payload << " B, chunks "
                      << (c.chunk_frame_id ? "FrameID " : "")
                      << (c.chunk_timestamp ? "Timestamp" : "")
                      << ((!c.chunk_frame_id && !c.chunk_timestamp) ? "none" : "") << "\n";
        }

        if (!fs::exists(cfg.output_root))
        {
            std::cerr << "[FATAL] Output root " << cfg.output_root << " does not exist.\n";
            session.release();
            system->ReleaseInstance();
            return kExitRefused;
        }

        if (!checkFreeSpace(cfg.output_root, required, err))
        {
            std::cerr << "[FATAL] " << err << "\n";
            session.release();
            system->ReleaseInstance();
            return kExitRefused;
        }

        if (cfg.dry_run)
        {
            std::cout << "[DRY RUN] Configuration applied and read back, cameras bound, space "
                         "sufficient. Nothing written, nothing acquired.\n";
            session.release();
            system->ReleaseInstance();
            return kExitOk;
        }

        // ---- Burst directory tree ----

        const std::string burst_id   = utcStamp(true);
        const std::string burst_path = cfg.output_root + "/" + burst_id;

        std::vector<std::string> cam_dirs;
        try
        {
            if (fs::exists(burst_path))
            {
                std::cerr << "[FATAL] Burst directory " << burst_path << " already exists.\n";
                session.release();
                system->ReleaseInstance();
                return kExitRefused;
            }
            fs::create_directories(burst_path);
            for (const CameraInfo& c : cams)
            {
                const std::string d = burst_path + "/" + c.dirName();
                fs::create_directories(d);
                cam_dirs.push_back(d);
            }
        }
        catch (const fs::filesystem_error& e)
        {
            std::cerr << "[FATAL] Cannot create burst tree: " << e.what() << "\n";
            session.release();
            system->ReleaseInstance();
            return kExitRefused;
        }

        if (!writeManifest(burst_path + "/manifest.json", cfg, cams, burst_id, clock_ok))
        {
            std::cerr << "[FATAL] Manifest could not be written; aborting rather than "
                         "producing an undocumented archive.\n";
            session.release();
            system->ReleaseInstance();
            return kExitRefused;
        }

        std::cout << "[BURST] " << burst_path << "\n";

        // ---- Acquisition ----

        auto stats  = std::make_shared<BurstStats>();
        auto buffer = std::make_shared<RingBuffer>(cfg.buffer_frames,
                                                   static_cast<size_t>(payload));

        const auto t_start = std::chrono::steady_clock::now();

        std::thread t2(consumer_thread, buffer, stats, cam_dirs);
        std::thread t1(producer_thread, session.cam(0), session.cam(1), buffer, stats, cfg,
                       cams);

        t1.join();
        t2.join();

        const int sig = caught_signal.load();
        if (sig != 0)
        {
            std::cout << "\n[SYSTEM] Signal " << sig
                      << " received; the ring buffer was drained before exit.\n";
        }

        const double wall_s =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();

        // A burst that ran to term but shed frames is not a complete burst.
        const bool completed =
            keep_running.load() &&
            (stats->triggers_issued.load() >= cfg.total_triggers()) &&
            (stats->buffer_overflows.load() == 0) &&
            (stats->late_frames.load() == 0);

        writeSummary(burst_path + "/summary.json", cfg, *stats, wall_s, payload, cams.size(),
                     completed);
        writeTimingCsv(burst_path + "/timing.csv", stats->timing);

        std::cout << "\n[SUMMARY] " << stats->triggers_issued.load() << " triggers, "
                  << stats->frames_pushed.load() << " pushed, "
                  << stats->frames_written.load() << " written, "
                  << stats->incomplete.load() << " incomplete, "
                  << stats->retrieval_errors.load() << " retrieval errors, "
                  << stats->pps_timeouts.load() << " PPS timeouts, "
                  << stats->write_errors.load() << " write errors\n";
        std::cout << "[SUMMARY] " << wall_s << " s wall clock, "
                  << (static_cast<double>(stats->frames_written.load()) *
                      static_cast<double>(payload) * cams.size() / wall_s / 1e6)
                  << " MB/s mean\n";
        std::cout << "[SUMMARY] trigger errors " << stats->trigger_errors.load() << "\n";
        std::cout << "[SUMMARY] buffer overflows " << stats->buffer_overflows.load()
                  << ", late frames skipped " << stats->late_frames.load() << "\n";
        std::cout << "[SUMMARY] transport frame id gaps " << stats->fid_gaps.load()
                  << ", ring buffer peak " << stats->buffer_high_water.load() << "/"
                  << stats->buffer_capacity.load() << " frames\n";
        for (int c = 0; c < 2; ++c)
        {
            std::cout << "[SUMMARY] cam" << c << " transport: started "
                      << stats->stream[c].started << ", delivered "
                      << stats->stream[c].delivered << ", lost " << stats->stream[c].lost
                      << ", incomplete " << stats->stream[c].incomplete << ", dropped "
                      << stats->stream[c].dropped << "\n";
        }

        {
            const IntervalStats iv = computeIntervals(stats->timing);
            double   skew_mean = 0.0, skew_sd = 0.0;
            uint64_t skew_n = 0;
            computeDeviceSkew(stats->timing, skew_mean, skew_sd, skew_n);

            std::cout << "[CADENCE] " << iv.count << " intervals, mean " << iv.mean_ms
                      << " ms, sd " << iv.sd_ms << " ms, range [" << iv.min_ms << ", "
                      << iv.max_ms << "]\n";
            std::cout << "[CADENCE] A to B " << iv.mean_a_to_b_ms << " ms, B to A "
                      << iv.mean_b_to_a_ms << " ms (both should be 500)\n";
            std::cout << "[CADENCE] device skew mean " << skew_mean << " ms, sd " << skew_sd
                      << " ms over " << skew_n << " frames\n";
        }

        std::cout << "[SUMMARY] completed=" << (completed ? "true" : "false") << ", burst at "
                  << burst_path << "\n";

        exit_code = completed ? kExitOk : kExitIncomplete;
        session.release();
    }

    system->ReleaseInstance();
    return exit_code;
}