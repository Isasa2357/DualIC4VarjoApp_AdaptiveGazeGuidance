#include "StereoCalibrationSupport.hpp"

#include <VdcaStereoCalibration/D3D12StereoFrame.hpp>

#include <VarjoXR/VarjoXR.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace DualIC4Varjo {
namespace {

using Vdca::StereoCalibration::CalibrationDocument;
using Vdca::StereoCalibration::CalibrationSnapshot;
using Vdca::StereoCalibration::D3D12ReadyPoint;
using Vdca::StereoCalibration::Homography3x3;
using Vdca::StereoCalibration::ImageSize;
using Vdca::StereoCalibration::RectificationProfile;
using Vdca::StereoCalibration::StereoD3D12Frame;

const IC4Ext::D3D12IndexedCameraFrame& RequireCamera(
    const IC4Ext::D3D12SyncedFrameSet& set,
    std::uint32_t cameraIndex)
{
    for (const auto& item : set.frames) {
        if (item.cameraIndex == cameraIndex) return item;
    }
    throw std::runtime_error("calibration frame set does not contain both camera indices");
}

std::int64_t Timestamp100ns(const IC4Ext::D3D12CameraFrame& frame)
{
    if (frame.timing.deviceTimestampNs != 0) {
        return static_cast<std::int64_t>(frame.timing.deviceTimestampNs / 100u);
    }
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        frame.timing.hostReceivedTime.time_since_epoch()).count() / 100;
}

StereoD3D12Frame MakeCalibrationFrame(
    std::shared_ptr<IC4Ext::D3D12SyncedFrameSet> owner)
{
    if (!owner) throw std::invalid_argument("MakeCalibrationFrame received null owner");

    const auto& left = RequireCamera(*owner, 0).frame;
    const auto& right = RequireCamera(*owner, 1).frame;
    if (!left.texture || !right.texture) {
        throw std::runtime_error("calibration source contains a null D3D12 texture");
    }

    D3D12ReadyPoint leftReady;
    leftReady.fence = left.ready.fence;
    leftReady.value = left.ready.value;
    D3D12ReadyPoint rightReady;
    rightReady.fence = right.ready.fence;
    rightReady.value = right.ready.value;

    StereoD3D12Frame result;
    result.left = Vdca::StereoCalibration::MakeD3D12ImageFrame(
        left.texture.Get(), left.resourceState, std::move(leftReady));
    result.right = Vdca::StereoCalibration::MakeD3D12ImageFrame(
        right.texture.Get(), right.resourceState, std::move(rightReady));
    result.frameNumber = owner->syncGroupId;
    result.leftTimestamp100ns = Timestamp100ns(left);
    result.rightTimestamp100ns = Timestamp100ns(right);
    result.lifetimeToken = std::move(owner);
    return result;
}

struct alignas(16) RectificationConstants {
    std::array<float, 4> inverseRow0{};
    std::array<float, 4> inverseRow1{};
    std::array<float, 4> inverseRow2{};
    std::array<float, 4> borderRgba{};
};

static_assert(sizeof(RectificationConstants) == 64, "Unexpected rectification constant layout");

float CheckedFloat(double value)
{
    const float result = static_cast<float>(value);
    if (!std::isfinite(result)) {
        throw std::invalid_argument("calibration matrix contains a non-finite value");
    }
    return result;
}

RectificationConstants MakeConstants(
    const Homography3x3& inverse,
    const std::array<float, 4>& borderRgba)
{
    RectificationConstants result;
    for (std::size_t c = 0; c < 3; ++c) {
        result.inverseRow0[c] = CheckedFloat(inverse.rows[c]);
        result.inverseRow1[c] = CheckedFloat(inverse.rows[3 + c]);
        result.inverseRow2[c] = CheckedFloat(inverse.rows[6 + c]);
    }
    result.borderRgba = borderRgba;
    return result;
}

const char* RemapHlsl() noexcept
{
    return R"hlsl(
Texture2D<float4> xrInput : register(t0);
RWTexture2D<float4> xrOutput : register(u0);

cbuffer RectificationConstants : register(b0)
{
    float4 inverseRow0;
    float4 inverseRow1;
    float4 inverseRow2;
    float4 borderRgba;
};

cbuffer XRTextureProcessingFrameConstants : register(b1)
{
    uint srcWidth;
    uint srcHeight;
    uint dstWidth;
    uint dstHeight;
    float4 frameParams;
};

float4 LoadWithConstantBorder(int2 pixel)
{
    if (pixel.x < 0 || pixel.y < 0 || pixel.x >= (int)srcWidth || pixel.y >= (int)srcHeight) {
        return borderRgba;
    }
    return xrInput.Load(int3(pixel, 0));
}

float4 SampleLinear(float2 sourcePixel)
{
    const float2 baseFloat = floor(sourcePixel);
    const int2 basePixel = int2(baseFloat);
    const float2 fraction = sourcePixel - baseFloat;
    const float4 c00 = LoadWithConstantBorder(basePixel);
    const float4 c10 = LoadWithConstantBorder(basePixel + int2(1, 0));
    const float4 c01 = LoadWithConstantBorder(basePixel + int2(0, 1));
    const float4 c11 = LoadWithConstantBorder(basePixel + int2(1, 1));
    return lerp(lerp(c00, c10, fraction.x), lerp(c01, c11, fraction.x), fraction.y);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= dstWidth || id.y >= dstHeight) return;

    const float3 destinationPixel = float3((float)id.x, (float)id.y, 1.0f);
    const float3 sourceH = float3(
        dot(inverseRow0.xyz, destinationPixel),
        dot(inverseRow1.xyz, destinationPixel),
        dot(inverseRow2.xyz, destinationPixel));

    if (!isfinite(sourceH.z) || abs(sourceH.z) < 1.0e-8f) {
        xrOutput[id.xy] = borderRgba;
        return;
    }

    const float2 sourcePixel = sourceH.xy / sourceH.z;
    if (!all(isfinite(sourcePixel)) ||
        sourcePixel.x <= -1.0f || sourcePixel.y <= -1.0f ||
        sourcePixel.x >= (float)srcWidth || sourcePixel.y >= (float)srcHeight) {
        xrOutput[id.xy] = borderRgba;
        return;
    }

    xrOutput[id.xy] = SampleLinear(sourcePixel);
}
)hlsl";
}

VarjoXR::TextureProcessingDesc MakeProcessing(
    const Homography3x3& inverse,
    const std::array<float, 4>& borderRgba,
    ImageSize outputSize)
{
    VarjoXR::TextureProcessingDesc result{};
    result.enabled = true;
    result.timing = VarjoXR::ProcessingTiming::OnTextureChanged;
    result.hlsl = RemapHlsl();
    result.entryPoint = "main";
    result.target = "cs_5_0";
    result.sourceName = "DualIC4Varjo_StereoRemap.hlsl";
    result.outputSize = {outputSize.width, outputSize.height};
    result.userConstants.registerIndex = 0;
    result.userConstants.set(MakeConstants(inverse, borderRgba));
    result.frameConstants.enabled = true;
    result.frameConstants.registerIndex = 1;
    return result;
}

} // namespace

struct LiveStereoCalibration::Impl {
    std::unique_ptr<Vdca::StereoCalibration::RealtimeStereoCalibrator> calibrator;

    Impl(
        std::shared_ptr<D3D12CoreLib::D3D12Core> core,
        std::uint32_t inputWidth,
        std::uint32_t inputHeight,
        LiveStereoCalibrationOptions options,
        std::optional<CalibrationDocument> initialCalibration)
    {
        if (!core) throw std::invalid_argument("LiveStereoCalibration requires D3D12Core");
        if (inputWidth == 0 || inputHeight == 0) {
            throw std::invalid_argument("LiveStereoCalibration requires a non-zero input size");
        }

        Vdca::StereoCalibration::RealtimeStereoCalibratorConfig config;
        config.d3d12 = std::move(core);
        config.sourceSize = {inputWidth, inputHeight};
        config.processingInputSize = {inputWidth, inputHeight};
        config.rectifiedOutputSize = {inputWidth, inputHeight};
        config.boardColumns = options.boardColumns;
        config.boardRows = options.boardRows;
        config.rightOrder = "same";
        config.activeProfile = std::move(options.activeProfile);
        config.maxObservationCount = options.maxObservationCount;
        config.minObservationCountForUpdate = options.minObservationCountForUpdate;
        config.minMeanCornerMotionPx = options.minMeanCornerMotionPx;
        config.fundamentalRansacThresholdPx = options.fundamentalRansacThresholdPx;
        config.fitUncalibratedResultToCanvas = true;
        config.useFindChessboardCornersSB = options.useFindChessboardCornersSB;
        config.initialCalibration = std::move(initialCalibration);
        calibrator = std::make_unique<Vdca::StereoCalibration::RealtimeStereoCalibrator>(
            std::move(config));
    }
};

LiveStereoCalibration::LiveStereoCalibration(
    std::shared_ptr<D3D12CoreLib::D3D12Core> core,
    std::uint32_t inputWidth,
    std::uint32_t inputHeight,
    LiveStereoCalibrationOptions options,
    std::optional<CalibrationDocument> initialCalibration)
    : impl_(std::make_unique<Impl>(
          std::move(core),
          inputWidth,
          inputHeight,
          std::move(options),
          std::move(initialCalibration)))
{
}

LiveStereoCalibration::~LiveStereoCalibration()
{
    try {
        stop();
    } catch (...) {
    }
}

void LiveStereoCalibration::start()
{
    impl_->calibrator->start();
}

void LiveStereoCalibration::stop()
{
    if (impl_ && impl_->calibrator) impl_->calibrator->stop();
}

void LiveStereoCalibration::submitLatest(
    std::shared_ptr<IC4Ext::D3D12SyncedFrameSet> frameSet)
{
    impl_->calibrator->submitLatestFrame(MakeCalibrationFrame(std::move(frameSet)));
}

std::shared_ptr<const CalibrationSnapshot> LiveStereoCalibration::latestSnapshot() const
{
    return impl_->calibrator->latestSnapshot();
}

Vdca::StereoCalibration::RealtimeStereoCalibratorStats
LiveStereoCalibration::stats() const noexcept
{
    return impl_->calibrator->stats();
}

void LiveStereoCalibration::rethrowWorkerExceptionIfAny() const
{
    impl_->calibrator->rethrowWorkerExceptionIfAny();
}

void ValidateCalibrationInputGeometry(
    const CalibrationDocument& document,
    std::uint32_t inputWidth,
    std::uint32_t inputHeight)
{
    Vdca::StereoCalibration::ValidateCalibrationDocument(document);
    if (document.calibrationInputSize.width != inputWidth ||
        document.calibrationInputSize.height != inputHeight) {
        throw std::invalid_argument(
            "calibration JSON input size does not match the IC4 output texture size");
    }
}

void ApplyCalibrationToPlane(
    VarjoXR::XRPlane& plane,
    const CalibrationDocument& document,
    const std::string& profileName)
{
    const RectificationProfile& profile = document.profile(profileName);
    plane.setProcessing(
        VarjoXR::Eye::Left,
        MakeProcessing(profile.leftInverse, document.borderRgba, document.rectifiedOutputSize));
    plane.setProcessing(
        VarjoXR::Eye::Right,
        MakeProcessing(profile.rightInverse, document.borderRgba, document.rectifiedOutputSize));
}

void ClearCalibrationFromPlane(VarjoXR::XRPlane& plane)
{
    VarjoXR::TextureProcessingDesc disabled{};
    plane.setProcessing(VarjoXR::Eye::Left, disabled);
    plane.setProcessing(VarjoXR::Eye::Right, disabled);
}

void UpdatePlaneAspectFromCalibration(
    VarjoXR::XRPlane& plane,
    const CalibrationDocument& document)
{
    if (!document.rectifiedOutputSize.valid()) {
        throw std::invalid_argument("calibration rectified output size is invalid");
    }
    const auto current = plane.size();
    const float width = std::max(0.001f, current.x);
    const float aspect = static_cast<float>(document.rectifiedOutputSize.height) /
                         static_cast<float>(document.rectifiedOutputSize.width);
    plane.setSize({width, width * aspect});
}

bool IsLiveCalibrationReady(
    const CalibrationSnapshot& snapshot,
    std::size_t minimumObservationCount)
{
    if (!snapshot || !snapshot.document || !snapshot.estimatedFromLiveFrames) return false;
    if (!snapshot.document->hasProfile(snapshot.activeProfile)) return false;
    return snapshot.document->profile(snapshot.activeProfile).quality.usedPairs >=
           minimumObservationCount;
}

} // namespace DualIC4Varjo
