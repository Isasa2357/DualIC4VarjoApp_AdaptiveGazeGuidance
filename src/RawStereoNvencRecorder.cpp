#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "RawStereoNvencRecorder.hpp"
#include "TimeUtil.hpp"

#include <D3DVideoEncoder/D3D12VideoEncoder.hpp>

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace DualIC4Varjo {
namespace {

using Clock = std::chrono::steady_clock;
namespace D3DVE = D3DVideoEncoderLib;

struct SideRow {
    std::uint64_t rowIndex = 0;
    std::uint64_t syncGroupId = 0;
    std::int64_t syncEmittedUnixUs = 0;
    std::int64_t pairSyncTimestampDiffNs = 0;
    std::int64_t pairHostReceivedDiffUs = 0;
    std::int64_t encodeBeginUnixUs = 0;
    std::int64_t encodeEndUnixUs = 0;
    std::int64_t encodeDurationUs = 0;
    std::uint64_t encoderFrameIndex = 0;
    std::uint64_t frameNumber = 0;
    std::uint64_t pairedFrameNumber = 0;
    std::uint64_t deviceTimestampNs = 0;
    std::int64_t hostReceivedUnixUs = 0;
    int width = 0;
    int height = 0;
    IC4Ext::CameraPixelFormat inputFormat = IC4Ext::CameraPixelFormat::BayerRG8;
    IC4Ext::GpuFrameFormat outputFormat = IC4Ext::GpuFrameFormat::RGBA8;
    DXGI_FORMAT dxgiFormat = DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_STATES resourceState = D3D12_RESOURCE_STATE_COMMON;
    std::int64_t readyWaitUs = 0;
    std::uint64_t inputConsumedFenceValue = 0;
    IC4Ext::FrameChunkMetadata chunk;
};

struct PairRow {
    std::uint64_t pairSequence = 0;
    std::uint64_t encoderFrameIndex = 0;
    std::uint64_t syncGroupId = 0;
    std::uint64_t leftFrameNumber = 0;
    std::uint64_t rightFrameNumber = 0;
    std::int64_t leftHostReceivedUnixUs = 0;
    std::int64_t rightHostReceivedUnixUs = 0;
    std::int64_t pairHostReceivedDiffUs = 0;
    std::uint64_t leftDeviceTimestampNs = 0;
    std::uint64_t rightDeviceTimestampNs = 0;
    std::int64_t pairDeviceTimestampDiffNs = 0;
    std::int64_t pairSyncTimestampDiffNs = 0;
    IC4Ext::FrameSyncTimestampSource syncTimestampSource =
        IC4Ext::FrameSyncTimestampSource::HostReceived;
};

const IC4Ext::D3D12IndexedCameraFrame* FindCamera(
    const IC4Ext::D3D12SyncedFrameSet& set,
    std::uint32_t cameraIndex)
{
    for (const auto& item : set.frames) {
        if (item.cameraIndex == cameraIndex) return &item;
    }
    return nullptr;
}

std::uint64_t HostTimestampNs(const IC4Ext::D3D12CameraFrame& frame)
{
    const auto value = std::chrono::duration_cast<std::chrono::nanoseconds>(
        frame.timing.hostReceivedTime.time_since_epoch()).count();
    return value > 0 ? static_cast<std::uint64_t>(value) : 0;
}

std::uint64_t SyncTimestampNs(
    const IC4Ext::D3D12CameraFrame& frame,
    IC4Ext::FrameSyncTimestampSource source)
{
    const auto host = HostTimestampNs(frame);
    const auto device = frame.timing.deviceTimestampNs;
    switch (source) {
    case IC4Ext::FrameSyncTimestampSource::HostReceived:
        return host;
    case IC4Ext::FrameSyncTimestampSource::Device:
        return device;
    case IC4Ext::FrameSyncTimestampSource::Auto:
    default:
        return host != 0 ? host : device;
    }
}

const char* SyncTimestampSourceName(
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

std::int64_t SignedDifference(std::uint64_t left, std::uint64_t right)
{
    if (left >= right) {
        const auto value = left - right;
        return value > static_cast<std::uint64_t>(INT64_MAX)
            ? INT64_MAX
            : static_cast<std::int64_t>(value);
    }
    const auto value = right - left;
    return value > static_cast<std::uint64_t>(INT64_MAX)
        ? INT64_MIN
        : -static_cast<std::int64_t>(value);
}

std::pair<std::uint32_t, std::uint32_t> FrameRateRational(double fps)
{
    if (!std::isfinite(fps) || fps <= 0.0) return {160u, 1u};
    const double rounded = std::round(fps);
    if (std::abs(fps - rounded) < 0.000001 && rounded <= 1000.0) {
        return {static_cast<std::uint32_t>(rounded), 1u};
    }
    return {
        static_cast<std::uint32_t>(std::llround(fps * 1000.0)),
        1000u};
}

std::int64_t FrameDuration100ns(
    std::uint32_t fpsNumerator,
    std::uint32_t fpsDenominator)
{
    const double duration = 10'000'000.0 *
        static_cast<double>(std::max<std::uint32_t>(1u, fpsDenominator)) /
        static_cast<double>(std::max<std::uint32_t>(1u, fpsNumerator));
    return std::max<std::int64_t>(1, static_cast<std::int64_t>(std::llround(duration)));
}

void WriteHeader(std::ofstream& stream)
{
    stream
        << "row_index,sync_group_id,sync_emitted_unix_us,"
        << "pair_sync_timestamp_diff_ns,pair_host_received_diff_us,"
        << "encode_begin_unix_us,encode_end_unix_us,encode_duration_us,"
        << "encoder_frame_index,frame_number,paired_frame_number,"
        << "device_timestamp_ns,host_received_unix_us,"
        << "width,height,input_format,output_format,dxgi_format,resource_state_before,"
        << "ready_wait_us,input_consumed_fence_value,"
        << "has_block_id,block_id,"
        << "has_exposure_time,exposure_time_us,"
        << "has_gain,gain,"
        << "has_imx174_frame_id,imx174_frame_id,"
        << "has_imx174_frame_set,imx174_frame_set,"
        << "has_multi_frame_set_id,multi_frame_set_id,"
        << "has_multi_frame_set_frame_id,multi_frame_set_frame_id\n";
}

void WriteRow(std::ofstream& stream, const SideRow& row)
{
    const auto& chunk = row.chunk;
    stream
        << row.rowIndex << ','
        << row.syncGroupId << ','
        << row.syncEmittedUnixUs << ','
        << row.pairSyncTimestampDiffNs << ','
        << row.pairHostReceivedDiffUs << ','
        << row.encodeBeginUnixUs << ','
        << row.encodeEndUnixUs << ','
        << row.encodeDurationUs << ','
        << row.encoderFrameIndex << ','
        << row.frameNumber << ','
        << row.pairedFrameNumber << ','
        << row.deviceTimestampNs << ','
        << row.hostReceivedUnixUs << ','
        << row.width << ','
        << row.height << ','
        << IC4Ext::ToString(row.inputFormat) << ','
        << IC4Ext::ToString(row.outputFormat) << ','
        << static_cast<unsigned int>(row.dxgiFormat) << ','
        << static_cast<unsigned int>(row.resourceState) << ','
        << row.readyWaitUs << ','
        << row.inputConsumedFenceValue << ','
        << (chunk.hasBlockId ? 1 : 0) << ',' << chunk.blockId << ','
        << (chunk.hasExposureTime ? 1 : 0) << ','
        << std::setprecision(17) << chunk.exposureTimeUs << ','
        << (chunk.hasGain ? 1 : 0) << ',' << chunk.gain << ','
        << (chunk.hasIMX174FrameId ? 1 : 0) << ',' << chunk.imx174FrameId << ','
        << (chunk.hasIMX174FrameSet ? 1 : 0) << ',' << chunk.imx174FrameSet << ','
        << (chunk.hasMultiFrameSetId ? 1 : 0) << ',' << chunk.multiFrameSetId << ','
        << (chunk.hasMultiFrameSetFrameId ? 1 : 0) << ','
        << chunk.multiFrameSetFrameId
        << '\n';
}

void WritePairHeader(std::ofstream& stream)
{
    stream
        << "pair_sequence,encoder_frame_index,sync_group_id,"
        << "left_frame_number,right_frame_number,"
        << "left_host_received_unix_us,right_host_received_unix_us,pair_host_received_diff_us,"
        << "left_device_timestamp_ns,right_device_timestamp_ns,pair_device_timestamp_diff_ns,"
        << "pair_sync_timestamp_diff_ns,sync_timestamp_source\n";
}

void WritePairRow(std::ofstream& stream, const PairRow& row)
{
    stream
        << row.pairSequence << ','
        << row.encoderFrameIndex << ','
        << row.syncGroupId << ','
        << row.leftFrameNumber << ','
        << row.rightFrameNumber << ','
        << row.leftHostReceivedUnixUs << ','
        << row.rightHostReceivedUnixUs << ','
        << row.pairHostReceivedDiffUs << ','
        << row.leftDeviceTimestampNs << ','
        << row.rightDeviceTimestampNs << ','
        << row.pairDeviceTimestampDiffNs << ','
        << row.pairSyncTimestampDiffNs << ','
        << SyncTimestampSourceName(row.syncTimestampSource)
        << '\n';
}

std::wstring QuoteArgument(const std::filesystem::path& path)
{
    std::wstring value = path.wstring();
    std::wstring quoted = L"\"";
    for (const wchar_t character : value) {
        if (character == L'\"') quoted += L'\\';
        quoted += character;
    }
    quoted += L"\"";
    return quoted;
}

void RemuxH264ToMp4(
    const std::filesystem::path& input,
    const std::filesystem::path& output,
    std::uint32_t fpsNumerator,
    std::uint32_t fpsDenominator)
{
    std::wostringstream command;
    command
        << L"ffmpeg -y -hide_banner -loglevel error -fflags +genpts -r "
        << fpsNumerator << L'/' << fpsDenominator
        << L" -i " << QuoteArgument(input)
        << L" -c copy " << QuoteArgument(output);

    std::wstring commandLine = command.str();
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            nullptr,
            mutableCommand.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startup,
            &process)) {
        throw std::runtime_error(
            "failed to launch ffmpeg for raw-video MP4 remux; H.264 file was preserved");
    }

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (exitCode != 0) {
        throw std::runtime_error(
            "ffmpeg raw-video MP4 remux failed with exit code " +
            std::to_string(exitCode) + "; H.264 file was preserved");
    }

    std::error_code error;
    std::filesystem::remove(input, error);
}

} // namespace

struct RawStereoNvencRecorder::Impl {
    explicit Impl(RawStereoNvencRecorderConfig value)
        : config(std::move(value))
        , leftH264(config.outputDirectory / (config.baseFilename + "_left_raw.h264"))
        , rightH264(config.outputDirectory / (config.baseFilename + "_right_raw.h264"))
        , leftMp4(config.outputDirectory / (config.baseFilename + "_left_raw.mp4"))
        , rightMp4(config.outputDirectory / (config.baseFilename + "_right_raw.mp4"))
        , leftCsv(config.outputDirectory / (config.baseFilename + "_left_raw_metadata.csv"))
        , rightCsv(config.outputDirectory / (config.baseFilename + "_right_raw_metadata.csv"))
        , pairCsv(config.outputDirectory / (config.baseFilename + "_raw_pairs.csv"))
    {
    }

    RawStereoNvencRecorderConfig config;
    std::filesystem::path leftH264;
    std::filesystem::path rightH264;
    std::filesystem::path leftMp4;
    std::filesystem::path rightMp4;
    std::filesystem::path leftCsv;
    std::filesystem::path rightCsv;
    std::filesystem::path pairCsv;

    std::thread worker;
    std::atomic<bool> stopRequested{true};
    std::atomic<bool> running{false};
    std::atomic<std::uint64_t> received{0};
    std::atomic<std::uint64_t> written{0};
    std::atomic<std::uint64_t> failed{0};

    mutable std::mutex errorMutex;
    std::string errorText;
    std::exception_ptr workerException;

    std::ofstream leftMetadata;
    std::ofstream rightMetadata;
    std::ofstream pairMetadata;
    std::unique_ptr<D3DVE::D3D12VideoEncoder> leftWriter;
    std::unique_ptr<D3DVE::D3D12VideoEncoder> rightWriter;
    ClockMapper clockMapper;
    std::uint64_t rowIndex = 0;
    std::uint32_t fpsNumerator = 160;
    std::uint32_t fpsDenominator = 1;
    std::int64_t frameDuration100ns = 62500;

    std::filesystem::path effectiveLeftVideoPath() const
    {
        return config.remuxToMp4 ? leftMp4 : leftH264;
    }

    std::filesystem::path effectiveRightVideoPath() const
    {
        return config.remuxToMp4 ? rightMp4 : rightH264;
    }

    void setException(std::exception_ptr exception) noexcept
    {
        try {
            std::lock_guard<std::mutex> lock(errorMutex);
            workerException = exception;
            try {
                if (exception) std::rethrow_exception(exception);
            } catch (const std::exception& value) {
                errorText = value.what();
            } catch (...) {
                errorText = "unknown raw stereo NVENC recorder failure";
            }
        } catch (...) {
        }
    }

    void openMetadata()
    {
        std::error_code error;
        std::filesystem::create_directories(config.outputDirectory, error);
        if (error) {
            throw std::runtime_error(
                "failed to create raw-video output directory: " + error.message());
        }

        leftMetadata.open(leftCsv, std::ios::out | std::ios::trunc);
        rightMetadata.open(rightCsv, std::ios::out | std::ios::trunc);
        pairMetadata.open(pairCsv, std::ios::out | std::ios::trunc);
        if (!leftMetadata || !rightMetadata || !pairMetadata) {
            throw std::runtime_error("failed to open raw-video metadata CSV files");
        }
        WriteHeader(leftMetadata);
        WriteHeader(rightMetadata);
        WritePairHeader(pairMetadata);
    }

    D3DVE::D3D12VideoEncoderDesc makeEncoderDesc(
        const std::filesystem::path& outputPath,
        const IC4Ext::D3D12CameraFrame& frame) const
    {
        D3DVE::D3D12VideoEncoderDesc desc{};
        desc.outputPath = outputPath.wstring();
        desc.width = static_cast<std::uint32_t>(frame.format.width);
        desc.height = static_cast<std::uint32_t>(frame.format.height);
        desc.frameRateNum = fpsNumerator;
        desc.frameRateDen = fpsDenominator;
        desc.backend = D3DVE::D3DVideoEncoderBackendType::NvencD3D12;
        desc.codec = D3DVE::VideoCodec::H264;
        desc.internalFormat = D3DVE::VideoPixelFormat::NV12;
        desc.rateControl = D3DVE::VideoRateControlMode::ConstantQP;
        desc.bitrate = 200'000'000;
        desc.gopLength = std::max(
            1u,
            static_cast<std::uint32_t>(std::llround(config.frameRate * 2.0)));
        desc.bFrameCount = 0;
        desc.colorRange = D3DVE::VideoColorRange::Limited;
        desc.colorMatrix = D3DVE::VideoColorMatrix::BT709;
        desc.asyncMode = false;
        desc.queueDepth = 4;
        desc.enableDebugLog = true;
        desc.input.core = config.core.get();
        desc.input.inputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.input.allowFormatConversion = true;
        desc.input.sourceWidth = static_cast<std::uint32_t>(frame.format.width);
        desc.input.sourceHeight = static_cast<std::uint32_t>(frame.format.height);
        desc.input.restoreStateAfterEncode = true;
        desc.input.processingShaderDirectory = L"shaders/D3D12Processing";
        return desc;
    }

    void initializeWriters(const IC4Ext::D3D12CameraFrame& left,
                           const IC4Ext::D3D12CameraFrame& right)
    {
        if (leftWriter || rightWriter) return;
        if (!config.core) throw std::runtime_error("raw recorder received null D3D12Core");
        if (left.format.width <= 0 || left.format.height <= 0 ||
            right.format.width != left.format.width ||
            right.format.height != left.format.height) {
            throw std::runtime_error("raw recorder requires matching non-zero stereo geometry");
        }

        ID3D12Resource* leftResource = left.textureResource
            ? left.textureResource.Get()
            : left.texture.Get();
        ID3D12Resource* rightResource = right.textureResource
            ? right.textureResource.Get()
            : right.texture.Get();
        if (!leftResource || !rightResource) {
            throw std::runtime_error("raw recorder received null D3D12 textures");
        }
        if (leftResource->GetDesc().Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
            rightResource->GetDesc().Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
            throw std::runtime_error(
                "raw recorder requires DXGI_FORMAT_R8G8B8A8_UNORM input");
        }

        const auto rational = FrameRateRational(config.frameRate);
        fpsNumerator = rational.first;
        fpsDenominator = rational.second;
        frameDuration100ns = FrameDuration100ns(fpsNumerator, fpsDenominator);

        leftWriter = std::make_unique<D3DVE::D3D12VideoEncoder>(
            makeEncoderDesc(leftH264, left));
        rightWriter = std::make_unique<D3DVE::D3D12VideoEncoder>(
            makeEncoderDesc(rightH264, right));

        std::cout
            << "[RAW_NVENC] initialized " << left.format.width << 'x'
            << left.format.height << " @ " << fpsNumerator << '/'
            << fpsDenominator
            << " fps, H.264 NVENC via app recorder backend, CQP mode\n";
    }

    SideRow makeRow(
        const IC4Ext::D3D12SyncedFrameSet& set,
        const IC4Ext::D3D12CameraFrame& frame,
        const IC4Ext::D3D12CameraFrame& paired,
        std::int64_t pairSyncDiff,
        std::int64_t pairHostDiff,
        std::int64_t beginUnix,
        std::int64_t endUnix,
        std::int64_t durationUs,
        std::int64_t readyWaitUs,
        std::uint64_t encoderIndex)
    {
        SideRow row;
        row.rowIndex = rowIndex;
        row.syncGroupId = set.syncGroupId;
        row.syncEmittedUnixUs = clockMapper.unixMicroseconds(set.emittedTime);
        row.pairSyncTimestampDiffNs = pairSyncDiff;
        row.pairHostReceivedDiffUs = pairHostDiff;
        row.encodeBeginUnixUs = beginUnix;
        row.encodeEndUnixUs = endUnix;
        row.encodeDurationUs = durationUs;
        row.encoderFrameIndex = encoderIndex;
        row.frameNumber = frame.timing.frameNumber;
        row.pairedFrameNumber = paired.timing.frameNumber;
        row.deviceTimestampNs = frame.timing.deviceTimestampNs;
        row.hostReceivedUnixUs =
            clockMapper.unixMicroseconds(frame.timing.hostReceivedTime);
        row.width = frame.format.width;
        row.height = frame.format.height;
        row.inputFormat = frame.format.actualInputFormat;
        row.outputFormat = frame.format.outputFormat;
        row.dxgiFormat = frame.dxgiFormat;
        row.resourceState = frame.resourceState;
        row.readyWaitUs = readyWaitUs;
        row.inputConsumedFenceValue = 0;
        row.chunk = frame.chunkMetadata;
        return row;
    }

    void processPair(IC4Ext::D3D12SyncedFrameSet set)
    {
        received.fetch_add(1, std::memory_order_acq_rel);
        const auto* leftIndexed = FindCamera(set, 0);
        const auto* rightIndexed = FindCamera(set, 1);
        if (!leftIndexed || !rightIndexed) {
            throw std::runtime_error(
                "raw recorder received an incomplete synchronized pair");
        }
        const auto& left = leftIndexed->frame;
        const auto& right = rightIndexed->frame;
        initializeWriters(left, right);

        const std::int64_t pairSyncDiff = SignedDifference(
            SyncTimestampNs(left, config.timestampSource),
            SyncTimestampNs(right, config.timestampSource));
        const std::int64_t pairHostDiff = SignedDifference(
            HostTimestampNs(left), HostTimestampNs(right)) / 1000;
        const std::int64_t pairDeviceDiff = SignedDifference(
            left.timing.deviceTimestampNs,
            right.timing.deviceTimestampNs);

        const auto leftWaitBegin = Clock::now();
        if (left.ready.isValid() && !left.ready.wait()) {
            throw std::runtime_error("left raw frame GPU-ready wait failed");
        }
        const auto leftWaitEnd = Clock::now();
        const auto rightWaitBegin = Clock::now();
        if (right.ready.isValid() && !right.ready.wait()) {
            throw std::runtime_error("right raw frame GPU-ready wait failed");
        }
        const auto rightWaitEnd = Clock::now();

        ID3D12Resource* leftResource = left.textureResource
            ? left.textureResource.Get()
            : left.texture.Get();
        ID3D12Resource* rightResource = right.textureResource
            ? right.textureResource.Get()
            : right.texture.Get();

        const auto encoderIndex = rowIndex;
        const std::int64_t timestamp100ns =
            static_cast<std::int64_t>(encoderIndex) * frameDuration100ns;

        const auto leftBeginSystem = std::chrono::system_clock::now();
        const auto leftBeginSteady = Clock::now();
        leftWriter->write(
            leftResource,
            left.resourceState,
            timestamp100ns,
            frameDuration100ns);
        const auto leftEndSteady = Clock::now();
        const auto leftEndSystem = std::chrono::system_clock::now();

        const auto rightBeginSystem = std::chrono::system_clock::now();
        const auto rightBeginSteady = Clock::now();
        rightWriter->write(
            rightResource,
            right.resourceState,
            timestamp100ns,
            frameDuration100ns);
        const auto rightEndSteady = Clock::now();
        const auto rightEndSystem = std::chrono::system_clock::now();

        const auto toUnixUs = [](const auto& value) {
            return std::chrono::duration_cast<std::chrono::microseconds>(
                value.time_since_epoch()).count();
        };
        const auto leftWaitUs =
            std::chrono::duration_cast<std::chrono::microseconds>(
                leftWaitEnd - leftWaitBegin).count();
        const auto rightWaitUs =
            std::chrono::duration_cast<std::chrono::microseconds>(
                rightWaitEnd - rightWaitBegin).count();
        const auto leftDurationUs =
            std::chrono::duration_cast<std::chrono::microseconds>(
                leftEndSteady - leftBeginSteady).count();
        const auto rightDurationUs =
            std::chrono::duration_cast<std::chrono::microseconds>(
                rightEndSteady - rightBeginSteady).count();

        const SideRow leftRow = makeRow(
            set,
            left,
            right,
            pairSyncDiff,
            pairHostDiff,
            toUnixUs(leftBeginSystem),
            toUnixUs(leftEndSystem),
            leftDurationUs,
            leftWaitUs,
            encoderIndex);
        const SideRow rightRow = makeRow(
            set,
            right,
            left,
            pairSyncDiff,
            pairHostDiff,
            toUnixUs(rightBeginSystem),
            toUnixUs(rightEndSystem),
            rightDurationUs,
            rightWaitUs,
            encoderIndex);
        const PairRow pairRow{
            rowIndex + 1u,
            encoderIndex,
            set.syncGroupId,
            left.timing.frameNumber,
            right.timing.frameNumber,
            clockMapper.unixMicroseconds(left.timing.hostReceivedTime),
            clockMapper.unixMicroseconds(right.timing.hostReceivedTime),
            pairHostDiff,
            left.timing.deviceTimestampNs,
            right.timing.deviceTimestampNs,
            pairDeviceDiff,
            pairSyncDiff,
            config.timestampSource,
        };

        WriteRow(leftMetadata, leftRow);
        WriteRow(rightMetadata, rightRow);
        WritePairRow(pairMetadata, pairRow);
        if (!leftMetadata || !rightMetadata || !pairMetadata) {
            throw std::runtime_error("raw-video metadata CSV write failed");
        }
        ++rowIndex;
        written.fetch_add(1, std::memory_order_acq_rel);
        if ((rowIndex % 300u) == 0u) {
            leftMetadata.flush();
            rightMetadata.flush();
            pairMetadata.flush();
        }
    }

    void finalize(bool remux)
    {
        if (leftWriter) leftWriter->close();
        if (rightWriter) rightWriter->close();
        leftWriter.reset();
        rightWriter.reset();
        if (leftMetadata.is_open()) {
            leftMetadata.flush();
            leftMetadata.close();
        }
        if (rightMetadata.is_open()) {
            rightMetadata.flush();
            rightMetadata.close();
        }
        if (pairMetadata.is_open()) {
            pairMetadata.flush();
            pairMetadata.close();
        }

        if (remux && config.remuxToMp4 &&
            written.load(std::memory_order_acquire) > 0) {
            RemuxH264ToMp4(leftH264, leftMp4, fpsNumerator, fpsDenominator);
            RemuxH264ToMp4(rightH264, rightMp4, fpsNumerator, fpsDenominator);
        }
    }

    void workerMain() noexcept
    {
        try {
            openMetadata();
            while (!stopRequested.load(std::memory_order_acquire)) {
                auto value = config.inputQueue->waitPopFor(
                    std::chrono::milliseconds(20));
                if (value) processPair(std::move(*value));
            }
            finalize(true);
        } catch (...) {
            failed.fetch_add(1, std::memory_order_acq_rel);
            setException(std::current_exception());
            try {
                finalize(false);
            } catch (...) {
            }
        }
        running.store(false, std::memory_order_release);
    }
};

RawStereoNvencRecorder::RawStereoNvencRecorder(
    RawStereoNvencRecorderConfig config)
    : impl_(std::make_unique<Impl>(std::move(config)))
{
}

RawStereoNvencRecorder::~RawStereoNvencRecorder()
{
    stopAndJoin();
}

bool RawStereoNvencRecorder::start()
{
    if (!impl_ || !impl_->config.core || !impl_->config.inputQueue) return false;
    if (impl_->worker.joinable()) return true;
    impl_->stopRequested.store(false, std::memory_order_release);
    impl_->running.store(true, std::memory_order_release);
    try {
        impl_->worker = std::thread(&Impl::workerMain, impl_.get());
    } catch (...) {
        impl_->running.store(false, std::memory_order_release);
        impl_->setException(std::current_exception());
        return false;
    }
    std::cout
        << "[RAW_NVENC] recording worker started\n"
        << "[RAW_NVENC] left video: " << impl_->effectiveLeftVideoPath().string() << '\n'
        << "[RAW_NVENC] right video: " << impl_->effectiveRightVideoPath().string() << '\n'
        << "[RAW_NVENC] left metadata: " << impl_->leftCsv.string() << '\n'
        << "[RAW_NVENC] right metadata: " << impl_->rightCsv.string() << '\n'
        << "[RAW_NVENC] pair metadata: " << impl_->pairCsv.string() << '\n';
    return true;
}

void RawStereoNvencRecorder::requestStop() noexcept
{
    if (impl_) impl_->stopRequested.store(true, std::memory_order_release);
}

void RawStereoNvencRecorder::stopAndJoin() noexcept
{
    if (!impl_) return;
    requestStop();
    if (impl_->worker.joinable() &&
        impl_->worker.get_id() != std::this_thread::get_id()) {
        try {
            impl_->worker.join();
        } catch (...) {
        }
    }
}

bool RawStereoNvencRecorder::running() const noexcept
{
    return impl_ && impl_->running.load(std::memory_order_acquire);
}

void RawStereoNvencRecorder::rethrowWorkerExceptionIfAny() const
{
    if (!impl_) return;
    std::exception_ptr error;
    {
        std::lock_guard<std::mutex> lock(impl_->errorMutex);
        error = impl_->workerException;
    }
    if (error) std::rethrow_exception(error);
}

std::string RawStereoNvencRecorder::lastError() const
{
    if (!impl_) return "raw recorder is not initialized";
    std::lock_guard<std::mutex> lock(impl_->errorMutex);
    return impl_->errorText;
}

std::uint64_t RawStereoNvencRecorder::receivedPairCount() const noexcept
{
    return impl_ ? impl_->received.load(std::memory_order_acquire) : 0;
}

std::uint64_t RawStereoNvencRecorder::writtenPairCount() const noexcept
{
    return impl_ ? impl_->written.load(std::memory_order_acquire) : 0;
}

std::uint64_t RawStereoNvencRecorder::failedPairCount() const noexcept
{
    return impl_ ? impl_->failed.load(std::memory_order_acquire) : 0;
}

std::filesystem::path RawStereoNvencRecorder::leftVideoPath() const
{
    return impl_ ? impl_->effectiveLeftVideoPath() : std::filesystem::path{};
}

std::filesystem::path RawStereoNvencRecorder::rightVideoPath() const
{
    return impl_ ? impl_->effectiveRightVideoPath() : std::filesystem::path{};
}

std::filesystem::path RawStereoNvencRecorder::leftMetadataPath() const
{
    return impl_ ? impl_->leftCsv : std::filesystem::path{};
}

std::filesystem::path RawStereoNvencRecorder::rightMetadataPath() const
{
    return impl_ ? impl_->rightCsv : std::filesystem::path{};
}

std::filesystem::path RawStereoNvencRecorder::pairMetadataPath() const
{
    return impl_ ? impl_->pairCsv : std::filesystem::path{};
}

} // namespace DualIC4Varjo
