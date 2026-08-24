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
    f << "    " << jstr("trigger_source", "software") << ",\n";
    f << "    " << jstr("cadence_anchor", "gnss_pps_1hz_plus_500ms_interpolation") << "\n";
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
    f << "    \"write_errors\": "     << stats.write_errors.load() << "\n";
    f << "  },\n";
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