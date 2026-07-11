#pragma once

#include <Varjo.h>
#include <VarjoToolkit/Core/VarjoFrameInfo.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

class VarjoEyeTrackingService;
class VarjoIMUService;
class VarjoVSTService;

namespace DualIC4Varjo {

struct VarjoServiceLoggingSummary {
    std::uint64_t gazeReceived = 0;
    std::uint64_t gazeDropped = 0;
    std::uint64_t gazeFrameInfoSubmitted = 0;
    std::uint64_t gazeFrameInfoDropped = 0;
    double gazeSamplesPerSecond = 0.0;

    std::uint64_t imuRows = 0;
    std::uint64_t imuReceived = 0;
    std::uint64_t imuProcessed = 0;
    std::uint64_t imuWritten = 0;
    std::uint64_t imuDropped = 0;
    double imuSamplesPerSecond = 0.0;

    std::uint64_t vstLeftFrames = 0;
    std::uint64_t vstRightFrames = 0;
    std::uint64_t vstDroppedFrames = 0;
    std::uint64_t vstWriteFailures = 0;
    double vstLeftFramesPerSecond = 0.0;
    double vstRightFramesPerSecond = 0.0;
};

class VarjoServiceLogging {
public:
    VarjoServiceLogging(
        std::shared_ptr<varjo_Session> session,
        std::filesystem::path outputDirectory);
    ~VarjoServiceLogging();

    VarjoServiceLogging(const VarjoServiceLogging&) = delete;
    VarjoServiceLogging& operator=(const VarjoServiceLogging&) = delete;

    bool start(std::string& error);

    // Distribute the single snapshot captured by VarjoXR's rendering WaitSync to
    // Eye Tracking and IMU. This function never performs another WaitSync.
    bool submitFrameInfo(const VarjoFrameInfoSnapshot& snapshot) noexcept;

    // Drain the optional application-facing Eye Tracking queue. CSV output is
    // performed by the service worker and is not affected by this drain.
    void pump();
    void stop() noexcept;

    bool running() const noexcept { return running_; }
    VarjoServiceLoggingSummary summary() const;

    const std::filesystem::path& outputDirectory() const noexcept
    {
        return outputDirectory_;
    }

private:
    void writeSummaryFileNoThrow() const noexcept;

    std::shared_ptr<varjo_Session> session_;
    std::filesystem::path outputDirectory_;

    std::unique_ptr<VarjoEyeTrackingService> eyeTracking_;
    std::unique_ptr<VarjoIMUService> imu_;
    std::unique_ptr<VarjoVSTService> vst_;

    bool eyeStarted_ = false;
    bool imuStarted_ = false;
    bool vstStarted_ = false;
    bool running_ = false;
};

} // namespace DualIC4Varjo
