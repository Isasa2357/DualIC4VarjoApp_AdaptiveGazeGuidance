#include "AppConfig.hpp"

#include <VarjoXR/VarjoXR.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace DualIC4Varjo {
namespace {

template <typename T>
bool ParseNumber(const std::string& text, T& value)
{
    try {
        std::size_t consumed = 0;
        if constexpr (std::is_integral_v<T>) {
            const long long parsed = std::stoll(text, &consumed, 10);
            if (consumed != text.size()) return false;
            if constexpr (std::is_unsigned_v<T>) {
                if (parsed < 0 || static_cast<unsigned long long>(parsed) > std::numeric_limits<T>::max()) return false;
            } else {
                if (parsed < static_cast<long long>(std::numeric_limits<T>::min()) ||
                    parsed > static_cast<long long>(std::numeric_limits<T>::max())) return false;
            }
            value = static_cast<T>(parsed);
        } else {
            const double parsed = std::stod(text, &consumed);
            if (consumed != text.size()) return false;
            value = static_cast<T>(parsed);
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseBool(const std::string& text, bool& value)
{
    if (text == "1" || text == "true" || text == "on") {
        value = true;
        return true;
    }
    if (text == "0" || text == "false" || text == "off") {
        value = false;
        return true;
    }
    return false;
}

} // namespace

bool ParseArguments(int argc, char** argv, AppConfig& config, std::string& error)
{
    config.left.selector.deviceIndex = 0;
    config.right.selector.deviceIndex = 1;

    auto requireValue = [&](int& index, const std::string& option) -> const char* {
        if (index + 1 >= argc) {
            error = "Missing value for " + option;
            return nullptr;
        }
        return argv[++index];
    };
    auto invalidValue = [&](const std::string& option, const std::string& value) {
        error = "Invalid value for " + option + ": " + value;
        return false;
    };

    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--help" || option == "-h") {
            config.showHelp = true;
            return true;
        }

        const char* raw = requireValue(i, option);
        if (!raw) return false;
        const std::string value = raw;

        if (option == "--left-device-index") {
            if (!ParseNumber(value, config.left.selector.deviceIndex)) return invalidValue(option, value);
        } else if (option == "--right-device-index") {
            if (!ParseNumber(value, config.right.selector.deviceIndex)) return invalidValue(option, value);
        } else if (option == "--left-serial") {
            config.left.selector.serial = value;
        } else if (option == "--right-serial") {
            config.right.selector.serial = value;
        } else if (option == "--left-unique-name") {
            config.left.selector.uniqueName = value;
        } else if (option == "--right-unique-name") {
            config.right.selector.uniqueName = value;
        } else if (option == "--left-json") {
            config.left.stateJson = value;
        } else if (option == "--right-json") {
            config.right.stateJson = value;
        } else if (option == "--left-json-device-index") {
            if (!ParseNumber(value, config.left.stateJsonDeviceIndex)) return invalidValue(option, value);
        } else if (option == "--right-json-device-index") {
            if (!ParseNumber(value, config.right.stateJsonDeviceIndex)) return invalidValue(option, value);
        } else if (option == "--left-offset-x") {
            int parsed = 0;
            if (!ParseNumber(value, parsed)) return invalidValue(option, value);
            config.left.offsetX = parsed;
        } else if (option == "--left-offset-y") {
            int parsed = 0;
            if (!ParseNumber(value, parsed)) return invalidValue(option, value);
            config.left.offsetY = parsed;
        } else if (option == "--right-offset-x") {
            int parsed = 0;
            if (!ParseNumber(value, parsed)) return invalidValue(option, value);
            config.right.offsetX = parsed;
        } else if (option == "--right-offset-y") {
            int parsed = 0;
            if (!ParseNumber(value, parsed)) return invalidValue(option, value);
            config.right.offsetY = parsed;
        } else if (option == "--width") {
            if (!ParseNumber(value, config.width)) return invalidValue(option, value);
        } else if (option == "--height") {
            if (!ParseNumber(value, config.height)) return invalidValue(option, value);
        } else if (option == "--fps") {
            if (!ParseNumber(value, config.fps)) return invalidValue(option, value);
        } else if (option == "--format") {
            if (!IC4Ext::ParseCameraPixelFormat(value, config.inputFormat)) {
                error = "Unsupported --format value: " + value;
                return false;
            }
            config.formatExplicit = true;
        } else if (option == "--sync-tolerance-ms") {
            if (!ParseNumber(value, config.syncToleranceMs)) return invalidValue(option, value);
        } else if (option == "--sync-timestamp") {
            if (value == "host") config.timestampSource = IC4Ext::FrameSyncTimestampSource::HostReceived;
            else if (value == "device") config.timestampSource = IC4Ext::FrameSyncTimestampSource::Device;
            else if (value == "auto") config.timestampSource = IC4Ext::FrameSyncTimestampSource::Auto;
            else {
                error = "--sync-timestamp must be host, device, or auto";
                return false;
            }
        } else if (option == "--sync-buffer-frames") {
            if (!ParseNumber(value, config.syncBufferedFramesPerCamera)) return invalidValue(option, value);
        } else if (option == "--input-queue-size") {
            if (!ParseNumber(value, config.inputQueueSize)) return invalidValue(option, value);
        } else if (option == "--output-queue-size") {
            if (!ParseNumber(value, config.outputQueueSize)) return invalidValue(option, value);
        } else if (option == "--read-timeout-ms") {
            if (!ParseNumber(value, config.cameraReadTimeoutMs)) return invalidValue(option, value);
        } else if (option == "--camera-start-delay-ms") {
            if (!ParseNumber(value, config.cameraStartDelayMs)) return invalidValue(option, value);
        } else if (option == "--initial-frame-timeout-ms") {
            if (!ParseNumber(value, config.initialFrameTimeoutMs)) return invalidValue(option, value);
        } else if (option == "--plane-width-m") {
            if (!ParseNumber(value, config.planeWidthMeters)) return invalidValue(option, value);
        } else if (option == "--plane-height-m") {
            if (!ParseNumber(value, config.planeHeightMeters)) return invalidValue(option, value);
        } else if (option == "--plane-x-m") {
            if (!ParseNumber(value, config.planeX)) return invalidValue(option, value);
        } else if (option == "--plane-y-m") {
            if (!ParseNumber(value, config.planeY)) return invalidValue(option, value);
        } else if (option == "--plane-distance-m") {
            if (!ParseNumber(value, config.planeDistanceMeters)) return invalidValue(option, value);
        } else if (option == "--placement") {
            if (value == "head") config.placementMode = VarjoXR::PlacementMode::HeadRelative;
            else if (value == "world") config.placementMode = VarjoXR::PlacementMode::World;
            else {
                error = "--placement must be head or world";
                return false;
            }
        } else if (option == "--display-ring-size") {
            if (!ParseNumber(value, config.displayRingSize)) return invalidValue(option, value);
        } else if (option == "--d3d12-debug") {
            if (!ParseBool(value, config.enableD3D12DebugLayer)) return invalidValue(option, value);
        } else if (option == "--max-runtime-seconds") {
            if (!ParseNumber(value, config.maxRuntimeSeconds)) return invalidValue(option, value);
        } else if (option == "--metadata-csv") {
            config.metadataCsv = value;
        } else {
            error = "Unknown option: " + option;
            return false;
        }
    }

    if (config.left.selector.deviceIndex == config.right.selector.deviceIndex &&
        config.left.selector.serial.empty() && config.right.selector.serial.empty() &&
        config.left.selector.uniqueName.empty() && config.right.selector.uniqueName.empty()) {
        error = "Left and right cameras resolve to the same device index";
        return false;
    }
    if (config.width < 0 || config.height < 0 || config.fps < 0.0) {
        error = "Width, height, and fps must be non-negative";
        return false;
    }
    if (config.syncToleranceMs < 0.0) {
        error = "--sync-tolerance-ms must be non-negative";
        return false;
    }
    if (config.syncBufferedFramesPerCamera == 0 || config.inputQueueSize == 0 || config.outputQueueSize == 0) {
        error = "Queue sizes and --sync-buffer-frames must be greater than zero";
        return false;
    }
    if (config.displayRingSize < 3) {
        error = "--display-ring-size must be at least 3";
        return false;
    }
    if (config.planeWidthMeters <= 0.0f || config.planeDistanceMeters <= 0.0f || config.planeHeightMeters < 0.0f) {
        error = "Plane width and distance must be positive; plane height must be zero or positive";
        return false;
    }
    if (config.maxRuntimeSeconds < 0.0) {
        error = "--max-runtime-seconds must be non-negative";
        return false;
    }
    if (config.metadataCsv.empty()) {
        config.metadataCsv = MakeDefaultMetadataPath();
    }
    return true;
}

void PrintUsage(std::ostream& out)
{
    out <<
        "DualIC4VarjoApp (D3D12)\n\n"
        "Required runtime: two IC4 cameras, Varjo Runtime, and a connected Varjo HMD.\n\n"
        "Camera selection:\n"
        "  --left-device-index N          Default: 0\n"
        "  --right-device-index N         Default: 1\n"
        "  --left-serial TEXT             Serial takes precedence over device index\n"
        "  --right-serial TEXT\n"
        "  --left-unique-name TEXT\n"
        "  --right-unique-name TEXT\n"
        "  --left-json PATH               IC Capture 4 exported state JSON\n"
        "  --right-json PATH\n"
        "  --left-json-device-index N     Default: 0\n"
        "  --right-json-device-index N    Default: 0\n"
        "  --left-offset-x N / --left-offset-y N\n"
        "  --right-offset-x N / --right-offset-y N\n\n"
        "Capture and synchronization:\n"
        "  --width N --height N --fps N\n"
        "  --format BGR8|BGRa8|Mono8|BayerRG8|BayerGR8|BayerGB8|BayerBG8\n"
        "  --sync-tolerance-ms N          Default: 5.0\n"
        "  --sync-timestamp host|device|auto   Default: host\n"
        "  --camera-start-delay-ms N      Default: 2000\n"
        "  --initial-frame-timeout-ms N   Default: 15000\n\n"
        "Plane:\n"
        "  --placement head|world         Default: head\n"
        "  --plane-width-m N              Default: 1.0\n"
        "  --plane-height-m N             0 computes from the camera aspect ratio\n"
        "  --plane-x-m N --plane-y-m N\n"
        "  --plane-distance-m N           Default: 1.0\n\n"
        "Logging and execution:\n"
        "  --metadata-csv PATH            Default: logs/rendered_frames_<time>.csv\n"
        "  --display-ring-size N          Default: 4, minimum: 3\n"
        "  --d3d12-debug 0|1              Default: 1\n"
        "  --max-runtime-seconds N        0 means unlimited\n"
        "  --help\n\n"
        "Press Esc or Ctrl+C to stop.\n";
}

IC4Ext::CameraCaptureConfig MakeCaptureConfig(const AppConfig& app, const CameraAppConfig& camera)
{
    IC4Ext::CameraCaptureConfig config;
    if (!camera.stateJson.empty()) {
        config.ic4StateJson.path = camera.stateJson;
        config.ic4StateJson.deviceIndex = camera.stateJsonDeviceIndex;
        config.ic4StateJson.strict = false;
    }

    if (camera.stateJson.empty() || app.formatExplicit) {
        config.streamRequest.requestedFormat = app.inputFormat;
        config.streamRequest.forceRequestedFormat = true;
    }
    if (app.width > 0) config.streamRequest.width = app.width;
    if (app.height > 0) config.streamRequest.height = app.height;
    if (app.fps > 0.0) config.streamRequest.fps = app.fps;
    config.streamRequest.offsetX = camera.offsetX;
    config.streamRequest.offsetY = camera.offsetY;

    config.outputSpec.outputFormat = IC4Ext::GpuFrameFormat::RGBA8;
    config.outputSpec.createSrv = false;
    config.outputSpec.createUav = false;
    config.queuePolicy = IC4Ext::FrameQueuePolicy::PreserveFrames;
    config.maxPendingBuffers = 0;
    return config;
}

std::filesystem::path MakeDefaultMetadataPath()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t timeValue = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &timeValue);

    std::ostringstream name;
    name << "rendered_frames_" << std::put_time(&local, "%Y%m%d_%H%M%S") << ".csv";
    return std::filesystem::path("logs") / name.str();
}

const char* TimestampSourceName(IC4Ext::FrameSyncTimestampSource source) noexcept
{
    switch (source) {
    case IC4Ext::FrameSyncTimestampSource::HostReceived: return "host";
    case IC4Ext::FrameSyncTimestampSource::Device: return "device";
    case IC4Ext::FrameSyncTimestampSource::Auto: return "auto";
    default: return "unknown";
    }
}

} // namespace DualIC4Varjo
