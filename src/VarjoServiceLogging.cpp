#include "VarjoServiceLogging.hpp"

#include <VarjoToolkit/Services/EyeTracking/VarjoEyeTrackingService.hpp>
#include <VarjoToolkit/Services/IMU/VarjoIMUService.hpp>
#include <VarjoToolkit/Services/VST/VarjoVSTService.hpp>

#include <Windows.h>

#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <utility>

namespace DualIC4Varjo {
namespace {

std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty()) return {};

    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0) return "<wide string conversion failed>";

    std::string result(static_cast<std::size_t>(required), '\0');
    const int converted = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        required,
        nullptr,
        nullptr);
    return converted == required
        ? result
        : std::string("<wide string conversion failed>");
}

} // namespace

VarjoServiceLogging::VarjoServiceLogging(
    std::shared_ptr<varjo_Session> session,
    std::filesystem::path outputDirectory)
    : session_(std::move(session))
    , outputDirectory_(std::move(outputDirectory))
{
}

VarjoServiceLogging::~VarjoServiceLogging()
{
    stop();
}

bool VarjoServiceLogging::start(std::string& error)
{
    error.clear();
    stop();

    try {
        if (!session_) {
            error = "Varjo service logging requires a valid shared Varjo session";
            return false;
        }
        if (outputDirectory_.empty() ||
            !std::filesystem::is_directory(outputDirectory_)) {
            error = "Varjo service output directory does not exist: " +
                outputDirectory_.string();
            return false;
        }

        const auto eyePath = outputDirectory_ / "eye_tracking.csv";
        const auto imuPath = outputDirectory_ / "imu.csv";

        eyeTracking_ = std::make_unique<VarjoEyeTrackingService>(
            session_,
            VarjoEyeTrackingProvider::OutputFilterType::NONE,
            VarjoEyeTrackingProvider::OutputFrequency::MAXIMUM,
            eyePath.u8string(),
            1000,
            5);
        imu_ = std::make_unique<VarjoIMUService>(
            session_,
            imuPath.wstring(),
            512);
        vst_ = std::make_unique<VarjoVSTService>(
            session_,
            outputDirectory_.wstring(),
            L"varjo",
            360);

        if (!eyeTracking_->start()) {
            error = "VarjoEyeTrackingService failed to start; check the output path and gaze access/calibration settings";
            stop();
            return false;
        }
        eyeStarted_ = true;

        if (!imu_->start(false)) {
            error = "VarjoIMUService failed to start: " +
                WideToUtf8(imu_->lastError());
            stop();
            return false;
        }
        imuStarted_ = true;

        if (!vst_->start()) {
            error = "VarjoVSTService failed to start: " +
                WideToUtf8(vst_->lastError()) +
                ". Ensure ffmpeg.exe is available on PATH and the VST stream is accessible.";
            stop();
            return false;
        }
        vstStarted_ = true;

        running_ = true;
        return true;
    } catch (const std::exception& exception) {
        error = std::string("Varjo service logging start failed: ") + exception.what();
        stop();
        return false;
    }
}

void VarjoServiceLogging::pump()
{
    // Eye tracking logs are written by the service worker. Drain the optional
    // application-facing queue so it does not fill and report avoidable drops.
    if (eyeStarted_ && eyeTracking_) {
        static_cast<void>(eyeTracking_->requestData());
    }
}

void VarjoServiceLogging::stop() noexcept
{
    // stop() is intentionally called for every constructed service, even when
    // start() did not complete. Each VarjoToolkit service supports idempotent
    // stop and this also cleans up partial initialization after an exception.
    try {
        if (eyeTracking_) eyeTracking_->stop();
    } catch (...) {
    }
    eyeStarted_ = false;

    try {
        if (imu_) imu_->stop();
    } catch (...) {
    }
    imuStarted_ = false;

    try {
        if (vst_) vst_->stop();
    } catch (...) {
    }
    vstStarted_ = false;

    running_ = false;
    writeSummaryFileNoThrow();
}

VarjoServiceLoggingSummary VarjoServiceLogging::summary() const
{
    VarjoServiceLoggingSummary result;
    if (eyeTracking_) {
        result.gazeReceived = eyeTracking_->receivedSampleCount();
        result.gazeDropped = eyeTracking_->droppedSampleCount();
        result.gazeSamplesPerSecond = eyeTracking_->getSamplesPerSecond();
    }
    if (imu_) {
        result.imuRows = imu_->rowCount();
        result.imuSamplesPerSecond = imu_->getSamplesPerSecond();
    }
    if (vst_) {
        result.vstLeftFrames = vst_->leftFrameCount();
        result.vstRightFrames = vst_->rightFrameCount();
        result.vstDroppedFrames = vst_->droppedFrameCount();
        result.vstWriteFailures = vst_->writeFailureCount();
        result.vstLeftFramesPerSecond = vst_->getLeftFramesPerSecond();
        result.vstRightFramesPerSecond = vst_->getRightFramesPerSecond();
    }
    return result;
}

void VarjoServiceLogging::writeSummaryFileNoThrow() const noexcept
{
    if (outputDirectory_.empty() || (!eyeTracking_ && !imu_ && !vst_)) return;

    try {
        const auto values = summary();
        std::ofstream output(
            outputDirectory_ / "varjo_service_summary.txt",
            std::ios::binary | std::ios::trunc);
        if (!output) return;

        output << "gaze_received=" << values.gazeReceived << '\n'
               << "gaze_queue_dropped=" << values.gazeDropped << '\n'
               << "gaze_samples_per_second=" << std::fixed << std::setprecision(3)
               << values.gazeSamplesPerSecond << '\n'
               << "imu_rows=" << values.imuRows << '\n'
               << "imu_samples_per_second=" << values.imuSamplesPerSecond << '\n'
               << "vst_left_frames=" << values.vstLeftFrames << '\n'
               << "vst_right_frames=" << values.vstRightFrames << '\n'
               << "vst_dropped_frames=" << values.vstDroppedFrames << '\n'
               << "vst_write_failures=" << values.vstWriteFailures << '\n'
               << "vst_left_frames_per_second=" << values.vstLeftFramesPerSecond << '\n'
               << "vst_right_frames_per_second=" << values.vstRightFramesPerSecond << '\n';

        if (vst_) {
            const auto paths = vst_->paths();
            output << "vst_left_video=" << WideToUtf8(paths.left_video) << '\n'
                   << "vst_right_video=" << WideToUtf8(paths.right_video) << '\n'
                   << "vst_left_metadata=" << WideToUtf8(paths.left_metadata_csv) << '\n'
                   << "vst_right_metadata=" << WideToUtf8(paths.right_metadata_csv) << '\n';
        }
    } catch (...) {
    }
}

} // namespace DualIC4Varjo
