#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "RawStereoNvencRecorder.hpp"
#include "TimeUtil.hpp"

#include <D3DVideoEncoder/D3D12VideoEncoder.hpp>

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
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

std::int64_t ToUnixUs(const std::chrono::system_clock::time_point& value)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        value.time_since_epoch()).count();
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

    struct PairState {
        IC4Ext::D3D12SyncedFrameSet frameSet;
        std::uint64_t rowIndex = 0;
        std::int64_t pairSyncDiff = 0;
        std::int64_t pairHostDiff = 0;
        std::int64_t leftReadyWaitUs = 0;
        std::int64_t rightReadyWaitUs = 0;
        std::int64_t timestamp100ns = 0;
        std::int64_t duration100ns = 0;
        std::atomic<int> completedSides{0};
    };

    struct SideEncodeJob {
        std::shared_ptr<PairState> pair;
    };

    class SideEncoderWorker final {
    public:
        void start(Impl& owner, bool leftSide, std::size_t capacity)
        {
            owner_ = &owner;
            leftSide_ = leftSide;
            capacity_ = std::max<std::size_t>(1, capacity);
            closed_ = false;
            worker_ = std::thread(&SideEncoderWorker::run, this);
        }

        void push(SideEncodeJob job)
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cvNotFull_.wait(lock, [&]() {
                return closed_ || queue_.size() < capacity_;
            });
            if (closed_) {
                throw std::runtime_error("raw recorder side encoder queue is closed");
            }
            queue_.push_back(std::move(job));
            cvNotEmpty_.notify_one();
        }

        void closeAndJoin() noexcept
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                closed_ = true;
            }
            cvNotEmpty_.notify_all();
            cvNotFull_.notify_all();
            if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
                try {
                    worker_.join();
                } catch (...) {
                }
            }
        }

    private:
        void run() noexcept
        {
            for (;;) {
                SideEncodeJob job;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    cvNotEmpty_.wait(lock, [&]() {
                        return closed_ || !queue_.empty();
                    });
                    if (queue_.empty()) {
                        if (closed_) break;
                        continue;
                    }
                    job = std::move(queue_.front());
                    queue_.pop_front();
                    cvNotFull_.notify_one();
                }

                try {
                    if (owner_) owner_->encodeSide(leftSide_, std::move(job));
                } catch (...) {
                    if (owner_) {
                        owner_->failed.fetch_add(1, std::memory_order_acq_rel);
                        owner_->setException(std::current_exception());
                        owner_->stopRequested.store(true, std::memory_order_release);
                    }
                    break;
                }
            }
        }

        Impl* owner_ = nullptr;
        bool leftSide_ = true;
        std::size_t capacity_ = 1;
        std::thread worker_;
        std::mutex mutex_;
        std::condition_variable cvNotEmpty_;
        std::condition_variable cvNotFull_;
        std::deque<SideEncodeJob> queue_;
        bool closed_ = false;
    };

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
    SideEncoderWorker leftWorker;
    SideEncoderWorker rightWorker;
    bool workersStarted = false;

    std::size_t sideQueueCapacity() const noexcept
    {
        return std::max<std::size_t>(64, config.maximumPendingGpuPairs);
    }

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
        desc.queueDepth = 1;
        desc.enableDebugLog = false;
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

        const std::size_t capacity = sideQueueCapacity();
        leftWorker.start(*this, true, capacity);
        rightWorker.start(*this, false, capacity);
        workersStarted = true;

        std::cout
            << "[RAW_NVENC] initialized " << left.format.width << 'x'
            << left.format.height << " @ " << fpsNumerator << '/'
            << fpsDenominator
            << " fps, H.264 NVENC via app recorder backend, CQP=23, "
            << "left/right encode workers=2, side_queue_capacity="
            << capacity << '\n';
    }

    SideRow makeRow(
        const PairState& pair,
        const IC4Ext::D3D12CameraFrame& frame,
        const IC4Ext::D3D12CameraFrame& paired,
        std::int64_t beginUnix,
        std::int64_t endUnix,
        std::int64_t durationUs,
        std::int64_t readyWaitUs) const
    {
        SideRow row;
        row.rowIndex = pair.rowIndex;
        row.syncGroupId = pair.frameSet.syncGroupId;
        row.syncEmittedUnixUs = clockMapper.unixMicroseconds(pair.frameSet.emittedTime);
        row.pairSyncTimestampDiffNs = pair.pairSyncDiff;
        row.pairHostReceivedDiffUs = pair.pairHostDiff;
        row.encodeBeginUnixUs = beginUnix;
        row.encodeEndUnixUs = endUnix;
        row.encodeDurationUs = durationUs;
        row.encoderFrameIndex = pair.rowIndex;
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

    void encodeSide(bool leftSide, SideEncodeJob job)
    {
        if (!job.pair) return;
        auto& pair = *job.pair;
        const auto* selfIndexed = FindCamera(pair.frameSet, leftSide ? 0u : 1u);
        const auto* otherIndexed = FindCamera(pair.frameSet, leftSide ? 1u : 0u);
        if (!selfIndexed || !otherIndexed) {
            throw std::runtime_error("raw recorder side worker received an incomplete pair");
        }

        const auto& frame = selfIndexed->frame;
        const auto& paired = otherIndexed->frame;
        ID3D12Resource* resource = frame.textureResource
            ? frame.textureResource.Get()
            : frame.texture.Get();
        if (!resource) {
            throw std::runtime_error("raw recorder side worker received a null texture");
        }

        auto& writer = leftSide ? leftWriter : rightWriter;
        auto& metadata = leftSide ? leftMetadata : rightMetadata;
        if (!writer) {
            throw std::runtime_error("raw recorder side writer was not initialized");
        }

        const auto beginSystem = std::chrono::system_clock::now();
        const auto beginSteady = Clock::now();
        writer->write(
            resource,
            frame.resourceState,
            pair.timestamp100ns,
            pair.duration100ns);
        const auto endSteady = Clock::now();
        const auto endSystem = std::chrono::system_clock::now();

        const auto durationUs = std::chrono::duration_cast<std::chrono::microseconds>(
            endSteady - beginSteady).count();
        const SideRow row = makeRow(
            pair,
            frame,
            paired,
            ToUnixUs(beginSystem),
            ToUnixUs(endSystem),
            durationUs,
            leftSide ? pair.leftReadyWaitUs : pair.rightReadyWaitUs);

        WriteRow(metadata, row);
        if (!metadata) {
            throw std::runtime_error("raw-video side metadata CSV write failed");
        }
        if (((pair.rowIndex + 1u) % 300u) == 0u) {
            metadata.flush();
        }

        if (pair.completedSides.fetch_add(1, std::memory_order_acq_rel) + 1 == 2) {
            written.fetch_add(1, std::memory_order_acq_rel);
        }
    }

    void processPair(IC4Ext::D3D12SyncedFrameSet set)
    {
        received.fetch_add(1, std::memory_order_acq_rel);

        auto pair = std::make_shared<PairState>();
        pair->frameSet = std::move(set);
        pair->rowIndex = rowIndex;
        pair->timestamp100ns = static_cast<std::int64_t>(rowIndex) * frameDuration100ns;
        pair->duration100ns = frameDuration100ns;

        const auto* leftIndexed = FindCamera(pair->frameSet, 0);
        const auto* rightIndexed = FindCamera(pair->frameSet, 1);
        if (!leftIndexed || !rightIndexed) {
            throw std::runtime_error(
                "raw recorder received an incomplete synchronized pair");
        }
        const auto& left = leftIndexed->frame;
        const auto& right = rightIndexed->frame;
        initializeWriters(left, right);
        pair->timestamp100ns = static_cast<std::int64_t>(rowIndex) * frameDuration100ns;
        pair->duration100ns = frameDuration100ns;

        pair->pairSyncDiff = SignedDifference(
            SyncTimestampNs(left, config.timestampSource),
            SyncTimestampNs(right, config.timestampSource));
        pair->pairHostDiff = SignedDifference(
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
        pair->leftReadyWaitUs = std::chrono::duration_cast<std::chrono::microseconds>(
            leftWaitEnd - leftWaitBegin).count();
        pair->rightReadyWaitUs = std::chrono::duration_cast<std::chrono::microseconds>(
            rightWaitEnd - rightWaitBegin).count();

        const PairRow pairRow{
            rowIndex + 1u,
            rowIndex,
            pair->frameSet.syncGroupId,
            left.timing.frameNumber,
            right.timing.frameNumber,
            clockMapper.unixMicroseconds(left.timing.hostReceivedTime),
            clockMapper.unixMicroseconds(right.timing.hostReceivedTime),
            pair->pairHostDiff,
            left.timing.deviceTimestampNs,
            right.timing.deviceTimestampNs,
            pairDeviceDiff,
            pair->pairSyncDiff,
            config.timestampSource,
        };

        WritePairRow(pairMetadata, pairRow);
        if (!pairMetadata) {
            throw std::runtime_error("raw-video pair metadata CSV write failed");
        }
        if (((rowIndex + 1u) % 300u) == 0u) {
            pairMetadata.flush();
        }

        leftWorker.push(SideEncodeJob{pair});
        rightWorker.push(SideEncodeJob{std::move(pair)});
        ++rowIndex;
    }

    void finalize(bool remux)
    {
        if (workersStarted) {
            leftWorker.closeAndJoin();
            rightWorker.closeAndJoin();
            workersStarted = false;
        }
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
