#pragma once

#include <IC4Ext/IC4Ext.hpp>
#include <ThreadKit/Queues/BlockingQueue.hpp>
#include <ThreadKit/Queues/QueueCommon.hpp>

#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace IC4Ext {

// Application-local bridge from the removed v1 D3D12 pipeline surface to the
// IC4Ext v2 ReadOnly pipeline. The compatibility objects below never duplicate
// camera GPU textures. They only retain D3D12::ReadOnlyFrame/ReadOnlyFrameSet
// handles so the producer FramePool cannot recycle a texture while an existing
// application consumer still references it.
struct AppSharedD3D12CameraFrame
{
    D3D12::ReadOnlyFrame sharedFrame;

    // Keep the field surface used by the existing application. textureResource
    // intentionally stays empty; existing consumers already fall back to
    // texture, which points at the shared IC4Ext FramePool resource.
    D3D12CoreLib::D3D12Resource textureResource;
    D3D12CoreLib::D3D12Resource inputBufferResource;
    Microsoft::WRL::ComPtr<ID3D12Resource> texture;
    Microsoft::WRL::ComPtr<ID3D12Resource> uploadKeepAlive;
    Microsoft::WRL::ComPtr<ID3D12Resource> inputBufferKeepAlive;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocatorKeepAlive;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandListKeepAlive;
    D3D12CoreLib::D3D12DescriptorHeap srvHeapHelper;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpuHandle{};
    DXGI_FORMAT dxgiFormat = DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_STATES resourceState = D3D12_RESOURCE_STATE_COMMON;

    D3D12ReadyToken ready;
    FrameTiming timing;
    FrameFormatMetadata format;
    FrameChunkMetadata chunkMetadata;

    AppSharedD3D12CameraFrame() = default;

    explicit AppSharedD3D12CameraFrame(const D3D12::ReadOnlyFrame& frame)
        : sharedFrame(frame)
    {
        if (!frame) return;
        texture = frame.resource();
        srvHeap = frame.descriptorHeap();
        srvCpuHandle = frame.srvCpuHandle();
        srvGpuHandle = frame.srvGpuHandle();
        dxgiFormat = frame.dxgiFormat();
        resourceState = frame.publishedState();
        ready = frame.readyToken();
        timing = frame.timing();
        format = frame.format();
        chunkMetadata = frame.chunkMetadata();
    }

    // Allows existing IC4Ext helpers such as D3D12FrameReadback to select the
    // v2 immutable-frame overload without another GPU copy.
    operator const D3D12::ReadOnlyFrame&() const noexcept
    {
        return sharedFrame;
    }
};

struct AppSharedD3D12IndexedCameraFrame
{
    std::uint32_t cameraIndex = 0;
    AppSharedD3D12CameraFrame frame;
};

struct AppSharedD3D12SyncedFrameSet
{
    // Holding the complete native set is deliberate: it provides one simple
    // lifetime anchor for every shared texture in this synchronized pair.
    D3D12::ReadOnlyFrameSet sharedSet;
    std::vector<AppSharedD3D12IndexedCameraFrame> frames;
    std::uint64_t syncGroupId = 0;
    std::chrono::steady_clock::time_point emittedTime{};

    static AppSharedD3D12SyncedFrameSet FromNative(
        const D3D12::ReadOnlyFrameSet& source)
    {
        AppSharedD3D12SyncedFrameSet result;
        result.sharedSet = source;
        result.syncGroupId = source.syncGroupId();
        result.emittedTime = source.completedTime();
        result.frames.reserve(source.size());
        for (const auto& item : source.frames()) {
            result.frames.push_back({
                item.cameraId,
                AppSharedD3D12CameraFrame(item.frame)});
        }
        return result;
    }
};

// v1 input queues are now only logical pipeline tokens. IC4Ext v2 has exactly
// one real camera ingress queue, owned by SharedReadOnlyPipelineCoordinator.
class AppSharedD3D12IndexedFrameQueue final
{
public:
    explicit AppSharedD3D12IndexedFrameQueue(
        ThreadKit::Queues::QueueOptions options = {})
        : options_(options)
        , nativeQueue_(
              std::make_shared<D3D12::IndexedReadOnlyFrameQueue>(options))
    {
    }

    const ThreadKit::Queues::QueueOptions& options() const noexcept
    {
        return options_;
    }

    std::shared_ptr<D3D12::IndexedReadOnlyFrameQueue> nativeQueue() const
    {
        return nativeQueue_;
    }

    void close()
    {
        closed_ = true;
        if (nativeQueue_) nativeQueue_->close();
    }

    bool closed() const noexcept { return closed_; }

private:
    ThreadKit::Queues::QueueOptions options_{};
    std::shared_ptr<D3D12::IndexedReadOnlyFrameQueue> nativeQueue_;
    bool closed_ = false;
};

// Consumer-facing queue that preserves the application's v1 set layout while
// reading from an IC4Ext v2 ReadOnlyFrameSetQueue. Conversion is metadata/handle
// only; no D3D12 resource is copied.
class AppSharedD3D12SyncedFrameQueue final
{
public:
    explicit AppSharedD3D12SyncedFrameQueue(
        ThreadKit::Queues::QueueOptions options = {})
        : nativeQueue_(
              std::make_shared<D3D12::ReadOnlyFrameSetQueue>(options))
    {
    }

    std::shared_ptr<D3D12::ReadOnlyFrameSetQueue> nativeQueue() const
    {
        return nativeQueue_;
    }

    std::optional<AppSharedD3D12SyncedFrameSet> tryPopLatest()
    {
        return convertAndRetain(nativeQueue_->tryPopLatest());
    }

    std::optional<AppSharedD3D12SyncedFrameSet> tryPop()
    {
        return convertAndRetain(nativeQueue_->tryPop());
    }

    std::optional<AppSharedD3D12SyncedFrameSet> waitPop()
    {
        return convertAndRetain(nativeQueue_->waitPop());
    }

    template <class Rep, class Period>
    std::optional<AppSharedD3D12SyncedFrameSet> waitPopFor(
        const std::chrono::duration<Rep, Period>& timeout)
    {
        return convertAndRetain(nativeQueue_->waitPopFor(timeout));
    }

    template <class Rep, class Period>
    std::optional<AppSharedD3D12SyncedFrameSet> waitPopLatestFor(
        const std::chrono::duration<Rep, Period>& timeout)
    {
        return convertAndRetain(nativeQueue_->waitPopLatestFor(timeout));
    }

    void clear()
    {
        if (nativeQueue_) nativeQueue_->clear();
        std::lock_guard<std::mutex> lock(retainedMutex_);
        retainedSets_.clear();
    }

    void close()
    {
        if (nativeQueue_) nativeQueue_->close();
    }

    ThreadKit::Queues::QueueStats stats() const
    {
        return nativeQueue_ ? nativeQueue_->stats()
                            : ThreadKit::Queues::QueueStats{};
    }

private:
    // ImGui uses a two-buffer swap chain and intentionally stores only ComPtr
    // handles to the current camera resources. A ComPtr keeps the D3D12 object
    // alive but does not keep IC4Ext's producer-side FramePool lease alive. Keep
    // three recently popped synchronized sets per logical output so asynchronous
    // consumers cannot see a resource recycled while one of two GPU frames is
    // still in flight. Other consumers already hold a ReadOnlyFrame(Set) for
    // their exact work duration; this small history is a bounded extra guard.
    static constexpr std::size_t kRetainedPoppedSets = 3;

    std::optional<AppSharedD3D12SyncedFrameSet> convertAndRetain(
        std::optional<D3D12::ReadOnlyFrameSet> source)
    {
        if (!source) return std::nullopt;

        auto converted = AppSharedD3D12SyncedFrameSet::FromNative(*source);
        {
            std::lock_guard<std::mutex> lock(retainedMutex_);
            retainedSets_.push_back(*source);
            while (retainedSets_.size() > kRetainedPoppedSets) {
                retainedSets_.pop_front();
            }
        }
        return converted;
    }

    std::shared_ptr<D3D12::ReadOnlyFrameSetQueue> nativeQueue_;
    mutable std::mutex retainedMutex_;
    std::deque<D3D12::ReadOnlyFrameSet> retainedSets_;
};

namespace AppV2Bridge {

inline D3D12::FrameSyncTimestampSource ToV2TimestampSource(
    FrameSyncTimestampSource source) noexcept
{
    switch (source) {
    case FrameSyncTimestampSource::HostReceived:
        return D3D12::FrameSyncTimestampSource::HostReceived;
    case FrameSyncTimestampSource::Device:
        return D3D12::FrameSyncTimestampSource::Device;
    case FrameSyncTimestampSource::Auto:
    default:
        return D3D12::FrameSyncTimestampSource::Auto;
    }
}

// One process-wide central IC4Ext v2 synchronization worker. Legacy-looking
// FrameSyncThread wrappers register independent outputs here, so Varjo, ImGui,
// calibration and recording all receive references to the same immutable GPU
// frame resources.
class SharedReadOnlyPipelineCoordinator final
{
public:
    static SharedReadOnlyPipelineCoordinator& instance()
    {
        static SharedReadOnlyPipelineCoordinator value;
        return value;
    }

    bool ensurePipeline(
        const std::shared_ptr<AppSharedD3D12IndexedFrameQueue>& logicalInput,
        const FrameSyncOptions& options,
        ErrorInfo& error)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (syncThread_) {
            if (!compatibleLocked(options)) {
                error = MakeError(
                    ErrorCode::InvalidArgument,
                    "SharedReadOnlyPipelineCoordinator::ensurePipeline",
                    "all application outputs must use the same central FrameSync configuration");
                return false;
            }
            return true;
        }

        if (!logicalInput) {
            error = MakeError(
                ErrorCode::InvalidArgument,
                "SharedReadOnlyPipelineCoordinator::ensurePipeline",
                "logical input queue is null");
            return false;
        }

        D3D12::FrameSyncConfig config;
        config.cameraIds = options.cameraIndices.empty()
            ? std::vector<D3D12::CameraId>{0}
            : std::vector<D3D12::CameraId>(
                  options.cameraIndices.begin(),
                  options.cameraIndices.end());
        config.timestampSource = ToV2TimestampSource(options.timestampSource);
        config.maxTimestampDiffNs = options.maxTimestampDiffNs;
        config.maxBufferedFramesPerCamera = options.maxBufferedFramesPerCamera;
        config.groupTimeout = std::chrono::milliseconds(50);

        if (options.policy != FrameSyncPolicy::TimestampNearest &&
            config.cameraIds.size() > 1) {
            error = MakeError(
                ErrorCode::InvalidArgument,
                "SharedReadOnlyPipelineCoordinator::ensurePipeline",
                "IC4Ext v2 multi-camera synchronization supports TimestampNearest only");
            return false;
        }

        ingress_ = logicalInput->nativeQueue();
        syncConfig_ = config;
        legacyOptions_ = options;
        syncThread_ = std::make_unique<D3D12::FrameSyncThread>(
            ingress_,
            syncConfig_);
        return true;
    }

    D3D12::FrameSyncOutputId registerOutput(
        const std::shared_ptr<AppSharedD3D12SyncedFrameQueue>& output,
        const std::shared_ptr<AppSharedD3D12IndexedFrameQueue>& logicalInput,
        const FrameSyncOptions& options,
        ErrorInfo& error)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ensurePipelineLocked(logicalInput, options, error)) {
            return D3D12::InvalidFrameSyncOutputId;
        }
        if (!output) {
            error = MakeError(
                ErrorCode::InvalidArgument,
                "SharedReadOnlyPipelineCoordinator::registerOutput",
                "output queue is null");
            return D3D12::InvalidFrameSyncOutputId;
        }

        D3D12::FrameSyncOutputConfig outputConfig;
        outputConfig.requiredCameras = syncConfig_.cameraIds;
        outputConfig.frameRate = D3D12::FrameRateLimit::Maximum();
        outputConfig.priority = 100;
        outputConfig.enabled = true;

        const auto outputId = syncThread_->registerOutput(
            output->nativeQueue(),
            outputConfig);
        if (outputId == D3D12::InvalidFrameSyncOutputId) {
            error = syncThread_->lastError();
            return outputId;
        }

        if (!started_) {
            if (!syncThread_->start()) {
                error = syncThread_->lastError();
                syncThread_->stopOutputSupply(outputId);
                syncThread_->closeOutputChannel(outputId);
                return D3D12::InvalidFrameSyncOutputId;
            }
            started_ = true;
        }

        ++activeOutputs_;
        return outputId;
    }

    void retireOutput(D3D12::FrameSyncOutputId outputId) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!syncThread_ || outputId == D3D12::InvalidFrameSyncOutputId) return;

        syncThread_->stopOutputSupply(outputId);
        syncThread_->closeOutputChannel(outputId);
        if (activeOutputs_ > 0) --activeOutputs_;

        if (activeOutputs_ == 0) {
            syncThread_->stopAndJoin();
            if (ingress_) ingress_->close();
            syncThread_.reset();
            ingress_.reset();
            syncConfig_ = {};
            legacyOptions_ = {};
            started_ = false;
        }
    }

    std::shared_ptr<D3D12::IndexedReadOnlyFrameQueue> ingress() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return ingress_;
    }

    FrameSyncStats legacyStats(D3D12::FrameSyncOutputId outputId) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        FrameSyncStats result;
        if (!syncThread_) return result;

        const auto central = syncThread_->stats();
        result.inputFrames = central.inputFrames;
        result.ignoredFrames = central.ignoredFrames;
        result.droppedFrames = central.droppedFrames;

        const auto output = syncThread_->outputStats(outputId);
        if (output) {
            result.emittedSets = output->emittedSets;
            result.pushFailures =
                output->dispatchErrors + output->closedQueuePushes;
        }
        return result;
    }

    ErrorInfo lastError() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return syncThread_ ? syncThread_->lastError() : ErrorInfo{};
    }

private:
    SharedReadOnlyPipelineCoordinator() = default;

    bool compatibleLocked(const FrameSyncOptions& options) const noexcept
    {
        return legacyOptions_.policy == options.policy &&
               legacyOptions_.cameraIndices == options.cameraIndices &&
               legacyOptions_.maxTimestampDiffNs == options.maxTimestampDiffNs &&
               legacyOptions_.maxBufferedFramesPerCamera ==
                   options.maxBufferedFramesPerCamera &&
               legacyOptions_.timestampSource == options.timestampSource;
    }

    bool ensurePipelineLocked(
        const std::shared_ptr<AppSharedD3D12IndexedFrameQueue>& logicalInput,
        const FrameSyncOptions& options,
        ErrorInfo& error)
    {
        if (syncThread_) {
            if (!compatibleLocked(options)) {
                error = MakeError(
                    ErrorCode::InvalidArgument,
                    "SharedReadOnlyPipelineCoordinator::ensurePipeline",
                    "all application outputs must use the same central FrameSync configuration");
                return false;
            }
            return true;
        }

        if (!logicalInput) {
            error = MakeError(
                ErrorCode::InvalidArgument,
                "SharedReadOnlyPipelineCoordinator::ensurePipeline",
                "logical input queue is null");
            return false;
        }

        D3D12::FrameSyncConfig config;
        config.cameraIds = options.cameraIndices.empty()
            ? std::vector<D3D12::CameraId>{0}
            : std::vector<D3D12::CameraId>(
                  options.cameraIndices.begin(),
                  options.cameraIndices.end());
        config.timestampSource = ToV2TimestampSource(options.timestampSource);
        config.maxTimestampDiffNs = options.maxTimestampDiffNs;
        config.maxBufferedFramesPerCamera = options.maxBufferedFramesPerCamera;
        config.groupTimeout = std::chrono::milliseconds(50);

        if (options.policy != FrameSyncPolicy::TimestampNearest &&
            config.cameraIds.size() > 1) {
            error = MakeError(
                ErrorCode::InvalidArgument,
                "SharedReadOnlyPipelineCoordinator::ensurePipeline",
                "IC4Ext v2 multi-camera synchronization supports TimestampNearest only");
            return false;
        }

        ingress_ = logicalInput->nativeQueue();
        syncConfig_ = config;
        legacyOptions_ = options;
        syncThread_ = std::make_unique<D3D12::FrameSyncThread>(
            ingress_,
            syncConfig_);
        return true;
    }

    mutable std::mutex mutex_;
    std::shared_ptr<D3D12::IndexedReadOnlyFrameQueue> ingress_;
    std::unique_ptr<D3D12::FrameSyncThread> syncThread_;
    D3D12::FrameSyncConfig syncConfig_{};
    FrameSyncOptions legacyOptions_{};
    std::size_t activeOutputs_ = 0;
    bool started_ = false;
};

} // namespace AppV2Bridge
} // namespace IC4Ext

// The rest of the application was written against the v1 frame layout. Map
// only those frame/queue type tokens to the local shared-resource bridge. The
// actual capture and synchronization workers are replaced separately by the
// coordinated wrappers in this repository.
#ifndef DUAL_IC4_VARJO_DISABLE_IC4EXT_V2_TYPE_ALIASES
#define D3D12CameraFrame AppSharedD3D12CameraFrame
#define D3D12IndexedCameraFrame AppSharedD3D12IndexedCameraFrame
#define D3D12SyncedFrameSet AppSharedD3D12SyncedFrameSet
#define D3D12IndexedFrameQueue AppSharedD3D12IndexedFrameQueue
#define D3D12SyncedFrameQueue AppSharedD3D12SyncedFrameQueue
#endif
