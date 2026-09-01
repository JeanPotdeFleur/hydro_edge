// cam_focus.cpp
//
// Field focus and exposure aid for the Hopkins rocky-shore camera station.
// Threshold No.3 requires a visual debugging interface for a headless node
// during in-field optical calibration; this is it.
//
// One or two cameras, free-running, no PPS, no ring buffer, no vault, no
// manifest. It grabs frames, measures how sharp a chosen region is and how
// close the scene is to clipping, and writes one annotated JPEG the operator
// watches while turning the focus and iris rings.
//
// With two serials the panels are composed side by side into a single file.
// Two separate previews would not show what aiming the station actually
// requires, which is how the two fields of view overlap; and a setting is
// applied to both sensors at once, since two cameras metering independently
// lose radiometric comparability exactly where their fields meet.
//
// Deliberately a separate binary from the acquisition path, for the same
// reason cam_probe is: a diagnostic that cannot touch the critical path is a
// diagnostic that can be run on a deployed station without risk. The
// acquisition binary also refuses to start with fewer than two cameras, and
// housings are adjusted one at a time.
//
// The GenICam helpers below duplicate those in main.cpp. The duplication is
// accepted: factoring them out would mean editing the file validated by
// GATE A1.
//
// --- Why the sharpness metric is not computed on the raw buffer ---
//
// In BayerRG8 two neighbouring pixels belong to different colour channels, so
// a Laplacian over the raw mosaic measures the colour filter array before it
// measures the scene. Simulated on a synthetic coastal scene (blue water,
// white foam, dark rock) swept through defocus, the variance of the Laplacian
// computed on the raw mosaic moved by 10 per cent end to end, against a
// factor of 157 once demosaiced. The tool therefore crops the region of
// interest at even offsets, which preserves the CFA phase, demosaics that
// crop alone, converts to grey and measures there. The cost is negligible on
// a few hundred kilopixels, and the metric then describes the sharpness of
// the quantity the OCM actually consumes: demosaiced luminance.
//
// Two consequences of the same simulation, worth knowing before turning a
// ring. Sensor noise raises the floor and flattens the metric: at 5 DN of
// noise the same sweep collapsed from a factor of 157 to a factor of 7, which
// argues for focusing at full aperture where there is light to spare. And
// near best focus the metric is intrinsically shallow, a 1.2x change for the
// first half-pixel of blur, which matches the depth of focus: at f/4 with a
// circle of confusion of two pixels, every ring position between roughly 20 m
// and beyond infinity is optically indistinguishable on a distant target.
// What this tool establishes is that the lens sits on that plateau and not
// grossly outside it.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/opencv.hpp>

#include "Spinnaker.h"
#include "SpinGenApi/SpinnakerGenApi.h"

using namespace Spinnaker;
using namespace Spinnaker::GenApi;
using namespace Spinnaker::GenICam;

// ---------------------------------------------------------------------------

static std::atomic<bool> keep_running{true};

extern "C" void focus_signal_handler(int) { keep_running.store(false, std::memory_order_relaxed); }

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

struct FocusConfig
{
    std::vector<std::string> serials;            // one or two, required
    std::string out_path   = "/dev/shm/focus.jpg";
    double      exposure_us = 2000.0;
    double      gain_db     = 0.0;
    bool        exposure_auto = false;
    double      rate_hz     = 2.0;
    int         roi_x = -1, roi_y = -1, roi_w = 800, roi_h = 600;   // -1: centre
    int         preview_width = 1330;
    int         jpeg_quality  = 85;
    size_t      history       = 120;             // 60 s at 2 Hz
    size_t      median_window = 5;
};

static void printUsage(const char* argv0)
{
    std::cout
        << "Usage: " << argv0 << " --serial <n> [options]\n\n"
        << "Free-running single-camera focus and exposure aid. Writes an annotated\n"
        << "JPEG at a fixed path once per frame and prints one line per frame.\n\n"
        << "  --serial <n>          Device serial number. Required, and may be given\n"
        << "                        twice: the two panels are then composed side by side\n"
        << "                        into one file, which is what judging the overlap\n"
        << "                        between the two fields of view needs. Enumeration\n"
        << "                        order is not stable, hence serials rather than index.\n"
        << "  --out <path>          Preview image (default: /dev/shm/focus.jpg). Kept on\n"
        << "                        tmpfs on purpose: hours of JPEG writes at 2 Hz have\n"
        << "                        no business on the boot microSD or the data vault.\n"
        << "                        Written to a temporary and renamed, so a viewer never\n"
        << "                        sees a half-written file.\n"
        << "  --roi x,y,w,h         Measurement region in sensor pixels. Offsets and size\n"
        << "                        are forced even to preserve the Bayer phase. Default:\n"
        << "                        800x600 centred. A centred default is rarely right on\n"
        << "                        a grazing view, where the top of the frame is 200 m\n"
        << "                        away or sky; the drawn rectangle shows what is being\n"
        << "                        measured.\n"
        << "  --exposure-us <us>    Fixed exposure (default: 2000). In full sun at f/4 the\n"
        << "                        working value is a few hundred microseconds, not the\n"
        << "                        5000 of the acquisition default: white foam clips well\n"
        << "                        before that. Adjustable live, see below.\n"
        << "  --gain-db <dB>        Fixed analog gain (default: 0).\n"
        << "  --exposure-auto       Let the camera meter. Useful to find the order of\n"
        << "                        magnitude, then read it off and lock it.\n"
        << "  --rate <Hz>           Free-run frame rate (default: 2).\n"
        << "  --preview-width <px>  Preview width (default: 1330).\n"
        << "  --help                This message.\n\n"
        << "Live commands on stdin, one per line:\n"
        << "  e <us>   set exposure        g <dB>  set gain      (applied to both)\n"
        << "  a        automatic exposure  m       manual, keep current values\n"
        << "  r        reset the peak      q       quit\n";
}

static bool parseRoi(const std::string& s, FocusConfig& cfg)
{
    int v[4];
    if (std::sscanf(s.c_str(), "%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3]) != 4) return false;
    cfg.roi_x = v[0]; cfg.roi_y = v[1]; cfg.roi_w = v[2]; cfg.roi_h = v[3];
    return true;
}

static bool parseArgs(int argc, char** argv, FocusConfig& cfg, bool& help)
{
    help = false;
    auto need = [&](int i, const char* o) {
        if (i + 1 >= argc) { std::cerr << "[CONFIG] " << o << " requires a value.\n"; return false; }
        return true;
    };

    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") { help = true; return true; }
        else if (a == "--serial")
        {
            if (!need(i, "--serial")) return false;
            if (cfg.serials.size() >= 2)
            {
                std::cerr << "[CONFIG] at most two --serial may be given.\n";
                return false;
            }
            cfg.serials.push_back(argv[++i]);
        }
        else if (a == "--out")           { if (!need(i,"--out")) return false; cfg.out_path = argv[++i]; }
        else if (a == "--exposure-us")   { if (!need(i,"--exposure-us")) return false; cfg.exposure_us = std::atof(argv[++i]); }
        else if (a == "--gain-db")       { if (!need(i,"--gain-db")) return false; cfg.gain_db = std::atof(argv[++i]); }
        else if (a == "--exposure-auto") { cfg.exposure_auto = true; }
        else if (a == "--rate")          { if (!need(i,"--rate")) return false; cfg.rate_hz = std::atof(argv[++i]); }
        else if (a == "--preview-width") { if (!need(i,"--preview-width")) return false; cfg.preview_width = std::atoi(argv[++i]); }
        else if (a == "--roi")
        {
            if (!need(i, "--roi")) return false;
            if (!parseRoi(argv[++i], cfg))
            {
                std::cerr << "[CONFIG] --roi expects x,y,w,h with no spaces.\n";
                return false;
            }
        }
        else { std::cerr << "[CONFIG] Unknown option: " << a << "\n"; return false; }
    }

    if (cfg.serials.empty())
    {
        std::cerr << "[CONFIG] --serial is required.\n";
        return false;
    }
    if (cfg.serials.size() == 2 && cfg.serials[0] == cfg.serials[1])
    {
        std::cerr << "[CONFIG] the two --serial values are identical.\n";
        return false;
    }
    if (cfg.rate_hz <= 0.0 || cfg.exposure_us <= 0.0 || cfg.gain_db < 0.0)
    {
        std::cerr << "[CONFIG] rate and exposure must be positive, gain non-negative.\n";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// GenICam helpers (duplicated from main.cpp on purpose, see header comment)
// ---------------------------------------------------------------------------

static std::string nodeStr(INodeMap& m, const char* n)
{
    try {
        CNodePtr node = m.GetNode(n);
        if (!node.IsValid() || !IsAvailable(node) || !IsReadable(node)) return "<unavailable>";
        CValuePtr v = static_cast<CValuePtr>(node);
        return v.IsValid() ? std::string(v->ToString().c_str()) : "<unavailable>";
    } catch (...) { return "<unavailable>"; }
}

static double nodeFloat(INodeMap& m, const char* n, double fb)
{
    try {
        CFloatPtr f = m.GetNode(n);
        if (!f.IsValid() || !IsAvailable(f) || !IsReadable(f)) return fb;
        return f->GetValue();
    } catch (...) { return fb; }
}

static void setEnum(INodeMap& m, const char* n, const char* v)
{
    CEnumerationPtr e = m.GetNode(n);
    if (!e.IsValid() || !IsAvailable(e) || !IsWritable(e))
        throw std::runtime_error(std::string("node not writable: ") + n);
    CEnumEntryPtr entry = e->GetEntryByName(v);
    if (!entry.IsValid() || !IsAvailable(entry))
        throw std::runtime_error(std::string("value not available: ") + n + " = " + v);
    e->SetIntValue(entry->GetValue());
}

static void setBool(INodeMap& m, const char* n, bool v)
{
    CBooleanPtr b = m.GetNode(n);
    if (!b.IsValid() || !IsAvailable(b) || !IsWritable(b))
        throw std::runtime_error(std::string("node not writable: ") + n);
    b->SetValue(v);
}

static void setInt(INodeMap& m, const char* n, int64_t v)
{
    CIntegerPtr i = m.GetNode(n);
    if (!i.IsValid() || !IsAvailable(i) || !IsWritable(i))
        throw std::runtime_error(std::string("node not writable: ") + n);
    i->SetValue(v);
}

static double setFloatClamped(INodeMap& m, const char* n, double v)
{
    CFloatPtr f = m.GetNode(n);
    if (!f.IsValid() || !IsAvailable(f) || !IsWritable(f))
        throw std::runtime_error(std::string("node not writable: ") + n);
    const double lo = f->GetMin(), hi = f->GetMax();
    double x = std::min(std::max(v, lo), hi);
    if (x != v) std::cerr << "[CONFIG] " << n << " clamped to " << x
                          << " (range " << lo << " to " << hi << ")\n";
    f->SetValue(x);
    return x;
}

static bool trySetBool(INodeMap& m, const char* n, bool v)
{ try { setBool(m, n, v); return true; } catch (...) { return false; } }

static bool trySetEnum(INodeMap& m, const char* n, const char* v)
{ try { setEnum(m, n, v); return true; } catch (...) { return false; } }

// ---------------------------------------------------------------------------
// Measurement
// ---------------------------------------------------------------------------

struct FrameStats
{
    double sharpness   = 0.0;   // variance of the Laplacian over the demosaiced ROI
    double sat_pct     = 0.0;   // per cent of ROI photosites at 255
    double sat_r       = 0.0;
    double sat_g       = 0.0;
    double sat_b       = 0.0;
    double black_pct   = 0.0;   // per cent at 0
    int    p50 = 0, p99 = 0, p999 = 0, pmax = 0;
};

// Percentiles and clipping straight off the mosaic, per Bayer channel. The CFA
// means one channel can clip while the others do not, which after demosaicing
// corrupts luminance without looking obviously wrong. BayerRG8: R at (0,0),
// G at (0,1) and (1,0), B at (1,1).
static void rawStats(const cv::Mat& roi_raw, FrameStats& st)
{
    uint64_t hist[256] = {0};
    uint64_t n_r = 0, n_g = 0, n_b = 0, s_r = 0, s_g = 0, s_b = 0, zeros = 0;

    for (int y = 0; y < roi_raw.rows; ++y)
    {
        const uint8_t* p = roi_raw.ptr<uint8_t>(y);
        const bool even_row = (y % 2 == 0);
        for (int x = 0; x < roi_raw.cols; ++x)
        {
            const uint8_t v = p[x];
            ++hist[v];
            if (v == 0) ++zeros;
            const bool even_col = (x % 2 == 0);
            if (even_row && even_col)        { ++n_r; if (v == 255) ++s_r; }
            else if (!even_row && !even_col) { ++n_b; if (v == 255) ++s_b; }
            else                             { ++n_g; if (v == 255) ++s_g; }
        }
    }

    const uint64_t total = static_cast<uint64_t>(roi_raw.rows) * roi_raw.cols;
    if (total == 0) return;

    uint64_t cum = 0;
    bool got50 = false, got99 = false, got999 = false;
    for (int v = 0; v < 256; ++v)
    {
        cum += hist[v];
        const double f = static_cast<double>(cum) / static_cast<double>(total);
        if (!got50  && f >= 0.500) { st.p50  = v; got50  = true; }
        if (!got99  && f >= 0.990) { st.p99  = v; got99  = true; }
        if (!got999 && f >= 0.999) { st.p999 = v; got999 = true; }
        if (hist[v]) st.pmax = v;
    }

    st.sat_pct   = 100.0 * static_cast<double>(hist[255]) / static_cast<double>(total);
    st.black_pct = 100.0 * static_cast<double>(zeros)     / static_cast<double>(total);
    st.sat_r = n_r ? 100.0 * static_cast<double>(s_r) / static_cast<double>(n_r) : 0.0;
    st.sat_g = n_g ? 100.0 * static_cast<double>(s_g) / static_cast<double>(n_g) : 0.0;
    st.sat_b = n_b ? 100.0 * static_cast<double>(s_b) / static_cast<double>(n_b) : 0.0;
}

// Sharpness on the demosaiced crop. The crop must start on even coordinates or
// the Bayer phase shifts and the demosaic swaps colour channels.
static double sharpnessOf(const cv::Mat& roi_raw)
{
    cv::Mat bgr, grey, lap;
    cv::cvtColor(roi_raw, bgr, cv::COLOR_BayerBG2BGR);
    cv::cvtColor(bgr, grey, cv::COLOR_BGR2GRAY);
    cv::Laplacian(grey, lap, CV_32F);
    cv::Scalar mu, sigma;
    cv::meanStdDev(lap, mu, sigma);
    return sigma[0] * sigma[0];
}

// Quick colour preview without interpolation: one output pixel per 2x2 Bayer
// block, taking every step-th block. Cheap enough to run every frame, where
// demosaicing the full 16 MP would not be, and free of the artefacts a naive
// decimation of the mosaic would introduce.
static cv::Mat bayerQuickBGR(const cv::Mat& raw, int step)
{
    if (step < 1) step = 1;
    const int ow = raw.cols / (2 * step);
    const int oh = raw.rows / (2 * step);
    cv::Mat out(oh, ow, CV_8UC3);

    for (int y = 0; y < oh; ++y)
    {
        const int sy = y * 2 * step;
        const uint8_t* r0 = raw.ptr<uint8_t>(sy);
        const uint8_t* r1 = raw.ptr<uint8_t>(sy + 1);
        cv::Vec3b*     o  = out.ptr<cv::Vec3b>(y);
        for (int x = 0; x < ow; ++x)
        {
            const int sx = x * 2 * step;
            const int R  = r0[sx];
            const int G  = (r0[sx + 1] + r1[sx]) / 2;
            const int B  = r1[sx + 1];
            o[x] = cv::Vec3b(static_cast<uint8_t>(B), static_cast<uint8_t>(G),
                             static_cast<uint8_t>(R));
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Overlay
// ---------------------------------------------------------------------------

static void drawText(cv::Mat& img, const std::string& s, cv::Point p, double scale,
                     const cv::Scalar& colour)
{
    // Drawn twice: a thick black pass then the colour on top. Plain text on a
    // bright scene is unreadable on a laptop screen in sunlight.
    cv::putText(img, s, p, cv::FONT_HERSHEY_SIMPLEX, scale, cv::Scalar(0, 0, 0),
                static_cast<int>(scale * 5.0), cv::LINE_AA);
    cv::putText(img, s, p, cv::FONT_HERSHEY_SIMPLEX, scale, colour,
                static_cast<int>(scale * 2.0), cv::LINE_AA);
}

static void drawOverlay(cv::Mat& view, const FrameStats& st, double median, double peak,
                        const std::deque<double>& hist, double exposure_us, double gain_db,
                        const std::string& serial, bool auto_exp, double proc_ms)
{
    const int W = view.cols, H = view.rows;
    const double s = std::max(0.55, W / 1400.0);

    // Peak-relative bar across the top. Turning a ring is a search for a
    // plateau, and a bar shows the trend where a number does not.
    const int bar_h = static_cast<int>(34 * s);
    const double frac = (peak > 0.0) ? std::min(1.0, median / peak) : 0.0;
    cv::rectangle(view, cv::Rect(0, 0, W, bar_h), cv::Scalar(0, 0, 0), cv::FILLED);
    const cv::Scalar bar_col = (frac > 0.97) ? cv::Scalar(80, 255, 80)
                             : (frac > 0.85) ? cv::Scalar(80, 220, 255)
                                             : cv::Scalar(120, 120, 255);
    cv::rectangle(view, cv::Rect(0, 0, static_cast<int>(W * frac), bar_h), bar_col, cv::FILLED);

    std::ostringstream l0, l1, l2, l3;
    l0 << "SHARP " << std::fixed << std::setprecision(0) << median
       << "   peak " << peak << "   " << std::setprecision(0) << (frac * 100.0) << "% of peak";
    l1 << "SAT " << std::fixed << std::setprecision(2) << st.sat_pct << "%"
       << "  R " << st.sat_r << "  G " << st.sat_g << "  B " << st.sat_b
       << "   black " << st.black_pct << "%";
    l2 << "DN  p50 " << st.p50 << "  p99 " << st.p99 << "  p99.9 " << st.p999
       << "  max " << st.pmax;
    l3 << "EXP " << std::fixed << std::setprecision(0) << exposure_us << " us"
       << (auto_exp ? " (AUTO)" : "") << "   GAIN " << std::setprecision(1) << gain_db
       << " dB   sn " << serial << "   " << std::setprecision(0) << proc_ms << " ms";

    const int line = static_cast<int>(38 * s);
    int y = bar_h + line;
    drawText(view, l0.str(), cv::Point(10, y), s * 1.05, cv::Scalar(255, 255, 255)); y += line;

    const cv::Scalar sat_col = (st.sat_pct > 1.0)  ? cv::Scalar(80, 80, 255)
                             : (st.sat_pct > 0.05) ? cv::Scalar(80, 220, 255)
                                                   : cv::Scalar(255, 255, 255);
    drawText(view, l1.str(), cv::Point(10, y), s * 0.85, sat_col); y += line;
    drawText(view, l2.str(), cv::Point(10, y), s * 0.85, cv::Scalar(255, 255, 255)); y += line;
    drawText(view, l3.str(), cv::Point(10, y), s * 0.85, cv::Scalar(200, 200, 200));

    // Sparkline of the last minute, so a slow drift is visible as a slope
    // rather than as a number that seems merely noisy.
    const int spw = std::min(static_cast<int>(W * 0.42), W - 24);
    const int sph = std::min(static_cast<int>(90 * s), H - 24);
    if (spw < 40 || sph < 20) return;
    const int spx = W - spw - 12, spy = H - sph - 12;
    cv::Mat panel = view(cv::Rect(spx, spy, spw, sph));
    panel.convertTo(panel, -1, 0.35, 0);
    cv::rectangle(view, cv::Rect(spx, spy, spw, sph), cv::Scalar(180, 180, 180), 1);
    if (hist.size() > 1 && peak > 0.0)
    {
        for (size_t i = 1; i < hist.size(); ++i)
        {
            const double x0 = spx + spw * (static_cast<double>(i - 1) / (hist.size() - 1));
            const double x1 = spx + spw * (static_cast<double>(i)     / (hist.size() - 1));
            const double y0 = spy + sph * (1.0 - std::min(1.0, hist[i - 1] / peak));
            const double y1 = spy + sph * (1.0 - std::min(1.0, hist[i]     / peak));
            cv::line(view, cv::Point(static_cast<int>(x0), static_cast<int>(y0)),
                     cv::Point(static_cast<int>(x1), static_cast<int>(y1)),
                     cv::Scalar(80, 255, 80), 2, cv::LINE_AA);
        }
    }
    drawText(view, "60 s", cv::Point(spx + 6, spy + sph - 6), s * 0.5,
             cv::Scalar(200, 200, 200));
}

// ---------------------------------------------------------------------------
// Live commands on stdin
// ---------------------------------------------------------------------------

struct Command
{
    char   verb = 0;      // 'e' 'g' 'a' 'm' 'r' 'q'
    double value = 0.0;
};

static std::mutex          cmd_mtx;
static std::vector<Command> cmd_queue;

static void stdinThread()
{
    std::string line;
    while (keep_running.load() && std::getline(std::cin, line))
    {
        std::istringstream is(line);
        std::string verb;
        if (!(is >> verb) || verb.empty()) continue;

        Command c;
        c.verb = verb[0];
        if (c.verb == 'e' || c.verb == 'g')
        {
            if (!(is >> c.value)) { std::cerr << "[CMD] " << verb << " needs a number\n"; continue; }
        }
        if (std::string("egamrq").find(c.verb) == std::string::npos)
        {
            std::cerr << "[CMD] unknown: " << verb << "\n";
            continue;
        }
        { std::lock_guard<std::mutex> lk(cmd_mtx); cmd_queue.push_back(c); }
        if (c.verb == 'q') keep_running.store(false);
    }
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Per-camera state
// ---------------------------------------------------------------------------
// One of these per sensor. The measurement history is per camera because the
// two fields of view differ, and a peak shared between them would make one
// panel judge itself against the other's scene.

struct CamState
{
    std::string        serial;
    CameraPtr          cam;
    INodeMap*          nm = nullptr;
    bool               acquiring = false;

    cv::Rect           roi;
    bool               roi_set = false;

    std::deque<double> hist;
    std::deque<double> med_win;
    double             peak        = 0.0;
    double             median      = 0.0;
    double             exposure_us = 0.0;
    double             gain_db     = 0.0;
    bool               auto_exp    = false;

    FrameStats         st;
    double             proc_ms = 0.0;
    cv::Mat            panel;          // last preview built, kept if a grab fails
    uint64_t           frames  = 0;
    uint64_t           misses  = 0;
};

// Applied identically to every sensor. Nothing is inherited from camera flash:
// a factory user set gives a reproducible starting point, without which the
// configuration sits on top of whatever the last SpinView session left behind.
static void configureFocusCamera(CameraPtr cam, FocusConfig& cfg, CamState& cs)
{
    INodeMap& m = cam->GetNodeMap();
    cs.nm = &m;

    setEnum(m, "UserSetSelector", "Default");
    { CCommandPtr c = m.GetNode("UserSetLoad"); if (c.IsValid() && IsWritable(c)) c->Execute(); }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    setEnum(m, "PixelFormat", "BayerRG8");
    trySetBool(m, "IspEnable", false);
    trySetBool(m, "GammaEnable", false);
    setEnum(m, "AcquisitionMode", "Continuous");

    // Free-running, paced by the camera. No PPS, no trigger: a viewfinder, not
    // an acquisition.
    setEnum(m, "TriggerSelector", "FrameStart");
    setEnum(m, "TriggerMode", "Off");
    double rate = cfg.rate_hz;
    if (trySetBool(m, "AcquisitionFrameRateEnable", true))
    {
        try { rate = setFloatClamped(m, "AcquisitionFrameRate", cfg.rate_hz); }
        catch (const std::exception& e)
        {
            std::cerr << "[CONFIG] frame rate not settable (" << e.what()
                      << "); the camera will free-run at its own pace.\n";
        }
    }

    if (cfg.exposure_auto)
    {
        // The default upper limit is 15 ms. Once the loop reaches it under dim
        // light it can only act on gain, and it then swings by several stops,
        // which no sharpness measurement survives.
        trySetEnum(m, "ExposureAuto", "Continuous");
        trySetEnum(m, "GainAuto", "Continuous");
        cs.auto_exp = true;
    }
    else
    {
        setEnum(m, "ExposureAuto", "Off");
        setEnum(m, "ExposureMode", "Timed");
        cs.exposure_us = setFloatClamped(m, "ExposureTime", cfg.exposure_us);
        setEnum(m, "GainAuto", "Off");
        cs.gain_db = setFloatClamped(m, "Gain", cfg.gain_db);
        cs.auto_exp = false;
    }
    trySetEnum(m, "BalanceWhiteAuto", "Off");

    // NewestOnly, the opposite of the acquisition binary. A viewfinder that
    // falls behind must show the present, not work through a queue of the past.
    try
    {
        INodeMap& sm = cam->GetTLStreamNodeMap();
        setEnum(sm, "StreamBufferCountMode", "Manual");
        setInt(sm, "StreamBufferCountManual", 4);
        setEnum(sm, "StreamBufferHandlingMode", "NewestOnly");
    }
    catch (const std::exception& e)
    {
        std::cerr << "[CONFIG] stream buffers: " << e.what() << "\n";
    }

    std::cout << "[FOCUS] " << nodeStr(m, "DeviceModelName") << " sn " << cs.serial << ", "
              << nodeStr(m, "Width") << "x" << nodeStr(m, "Height") << " "
              << nodeStr(m, "PixelFormat") << ", free-run " << rate << " Hz\n";
}

// A panel standing in for a camera that returned nothing, so the composite
// keeps its geometry and the failure is visible rather than inferred from a
// frozen image.
static cv::Mat deadPanel(int w, int h, const std::string& serial)
{
    cv::Mat p(h > 0 ? h : 300, w > 0 ? w : 500, CV_8UC3, cv::Scalar(20, 20, 40));
    drawText(p, "NO FRAME", cv::Point(20, p.rows / 2), 1.2, cv::Scalar(80, 80, 255));
    drawText(p, "sn " + serial, cv::Point(20, p.rows / 2 + 50), 0.7,
             cv::Scalar(200, 200, 200));
    return p;
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv)
{
    FocusConfig cfg;
    bool        help = false;
    if (!parseArgs(argc, argv, cfg, help)) return 2;
    if (help) { printUsage(argv[0]); return 0; }

    std::signal(SIGINT,  focus_signal_handler);
    std::signal(SIGTERM, focus_signal_handler);

    SystemPtr system = System::GetInstance();
    int       rc     = 1;

    {
        CameraList            list = system->GetCameras();
        std::vector<CamState> cams(cfg.serials.size());

        bool bound = true;
        for (size_t i = 0; i < cams.size(); ++i)
        {
            cams[i].serial = cfg.serials[i];
            try { cams[i].cam = list.GetBySerial(cams[i].serial.c_str()); }
            catch (Spinnaker::Exception&) { cams[i].cam = nullptr; }
            if (!cams[i].cam.IsValid())
            {
                std::cerr << "[FATAL] No camera with serial " << cams[i].serial
                          << " is attached. " << list.GetSize() << " camera(s) enumerated.\n";
                bound = false;
            }
        }

        if (!bound)
        {
            for (CamState& c : cams) c.cam = nullptr;
            list.Clear();
            system->ReleaseInstance();
            return 1;
        }

        try
        {
            for (CamState& c : cams)
            {
                c.cam->Init();
                configureFocusCamera(c.cam, cfg, c);
            }
            for (CamState& c : cams) { c.cam->BeginAcquisition(); c.acquiring = true; }

            std::thread reader(stdinThread);
            reader.detach();

            std::cout << "[FOCUS] Writing " << cfg.out_path << " ("
                      << cams.size() << " panel" << (cams.size() > 1 ? "s" : "")
                      << "). Commands (type, then Enter): e <us>, g <dB>, a, m, r, q. "
                         "Ctrl-C also stops.\n";

            const size_t      dot = cfg.out_path.find_last_of('.');
            const std::string ext =
                (dot == std::string::npos || dot + 1 >= cfg.out_path.size())
                    ? std::string(".png")
                    : cfg.out_path.substr(dot);
            const std::string tmp_path = cfg.out_path + ".tmp";
            // Chosen by extension. Handing every parameter to every encoder
            // makes the JPEG writer warn about the PNG key on every frame,
            // which at 2 Hz buries the measurement it is printed beside.
            std::vector<int> enc_params;
            if (ext == ".png")       enc_params = {cv::IMWRITE_PNG_COMPRESSION, 1};
            else if (ext == ".jpg" || ext == ".jpeg")
                                     enc_params = {cv::IMWRITE_JPEG_QUALITY, cfg.jpeg_quality};
            bool warned_io = false;

            while (keep_running.load())
            {
                // Pending commands, applied to every sensor. Two cameras
                // metering independently would diverge, and the overlap
                // between their fields is exactly where that matters.
                {
                    std::vector<Command> pending;
                    { std::lock_guard<std::mutex> lk(cmd_mtx); pending.swap(cmd_queue); }
                    for (const Command& c : pending)
                    {
                        for (CamState& cs : cams)
                        {
                            INodeMap& m = *cs.nm;
                            try
                            {
                                switch (c.verb)
                                {
                                    case 'e':
                                        trySetEnum(m, "ExposureAuto", "Off");
                                        cs.auto_exp = false;
                                        cs.exposure_us = setFloatClamped(m, "ExposureTime", c.value);
                                        break;
                                    case 'g':
                                        trySetEnum(m, "GainAuto", "Off");
                                        cs.gain_db = setFloatClamped(m, "Gain", c.value);
                                        break;
                                    case 'a':
                                        trySetEnum(m, "ExposureAuto", "Continuous");
                                        trySetEnum(m, "GainAuto", "Continuous");
                                        cs.auto_exp = true;
                                        break;
                                    case 'm':
                                        trySetEnum(m, "ExposureAuto", "Off");
                                        trySetEnum(m, "GainAuto", "Off");
                                        cs.auto_exp = false;
                                        break;
                                    case 'r':
                                        cs.peak = 0.0; cs.hist.clear();
                                        break;
                                    default: break;
                                }
                            }
                            catch (const std::exception& e)
                            { std::cerr << "[CMD] sn " << cs.serial << ": " << e.what() << "\n"; }
                        }

                        switch (c.verb)
                        {
                            case 'e': std::cout << "[CMD] exposure -> " << cams[0].exposure_us
                                                << " us on all sensors\n"; break;
                            case 'g': std::cout << "[CMD] gain -> " << cams[0].gain_db
                                                << " dB on all sensors\n"; break;
                            case 'a': std::cout << "[CMD] automatic exposure and gain\n"; break;
                            case 'm': std::cout << "[CMD] locked\n"; break;
                            case 'r': std::cout << "[CMD] peak reset\n"; break;
                            default: break;
                        }
                    }
                }

                for (CamState& cs : cams)
                {
                    ImagePtr img;
                    try { img = cs.cam->GetNextImage(2000); }
                    catch (Spinnaker::Exception& e)
                    {
                        ++cs.misses;
                        if (keep_running.load())
                            std::cerr << "[GRAB] sn " << cs.serial << ": " << e.what() << "\n";
                        cs.panel = deadPanel(cs.panel.cols, cs.panel.rows, cs.serial);
                        continue;
                    }

                    if (img->IsIncomplete())
                    {
                        ++cs.misses;
                        std::cerr << "[GRAB] sn " << cs.serial << ": incomplete frame\n";
                        img->Release();
                        continue;
                    }

                    const auto t0 = std::chrono::steady_clock::now();

                    // Read back rather than echoed. Under automatic exposure
                    // the requested value is meaningless and the metered one is
                    // the whole point: it is what the burst will be given.
                    cs.exposure_us = nodeFloat(*cs.nm, "ExposureTime", cs.exposure_us);
                    cs.gain_db     = nodeFloat(*cs.nm, "Gain", cs.gain_db);

                    const int iw = static_cast<int>(img->GetWidth());
                    const int ih = static_cast<int>(img->GetHeight());
                    cv::Mat   raw(ih, iw, CV_8UC1, img->GetData());

                    if (!cs.roi_set)
                    {
                        cs.roi_set = true;
                        int x  = (cfg.roi_x < 0) ? (iw - cfg.roi_w) / 2 : cfg.roi_x;
                        int y  = (cfg.roi_y < 0) ? (ih - cfg.roi_h) / 2 : cfg.roi_y;
                        int rw = cfg.roi_w, rh = cfg.roi_h;
                        // Even offsets and sizes: an odd crop shifts the Bayer
                        // phase and the demosaic returns the wrong colours,
                        // which changes the luminance the metric works on.
                        x &= ~1; y &= ~1; rw &= ~1; rh &= ~1;
                        x  = std::max(0, std::min(x, iw - 2));
                        y  = std::max(0, std::min(y, ih - 2));
                        rw = std::max(2, std::min(rw, iw - x));
                        rh = std::max(2, std::min(rh, ih - y));
                        cs.roi = cv::Rect(x, y, rw, rh);
                        std::cout << "[FOCUS] sn " << cs.serial << " ROI " << cs.roi.x << ","
                                  << cs.roi.y << " " << cs.roi.width << "x" << cs.roi.height
                                  << "\n";
                    }

                    const cv::Mat roi_raw = raw(cs.roi);
                    cs.st = FrameStats();
                    rawStats(roi_raw, cs.st);
                    cs.st.sharpness = sharpnessOf(roi_raw);

                    cs.med_win.push_back(cs.st.sharpness);
                    while (cs.med_win.size() > cfg.median_window) cs.med_win.pop_front();
                    std::vector<double> sorted(cs.med_win.begin(), cs.med_win.end());
                    std::sort(sorted.begin(), sorted.end());
                    cs.median = sorted[sorted.size() / 2];

                    cs.hist.push_back(cs.median);
                    while (cs.hist.size() > cfg.history) cs.hist.pop_front();
                    cs.peak = 0.0;
                    for (double v : cs.hist) cs.peak = std::max(cs.peak, v);

                    const int step =
                        std::max(1, iw / (2 * std::max(200, cfg.preview_width)));
                    cv::Mat view = bayerQuickBGR(raw, step);

                    const double sx = static_cast<double>(view.cols) / iw;
                    const double sy = static_cast<double>(view.rows) / ih;
                    cv::rectangle(view,
                                  cv::Rect(static_cast<int>(cs.roi.x * sx),
                                           static_cast<int>(cs.roi.y * sy),
                                           static_cast<int>(cs.roi.width * sx),
                                           static_cast<int>(cs.roi.height * sy)),
                                  cv::Scalar(0, 255, 255), 2);

                    cs.proc_ms = std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - t0).count();

                    drawOverlay(view, cs.st, cs.median, cs.peak, cs.hist, cs.exposure_us,
                                cs.gain_db, cs.serial, cs.auto_exp, cs.proc_ms);
                    cs.panel = view;
                    ++cs.frames;

                    img->Release();
                }

                // Compose. Panels are the same geometry by construction, both
                // sensors being identical, but the heights are equalised anyway
                // rather than trusting that.
                cv::Mat composite;
                {
                    std::vector<cv::Mat> panels;
                    int h = 0;
                    for (CamState& cs : cams)
                    {
                        if (cs.panel.empty())
                            cs.panel = deadPanel(cfg.preview_width, cfg.preview_width * 9 / 16,
                                                 cs.serial);
                        h = std::max(h, cs.panel.rows);
                    }
                    for (CamState& cs : cams)
                    {
                        cv::Mat p = cs.panel;
                        if (p.rows != h)
                        {
                            cv::Mat pad(h, p.cols, CV_8UC3, cv::Scalar(0, 0, 0));
                            p.copyTo(pad(cv::Rect(0, 0, p.cols, p.rows)));
                            p = pad;
                        }
                        if (!panels.empty())
                            panels.push_back(cv::Mat(h, 6, CV_8UC3, cv::Scalar(240, 240, 240)));
                        panels.push_back(p);
                    }
                    cv::hconcat(panels, composite);
                }

                // Encoded in memory, written, then renamed. Rename is atomic
                // within a filesystem, so a viewer polling the path never reads
                // a partial image. A failure warns once and the preview stops;
                // it does not end the session, imwrite having thrown rather
                // than returned false and taken the viewfinder with it.
                try
                {
                    std::vector<uchar> buf;
                    if (cv::imencode(ext, composite, buf, enc_params))
                    {
                        std::ofstream o(tmp_path, std::ios::binary);
                        o.write(reinterpret_cast<const char*>(buf.data()),
                                static_cast<std::streamsize>(buf.size()));
                        o.close();
                        if (o) { std::rename(tmp_path.c_str(), cfg.out_path.c_str()); }
                        else if (!warned_io)
                        {
                            warned_io = true;
                            std::cerr << "[IO] cannot write " << tmp_path
                                      << "; preview disabled, measurements continue.\n";
                        }
                    }
                    else if (!warned_io)
                    {
                        warned_io = true;
                        std::cerr << "[IO] no encoder for " << ext
                                  << "; preview disabled, measurements continue. Try --out "
                                     "with .png or .bmp.\n";
                    }
                }
                catch (const cv::Exception& e)
                {
                    if (!warned_io)
                    {
                        warned_io = true;
                        std::cerr << "[IO] encoding " << ext << " failed: " << e.what()
                                  << "\n[IO] preview disabled, measurements continue.\n";
                    }
                }

                for (size_t i = 0; i < cams.size(); ++i)
                {
                    const CamState& cs = cams[i];
                    std::printf("[%zu sn%s] sharp %8.0f med %8.0f %5.1f%% peak | sat %5.2f%% "
                                "(R%4.1f G%4.1f B%4.1f) p50 %3d p99.9 %3d max %3d | "
                                "exp %6.0f us gain %4.1f dB%s | %3.0f ms\n",
                                i, cs.serial.c_str(), cs.st.sharpness, cs.median,
                                cs.peak > 0 ? 100.0 * cs.median / cs.peak : 0.0,
                                cs.st.sat_pct, cs.st.sat_r, cs.st.sat_g, cs.st.sat_b,
                                cs.st.p50, cs.st.p999, cs.st.pmax, cs.exposure_us, cs.gain_db,
                                cs.auto_exp ? " AUTO" : "", cs.proc_ms);
                }
                std::fflush(stdout);
            }

            std::cout << "\n";
            for (const CamState& cs : cams)
            {
                std::cout << "[FOCUS] sn " << cs.serial << ": " << cs.frames << " frames, "
                          << cs.misses << " missed. Final exposure "
                          << nodeFloat(*cs.nm, "ExposureTime", 0) << " us, gain "
                          << nodeFloat(*cs.nm, "Gain", 0) << " dB.\n";
            }
            std::cout << "[FOCUS] Record those figures: they are what the burst must be given "
                         "with --exposure-us and --gain-db.\n";
            rc = 0;
        }
        catch (Spinnaker::Exception& e)
        {
            std::cerr << "[FATAL] " << e.what() << "\n";
        }
        catch (const std::exception& e)
        {
            std::cerr << "[FATAL] " << e.what() << "\n";
        }

        for (CamState& cs : cams)
        {
            try { if (cs.acquiring) cs.cam->EndAcquisition(); } catch (...) {}
            try { if (cs.cam.IsValid() && cs.cam->IsInitialized()) cs.cam->DeInit(); } catch (...) {}
            cs.cam = nullptr;
        }
        list.Clear();
    }

    system->ReleaseInstance();
    return rc;
}