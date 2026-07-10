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

struct CalibrationAppConfig {
    bool enabled = false;
    std::optional<std::filesystem::path> jsonPath;

    std::uint32_t boardColumns = 12;
    std::uint32_t boardRows = 9;
    std::string profile = "affine_vertical";
    bool profileExplicit = false;
    std::size_t maxObservations = 30;
    std::size_t minObservations = 8;
    double minCornerMotionPx = 15.0;
    double ransacThresholdPx = 1.5;
    bool useChessboardSb = false;
};

struct AppConfig {
    CameraAppConfig left;
    CameraAppConfig right;
    CalibrationAppConfig calibration;

    int width = 0;
    int height = 0;
    double fps = 0.0;
    IC4Ext::CameraPixelFormat inputFormat = IC4Ext::CameraPixelFormat::BGR8;
    bool formatExplicit = false;

    double syncToleranceMs = 5.0;
    IC4Ext::FrameSyncTimestampSource timestampSource = IC4Ext::FrameSyncTimestampSource::HostReceived;
    std::size_t syncBufferedFramesPerCamera = 16;
    std::size_t inputQueueSize = 64;
    std::size_t outputQueueSize = 8;

    std::uint32_t cameraReadTimeoutMs = 1000;
    std::uint32_t cameraStartDelayMs = 2000;
    std::uint32_t initialFrameTimeoutMs = 15000;

    float planeWidthMeters = 1.0f;
    float planeHeightMeters = 0.0f;
    float planeX = 0.0f;
    float planeY = 0.0f;
    float planeDistanceMeters = 1.0f;
    VarjoXR::PlacementMode placementMode = VarjoXR::PlacementMode::HeadRelative;

    std::size_t displayRingSize = 4;
    bool enableD3D12DebugLayer = true;
    double maxRuntimeSeconds = 0.0;

    std::filesystem::path metadataCsv;
    bool showHelp = false;
};

bool ParseArguments(int argc, char** argv, AppConfig& config, std::string& error);
void PrintUsage(std::ostream& out);
IC4Ext::CameraCaptureConfig MakeCaptureConfig(const AppConfig& app, const CameraAppConfig& camera);
std::filesystem::path MakeDefaultMetadataPath();
const char* TimestampSourceName(IC4Ext::FrameSyncTimestampSource source) noexcept;

} // namespace DualIC4Varjo
