// decode.cpp
//
// Reads one or more archived .raw frames and writes a viewable image, so that
// what a burst actually recorded can be checked on site rather than inferred
// from counters. Also reports the DN distribution, which is what says whether
// the exposure was right: verify_burst.py establishes that every file is the
// declared size, and says nothing about whether the frames are usable.
//
// Read-only on the archive: the output is written beside the source, or where
// --out says.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <opencv2/opencv.hpp>

// Sony IMX542, the sensor of the BFS-U3-161S7C. One byte per photosite in
// BayerRG8, so the file size is exactly the product of the two dimensions and
// any other size means the frame is not what it claims to be.
static constexpr int kWidth   = 5320;
static constexpr int kHeight  = 3032;
static constexpr long kPayload = static_cast<long>(kWidth) * kHeight;

static void usage(const char* a0)
{
    std::cout
        << "Usage: " << a0 << " [options] <file.raw> [more.raw ...]\n\n"
        << "  --scale <f>   Downscale the output by f, e.g. 4 (default: 1).\n"
        << "                A full-resolution PNG of 16 MP takes seconds to\n"
        << "                write and is not what a field check needs.\n"
        << "  --jpg         Write JPEG at quality 90 instead of PNG. Faster,\n"
        << "                and adequate for judging framing and focus; PNG\n"
        << "                remains the default because it is lossless and\n"
        << "                this tool is also used to inspect pixel values.\n"
        << "  --stats-only  Report the DN distribution and write nothing.\n"
        << "  --out <dir>   Output directory (default: beside the source).\n";
}

int main(int argc, char** argv)
{
    double            scale = 1.0;
    bool              jpg = false, stats_only = false;
    std::string       outdir;
    std::vector<std::string> files;

    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        else if (a == "--jpg")        jpg = true;
        else if (a == "--stats-only") stats_only = true;
        else if (a == "--scale" && i + 1 < argc) scale = std::atof(argv[++i]);
        else if (a == "--out"   && i + 1 < argc) outdir = argv[++i];
        else if (!a.empty() && a[0] == '-')
        {
            std::cerr << "Unknown option: " << a << "\n";
            return 2;
        }
        else files.push_back(a);
    }

    if (files.empty()) { usage(argv[0]); return 2; }
    if (scale < 1.0) { std::cerr << "--scale must be at least 1.\n"; return 2; }

    int failures = 0;

    for (const std::string& path : files)
    {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f)
        {
            std::cerr << "[FAIL] " << path << ": cannot open\n";
            ++failures;
            continue;
        }

        // The size check is the point of the tool being used at all. A
        // truncated file decodes into a plausible-looking image of noise, and
        // that is exactly the failure a power cut mid-write produces.
        const long size = static_cast<long>(f.tellg());
        if (size != kPayload)
        {
            std::cerr << "[FAIL] " << path << ": " << size << " bytes, expected "
                      << kPayload << " (" << kWidth << "x" << kHeight << " BayerRG8). "
                      << (size < kPayload ? "Truncated: the write was interrupted."
                                          : "Larger than one frame.")
                      << "\n";
            ++failures;
            continue;
        }
        f.seekg(0);

        std::vector<uint8_t> buf(kPayload);
        f.read(reinterpret_cast<char*>(buf.data()), kPayload);
        if (f.gcount() != kPayload)
        {
            std::cerr << "[FAIL] " << path << ": short read\n";
            ++failures;
            continue;
        }
        f.close();

        // Percentiles straight off the mosaic, and clipping per Bayer channel.
        // The CFA means one channel can clip while the others do not, which
        // after demosaicing corrupts luminance without looking obviously wrong.
        // BayerRG8: R at (0,0), G at (0,1) and (1,0), B at (1,1).
        uint64_t hist[256] = {0};
        uint64_t nr = 0, ng = 0, nb = 0, sr = 0, sg = 0, sb = 0;
        for (int y = 0; y < kHeight; ++y)
        {
            const uint8_t* p = buf.data() + static_cast<long>(y) * kWidth;
            const bool even_row = (y % 2 == 0);
            for (int x = 0; x < kWidth; ++x)
            {
                const uint8_t v = p[x];
                ++hist[v];
                const bool even_col = (x % 2 == 0);
                if (even_row && even_col)        { ++nr; if (v == 255) ++sr; }
                else if (!even_row && !even_col) { ++nb; if (v == 255) ++sb; }
                else                             { ++ng; if (v == 255) ++sg; }
            }
        }
        int      p50 = 0, p99 = 0, p999 = 0, pmax = 0;
        uint64_t cum = 0;
        bool     g50 = false, g99 = false, g999 = false;
        for (int v = 0; v < 256; ++v)
        {
            cum += hist[v];
            const double q = static_cast<double>(cum) / static_cast<double>(kPayload);
            if (!g50  && q >= 0.500) { p50  = v; g50  = true; }
            if (!g99  && q >= 0.990) { p99  = v; g99  = true; }
            if (!g999 && q >= 0.999) { p999 = v; g999 = true; }
            if (hist[v]) pmax = v;
        }
        const double sat  = 100.0 * static_cast<double>(hist[255]) / kPayload;
        const double dark = 100.0 * static_cast<double>(hist[0])   / kPayload;

        std::printf("[OK]   %s  p50 %3d  p99 %3d  p99.9 %3d  max %3d | "
                    "clipped %5.2f%% (R%5.2f G%5.2f B%5.2f)  black %5.2f%%\n",
                    path.c_str(), p50, p99, p999, pmax, sat,
                    nr ? 100.0 * sr / nr : 0.0,
                    ng ? 100.0 * sg / ng : 0.0,
                    nb ? 100.0 * sb / nb : 0.0, dark);

        if (stats_only) continue;

        cv::Mat raw(kHeight, kWidth, CV_8UC1, buf.data());
        cv::Mat bgr;
        // COLOR_BayerBG2BGR against a sensor reporting BayerRG: the mismatch is
        // apparent rather than real, the OpenCV naming convention designating
        // the tile of the second row rather than the first.
        cv::cvtColor(raw, bgr, cv::COLOR_BayerBG2BGR);

        if (scale > 1.0)
        {
            cv::Mat small;
            cv::resize(bgr, small,
                       cv::Size(static_cast<int>(kWidth / scale),
                                static_cast<int>(kHeight / scale)),
                       0, 0, cv::INTER_AREA);
            bgr = small;
        }

        std::string stem = path;
        const size_t slash = stem.find_last_of('/');
        if (!outdir.empty())
            stem = outdir + "/" + (slash == std::string::npos ? stem
                                                              : stem.substr(slash + 1));
        const size_t dot = stem.find_last_of('.');
        if (dot != std::string::npos && dot > (slash == std::string::npos ? 0 : slash))
            stem = stem.substr(0, dot);

        const std::string out = stem + (jpg ? ".jpg" : ".png");
        std::vector<int> params;
        if (jpg) params = {cv::IMWRITE_JPEG_QUALITY, 90};
        else     params = {cv::IMWRITE_PNG_COMPRESSION, 1};

        try
        {
            if (cv::imwrite(out, bgr, params))
            {
                std::cout << "       -> " << out << " (" << bgr.cols << "x" << bgr.rows
                          << ")\n";
            }
            else
            {
                std::cerr << "[FAIL] cannot write " << out << "\n";
                ++failures;
            }
        }
        catch (const cv::Exception& e)
        {
            std::cerr << "[FAIL] writing " << out << ": " << e.what() << "\n";
            ++failures;
        }
    }

    return failures == 0 ? 0 : 1;
}