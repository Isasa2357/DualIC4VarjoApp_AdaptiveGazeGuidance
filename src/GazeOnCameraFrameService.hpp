#pragma once

#include <VarjoToolkit/Core/VarjoFrameInfo.hpp>
#include <VarjoToolkit/Services/EyeTracking/VarjoEyeTrackingService.hpp>

#include <VarjoXR/Core/XRPlane.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace DualIC4Varjo {

struct GazeCameraPlaneSnapshot {
    float x = 0.0f;
    float y = 0.0f;
    float z = -1.0f;
    float width = 1.0f;
    float height = 1.0f;
    std::uint32_t leftFrameWidth = 0;
    std::uint32_t leftFrameHeight = 0;
    std::uint32_t rightFrameWidth = 0;
    std::uint32_t rightFrameHeight = 0;
    VarjoXR::PlacementMode placementMode = VarjoXR::PlacementMode::HeadRelative;
};

class GazeOnCameraFrameService {
public:
    GazeOnCameraFrameService(
        std::filesystem::path outputPath,
        std::optional<std::filesystem::path> calibrationJson);
    ~GazeOnCameraFrameService();

    GazeOnCameraFrameService(const GazeOnCameraFrameService&) = delete;
    GazeOnCameraFrameService& operator=(const GazeOnCameraFrameService&) = delete;

    bool start();
    void stop() noexcept;

    bool submitFrameInfo(
        VarjoFrameInfoSnapshot snapshot,
        GazeCameraPlaneSnapshot plane,
        std::shared_ptr<varjo_Session> session);
    bool submitGazeData(std::vector<VarjoEyeTrackingData> data);

    std::filesystem::path outputPath() const;
    std::string lastError() const;
    std::uint64_t receivedGazeCount() const noexcept;
    std::uint64_t writtenRowCount() const noexcept;
    std::uint64_t droppedGazeCount() const noexcept;
    std::uint64_t submittedFrameCount() const noexcept;
    std::uint64_t evictedFrameCount() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class GazeOnCameraFrameHook {
public:
    static bool configure(
        const std::filesystem::path& outputPath,
        const std::optional<std::filesystem::path>& calibrationJson);
    static void submitFrameInfo(
        const VarjoFrameInfoSnapshot& snapshot,
        const GazeCameraPlaneSnapshot& plane,
        const std::shared_ptr<varjo_Session>& session) noexcept;
    static void submitGazeData(std::vector<VarjoEyeTrackingData> data) noexcept;
    static void stop() noexcept;

    static std::filesystem::path outputPath();
    static std::string lastError();
    static std::uint64_t receivedGazeCount() noexcept;
    static std::uint64_t writtenRowCount() noexcept;
    static std::uint64_t droppedGazeCount() noexcept;
    static std::uint64_t submittedFrameCount() noexcept;
    static std::uint64_t evictedFrameCount() noexcept;
};

} // namespace DualIC4Varjo
