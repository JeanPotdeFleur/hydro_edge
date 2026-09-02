#pragma once

// Session.h
//
// Everything describing one acquisition burst that does not require the
// Spinnaker SDK: the command-line configuration, the inventory of the
// cameras taking part, the running counters, and the two JSON documents
// written to the burst directory.
//
// Kept free of Spinnaker headers on purpose, so that the burst layout and
// its metadata can be reasoned about, and later re-read, without the
// vendor SDK being installed.

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#ifndef HYDRO_EDGE_GIT_SHA
#define HYDRO_EDGE_GIT_SHA "unknown"
#endif

// Sampling rate of threshold No.8. Fixed here rather than exposed on the
// command line: the whole timing architecture, the 500 ms interpolation and
// the burst volumes of Appendix H all assume it.
constexpr double kSamplingRateHz = 2.0;

// Process exit codes. The distinction between a burst refused before it began
// and one that ran without completing is what makes a service unit safe to
// configure: the first must never be retried on a schedule that will refuse it
// again, and the second must never be retried at all, having already written
// its data.
constexpr int kExitOk         = 0;   // ran to term, no loss of any kind
constexpr int kExitIncomplete = 1;   // acquired, but shed frames or was stopped
constexpr int kExitUsage      = 2;   // malformed command line
constexpr int kExitRefused    = 3;   // refused before the first trigger

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

struct Config
{
    std::string output_root   = "/mnt/vault";
    double      duration_s    = 120.0;
    size_t      buffer_frames = 60;
    std::string cam0_serial;   // empty: fall back to enumeration order
    std::string cam1_serial;
    bool        dry_run       = false;

    // Radiometry. Locked by default: the OCM applies f-k transforms to
    // luminance time series, and an exposure that drifts frame to frame
    // injects its own modulation into the spectrum. 5000 us is the middle of
    // the 4-8 ms window of Appendix E, which freezes foam motion at f/4.
    double      exposure_us   = 5000.0;
    double      gain_db       = 0.0;
    bool        exposure_auto = false;   // bench escape hatch only

    // Triggering. Hardware branches are written but unvalidated: they need
    // cabling that is not yet on hand.
    std::string trigger       = "software";   // software | line2 | line0

    // Transport-layer buffer pool, per camera. Chosen rather than inherited:
    // the factory default of 9 is not a decision.
    int64_t     stream_buffers = 16;

    // Consecutive rejected pushes tolerated before the burst is abandoned.
    // A handful means the drive stuttered; a sustained run means it cannot
    // keep up, and continuing only produces an archive full of holes while
    // consuming the battery.
    int64_t     max_consecutive_overflows = 20;

    // PPS character device. Empty means resolve it at start-up by inspecting
    // the exported names under /sys/class/pps and confirming which source
    // actually pulses; filled on the command line, the operator's choice is
    // honoured. Whichever way it is obtained, the resolved path is what
    // reaches the manifest.
    std::string pps_device;

    // Consecutive PPS fetch timeouts tolerated before the burst is abandoned.
    // A timeout does not advance the trigger index, so without this ceiling a
    // silent receiver does not fail the burst, it makes it run forever. Thirty
    // fetches at a two-second timeout is a minute of silence.
    int64_t     max_pps_timeouts = 30;

    // GPIO used to drive the camera trigger lines in hardware modes. The
    // Raspberry Pi 5 exposes the 40-pin header as gpiochip4 through
    // pinctrl-rp1, where every previous generation used gpiochip0: an example
    // written for a Pi 4 addresses an internal Broadcom controller with no
    // header pins. Offsets 23 and 24 are physical pins 16 and 18; 18 is taken
    // by the PPS overlay and 14 and 15 carry the GNSS UART.
    std::string gpio_chip     = "gpiochip4";
    int64_t     gpio_line_cam0 = 23;
    int64_t     gpio_line_cam1 = 24;

    // Pulse width. Far above the sensor minimum and negligible against the
    // half-period; only the falling edge carries the instant, the line being
    // held high at rest by the camera pull-up.
    int64_t     gpio_pulse_us = 100;

    // Refuse to start unless the kernel reports the clock disciplined. The Pi
    // restores a stale date at boot and keeps it until the network answers, so
    // an unguarded burst can be filed weeks in the past. A missed slot is
    // recoverable; a misdated archive is not.
    bool        require_clock_sync = false;

    // Derived
    uint64_t total_triggers() const
    {
        return static_cast<uint64_t>(std::llround(duration_s * kSamplingRateHz));
    }
};

inline void printUsage(const char* argv0)
{
    std::cout
        << "Usage: " << argv0 << " [options]\n\n"
        << "  --output <dir>        Root under which the burst directory is created\n"
        << "                        (default: /mnt/vault). The burst directory itself\n"
        << "                        is named from the UTC start time and created here.\n"
        << "  --duration <seconds>  Burst length. Trigger count is duration x 2 Hz.\n"
        << "                        Reference profiles: 2400 (Strategy 3, field standard),\n"
        << "                        5400 (Strategy 2, boundary testing), 1024 (Strategy 1).\n"
        << "                        Default: 120.\n"
        << "  --buffer-frames <n>   Ring buffer depth in stereo slots (default: 60,\n"
        << "                        that is 30 s of absorption at 2 Hz).\n"
        << "  --cam0-serial <s>     Bind the cam0 role to this device serial number.\n"
        << "  --cam1-serial <s>     Bind the cam1 role to this device serial number.\n"
        << "                        If omitted, enumeration order is used and a warning\n"
        << "                        is emitted: USB enumeration order is not stable\n"
        << "                        across boots and the two fields of view may swap.\n"
        << "  --exposure-us <us>    Fixed exposure time (default: 5000). Ignored under\n"
        << "                        --exposure-auto.\n"
        << "  --gain-db <dB>        Fixed analog gain (default: 0).\n"
        << "  --exposure-auto       Leave ExposureAuto, GainAuto and BalanceWhiteAuto on\n"
        << "                        Continuous. Bench convenience only: a burst acquired\n"
        << "                        this way is radiometrically unusable for OCM, every\n"
        << "                        frame carrying its own exposure.\n"
        << "  --trigger <mode>      software (default), line2 or line0. The two hardware\n"
        << "                        modes are written but UNVALIDATED: no cabling yet.\n"
        << "  --stream-buffers <n>  Transport buffer pool per camera (default: 16, max 58).\n"
        << "  --max-overflows <n>   Consecutive ring buffer overflows tolerated before the\n"
        << "                        burst is abandoned (default: 20).\n"
        << "  --pps-device <path>   PPS character device, e.g. /dev/pps0. Omitted, the\n"
        << "                        device is resolved at start-up: every source under\n"
        << "                        /sys/class/pps is probed and the one whose pulse\n"
        << "                        counter advances is selected. Enumeration order is not\n"
        << "                        contractual, so a literal path is a hazard.\n"
        << "  --max-pps-timeouts <n>  Consecutive PPS fetch timeouts tolerated before the\n"
        << "                        burst is abandoned (default: 30, that is 60 s of\n"
        << "                        silence). A timeout does not advance the trigger\n"
        << "                        index, so without a ceiling a silent receiver makes\n"
        << "                        the burst run forever instead of failing.\n"
        << "  --gpio-chip <name>    gpiod chip carrying the trigger lines (default:\n"
        << "                        gpiochip4, which is the 40-pin header on a Pi 5;\n"
        << "                        gpiochip0 is an internal controller with no pins).\n"
        << "  --gpio-cam0 <n>       GPIO offset driving camera 0 (default: 23, pin 16).\n"
        << "  --gpio-cam1 <n>       GPIO offset driving camera 1 (default: 24, pin 18).\n"
        << "  --gpio-pulse-us <us>  Trigger pulse width (default: 100).\n"
        << "  --require-clock-sync  Refuse to start unless the kernel reports the system\n"
        << "                        clock disciplined. Intended for scheduled operation:\n"
        << "                        the Pi boots on a stale date until the network\n"
        << "                        answers, and a burst named from it is unusable.\n"
        << "  --dry-run             Validate configuration, enumerate cameras and check\n"
        << "                        free space, then exit without acquiring or writing.\n"
        << "  --help                This message.\n";
}

// Returns false on a malformed command line; ok is set to false and a message
// printed. Unknown options are rejected rather than ignored, so that a typo in
// a systemd unit fails loudly at start instead of silently acquiring with
// defaults.
inline bool parseArgs(int argc, char** argv, Config& cfg, bool& help_requested)
{
    help_requested = false;

    auto needValue = [&](int i, const char* opt) -> bool {
        if (i + 1 >= argc)
        {
            std::cerr << "[CONFIG] Option " << opt << " requires a value.\n";
            return false;
        }
        return true;
    };

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];

        if (arg == "--help" || arg == "-h")
        {
            help_requested = true;
            return true;
        }
        else if (arg == "--output")
        {
            if (!needValue(i, "--output")) return false;
            cfg.output_root = argv[++i];
        }
        else if (arg == "--duration")
        {
            if (!needValue(i, "--duration")) return false;
            cfg.duration_s = std::atof(argv[++i]);
            if (cfg.duration_s <= 0.0)
            {
                std::cerr << "[CONFIG] --duration must be strictly positive.\n";
                return false;
            }
        }
        else if (arg == "--buffer-frames")
        {
            if (!needValue(i, "--buffer-frames")) return false;
            const long n = std::atol(argv[++i]);
            if (n < 2)
            {
                std::cerr << "[CONFIG] --buffer-frames must be at least 2.\n";
                return false;
            }
            cfg.buffer_frames = static_cast<size_t>(n);
        }
        else if (arg == "--cam0-serial")
        {
            if (!needValue(i, "--cam0-serial")) return false;
            cfg.cam0_serial = argv[++i];
        }
        else if (arg == "--cam1-serial")
        {
            if (!needValue(i, "--cam1-serial")) return false;
            cfg.cam1_serial = argv[++i];
        }
        else if (arg == "--exposure-us")
        {
            if (!needValue(i, "--exposure-us")) return false;
            cfg.exposure_us = std::atof(argv[++i]);
            if (cfg.exposure_us <= 0.0)
            {
                std::cerr << "[CONFIG] --exposure-us must be strictly positive.\n";
                return false;
            }
        }
        else if (arg == "--gain-db")
        {
            if (!needValue(i, "--gain-db")) return false;
            cfg.gain_db = std::atof(argv[++i]);
            if (cfg.gain_db < 0.0)
            {
                std::cerr << "[CONFIG] --gain-db must not be negative.\n";
                return false;
            }
        }
        else if (arg == "--exposure-auto")
        {
            cfg.exposure_auto = true;
        }
        else if (arg == "--trigger")
        {
            if (!needValue(i, "--trigger")) return false;
            cfg.trigger = argv[++i];
            if (cfg.trigger != "software" && cfg.trigger != "line2" && cfg.trigger != "line0")
            {
                std::cerr << "[CONFIG] --trigger must be software, line2 or line0.\n";
                return false;
            }
        }
        else if (arg == "--stream-buffers")
        {
            if (!needValue(i, "--stream-buffers")) return false;
            cfg.stream_buffers = std::atol(argv[++i]);
            if (cfg.stream_buffers < 3 || cfg.stream_buffers > 58)
            {
                std::cerr << "[CONFIG] --stream-buffers must lie between 3 and 58.\n";
                return false;
            }
        }
        else if (arg == "--max-overflows")
        {
            if (!needValue(i, "--max-overflows")) return false;
            cfg.max_consecutive_overflows = std::atol(argv[++i]);
            if (cfg.max_consecutive_overflows < 1)
            {
                std::cerr << "[CONFIG] --max-overflows must be at least 1.\n";
                return false;
            }
        }
        else if (arg == "--pps-device")
        {
            if (!needValue(i, "--pps-device")) return false;
            cfg.pps_device = argv[++i];
        }
        else if (arg == "--max-pps-timeouts")
        {
            if (!needValue(i, "--max-pps-timeouts")) return false;
            cfg.max_pps_timeouts = std::atol(argv[++i]);
            if (cfg.max_pps_timeouts < 1)
            {
                std::cerr << "[CONFIG] --max-pps-timeouts must be at least 1.\n";
                return false;
            }
        }
        else if (arg == "--gpio-chip")
        {
            if (!needValue(i, "--gpio-chip")) return false;
            cfg.gpio_chip = argv[++i];
        }
        else if (arg == "--gpio-cam0")
        {
            if (!needValue(i, "--gpio-cam0")) return false;
            cfg.gpio_line_cam0 = std::atol(argv[++i]);
        }
        else if (arg == "--gpio-cam1")
        {
            if (!needValue(i, "--gpio-cam1")) return false;
            cfg.gpio_line_cam1 = std::atol(argv[++i]);
        }
        else if (arg == "--gpio-pulse-us")
        {
            if (!needValue(i, "--gpio-pulse-us")) return false;
            cfg.gpio_pulse_us = std::atol(argv[++i]);
            if (cfg.gpio_pulse_us < 1 || cfg.gpio_pulse_us > 100000)
            {
                std::cerr << "[CONFIG] --gpio-pulse-us must be between 1 and 100000.\n";
                return false;
            }
        }
        else if (arg == "--require-clock-sync")
        {
            cfg.require_clock_sync = true;
        }
        else if (arg == "--dry-run")
        {
            cfg.dry_run = true;
        }
        else
        {
            std::cerr << "[CONFIG] Unknown option: " << arg << "\n";
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Camera inventory
// ---------------------------------------------------------------------------

struct CameraInfo
{
    int         role = 0;          // 0 or 1, as bound by the operator
    std::string serial;
    std::string model;
    std::string firmware;
    int64_t     width        = 0;
    int64_t     height       = 0;
    int64_t     payload      = 0;  // bytes per frame
    std::string pixel_format;
    std::string shutter_mode;
    std::string exposure_auto;
    std::string gain_auto;
    double      exposure_us  = 0.0;
    double      gain_db      = 0.0;
    std::string white_balance_auto;
    std::string gamma_enable;
    std::string trigger_source;
    std::string user_set_loaded;
    int64_t     transport_payload   = 0;   // PayloadSize once chunks are on
    int64_t     stream_buffer_count = 0;
    bool        chunk_frame_id      = false;
    bool        chunk_timestamp     = false;

    // Sub-directory name inside the burst directory. Carries both the role,
    // which the analysis needs, and the serial, which fixes physical
    // identity: a camera swap then shows in the directory name instead of
    // silently transposing two fields of view.
    std::string dirName() const
    {
        return "cam" + std::to_string(role) + "_" + serial;
    }
};

// ---------------------------------------------------------------------------
// Per-frame timing record
// ---------------------------------------------------------------------------
//
// One entry per trigger, accumulated in a pre-reserved vector and written to
// disk only after the burst ends. Recording is deliberately kept off the
// critical path: appending a line to a file four times a second would be
// harmless in itself, but the measurement must not perturb what it measures.

struct TimingRecord
{
    uint64_t index         = 0;
    uint64_t pps_anchor_ns = 0;   // PPS assert, kernel timestamp
    uint64_t planned_ns    = 0;   // the instant written into the archive
    uint64_t trigger_ns    = 0;   // CLOCK_REALTIME immediately before firing
    uint64_t retrieved_ns  = 0;   // CLOCK_REALTIME once both frames are in hand
    int64_t  cam0_frame_id = -1;
    int64_t  cam1_frame_id = -1;
    uint64_t cam0_dev_ts   = 0;
    uint64_t cam1_dev_ts   = 0;
    // 0 pushed, 1 incomplete, 2 retrieval error, 3 buffer overflow,
    // 4 skipped because its slot instant had already passed
    int      status        = 0;
};

// Transport-layer counters, read from the stream node map at the end of the
// burst. They see what the application cannot: a frame exposed by the sensor
// and discarded inside the driver never reaches GetNextImage and is invisible
// to any count kept above it.
struct StreamCounters
{
    int64_t started    = -1;
    int64_t delivered  = -1;
    int64_t lost       = -1;
    int64_t incomplete = -1;
    int64_t dropped    = -1;
};

// ---------------------------------------------------------------------------
// Burst statistics
// ---------------------------------------------------------------------------
//
// Provisional accounting. It records what the application itself observes:
// triggers issued, frames accepted, frames committed to disk, and the reasons
// a trigger produced nothing. It cannot yet detect a frame exposed by the
// sensor and lost in transport without the application ever seeing it; that
// requires the device FrameID chunk and the transport-layer counters, and is
// the object of the next patch.

struct BurstStats
{
    std::atomic<uint64_t> triggers_issued{0};
    std::atomic<uint64_t> frames_pushed{0};
    std::atomic<uint64_t> frames_written{0};
    std::atomic<uint64_t> incomplete{0};
    std::atomic<uint64_t> retrieval_errors{0};
    std::atomic<uint64_t> pps_timeouts{0};
    std::atomic<uint64_t> write_errors{0};
    std::atomic<uint64_t> buffer_overflows{0};
    std::atomic<uint64_t> late_frames{0};

    // A pulse the kernel refused. Counted rather than thrown: this is raised
    // on the producer thread, where an exception crossing the thread boundary
    // is an abort with extra steps.
    std::atomic<uint64_t> trigger_errors{0};

    // Frames the device exposed and the driver never delivered, inferred
    // from discontinuities in the device frame identifier.
    std::atomic<uint64_t> fid_gaps{0};

    // Deepest ring buffer occupancy, and its capacity, captured at shutdown.
    std::atomic<uint64_t> buffer_high_water{0};
    std::atomic<uint64_t> buffer_capacity{0};

    // Producer-only until the threads are joined; read by main afterwards.
    std::vector<TimingRecord> timing;
    StreamCounters            stream[2];

    mutable std::mutex    missing_mtx;
    std::vector<uint64_t> missing_indices;

    void recordMissing(uint64_t index)
    {
        std::lock_guard<std::mutex> lock(missing_mtx);
        missing_indices.push_back(index);
    }
};

// ---------------------------------------------------------------------------
// Time and JSON helpers
// ---------------------------------------------------------------------------

// Colons are legal on ext4 but break on exFAT and complicate shell paths, so
// the ISO 8601 time separators are replaced by hyphens. The trailing Z marks
// UTC unambiguously, and the result sorts chronologically under a plain ls.
inline std::string utcStamp(bool for_path)
{
    std::time_t now = std::time(nullptr);
    std::tm     tm{};
    gmtime_r(&now, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), for_path ? "%Y-%m-%dT%H-%M-%SZ" : "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

inline std::string jsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s)
    {
        switch (c)
        {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

inline std::string jstr(const std::string& key, const std::string& value)
{
    return "\"" + key + "\": \"" + jsonEscape(value) + "\"";
}

// ---------------------------------------------------------------------------
// Manifest: written before the first trigger
// ---------------------------------------------------------------------------
//
// Written at the start so that it exists even if the process is killed
// mid-burst. It makes the archive self-describing: without it the .raw files
// carry no header at all, and their geometry survives only as a hard-coded
// constant in whatever decoder happens to be at hand.

inline bool writeManifest(const std::string&             path,
                          const Config&                  cfg,
                          const std::vector<CameraInfo>& cams,
                          const std::string&             burst_id,
                          bool                           clock_synchronized)
{
    std::ofstream f(path);
    if (!f)
    {
        std::cerr << "[MANIFEST] Cannot open " << path << " for writing.\n";
        return false;
    }

    f << "{\n";
    f << "  " << jstr("schema_version", "1") << ",\n";
    f << "  " << jstr("burst_id", burst_id) << ",\n";
    f << "  " << jstr("started_utc", utcStamp(false)) << ",\n";
    f << "  " << jstr("git_commit", HYDRO_EDGE_GIT_SHA) << ",\n";
    f << "  \"clock_synchronized\": " << (clock_synchronized ? "true" : "false") << ",\n";

    f << "  \"acquisition\": {\n";
    f << "    \"sampling_rate_hz\": " << kSamplingRateHz << ",\n";
    f << "    \"duration_s\": " << cfg.duration_s << ",\n";
    f << "    \"target_triggers\": " << cfg.total_triggers() << ",\n";
    f << "    \"ring_buffer_frames\": " << cfg.buffer_frames << ",\n";
    f << "    " << jstr("trigger_source", cfg.trigger) << ",\n";
    f << "    \"stream_buffers_per_camera\": " << cfg.stream_buffers << ",\n";
    f << "    \"exposure_locked\": " << (cfg.exposure_auto ? "false" : "true") << ",\n";
    f << "    " << jstr("cadence_anchor", "gnss_pps_1hz_plus_500ms_interpolation") << ",\n";
    f << "    " << jstr("pps_device", cfg.pps_device) << ",\n";
    if (cfg.trigger != "software")
    {
        f << "    " << jstr("gpio_chip", cfg.gpio_chip) << ",\n";
        f << "    \"gpio_line_cam0\": " << cfg.gpio_line_cam0 << ",\n";
        f << "    \"gpio_line_cam1\": " << cfg.gpio_line_cam1 << ",\n";
        f << "    \"gpio_pulse_us\": " << cfg.gpio_pulse_us << ",\n";
    }
    f << "    \"clock_sync_required\": " << (cfg.require_clock_sync ? "true" : "false") << "\n";
    f << "  },\n";

    f << "  \"frame_indexing\": {\n";
    f << "    " << jstr("semantics", "index is the trigger ordinal, not a write counter") << ",\n";
    f << "    " << jstr("filename_format", "%06llu.raw") << ",\n";
    f << "    \"first_index\": 1,\n";
    f << "    " << jstr("gaps", "a missing file means that trigger produced no usable frame; "
                                "indices remain aligned across cameras") << "\n";
    f << "  },\n";

    f << "  \"cameras\": [\n";
    for (size_t i = 0; i < cams.size(); ++i)
    {
        const CameraInfo& c = cams[i];
        f << "    {\n";
        f << "      \"role\": " << c.role << ",\n";
        f << "      " << jstr("serial", c.serial) << ",\n";
        f << "      " << jstr("directory", c.dirName()) << ",\n";
        f << "      " << jstr("model", c.model) << ",\n";
        f << "      " << jstr("firmware", c.firmware) << ",\n";
        f << "      \"width\": " << c.width << ",\n";
        f << "      \"height\": " << c.height << ",\n";
        f << "      \"payload_bytes\": " << c.payload << ",\n";
        f << "      " << jstr("pixel_format", c.pixel_format) << ",\n";
        f << "      " << jstr("shutter_mode", c.shutter_mode) << ",\n";
        f << "      " << jstr("exposure_auto", c.exposure_auto) << ",\n";
        f << "      " << jstr("gain_auto", c.gain_auto) << ",\n";
        f << "      \"exposure_us\": " << c.exposure_us << ",\n";
        f << "      \"gain_db\": " << c.gain_db << ",\n";
        f << "      " << jstr("white_balance_auto", c.white_balance_auto) << ",\n";
        f << "      " << jstr("gamma_enable", c.gamma_enable) << ",\n";
        f << "      " << jstr("trigger_source", c.trigger_source) << ",\n";
        f << "      " << jstr("user_set_loaded", c.user_set_loaded) << ",\n";
        f << "      \"transport_payload_bytes\": " << c.transport_payload << ",\n";
        f << "      \"stream_buffer_count\": " << c.stream_buffer_count << ",\n";
        f << "      \"chunk_frame_id\": " << (c.chunk_frame_id ? "true" : "false") << ",\n";
        f << "      \"chunk_timestamp\": " << (c.chunk_timestamp ? "true" : "false") << ",\n";
        // Reserved for the real-time ROI crop of section 1.6. Full-frame
        // values today; once cropping is active a reader that ignores these
        // fields would misinterpret the geometry of every archived frame.
        f << "      \"roi_offset_x\": 0,\n";
        f << "      \"roi_offset_y\": 0,\n";
        f << "      \"roi_width\": " << c.width << ",\n";
        f << "      \"roi_height\": " << c.height << "\n";
        f << "    }" << (i + 1 < cams.size() ? "," : "") << "\n";
    }
    f << "  ]\n";
    f << "}\n";

    return f.good();
}

// ---------------------------------------------------------------------------
// Derived timing statistics
// ---------------------------------------------------------------------------

struct IntervalStats
{
    double   mean_ms = 0.0;
    double   sd_ms   = 0.0;
    double   min_ms  = 0.0;
    double   max_ms  = 0.0;
    // The loop alternates a frame anchored on the PPS edge with one placed
    // half a second later. Separating the two legs exposes any asymmetry
    // between them, which a pooled mean would hide by construction.
    double   mean_a_to_b_ms = 0.0;
    double   mean_b_to_a_ms = 0.0;
    uint64_t count          = 0;
};

inline IntervalStats computeIntervals(const std::vector<TimingRecord>& t)
{
    IntervalStats s;
    std::vector<double> d;
    d.reserve(t.size());

    double sum_ab = 0.0, sum_ba = 0.0;
    uint64_t n_ab = 0, n_ba = 0;

    for (size_t i = 1; i < t.size(); ++i)
    {
        if (t[i].trigger_ns == 0 || t[i - 1].trigger_ns == 0) continue;
        const double ms = static_cast<double>(t[i].trigger_ns - t[i - 1].trigger_ns) / 1.0e6;
        d.push_back(ms);
        // Odd trigger ordinals are PPS-anchored, so an interval ending on an
        // even ordinal is the A to B leg.
        if (t[i].index % 2 == 0) { sum_ab += ms; ++n_ab; }
        else                     { sum_ba += ms; ++n_ba; }
    }

    if (d.empty()) return s;

    s.count  = d.size();
    s.min_ms = d[0];
    s.max_ms = d[0];
    double sum = 0.0;
    for (double v : d)
    {
        sum += v;
        if (v < s.min_ms) s.min_ms = v;
        if (v > s.max_ms) s.max_ms = v;
    }
    s.mean_ms = sum / static_cast<double>(d.size());

    double var = 0.0;
    for (double v : d) var += (v - s.mean_ms) * (v - s.mean_ms);
    s.sd_ms = std::sqrt(var / static_cast<double>(d.size()));

    s.mean_a_to_b_ms = n_ab ? sum_ab / static_cast<double>(n_ab) : 0.0;
    s.mean_b_to_a_ms = n_ba ? sum_ba / static_cast<double>(n_ba) : 0.0;
    return s;
}

// Mean and spread of the offset between the two sensors' own timestamps. A
// constant offset points to distinct clock origins and is harmless; a varying
// one is jitter in the software trigger and would be an argument for wiring
// the hardware line.
inline void computeDeviceSkew(const std::vector<TimingRecord>& t, double& mean_ms,
                              double& sd_ms, uint64_t& count)
{
    std::vector<double> d;
    d.reserve(t.size());
    for (const TimingRecord& r : t)
    {
        if (r.status != 0 || r.cam0_dev_ts == 0 || r.cam1_dev_ts == 0) continue;
        d.push_back((static_cast<double>(r.cam0_dev_ts) -
                     static_cast<double>(r.cam1_dev_ts)) / 1.0e6);
    }
    count = d.size();
    if (d.empty()) { mean_ms = 0.0; sd_ms = 0.0; return; }

    double sum = 0.0;
    for (double v : d) sum += v;
    mean_ms = sum / static_cast<double>(d.size());
    double var = 0.0;
    for (double v : d) var += (v - mean_ms) * (v - mean_ms);
    sd_ms = std::sqrt(var / static_cast<double>(d.size()));
}

// One row per trigger. Plain CSV rather than JSON: the file holds thousands
// of rows and is meant to be loaded straight into MATLAB or pandas.
inline bool writeTimingCsv(const std::string& path, const std::vector<TimingRecord>& t)
{
    std::ofstream f(path);
    if (!f)
    {
        std::cerr << "[TIMING] Cannot open " << path << " for writing.\n";
        return false;
    }

    f << "index,status,pps_anchor_ns,planned_ns,trigger_ns,retrieved_ns,"
         "retrieval_latency_us,interval_ms,cam0_frame_id,cam1_frame_id,"
         "cam0_dev_ts,cam1_dev_ts,dev_skew_us\n";

    for (size_t i = 0; i < t.size(); ++i)
    {
        const TimingRecord& r = t[i];

        const double latency_us =
            (r.retrieved_ns && r.trigger_ns)
                ? static_cast<double>(r.retrieved_ns - r.trigger_ns) / 1.0e3
                : 0.0;

        double interval_ms = 0.0;
        if (i > 0 && r.trigger_ns && t[i - 1].trigger_ns)
        {
            interval_ms = static_cast<double>(r.trigger_ns - t[i - 1].trigger_ns) / 1.0e6;
        }

        const double skew_us =
            (r.cam0_dev_ts && r.cam1_dev_ts)
                ? (static_cast<double>(r.cam0_dev_ts) - static_cast<double>(r.cam1_dev_ts)) / 1.0e3
                : 0.0;

        f << r.index << ',' << r.status << ',' << r.pps_anchor_ns << ',' << r.planned_ns << ','
          << r.trigger_ns << ',' << r.retrieved_ns << ',' << latency_us << ',' << interval_ms
          << ',' << r.cam0_frame_id << ',' << r.cam1_frame_id << ',' << r.cam0_dev_ts << ','
          << r.cam1_dev_ts << ',' << skew_us << '\n';
    }

    return f.good();
}

// ---------------------------------------------------------------------------
// Summary: written after a clean shutdown
// ---------------------------------------------------------------------------
//
// Its absence is itself a signal: a burst directory holding a manifest but no
// summary was interrupted, and no file count has to be compared to establish
// that.

inline bool writeSummary(const std::string& path,
                         const Config&      cfg,
                         const BurstStats&  stats,
                         double             wall_clock_s,
                         int64_t            payload_bytes,
                         size_t             camera_count,
                         bool               completed)
{
    std::ofstream f(path);
    if (!f)
    {
        std::cerr << "[SUMMARY] Cannot open " << path << " for writing.\n";
        return false;
    }

    const uint64_t written = stats.frames_written.load();
    const double   bytes   = static_cast<double>(written) *
                             static_cast<double>(payload_bytes) *
                             static_cast<double>(camera_count);
    const double   mbps    = (wall_clock_s > 0.0) ? (bytes / wall_clock_s / 1.0e6) : 0.0;

    std::vector<uint64_t> missing;
    {
        std::lock_guard<std::mutex> lock(stats.missing_mtx);
        missing = stats.missing_indices;
    }

    f << "{\n";
    f << "  " << jstr("schema_version", "1") << ",\n";
    f << "  " << jstr("ended_utc", utcStamp(false)) << ",\n";
    f << "  \"completed\": " << (completed ? "true" : "false") << ",\n";
    f << "  \"wall_clock_s\": " << wall_clock_s << ",\n";
    f << "  \"counters\": {\n";
    f << "    \"target_triggers\": "  << cfg.total_triggers() << ",\n";
    f << "    \"triggers_issued\": "  << stats.triggers_issued.load() << ",\n";
    f << "    \"frames_pushed\": "    << stats.frames_pushed.load() << ",\n";
    f << "    \"frames_written\": "   << written << ",\n";
    f << "    \"incomplete\": "       << stats.incomplete.load() << ",\n";
    f << "    \"retrieval_errors\": " << stats.retrieval_errors.load() << ",\n";
    f << "    \"pps_timeouts\": "     << stats.pps_timeouts.load() << ",\n";
    f << "    \"write_errors\": "     << stats.write_errors.load() << ",\n";
    f << "    \"transport_frame_id_gaps\": " << stats.fid_gaps.load() << ",\n";
    f << "    \"buffer_overflows\": "        << stats.buffer_overflows.load() << ",\n";
    f << "    \"late_frames_skipped\": "     << stats.late_frames.load() << ",\n";
    f << "    \"trigger_errors\": "          << stats.trigger_errors.load() << "\n";
    f << "  },\n";

    f << "  \"transport_counters\": [\n";
    for (int c = 0; c < 2; ++c)
    {
        f << "    { \"camera\": " << c
          << ", \"started\": "    << stats.stream[c].started
          << ", \"delivered\": "  << stats.stream[c].delivered
          << ", \"lost\": "       << stats.stream[c].lost
          << ", \"incomplete\": " << stats.stream[c].incomplete
          << ", \"dropped\": "    << stats.stream[c].dropped
          << " }" << (c == 0 ? "," : "") << "\n";
    }
    f << "  ],\n";

    f << "  \"ring_buffer\": {\n";
    f << "    \"capacity_frames\": "   << stats.buffer_capacity.load() << ",\n";
    f << "    \"high_water_frames\": " << stats.buffer_high_water.load() << "\n";
    f << "  },\n";

    {
        const IntervalStats iv = computeIntervals(stats.timing);
        double skew_mean = 0.0, skew_sd = 0.0;
        uint64_t skew_n = 0;
        computeDeviceSkew(stats.timing, skew_mean, skew_sd, skew_n);

        f << "  \"cadence\": {\n";
        f << "    \"intervals\": "       << iv.count << ",\n";
        f << "    \"mean_ms\": "         << iv.mean_ms << ",\n";
        f << "    \"sd_ms\": "           << iv.sd_ms << ",\n";
        f << "    \"min_ms\": "          << iv.min_ms << ",\n";
        f << "    \"max_ms\": "          << iv.max_ms << ",\n";
        f << "    \"mean_a_to_b_ms\": "  << iv.mean_a_to_b_ms << ",\n";
        f << "    \"mean_b_to_a_ms\": "  << iv.mean_b_to_a_ms << "\n";
        f << "  },\n";

        f << "  \"device_skew\": {\n";
        f << "    \"samples\": " << skew_n << ",\n";
        f << "    \"mean_ms\": " << skew_mean << ",\n";
        f << "    \"sd_ms\": "   << skew_sd << "\n";
        f << "  },\n";
    }
    f << "  \"mean_write_MBps\": " << mbps << ",\n";
    f << "  \"missing_index_count\": " << missing.size() << ",\n";
    f << "  \"missing_indices\": [";
    const size_t cap = 500;   // a longer list belongs in a log, not a summary
    for (size_t i = 0; i < missing.size() && i < cap; ++i)
    {
        f << (i ? ", " : "") << missing[i];
    }
    f << "],\n";
    f << "  \"missing_indices_truncated\": " << (missing.size() > cap ? "true" : "false") << "\n";
    f << "}\n";

    return f.good();
}