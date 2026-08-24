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
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
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

#include "RingBuffer.h"
#include "Session.h"

using namespace Spinnaker;
using namespace Spinnaker::GenApi;
using namespace Spinnaker::GenICam;

namespace fs = std::filesystem;

std::atomic<bool> keep_running{true};

void sigint_handler(int signum)
{
    std::cout << "\n[SYSTEM] Signal " << signum << " intercepted. Initiating graceful shutdown..."
              << std::endl;
    keep_running = false;
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
// Camera inventory and role binding
// ---------------------------------------------------------------------------

static bool inspectCameras(SystemPtr                system,
                           const Config&            cfg,
                           std::vector<CameraInfo>& out,
                           std::string&             err)
{
    out.clear();

    CameraList camList = system->GetCameras();
    const unsigned int n = camList.GetSize();

    if (n < 2)
    {
        err = "incomplete topology: " + std::to_string(n) + " camera(s) enumerated, 2 required";
        camList.Clear();
        return false;
    }

    const bool bind_by_serial = !cfg.cam0_serial.empty() && !cfg.cam1_serial.empty();
    if (!bind_by_serial)
    {
        std::cerr << "[WARN] No serial numbers supplied: falling back to USB enumeration "
                     "order. That order is not stable across boots and the two fields of "
                     "view may transpose between runs. Use --cam0-serial / --cam1-serial "
                     "for any acquisition whose output will be kept.\n";
    }
    if (bind_by_serial && cfg.cam0_serial == cfg.cam1_serial)
    {
        err = "--cam0-serial and --cam1-serial are identical";
        camList.Clear();
        return false;
    }

    const std::string wanted[2] = {cfg.cam0_serial, cfg.cam1_serial};

    for (int role = 0; role < 2; ++role)
    {
        CameraPtr pCam = nullptr;

        if (bind_by_serial)
        {
            try
            {
                pCam = camList.GetBySerial(wanted[role]);
            }
            catch (Spinnaker::Exception&)
            {
                pCam = nullptr;
            }
            if (!pCam.IsValid())
            {
                err = "no camera with serial " + wanted[role] + " is attached";
                camList.Clear();
                return false;
            }
        }
        else
        {
            pCam = camList.GetByIndex(static_cast<unsigned int>(role));
        }

        CameraInfo info;
        info.role = role;

        try
        {
            pCam->Init();
            INodeMap& m = pCam->GetNodeMap();

            info.serial        = nodeStr(m, "DeviceSerialNumber");
            info.model         = nodeStr(m, "DeviceModelName");
            info.firmware      = nodeStr(m, "DeviceFirmwareVersion");
            info.pixel_format  = nodeStr(m, "PixelFormat");
            info.shutter_mode  = nodeStr(m, "SensorShutterMode");
            info.exposure_auto = nodeStr(m, "ExposureAuto");
            info.gain_auto     = nodeStr(m, "GainAuto");
            info.width         = nodeInt(m, "Width", 0);
            info.height        = nodeInt(m, "Height", 0);
            info.payload       = nodeInt(m, "PayloadSize", 0);
            info.exposure_us   = nodeFloat(m, "ExposureTime", 0.0);
            info.gain_db       = nodeFloat(m, "Gain", 0.0);

            pCam->DeInit();
        }
        catch (Spinnaker::Exception& e)
        {
            err = std::string("camera inspection failed: ") + e.what();
            pCam = nullptr;
            camList.Clear();
            return false;
        }

        pCam = nullptr;

        if (info.payload <= 0)
        {
            err = "camera " + info.serial + " reports a null payload size";
            camList.Clear();
            return false;
        }

        // The serial ends up in a directory name and is the only durable
        // handle on physical identity, so an unreadable one is fatal rather
        // than cosmetic.
        if (info.serial.empty() || info.serial.find('<') != std::string::npos)
        {
            err = "camera at role " + std::to_string(role) +
                  " does not report a usable serial number";
            camList.Clear();
            return false;
        }

        out.push_back(info);
    }

    camList.Clear();

    if (out[0].payload != out[1].payload)
    {
        err = "the two cameras report different payload sizes (" +
              std::to_string(out[0].payload) + " and " + std::to_string(out[1].payload) +
              "): their configuration diverges and stereo frames would not match";
        return false;
    }

    return true;
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

void producer_thread(SystemPtr                      system,
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
    std::cout << "[THREAD 1] Producer running on core 2. Target: " << total_triggers
              << " triggers at " << kSamplingRateHz << " Hz.\n";

    int pps_fd = open("/dev/pps0", O_RDWR);
    if (pps_fd < 0)
    {
        std::cerr << "[CRITICAL] Cannot open /dev/pps0.\n";
        buffer->shutdown();
        return;
    }

    pps_handle_t pps_handle;
    if (time_pps_create(pps_fd, &pps_handle) < 0)
    {
        std::cerr << "[CRITICAL] time_pps_create failed.\n";
        close(pps_fd);
        buffer->shutdown();
        return;
    }

    {
        CameraList camList = system->GetCameras();

        CameraPtr cam0 = nullptr;
        CameraPtr cam1 = nullptr;
        try
        {
            cam0 = camList.GetBySerial(cams[0].serial);
            cam1 = camList.GetBySerial(cams[1].serial);
        }
        catch (Spinnaker::Exception& e)
        {
            std::cerr << "[CRITICAL] Serial binding failed: " << e.what() << "\n";
        }

        if (!cam0.IsValid() || !cam1.IsValid())
        {
            std::cerr << "[CRITICAL] Cameras " << cams[0].serial << " and " << cams[1].serial
                      << " could not both be bound.\n";
            camList.Clear();
            time_pps_destroy(pps_handle);
            close(pps_fd);
            buffer->shutdown();
            return;
        }

        try
        {
            cam0->Init();
            cam1->Init();

            cam0->TriggerMode.SetValue(TriggerModeEnums::TriggerMode_Off);
            cam0->TriggerSource.SetValue(TriggerSourceEnums::TriggerSource_Software);
            cam0->TriggerMode.SetValue(TriggerModeEnums::TriggerMode_On);

            cam1->TriggerMode.SetValue(TriggerModeEnums::TriggerMode_Off);
            cam1->TriggerSource.SetValue(TriggerSourceEnums::TriggerSource_Software);
            cam1->TriggerMode.SetValue(TriggerModeEnums::TriggerMode_On);

            cam0->BeginAcquisition();
            cam1->BeginAcquisition();

            std::cout << "[THREAD 1] Acquisition loop armed.\n";

            pps_info_t      info;
            struct timespec timeout = {2, 0};
            uint64_t        next_index = 1;

            // One trigger event: fire both sensors, retrieve both frames, and
            // either push the pair or account for its loss. A pair is pushed
            // only if both halves are complete, a half-pair being useless for
            // the stereo product and merely a way to de-align the archive.
            auto fireOnce = [&](uint64_t index, uint64_t ts_ns) {
                stats->triggers_issued.fetch_add(1);

                cam0->TriggerSoftware.Execute();
                cam1->TriggerSoftware.Execute();

                try
                {
                    ImagePtr img0 = cam0->GetNextImage(1000);
                    ImagePtr img1 = cam1->GetNextImage(1000);

                    if (!img0->IsIncomplete() && !img1->IsIncomplete())
                    {
                        buffer->push(static_cast<const uint8_t*>(img0->GetData()),
                                     static_cast<const uint8_t*>(img1->GetData()),
                                     ts_ns, index);
                        stats->frames_pushed.fetch_add(1);
                    }
                    else
                    {
                        stats->incomplete.fetch_add(1);
                        stats->recordMissing(index);
                    }

                    img0 = nullptr;
                    img1 = nullptr;
                }
                catch (Spinnaker::Exception& e)
                {
                    stats->retrieval_errors.fetch_add(1);
                    stats->recordMissing(index);
                    if (keep_running)
                    {
                        std::cerr << "[RETRIEVAL] index " << index << ": " << e.what() << "\n";
                    }
                }
            };

            while (keep_running && next_index <= total_triggers)
            {
                if (time_pps_fetch(pps_handle, PPS_TSFMT_TSPEC, &info, &timeout) < 0)
                {
                    stats->pps_timeouts.fetch_add(1);
                    if (keep_running) std::cerr << "[WARN] PPS timeout.\n";
                    continue;
                }

                const uint64_t t_base_ns =
                    (static_cast<uint64_t>(info.assert_timestamp.tv_sec) * 1000000000ULL) +
                    static_cast<uint64_t>(info.assert_timestamp.tv_nsec);

                // Frame A, on the second boundary.
                fireOnce(next_index++, t_base_ns);

                if (!keep_running || next_index > total_triggers) break;

                // Frame B, half a second later.
                //
                // KNOWN DEFECT, scheduled for a later patch: the delay is
                // measured from now(), that is from the moment frame A was
                // retrieved, not from the PPS edge. Retrieval latency is
                // therefore added to the half-period, producing alternating
                // intervals around 510 and 490 ms. The timestamp written to
                // the archive nevertheless claims an exact +500 ms.
                auto target = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                std::this_thread::sleep_until(target);

                if (!keep_running) break;

                fireOnce(next_index++, t_base_ns + 500000000ULL);
            }

            cam0->TriggerMode.SetValue(TriggerModeEnums::TriggerMode_Off);
            cam1->TriggerMode.SetValue(TriggerModeEnums::TriggerMode_Off);
            cam0->EndAcquisition();
            cam1->EndAcquisition();
            cam0->DeInit();
            cam1->DeInit();
        }
        catch (Spinnaker::Exception& e)
        {
            std::cerr << "[PIPELINE] " << e.what() << "\n";
        }

        cam0 = nullptr;
        cam1 = nullptr;
        camList.Clear();
    }

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
        return 2;
    }
    if (help_requested)
    {
        printUsage(argv[0]);
        return 0;
    }

    std::signal(SIGINT, sigint_handler);

    const bool clock_ok = clockIsSynchronized();
    if (!clock_ok)
    {
        std::cerr << "[WARN] System clock is not synchronised. The burst directory will be "
                     "named from a possibly stale date; this is recorded in the manifest.\n";
    }

    SystemPtr system = System::GetInstance();

    std::vector<CameraInfo> cams;
    std::string             err;
    if (!inspectCameras(system, cfg, cams, err))
    {
        std::cerr << "[FATAL] " << err << "\n";
        system->ReleaseInstance();
        return 1;
    }

    const int64_t  payload  = cams[0].payload;
    const uint64_t required = cfg.total_triggers() * static_cast<uint64_t>(payload) * cams.size();

    std::cout << "\n[PLAN] " << cfg.duration_s << " s, " << cfg.total_triggers()
              << " triggers, " << payload << " B/frame, " << cams.size() << " cameras -> "
              << (required / 1e9) << " GB\n";
    for (const CameraInfo& c : cams)
    {
        std::cout << "[PLAN] cam" << c.role << " serial " << c.serial << ", " << c.width << "x"
                  << c.height << " " << c.pixel_format << ", exposure " << c.exposure_us
                  << " us (" << c.exposure_auto << "), gain " << c.gain_db << " dB ("
                  << c.gain_auto << ")\n";
    }

    if (!fs::exists(cfg.output_root))
    {
        std::cerr << "[FATAL] Output root " << cfg.output_root << " does not exist.\n";
        system->ReleaseInstance();
        return 1;
    }

    if (!checkFreeSpace(cfg.output_root, required, err))
    {
        std::cerr << "[FATAL] " << err << "\n";
        system->ReleaseInstance();
        return 1;
    }

    if (cfg.dry_run)
    {
        std::cout << "[DRY RUN] Configuration valid, cameras bound, space sufficient. "
                     "Nothing written, nothing acquired.\n";
        system->ReleaseInstance();
        return 0;
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
            system->ReleaseInstance();
            return 1;
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
        system->ReleaseInstance();
        return 1;
    }

    if (!writeManifest(burst_path + "/manifest.json", cfg, cams, burst_id, clock_ok))
    {
        std::cerr << "[FATAL] Manifest could not be written; aborting rather than producing "
                     "an undocumented archive.\n";
        system->ReleaseInstance();
        return 1;
    }

    std::cout << "[BURST] " << burst_path << "\n";

    // ---- Acquisition ----

    auto stats  = std::make_shared<BurstStats>();
    auto buffer = std::make_shared<RingBuffer>(cfg.buffer_frames, static_cast<size_t>(payload));

    const auto t_start = std::chrono::steady_clock::now();

    std::thread t2(consumer_thread, buffer, stats, cam_dirs);
    std::thread t1(producer_thread, system, buffer, stats, cfg, cams);

    t1.join();
    t2.join();

    const double wall_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();

    const bool completed =
        keep_running.load() && (stats->triggers_issued.load() >= cfg.total_triggers());

    writeSummary(burst_path + "/summary.json", cfg, *stats, wall_s, payload, cams.size(),
                 completed);

    std::cout << "\n[SUMMARY] " << stats->triggers_issued.load() << " triggers, "
              << stats->frames_pushed.load() << " pushed, " << stats->frames_written.load()
              << " written, " << stats->incomplete.load() << " incomplete, "
              << stats->retrieval_errors.load() << " retrieval errors, "
              << stats->pps_timeouts.load() << " PPS timeouts, "
              << stats->write_errors.load() << " write errors\n";
    std::cout << "[SUMMARY] " << wall_s << " s wall clock, "
              << (static_cast<double>(stats->frames_written.load()) *
                  static_cast<double>(payload) * cams.size() / wall_s / 1e6)
              << " MB/s mean\n";
    std::cout << "[SUMMARY] completed=" << (completed ? "true" : "false") << ", burst at "
              << burst_path << "\n";

    system->ReleaseInstance();
    return completed ? 0 : 1;
}