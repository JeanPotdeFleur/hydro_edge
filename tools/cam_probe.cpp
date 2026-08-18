// tools/cam_probe.cpp
//
// Read-only inventory of the acquisition front-end.
//
// Enumerates every attached FLIR device and dumps the GenICam nodes that
// govern payload size, exposure, triggering and stream buffering, together
// with the full transport-layer node maps. Nothing is written: every access
// is a read, and the cameras are left in the state they were found in.
//
// Purpose is threefold. It records the "as found" configuration of the two
// BFS-U3-161S7C-C units, which is currently undocumented and which the
// acquisition binary silently inherits from camera flash. It establishes
// which transport-layer counters this SDK build exposes, since those
// counters are what will substantiate the zero-drop criterion of GATE A1.
// And it verifies that both sensors are configured identically, an
// assumption main.cpp makes when it reads PayloadSize from camera 0 alone.
//
// Build:  cmake --build build -j4
// Run:    ./build/cam_probe
//
// No other process may hold the cameras while this runs.

#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "SpinGenApi/SpinnakerGenApi.h"
#include "Spinnaker.h"

using namespace Spinnaker;
using namespace Spinnaker::GenApi;
using namespace Spinnaker::GenICam;

namespace {

void row(const std::string& label, const std::string& value)
{
    std::cout << "    " << std::left << std::setw(34) << label << value << "\n";
}

void heading(const std::string& text)
{
    std::cout << "\n  --- " << text << " ---\n";
}

// Reads a single node by name and degrades gracefully. A missing node is a
// finding, not a failure: it tells us the feature is absent on this model or
// under this SDK build, which is precisely what we are here to establish.
std::string readNode(INodeMap& nodeMap, const std::string& name)
{
    try
    {
        CNodePtr node = nodeMap.GetNode(name.c_str());
        if (!node.IsValid())
        {
            return "<absent>";
        }
        if (!IsAvailable(node))
        {
            return "<unavailable>";
        }
        if (!IsReadable(node))
        {
            return "<not readable>";
        }
        CValuePtr value = static_cast<CValuePtr>(node);
        if (!value.IsValid())
        {
            return "<not a value node>";
        }
        return std::string(value->ToString().c_str());
    }
    catch (Spinnaker::Exception& e)
    {
        return std::string("<error: ") + e.what() + ">";
    }
    catch (...)
    {
        return "<error: unknown>";
    }
}

void dumpCurated(INodeMap& nodeMap, const std::vector<std::string>& names)
{
    for (const std::string& name : names)
    {
        row(name, readNode(nodeMap, name));
    }
}

// Walks an entire node map and prints every readable scalar node. Used on the
// transport-layer maps, which hold a few dozen nodes, rather than on the
// device map, which holds several hundred.
void dumpAll(INodeMap& nodeMap)
{
    NodeList_t nodes;
    try
    {
        nodeMap.GetNodes(nodes);
    }
    catch (Spinnaker::Exception& e)
    {
        std::cout << "    <node map enumeration failed: " << e.what() << ">\n";
        return;
    }

    size_t printed = 0;
    for (size_t i = 0; i < nodes.size(); ++i)
    {
        CNodePtr node = nodes[i];
        if (!node.IsValid())
        {
            continue;
        }

        const EInterfaceType type = node->GetPrincipalInterfaceType();
        if (type != intfIInteger && type != intfIFloat && type != intfIBoolean &&
            type != intfIEnumeration && type != intfIString)
        {
            continue;
        }
        if (!IsAvailable(node) || !IsReadable(node))
        {
            continue;
        }

        try
        {
            CValuePtr value = static_cast<CValuePtr>(node);
            if (!value.IsValid())
            {
                continue;
            }
            row(std::string(node->GetName().c_str()), std::string(value->ToString().c_str()));
            ++printed;
        }
        catch (...)
        {
            // A node that throws on read is skipped rather than aborting the
            // inventory; the omission is visible in the printed count.
        }
    }
    std::cout << "    (" << printed << " readable nodes)\n";
}

void probeCamera(CameraPtr pCam, unsigned index)
{
    std::cout << "\n================================================================\n";
    std::cout << " CAMERA " << index << "\n";
    std::cout << "================================================================\n";

    // The transport-layer device map is readable before Init(), and is the
    // only place that reports the negotiated USB link speed.
    heading("TL DEVICE NODE MAP (pre-Init, full dump)");
    try
    {
        dumpAll(pCam->GetTLDeviceNodeMap());
    }
    catch (Spinnaker::Exception& e)
    {
        std::cout << "    <" << e.what() << ">\n";
    }

    try
    {
        pCam->Init();
    }
    catch (Spinnaker::Exception& e)
    {
        std::cout << "\n  [FATAL] Init() failed: " << e.what() << "\n";
        return;
    }

    INodeMap& dev = pCam->GetNodeMap();

    heading("IDENTITY AND FIRMWARE");
    dumpCurated(dev, {"DeviceModelName", "DeviceSerialNumber", "DeviceVersion",
                      "DeviceFirmwareVersion", "DeviceUserID", "DeviceTemperature",
                      "DeviceUptime"});

    heading("SENSOR GEOMETRY AND PAYLOAD");
    dumpCurated(dev, {"SensorWidth", "SensorHeight", "WidthMax", "HeightMax", "Width",
                      "Height", "OffsetX", "OffsetY", "BinningHorizontal",
                      "BinningVertical", "DecimationHorizontal", "DecimationVertical",
                      "ReverseX", "ReverseY", "PayloadSize"});

    heading("PIXEL FORMAT AND ON-BOARD PROCESSING");
    dumpCurated(dev, {"PixelFormat", "PixelSize", "PixelColorFilter", "PixelDynamicRangeMin",
                      "PixelDynamicRangeMax", "AdcBitDepth", "IspEnable", "GammaEnable",
                      "Gamma", "BlackLevel", "BlackLevelClampingEnable", "SharpeningEnable",
                      "SaturationEnable"});

    heading("SHUTTER, EXPOSURE AND GAIN");
    dumpCurated(dev, {"SensorShutterMode", "AcquisitionMode", "AcquisitionFrameRateEnable",
                      "AcquisitionFrameRate", "AcquisitionResultingFrameRate", "ExposureMode",
                      "ExposureAuto", "ExposureTime", "GainAuto", "Gain", "GainSelector",
                      "BalanceWhiteAuto"});

    heading("TRIGGERING");
    dumpCurated(dev, {"TriggerSelector", "TriggerMode", "TriggerSource", "TriggerActivation",
                      "TriggerOverlap", "TriggerDelay", "LineSelector", "LineMode",
                      "LineStatus", "LineInverter", "LineSource"});

    heading("LINK THROUGHPUT");
    dumpCurated(dev, {"DeviceLinkThroughputLimit", "DeviceLinkCurrentThroughput",
                      "DeviceMaxThroughput", "DeviceLinkSpeed", "DeviceLinkBandwidthReserve"});

    heading("CHUNK DATA (required for rigorous drop accounting)");
    dumpCurated(dev, {"ChunkModeActive", "ChunkSelector", "ChunkEnable"});
    std::cout << "    ChunkSelector entries available:\n";
    try
    {
        CEnumerationPtr selector = dev.GetNode("ChunkSelector");
        if (IsAvailable(selector) && IsReadable(selector))
        {
            NodeList_t entries;
            selector->GetEntries(entries);
            for (size_t i = 0; i < entries.size(); ++i)
            {
                CEnumEntryPtr entry = entries[i];
                if (entry.IsValid() && IsAvailable(entry))
                {
                    std::cout << "      - " << entry->GetSymbolic() << "\n";
                }
            }
        }
        else
        {
            std::cout << "      <ChunkSelector unavailable>\n";
        }
    }
    catch (Spinnaker::Exception& e)
    {
        std::cout << "      <" << e.what() << ">\n";
    }

    heading("USER SETS (persisted configuration)");
    dumpCurated(dev, {"UserSetSelector", "UserSetDefault"});

    heading("TL STREAM NODE MAP (full dump)");
    try
    {
        dumpAll(pCam->GetTLStreamNodeMap());
    }
    catch (Spinnaker::Exception& e)
    {
        std::cout << "    <" << e.what() << ">\n";
    }

    pCam->DeInit();
}

} // namespace

int main()
{
    SystemPtr system = System::GetInstance();

    const LibraryVersion version = system->GetLibraryVersion();
    std::cout << "Spinnaker library " << version.major << "." << version.minor << "."
              << version.type << "." << version.build << "\n";

    CameraList camList = system->GetCameras();
    const unsigned int count = camList.GetSize();
    std::cout << "Cameras enumerated: " << count << "\n";

    if (count == 0)
    {
        std::cerr << "[ERROR] No camera detected.\n";
        camList.Clear();
        system->ReleaseInstance();
        return 1;
    }

    for (unsigned int i = 0; i < count; ++i)
    {
        try
        {
            probeCamera(camList.GetByIndex(i), i);
        }
        catch (Spinnaker::Exception& e)
        {
            std::cerr << "[ERROR] Camera " << i << ": " << e.what() << "\n";
        }
    }

    camList.Clear();
    system->ReleaseInstance();
    std::cout << "\nProbe complete. No camera setting was modified.\n";
    return 0;
}