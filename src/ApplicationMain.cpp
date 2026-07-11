#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "AppConfig.hpp"
#include "DisplayTextureRing.hpp"
#include "ExperimentOutput.hpp"
#include "RenderedFrameMetadataLogger.hpp"
#include "TimeUtil.hpp"

#include <IC4Ext/IC4Ext.hpp>
#include <VarjoToolkit/Core/VarjoSession.hpp>
#include <VarjoXR/Backends/D3D12/D3D12Backend.hpp>
#include <VarjoXR/VarjoXR.hpp>

#include <D3D12Helper/D3D12Core/D3D12Core.hpp>
#include <ThreadKit/Queues/QueueCommon.hpp>

#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {

constexpr std::size_t kSyncPipelineCount = 3;
constexpr std::size_t kDisplayPipelineIndex = 0;
constexpr std::array<std::size_t, kSyncPipelineCount> kCameraOutputOrder{
    1,
    2,
    kDisplayPipelineIndex,
};

std::atomic<bool> gStopRequested{false};

BOOL WINAPI ConsoleControlHandler(DWORD controlType)
{
    switch (controlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        gStopRequested.store(true, std::memory_order_release);
        return TRUE;
    default:
        return FALSE;
    }
}

void PrintError(const char* label, const IC4Ext::ErrorInfo& error)
{
    std::cerr << label << " failed: "
              << error.where << ": " << error.message << '\n';
}

const char* PlacementModeName(VarjoXR::PlacementMode mode) noexcept
{
    return mode == VarjoXR::PlacementMode::HeadRelative ? "head" : "world";
}

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
    const std::uint64_t host = HostTimestampNs(frame);
    const std::uint64_t device = frame.timing.deviceTimestampNs;
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

DualIC4Varjo::CameraFrameMetadataRow BuildCameraMetadata(
    const IC4Ext::D3D12CameraFrame& frame,
    const DualIC4Varjo::ClockMapper& clock)
{
    DualIC4Varjo::CameraFrameMetadataRow row;
    row.frameNumber = frame.timing.frameNumber;
    row.deviceTimestampNs = frame.timing.deviceTimestampNs;
    row.hostReceivedUnixUs =
        clock.unixMicroseconds(frame.timing.hostReceivedTime);
    row.width = frame.format.width;
    row.height = frame.format.height;
    return row;
}

struct DisplayedPairMetadata {
    std::uint64_t syncGroupId = 0;
    std::chrono::steady_clock::time_point emittedTime{};
    std::int64_t syncTimestampDiffNs = 0;
    std::int64_t hostReceivedDiffUs = 0;
    DualIC4Varjo::CameraFrameMetadataRow left;
    DualIC4Varjo::CameraFrameMetadataRow right;
};

DisplayedPairMetadata BuildPairMetadata(
    const IC4Ext::D3D12SyncedFrameSet& set,
    const IC4Ext::D3D12CameraFrame& left,
    const IC4Ext::D3D12CameraFrame& right,
    IC4Ext::FrameSyncTimestampSource source,
    const DualIC4Varjo::ClockMapper& clock)
{
    DisplayedPairMetadata metadata;
    metadata.syncGroupId = set.syncGroupId;
    metadata.emittedTime = set.emittedTime;
    metadata.syncTimestampDiffNs = DualIC4Varjo::SignedDifference(
        SyncTimestampNs(left, source),
        SyncTimestampNs(right, source));
    metadata.hostReceivedDiffUs = DualIC4Varjo::SignedDifference(
        HostTimestampNs(left),
        HostTimestampNs(right)) / 1000;
    metadata.left = BuildCameraMetadata(left, clock);
    metadata.right = BuildCameraMetadata(right, clock);
    return metadata;
}

ThreadKit::Queues::QueueOptions MakeQueueOptions(std::size_t maxSize)
{
    ThreadKit::Queues::QueueOptions options;
    options.maxSize = maxSize;
    options.overflowPolicy =
        ThreadKit::Queues::QueueOverflowPolicy::DropOldest;
    return options;
}

void PrintSyncStats(
    const std::array<std::unique_ptr<IC4Ext::D3D12FrameSyncThread>,
                     kSyncPipelineCount>& syncThreads,
    const std::array<std::shared_ptr<IC4Ext::D3D12SyncedFrameQueue>,
                     kSyncPipelineCount>& outputQueues)
{
    for (std::size_t i = 0; i < kSyncPipelineCount; ++i) {
        const auto syncStats = syncThreads[i]->stats();
        const auto queueStats = outputQueues[i]->stats();
        std::cout
            << "[SYNC " << i << "]"
            << (i == kDisplayPipelineIndex ? " display" : " discard")
            << " input=" << syncStats.inputFrames
            << " emitted=" << syncStats.emittedSets
            << " dropped=" << syncStats.droppedFrames
            << " pushFailures=" << syncStats.pushFailures
            << " outputDroppedOldest=" << queueStats.droppedOldest
            << " outputDroppedByPopLatest=" << queueStats.droppedByPopLatest
            << '\n';
    }
}

} // namespace

int main(int argc, char** argv)
{
    using DualIC4Varjo::AppConfig;

    gStopRequested.store(false, std::memory_order_release);
    SetConsoleCtrlHandler(ConsoleControlHandler, TRUE);

    AppConfig config;
    std::string argumentError;
    if (!DualIC4Varjo::ParseArguments(
            argc,
            argv,
            config,
            argumentError)) {
        std::cerr << "Argument error: " << argumentError << "\n\n";
        DualIC4Varjo::PrintUsage(std::cerr);
        return 2;
    }
    if (config.showHelp) {
        DualIC4Varjo::PrintUsage(std::cout);
        return 0;
    }

    std::cout
        << "DualIC4VarjoApp minimal display stage\n"
        << "  Capture FPS            : " << config.fps << '\n'
        << "  FrameSyncThread count  : " << kSyncPipelineCount << '\n'
        << "  Display pipeline       : 0\n"
        << "  Auxiliary pipelines    : 1, 2 (DropOldest discard)\n"
        << "  Varjo eye textures     : identical left-camera image\n"
        << "  Render thread          : dedicated std::thread\n"
        << "  Frame selection        : tryPopLatest immediately after beginFrame\n"
        << "  Main thread id         : " << GetCurrentThreadId() << '\n';

    DualIC4Varjo::RenderedFrameMetadataLogger logger;
    std::shared_ptr<D3D12CoreLib::D3D12Core> core;
    std::shared_ptr<VarjoSession> session;

    std::array<std::shared_ptr<IC4Ext::D3D12IndexedFrameQueue>,
               kSyncPipelineCount> inputQueues;
    std::array<std::shared_ptr<IC4Ext::D3D12SyncedFrameQueue>,
               kSyncPipelineCount> outputQueues;
    std::array<std::unique_ptr<IC4Ext::D3D12FrameSyncThread>,
               kSyncPipelineCount> syncThreads;

    try {
        D3D12CoreLib::D3D12CoreConfig coreConfig{};
        coreConfig.enableDebugLayer = config.enableD3D12DebugLayer;
        coreConfig.enableInfoQueue = config.enableD3D12DebugLayer;
        coreConfig.enableDred = true;
        coreConfig.createDirectQueue = true;
        coreConfig.createCopyQueue = true;
        core = D3D12CoreLib::D3D12Core::CreateShared(coreConfig);

        session = std::make_shared<VarjoSession>();
        if (!session->valid() && !session->initialize()) {
            throw std::runtime_error(
                "Varjo session initialization failed: " +
                session->lastError());
        }

        auto backend = VarjoXR::Backends::D3D12::CreateBackend(core);
        VarjoXR::XRSpace space({session, std::move(backend)});
        auto& d3dBackend =
            static_cast<VarjoXR::Backends::D3D12::D3D12Backend&>(
                space.backend());

        auto& plane = space.createPlane(
            {config.planeWidthMeters, config.planeWidthMeters});
        plane.setPlacementMode(config.placementMode);
        plane.transform().position =
            {config.planeX, config.planeY, -config.planeDistanceMeters};

        IC4Ext::FrameSyncOptions syncOptions;
        syncOptions.policy = IC4Ext::FrameSyncPolicy::TimestampNearest;
        syncOptions.cameraIndices = {0, 1};
        syncOptions.maxTimestampDiffNs = static_cast<std::uint64_t>(
            std::llround(config.syncToleranceMs * 1'000'000.0));
        syncOptions.maxBufferedFramesPerCamera =
            config.syncBufferedFramesPerCamera;
        syncOptions.timestampSource = config.timestampSource;

        for (std::size_t i = 0; i < kSyncPipelineCount; ++i) {
            inputQueues[i] =
                std::make_shared<IC4Ext::D3D12IndexedFrameQueue>(
                    MakeQueueOptions(config.inputQueueSize));
            const std::size_t outputSize =
                i == kDisplayPipelineIndex ? config.outputQueueSize : 1;
            outputQueues[i] =
                std::make_shared<IC4Ext::D3D12SyncedFrameQueue>(
                    MakeQueueOptions(outputSize));
            syncThreads[i] =
                std::make_unique<IC4Ext::D3D12FrameSyncThread>(
                    inputQueues[i],
                    outputQueues[i],
                    syncOptions);
        }

        std::size_t startedSyncThreadCount = 0;
        for (std::size_t i = 0; i < kSyncPipelineCount; ++i) {
            if (!syncThreads[i]->start()) {
                PrintError("FrameSyncThread", syncThreads[i]->lastError());
                for (std::size_t j = 0; j < startedSyncThreadCount; ++j) {
                    syncThreads[j]->stopAndJoin();
                }
                return 1;
            }
            ++startedSyncThreadCount;
            std::cout << "[THREAD] FrameSyncThread " << i
                      << " started\n";
        }

        const auto captureBackend =
            IC4Ext::D3D12BackendContext::FromCore(core);
        IC4Ext::CameraThreadOptions cameraThreadOptions;
        cameraThreadOptions.readTimeoutMs = config.cameraReadTimeoutMs;
        cameraThreadOptions.copyPerOutputQueue = true;
        cameraThreadOptions.stopOnReadError = false;
        cameraThreadOptions.copiedOutputFrameStride = 1;
        cameraThreadOptions.copiedOutputFrameBurst = 1;

        IC4Ext::D3D12CameraCaptureThread leftCamera(
            config.left.selector,
            DualIC4Varjo::MakeCaptureConfig(config, config.left),
            captureBackend,
            cameraThreadOptions);
        IC4Ext::D3D12CameraCaptureThread rightCamera(
            config.right.selector,
            DualIC4Varjo::MakeCaptureConfig(config, config.right),
            captureBackend,
            cameraThreadOptions);

        // IC4Ext copies to all outputs except the last output, which receives the
        // original frame. Register the two discard pipelines first and the display
        // pipeline last so rendering gets the original D3D12 resource.
        for (const std::size_t pipelineIndex : kCameraOutputOrder) {
            leftCamera.addOutputQueue(0, inputQueues[pipelineIndex]);
            rightCamera.addOutputQueue(1, inputQueues[pipelineIndex]);
        }

        auto stopCaptureAndSync = [&]() noexcept {
            leftCamera.stopAndJoin();
            rightCamera.stopAndJoin();
            for (auto& syncThread : syncThreads) {
                if (syncThread) syncThread->stopAndJoin();
            }
            for (auto& queue : inputQueues) {
                if (queue) queue->close();
            }
            for (auto& queue : outputQueues) {
                if (queue) queue->close();
            }
        };

        if (!leftCamera.start()) {
            PrintError("Left camera", leftCamera.lastError());
            stopCaptureAndSync();
            return 1;
        }
        if (config.cameraStartDelayMs > 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(config.cameraStartDelayMs));
        }
        if (!rightCamera.start()) {
            PrintError("Right camera", rightCamera.lastError());
            stopCaptureAndSync();
            return 1;
        }

        auto initialSet = outputQueues[kDisplayPipelineIndex]
            ->waitPopLatestFor(
                std::chrono::milliseconds(config.initialFrameTimeoutMs));
        if (!initialSet) {
            const auto leftStats = leftCamera.stats();
            const auto rightStats = rightCamera.stats();
            PrintSyncStats(syncThreads, outputQueues);
            stopCaptureAndSync();
            std::cerr
                << "Timed out waiting for the first synchronized display frame.\n"
                << "  left read=" << leftStats.readFrames
                << " errors=" << leftStats.readErrors << '\n'
                << "  right read=" << rightStats.readFrames
                << " errors=" << rightStats.readErrors << '\n';
            return 1;
        }

        DualIC4Varjo::ClockMapper clock;
        DualIC4Varjo::DisplayTextureRing displayRing(
            core,
            d3dBackend,
            config.displayRingSize);
        std::shared_ptr<IC4Ext::D3D12SyncedFrameSet> currentSet =
            std::make_shared<IC4Ext::D3D12SyncedFrameSet>(
                std::move(*initialSet));
        DisplayedPairMetadata displayedMetadata;
        std::size_t activeSlot = 0;
        bool planeSizeInitialized = false;

        auto applySet = [&](
            const std::shared_ptr<IC4Ext::D3D12SyncedFrameSet>& owner) {
            if (!owner) {
                throw std::invalid_argument("applySet received null frame set");
            }
            const auto* left = FindCamera(*owner, 0);
            const auto* right = FindCamera(*owner, 1);
            if (!left || !right) {
                throw std::runtime_error(
                    "Synchronized frame set does not contain camera 0 and 1");
            }

            // No stereo disparity in this stage: one left-camera texture is copied
            // once and assigned identically to both Varjo eyes.
            const auto upload = displayRing.upload(left->frame);
            activeSlot = upload.slotIndex;
            plane.setTexture(VarjoXR::Eye::Left, upload.texture);
            plane.setTexture(VarjoXR::Eye::Right, upload.texture);

            if (!planeSizeInitialized) {
                const int width = std::max(0, left->frame.format.width);
                const int height = std::max(0, left->frame.format.height);
                const float imageAspect = height > 0
                    ? static_cast<float>(width) /
                          static_cast<float>(height)
                    : 1.0f;
                const float heightMeters = config.planeHeightMeters > 0.0f
                    ? config.planeHeightMeters
                    : config.planeWidthMeters /
                          std::max(0.001f, imageAspect);
                plane.setSize({config.planeWidthMeters, heightMeters});
                planeSizeInitialized = true;
            }

            displayedMetadata = BuildPairMetadata(
                *owner,
                left->frame,
                right->frame,
                config.timestampSource,
                clock);
        };

        applySet(currentSet);

        const auto experimentOutput =
            DualIC4Varjo::CreateExperimentOutputLayout(
                config.outputBaseDirectory,
                config.projectName,
                config.metadataCsv);
        config.metadataCsv = experimentOutput.renderedFramesCsv;
        if (!logger.start(config.metadataCsv)) {
            stopCaptureAndSync();
            throw std::runtime_error(logger.lastError());
        }

        std::cout
            << "Experiment output directory: "
            << experimentOutput.directory.string() << '\n'
            << "Rendered-frame CSV: "
            << config.metadataCsv.string() << '\n';

        const auto renderStart = std::chrono::steady_clock::now();
        std::atomic<bool> renderRunning{true};
        std::exception_ptr renderException;
        std::uint64_t renderRowIndex = 0;

        std::thread renderThread([&]() noexcept {
            if (!SetThreadPriority(
                    GetCurrentThread(),
                    THREAD_PRIORITY_HIGHEST)) {
                std::cerr
                    << "[THREAD] Failed to set render thread priority.\n";
            }
            std::cout << "[THREAD] Varjo render thread id="
                      << GetCurrentThreadId() << '\n';

            try {
                while (!gStopRequested.load(std::memory_order_acquire)) {
                    bool frameBegun = false;
                    try {
                        space.beginFrame();
                        frameBegun = true;

                        // This tryPopLatest() is the frame snapshot boundary. A
                        // synchronized frame arriving after this call remains in the
                        // queue and cannot affect the active Varjo frame.
                        bool newFrameFromQueue = false;
                        if (auto latest =
                                outputQueues[kDisplayPipelineIndex]
                                    ->tryPopLatest()) {
                            currentSet =
                                std::make_shared<IC4Ext::D3D12SyncedFrameSet>(
                                    std::move(*latest));
                            applySet(currentSet);
                            newFrameFromQueue = true;
                        }
                        const auto frameSnapshotTime =
                            std::chrono::steady_clock::now();

                        space.frameContext().timeSeconds =
                            std::chrono::duration<double>(
                                frameSnapshotTime - renderStart).count();
                        space.frameContext().frameNumber =
                            static_cast<std::int64_t>(renderRowIndex + 1u);

                        space.render();
                        frameBegun = false;
                        space.endFrame();
                        displayRing.markRendered(activeSlot);

                        DualIC4Varjo::RenderedFrameMetadataRow row;
                        row.renderRowIndex = renderRowIndex++;
                        row.renderSubmitUnixUs =
                            clock.unixMicroseconds(frameSnapshotTime);
                        row.renderSubmitLocalIso8601 =
                            clock.localIso8601(row.renderSubmitUnixUs);
                        row.newFrameFromQueue = newFrameFromQueue;
                        row.submitOk = true;
                        row.calibrationSource = "none";
                        row.calibrationProfile = "none";
                        row.calibrationRevision = 0;
                        row.planeMoved = false;
                        row.planeResized = false;
                        row.planePlacementMode =
                            PlacementModeName(config.placementMode);
                        const auto& position = plane.transform().position;
                        const auto size = plane.size();
                        row.planeX = position.x;
                        row.planeY = position.y;
                        row.planeZ = position.z;
                        row.planeWidth = size.x;
                        row.planeHeight = size.y;
                        row.syncGroupId = displayedMetadata.syncGroupId;
                        row.syncEmittedUnixUs =
                            clock.unixMicroseconds(
                                displayedMetadata.emittedTime);
                        row.syncTimestampSource =
                            DualIC4Varjo::TimestampSourceName(
                                config.timestampSource);
                        row.syncTimestampDiffNs =
                            displayedMetadata.syncTimestampDiffNs;
                        row.hostReceivedDiffUs =
                            displayedMetadata.hostReceivedDiffUs;
                        row.left = displayedMetadata.left;
                        row.right = displayedMetadata.right;
                        row.displaySlotIndex = activeSlot;
                        row.syncStats =
                            syncThreads[kDisplayPipelineIndex]->stats();
                        row.syncedQueueStats =
                            outputQueues[kDisplayPipelineIndex]->stats();
                        row.leftCameraStats = leftCamera.stats();
                        row.rightCameraStats = rightCamera.stats();

                        if (!logger.enqueue(std::move(row))) {
                            throw std::runtime_error(
                                "Metadata logger failed: " +
                                logger.lastError());
                        }
                    } catch (...) {
                        if (frameBegun) {
                            try {
                                space.endFrame();
                            } catch (...) {
                            }
                        }
                        throw;
                    }
                }
            } catch (...) {
                renderException = std::current_exception();
                gStopRequested.store(true, std::memory_order_release);
            }

            renderRunning.store(false, std::memory_order_release);
        });

        const auto mainWaitStart = std::chrono::steady_clock::now();
        while (renderRunning.load(std::memory_order_acquire) &&
               !gStopRequested.load(std::memory_order_acquire)) {
            if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
                gStopRequested.store(true, std::memory_order_release);
                break;
            }
            if (config.maxRuntimeSeconds > 0.0) {
                const double elapsed = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - mainWaitStart).count();
                if (elapsed >= config.maxRuntimeSeconds) {
                    gStopRequested.store(true, std::memory_order_release);
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        gStopRequested.store(true, std::memory_order_release);
        if (renderThread.joinable()) renderThread.join();

        logger.stop();
        displayRing.waitIdle();
        PrintSyncStats(syncThreads, outputQueues);
        stopCaptureAndSync();

        if (renderException) std::rethrow_exception(renderException);

        std::cout << "Stopped cleanly.\n";
        return 0;
    } catch (const std::exception& e) {
        gStopRequested.store(true, std::memory_order_release);
        logger.stop();
        std::cerr << "Fatal error: " << e.what() << '\n';
        return 1;
    }
}
