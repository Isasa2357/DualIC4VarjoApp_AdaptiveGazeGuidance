#pragma once

#include <IC4Ext/IC4Ext.hpp>
#include <VarjoXR/Foundation/PlacementMode.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>

namespace DualIC4Varjo {

struct CameraAppConfig {
    IC4Ext::IC4DeviceSelector selector;
    std::filesystem::path stateJson;
    std::size_t stateJsonDeviceIndex = 0;
    std::optional<int> offsetX;
    std::optional<int> offsetY;
};

struct AppConfig {
    CameraAppConfig left;
    CameraAppConfig right;

    int width = 0;
    int height = 0;
    double fps = 160.0;
    IC4Ext::CameraPixelFormat inputFormat = IC4Ext::CameraPixelFormat::BGR8;
    bool formatExplicit = false;

    double syncToleranceMs = 5.0;
    IC4Ext::FrameSyncTimestampSource timestampSource =
        IC4Ext::FrameSyncTimestampSource::HostReceived;
    std::size_t syncBufferedFramesPerCamera = 16;
    std::size_t inputQueueSize = 64;
    std::size_t outputQueueSize = 8;

    std::uint32_t cameraReadTimeoutMs = 1000;
    std::uint32_t cameraStartDelayMs = 0;
    std::uint32_t initialFrameTimeoutMs = 15000;

    float planeWidthMeters = 1.0f;
    float planeHeightMeters = 0.0f;
    float planeX = 0.0f;
    float planeY = 0.0f;
    float planeDistanceMeters = 1.0f;
    VarjoXR::PlacementMode placementMode = VarjoXR::PlacementMode::HeadRelative;
    std::size_t displayRingSize = 4;

    std::optional<std::filesystem::path> calibrationJson;
    std::string postProcessMode = "none";

    bool pcPreviewEnabled = true;
    int pcPreviewWidth = 1600;
    int pcPreviewHeight = 800;
    bool pcPreviewVsync = true;

    bool enableD3D12DebugLayer = false;
    double maxRuntimeSeconds = 0.0;

    std::filesystem::path outputBaseDirectory;
    std::string projectName;
    std::filesystem::path metadataCsv;
    bool showHelp = false;
};

bool ParseArguments(int argc, char** argv, AppConfig& config, std::string& error);
void PrintUsage(std::ostream& out);
IC4Ext::CameraCaptureConfig MakeCaptureConfig(
    const AppConfig& app,
    const CameraAppConfig& camera);
std::filesystem::path MakeDefaultMetadataPath();
const char* TimestampSourceName(
    IC4Ext::FrameSyncTimestampSource source) noexcept;

} // namespace DualIC4Varjo
