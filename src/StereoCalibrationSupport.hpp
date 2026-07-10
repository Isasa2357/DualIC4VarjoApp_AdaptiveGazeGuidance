#pragma once

#include <IC4Ext/IC4Ext.hpp>

#include <VdcaStereoCalibration/CalibrationDocument.hpp>
#include <VdcaStereoCalibration/CalibrationSnapshotStore.hpp>
#include <VdcaStereoCalibration/RealtimeStereoCalibrator.hpp>

#include <D3D12Helper/D3D12Core/D3D12Core.hpp>
#include <VarjoXR/Core/XRPlane.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace DualIC4Varjo {

struct LiveStereoCalibrationOptions {
    std::uint32_t boardColumns = 12;
    std::uint32_t boardRows = 9;
    std::string activeProfile = "affine_vertical";
    std::size_t maxObservationCount = 30;
    std::size_t minObservationCountForUpdate = 8;
    double minMeanCornerMotionPx = 15.0;
    double fundamentalRansacThresholdPx = 1.5;
    bool useFindChessboardCornersSB = false;
};

class LiveStereoCalibration {
public:
    LiveStereoCalibration(
        std::shared_ptr<D3D12CoreLib::D3D12Core> core,
        std::uint32_t inputWidth,
        std::uint32_t inputHeight,
        LiveStereoCalibrationOptions options,
        std::optional<Vdca::StereoCalibration::CalibrationDocument> initialCalibration = std::nullopt);
    ~LiveStereoCalibration();

    LiveStereoCalibration(const LiveStereoCalibration&) = delete;
    LiveStereoCalibration& operator=(const LiveStereoCalibration&) = delete;

    void start();
    void stop();
    void submitLatest(std::shared_ptr<IC4Ext::D3D12SyncedFrameSet> frameSet);

    std::shared_ptr<const Vdca::StereoCalibration::CalibrationSnapshot> latestSnapshot() const;
    Vdca::StereoCalibration::RealtimeStereoCalibratorStats stats() const noexcept;
    void rethrowWorkerExceptionIfAny() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void ValidateCalibrationInputGeometry(
    const Vdca::StereoCalibration::CalibrationDocument& document,
    std::uint32_t inputWidth,
    std::uint32_t inputHeight);

void ApplyCalibrationToPlane(
    VarjoXR::XRPlane& plane,
    const Vdca::StereoCalibration::CalibrationDocument& document,
    const std::string& profileName);

void ClearCalibrationFromPlane(VarjoXR::XRPlane& plane);

void UpdatePlaneAspectFromCalibration(
    VarjoXR::XRPlane& plane,
    const Vdca::StereoCalibration::CalibrationDocument& document);

bool IsLiveCalibrationReady(
    const Vdca::StereoCalibration::CalibrationSnapshot& snapshot,
    std::size_t minimumObservationCount);

} // namespace DualIC4Varjo
