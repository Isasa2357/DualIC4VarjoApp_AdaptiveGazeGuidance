#include "AppConfig.hpp"
#include "PersonalizationSettings.hpp"

#include <limits>
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
                if (parsed < 0 ||
                    static_cast<unsigned long long>(parsed) >
                        std::numeric_limits<T>::max()) {
                    return false;
                }
            } else if (
                parsed <
                    static_cast<long long>(
                        std::numeric_limits<T>::min()) ||
                parsed >
                    static_cast<long long>(
                        std::numeric_limits<T>::max())) {
                return false;
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

bool IsSingleFolderName(const std::string& value)
{
    if (value.empty()) return false;
    const std::filesystem::path path(value);
    return !path.is_absolute() &&
           !path.has_parent_path() &&
           path != "." &&
           path != "..";
}

bool ParsePostProcessMode(
    const std::string& value,
    std::string& normalized)
{
    if (value == "none" ||
        value == "off" ||
        value == "disabled" ||
        value == "disable") {
        normalized = "none";
        return true;
    }
    if (value == "darken") {
        normalized = "darken";
        return true;
    }
    if (value == "blur" || value == "blue") {
        normalized = "blur";
        return true;
    }
    return false;
}

bool HasHelpArgument(int argc, char** argv) noexcept
{
    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i] ? argv[i] : "";
        if (option == "--help" || option == "-h") return true;
    }
    return false;
}

bool ScanPersonalizationName(
    int argc,
    char** argv,
    AppConfig& config,
    std::string& error)
{
    config.personalizationName = "DEFAULT";
    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i] ? argv[i] : "";
        if (option != "--name") continue;
        if (i + 1 >= argc || !argv[i + 1]) {
            error = "Missing value for --name";
            return false;
        }
        config.personalizationName = argv[++i];
    }
    if (config.personalizationName.empty()) {
        config.personalizationName = "DEFAULT";
    }
    return true;
}

} // namespace

bool ParseArguments(
    int argc,
    char** argv,
    AppConfig& config,
    std::string& error)
{
    config.left.selector.deviceIndex = 0;
    config.right.selector.deviceIndex = 1;

    if (HasHelpArgument(argc, argv)) {
        config.showHelp = true;
        return true;
    }

    if (!ScanPersonalizationName(argc, argv, config, error)) {
        return false;
    }
    if (!PersonalizationSettings::LoadIntoConfig(config, error)) {
        return false;
    }

    auto requireValue =
        [&](int& index, const std::string& option) -> const char* {
        if (index + 1 >= argc) {
            error = "Missing value for " + option;
            return nullptr;
        }
        return argv[++index];
    };

    auto invalidValue =
        [&](const std::string& option, const std::string& value) {
        error = "Invalid value for " + option + ": " + value;
        return false;
    };

    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i] ? argv[i] : "";
        if (option == "--name") {
            ++i;
            continue;
        }

        const char* raw = requireValue(i, option);
        if (!raw) return false;
        const std::string value = raw;

        if (option == "--left-device-index") {
            if (!ParseNumber(
                    value,
                    config.left.selector.deviceIndex)) {
                return invalidValue(option, value);
            }
        } else if (option == "--right-device-index") {
            if (!ParseNumber(
                    value,
                    config.right.selector.deviceIndex)) {
                return invalidValue(option, value);
            }
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
            if (!ParseNumber(
                    value,
                    config.left.stateJsonDeviceIndex)) {
                return invalidValue(option, value);
            }
        } else if (option == "--right-json-device-index") {
            if (!ParseNumber(
                    value,
                    config.right.stateJsonDeviceIndex)) {
                return invalidValue(option, value);
            }
        } else if (option == "--left-offset-x") {
            int parsed = 0;
            if (!ParseNumber(value, parsed)) {
                return invalidValue(option, value);
            }
            config.left.offsetX = parsed;
        } else if (option == "--left-offset-y") {
            int parsed = 0;
            if (!ParseNumber(value, parsed)) {
                return invalidValue(option, value);
            }
            config.left.offsetY = parsed;
        } else if (option == "--right-offset-x") {
            int parsed = 0;
            if (!ParseNumber(value, parsed)) {
                return invalidValue(option, value);
            }
            config.right.offsetX = parsed;
        } else if (option == "--right-offset-y") {
            int parsed = 0;
            if (!ParseNumber(value, parsed)) {
                return invalidValue(option, value);
            }
            config.right.offsetY = parsed;
        } else if (option == "--width") {
            if (!ParseNumber(value, config.width)) {
                return invalidValue(option, value);
            }
        } else if (option == "--height") {
            if (!ParseNumber(value, config.height)) {
                return invalidValue(option, value);
            }
        } else if (option == "--fps") {
            if (!ParseNumber(value, config.fps)) {
                return invalidValue(option, value);
            }
        } else if (option == "--format") {
            if (!IC4Ext::ParseCameraPixelFormat(
                    value,
                    config.inputFormat)) {
                error = "Unsupported --format value: " + value;
                return false;
            }
            config.formatExplicit = true;
        } else if (option == "--sync-tolerance-ms") {
            if (!ParseNumber(
                    value,
                    config.syncToleranceMs)) {
                return invalidValue(option, value);
            }
        } else if (option == "--sync-timestamp") {
            if (value == "host") {
                config.timestampSource =
                    IC4Ext::FrameSyncTimestampSource::HostReceived;
            } else if (value == "device") {
                config.timestampSource =
                    IC4Ext::FrameSyncTimestampSource::Device;
            } else if (value == "auto") {
                config.timestampSource =
                    IC4Ext::FrameSyncTimestampSource::Auto;
            } else {
                error =
                    "--sync-timestamp must be host, device, or auto";
                return false;
            }
        } else if (option == "--sync-buffer-frames") {
            if (!ParseNumber(
                    value,
                    config.syncBufferedFramesPerCamera)) {
                return invalidValue(option, value);
            }
        } else if (option == "--input-queue-size") {
            if (!ParseNumber(
                    value,
                    config.inputQueueSize)) {
                return invalidValue(option, value);
            }
        } else if (option == "--output-queue-size") {
            if (!ParseNumber(
                    value,
                    config.outputQueueSize)) {
                return invalidValue(option, value);
            }
        } else if (option == "--read-timeout-ms") {
            if (!ParseNumber(
                    value,
                    config.cameraReadTimeoutMs)) {
                return invalidValue(option, value);
            }
        } else if (option == "--camera-start-delay-ms") {
            if (!ParseNumber(
                    value,
                    config.cameraStartDelayMs)) {
                return invalidValue(option, value);
            }
        } else if (option == "--initial-frame-timeout-ms") {
            if (!ParseNumber(
                    value,
                    config.initialFrameTimeoutMs)) {
                return invalidValue(option, value);
            }
        } else if (option == "--plane-width-m") {
            if (!ParseNumber(
                    value,
                    config.planeWidthMeters)) {
                return invalidValue(option, value);
            }
        } else if (option == "--plane-height-m") {
            if (!ParseNumber(
                    value,
                    config.planeHeightMeters)) {
                return invalidValue(option, value);
            }
        } else if (option == "--plane-x-m") {
            if (!ParseNumber(value, config.planeX)) {
                return invalidValue(option, value);
            }
        } else if (option == "--plane-y-m") {
            if (!ParseNumber(value, config.planeY)) {
                return invalidValue(option, value);
            }
        } else if (option == "--plane-distance-m") {
            if (!ParseNumber(
                    value,
                    config.planeDistanceMeters)) {
                return invalidValue(option, value);
            }
        } else if (option == "--placement") {
            if (value == "head") {
                config.placementMode =
                    VarjoXR::PlacementMode::HeadRelative;
            } else if (value == "world") {
                config.placementMode =
                    VarjoXR::PlacementMode::World;
            } else {
                error = "--placement must be head or world";
                return false;
            }
        } else if (option == "--display-ring-size") {
            if (!ParseNumber(
                    value,
                    config.displayRingSize)) {
                return invalidValue(option, value);
            }
        } else if (option == "--postprocess") {
            if (!ParsePostProcessMode(value, config.postProcessMode)) {
                error = "--postprocess must be none, darken, blur, or blue";
                return false;
            }
        } else if (option == "--pc-preview") {
            if (!ParseBool(value, config.pcPreviewEnabled)) {
                return invalidValue(option, value);
            }
        } else if (option == "--pc-preview-width") {
            if (!ParseNumber(
                    value,
                    config.pcPreviewWidth)) {
                return invalidValue(option, value);
            }
        } else if (option == "--pc-preview-height") {
            if (!ParseNumber(
                    value,
                    config.pcPreviewHeight)) {
                return invalidValue(option, value);
            }
        } else if (option == "--pc-preview-vsync") {
            if (!ParseBool(value, config.pcPreviewVsync)) {
                return invalidValue(option, value);
            }
        } else if (option == "--d3d12-debug") {
            if (!ParseBool(
                    value,
                    config.enableD3D12DebugLayer)) {
                return invalidValue(option, value);
            }
        } else if (option == "--max-runtime-seconds") {
            if (!ParseNumber(
                    value,
                    config.maxRuntimeSeconds)) {
                return invalidValue(option, value);
            }
        } else if (option == "--dir") {
            config.outputBaseDirectory =
                std::filesystem::path(value);
        } else if (option == "--project") {
            config.projectName = value;
        } else if (option == "--metadata-csv") {
            config.metadataCsv =
                std::filesystem::path(value);
        } else {
            error = "Unknown option: " + option;
            return false;
        }
    }

    if (config.outputBaseDirectory.empty() ||
        config.projectName.empty()) {
        error = "--dir and --project are required";
        return false;
    }
    if (!IsSingleFolderName(config.projectName)) {
        error =
            "--project must be a single folder name, not a path";
        return false;
    }
    if (config.left.selector.deviceIndex ==
            config.right.selector.deviceIndex &&
        config.left.selector.serial.empty() &&
        config.right.selector.serial.empty() &&
        config.left.selector.uniqueName.empty() &&
        config.right.selector.uniqueName.empty()) {
        error =
            "Left and right cameras resolve to the same device index";
        return false;
    }
    if (config.width < 0 ||
        config.height < 0 ||
        config.fps <= 0.0) {
        error =
            "Width and height must be non-negative; fps must be positive";
        return false;
    }
    if (config.syncToleranceMs < 0.0) {
        error =
            "--sync-tolerance-ms must be non-negative";
        return false;
    }
    if (config.syncBufferedFramesPerCamera == 0 ||
        config.inputQueueSize == 0 ||
        config.outputQueueSize == 0) {
        error =
            "Queue sizes and --sync-buffer-frames must be greater than zero";
        return false;
    }
    if (config.displayRingSize < 3) {
        error =
            "--display-ring-size must be at least 3";
        return false;
    }
    if (config.pcPreviewEnabled &&
        (config.pcPreviewWidth <= 0 ||
         config.pcPreviewHeight <= 0)) {
        error =
            "PC preview width and height must be positive";
        return false;
    }
    if (config.planeWidthMeters <= 0.0f ||
        config.planeDistanceMeters <= 0.0f ||
        config.planeHeightMeters < 0.0f) {
        error =
            "Plane width and distance must be positive; "
            "plane height must be zero or positive";
        return false;
    }
    if (config.maxRuntimeSeconds < 0.0) {
        error =
            "--max-runtime-seconds must be non-negative";
        return false;
    }
    if (config.metadataCsv.empty()) {
        config.metadataCsv = MakeDefaultMetadataPath();
    }
    PersonalizationSettings::EnableAutoSave();
    return true;
}

void PrintUsage(std::ostream& out)
{
    out <<
        "DualIC4VarjoApp minimal display stage (D3D12)\n\n"
        "Experiment output:\n"
        "  --dir PATH                       Parent directory for experiment folders\n"
        "  --project NAME                   Requested experiment folder name\n"
        "  --metadata-csv FILENAME          Default: rendered_frames.csv\n\n"
        "Camera selection:\n"
        "  --left-device-index N / --right-device-index N\n"
        "  --left-serial TEXT / --right-serial TEXT\n"
        "  --left-json PATH / --right-json PATH\n"
        "  --left-json-device-index N / --right-json-device-index N\n"
        "  --left-offset-x N / --left-offset-y N\n"
        "  --right-offset-x N / --right-offset-y N\n\n"
        "Capture and synchronization:\n"
        "  --width N --height N --fps N --format FORMAT\n"
        "                                   fps defaults to 160\n"
        "  --sync-tolerance-ms N\n"
        "  --sync-timestamp host|device|auto\n"
        "  --sync-buffer-frames N\n"
        "  --input-queue-size N --output-queue-size N\n"
        "  --camera-start-delay-ms N\n\n"
        "Varjo Plane:\n"
        "  --placement head|world\n"
        "  --plane-width-m N --plane-height-m N\n"
        "  --plane-x-m N --plane-y-m N --plane-distance-m N\n"
        "  --display-ring-size N\n"
        "  --postprocess none|darken|blur   Default: none; blue is accepted as blur alias\n"
        "  --name NAME                      Personalization profile in personalization/NAME.json; default DEFAULT\n\n"
        "PC ImGui preview:\n"
        "  --pc-preview 0|1                 Default: 1\n"
        "  --pc-preview-width N             Default: 1600\n"
        "  --pc-preview-height N            Default: 800\n"
        "  --pc-preview-vsync 0|1           Default: 1\n\n"
        "Execution:\n"
        "  Three FrameSyncThread pipelines are created at startup.\n"
        "  Pipeline 0 renders to Varjo.\n"
        "  Pipeline 1 renders left/right side-by-side with ImGui.\n"
        "  Pipeline 2 discards synchronized output.\n"
        "  Both Varjo eyes receive the same left-camera texture in this stage.\n"
        "  --d3d12-debug 0|1\n"
        "  --max-runtime-seconds N\n"
        "  --help\n";
}

IC4Ext::CameraCaptureConfig MakeCaptureConfig(
    const AppConfig& app,
    const CameraAppConfig& camera)
{
    IC4Ext::CameraCaptureConfig config;
    if (!camera.stateJson.empty()) {
        config.ic4StateJson.path = camera.stateJson;
        config.ic4StateJson.deviceIndex =
            camera.stateJsonDeviceIndex;
        config.ic4StateJson.strict = false;
    }
    if (camera.stateJson.empty() || app.formatExplicit) {
        config.streamRequest.requestedFormat =
            app.inputFormat;
        config.streamRequest.forceRequestedFormat = true;
    }
    if (app.width > 0) {
        config.streamRequest.width = app.width;
    }
    if (app.height > 0) {
        config.streamRequest.height = app.height;
    }
    config.streamRequest.fps = app.fps;
    config.streamRequest.offsetX = camera.offsetX;
    config.streamRequest.offsetY = camera.offsetY;
    config.outputSpec.outputFormat =
        IC4Ext::GpuFrameFormat::RGBA8;
    config.outputSpec.createSrv = false;
    config.outputSpec.createUav = false;
    config.queuePolicy =
        IC4Ext::FrameQueuePolicy::PreserveFrames;
    config.maxPendingBuffers = 0;
    return config;
}

std::filesystem::path MakeDefaultMetadataPath()
{
    return std::filesystem::path("rendered_frames.csv");
}

const char* TimestampSourceName(
    IC4Ext::FrameSyncTimestampSource source) noexcept
{
    switch (source) {
    case IC4Ext::FrameSyncTimestampSource::HostReceived:
        return "host";
    case IC4Ext::FrameSyncTimestampSource::Device:
        return "device";
    case IC4Ext::FrameSyncTimestampSource::Auto:
        return "auto";
    default:
        return "unknown";
    }
}

} // namespace DualIC4Varjo
