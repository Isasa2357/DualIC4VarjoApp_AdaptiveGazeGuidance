#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "AppConfig.hpp"
#include "RenderedFrameMetadataLogger.hpp"
#include "StereoDisplayTextureRing.hpp"
#include "TimeUtil.hpp"

#include <IC4Ext/IC4Ext.hpp>
#include <VarjoToolkit/Core/VarjoSession.hpp>
#include <VarjoXR/VarjoXR.hpp>
#include <VarjoXR/Backends/D3D12/D3D12Backend.hpp>

#include <D3D12Helper/D3D12Core/D3D12Core.hpp>
#include <ThreadKit/Queues/QueueCommon.hpp>

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {

std::atomic<bool> gStopRequested{false};

BOOL WINAPI ConsoleControlHandler(DWORD controlType)
{
    switch (controlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        gStopRequested.store(true);
        return TRUE;
    default:
        return FALSE;
    }
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
    case IC4Ext::FrameSyncTimestampSource::HostReceived: return host;
    case IC4Ext::FrameSyncTimestampSource::Device: return device;
    case IC4Ext::FrameSyncTimestampSource::Auto:
    default: return host != 0 ? host : device;
    }
}

DualIC4Varjo::CameraFrameMetadataRow BuildCameraMetadata(
    const IC4Ext::D3D12CameraFrame& frame,
    const DualIC4Varjo::ClockMapper& clock)
{
    DualIC4Varjo::CameraFrameMetadataRow row;
    row.frameNumber = frame.timing.frameNumber;
    row.deviceTimestampNs = frame.timing.deviceTimestampNs;
    row.hostReceivedSteadyUs = clock.steadyMicroseconds(frame.timing.hostReceivedTime);
    row.hostReceivedUnixUs = clock.unixMicroseconds(frame.timing.hostReceivedTime);
    row.hostReceivedLocalIso8601 = clock.localIso8601(row.hostReceivedUnixUs);
    row.width = frame.format.width;
    row.height = frame.format.height;
    row.dxgiFormat = static_cast<int>(frame.dxgiFormat);
    row.chunk = frame.chunkMetadata;
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
        SyncTimestampNs(left, source), SyncTimestampNs(right, source));
    metadata.hostReceivedDiffUs = DualIC4Varjo::SignedDifference(
        HostTimestampNs(left), HostTimestampNs(right)) / 1000;
    metadata.left = BuildCameraMetadata(left, clock);
    metadata.right = BuildCameraMetadata(right, clock);
    return metadata;
}

void PrintError(const char* label, const IC4Ext::ErrorInfo& error)
{
    std::cerr << label << " failed: " << error.where << ": " << error.message << '\n';
}

} // namespace

int main(int argc, char** argv)
{
    using DualIC4Varjo::AppConfig;

    SetConsoleCtrlHandler(ConsoleControlHandler, TRUE);

    AppConfig config;
    std::string argumentError;
    if (!DualIC4Varjo::ParseArguments(argc, argv, config, argumentError)) {
        std::cerr << "Argument error: " << argumentError << "\n\n";
        DualIC4Varjo::PrintUsage(std::cerr);
        return 2;
    }
    if (config.showHelp) {
        DualIC4Varjo::PrintUsage(std::cout);
        return 0;
    }

    std::cout << "DualIC4VarjoApp\n"
              << "  IC4Ext: v1.0.1\n"
              << "  VarjoXR: v0.1.0\n"
              << "  Backend: D3D12\n"
              << "  Metadata: " << config.metadataCsv.string() << '\n';

    DualIC4Varjo::RenderedFrameMetadataLogger logger;
    if (!logger.start(config.metadataCsv)) {
        std::cerr << logger.lastError() << '\n';
        return 1;
    }

    std::shared_ptr<D3D12CoreLib::D3D12Core> core;
    std::shared_ptr<VarjoSession> session;

    auto inputOptions = ThreadKit::Queues::QueueOptions{};
    inputOptions.maxSize = config.inputQueueSize;
    inputOptions.overflowPolicy = ThreadKit::Queues::QueueOverflowPolicy::DropOldest;
    auto inputQueue = std::make_shared<IC4Ext::D3D12IndexedFrameQueue>(inputOptions);

    auto outputOptions = ThreadKit::Queues::QueueOptions{};
    outputOptions.maxSize = config.outputQueueSize;
    outputOptions.overflowPolicy = ThreadKit::Queues::QueueOverflowPolicy::DropOldest;
    auto outputQueue = std::make_shared<IC4Ext::D3D12SyncedFrameQueue>(outputOptions);

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
            throw std::runtime_error("Varjo session initialization failed: " + session->lastError());
        }

        auto backend = VarjoXR::Backends::D3D12::CreateBackend(core);
        VarjoXR::XRSpace space({session, std::move(backend)});
        auto& d3dBackend = static_cast<VarjoXR::Backends::D3D12::D3D12Backend&>(space.backend());

        auto& plane = space.createPlane({config.planeWidthMeters, config.planeWidthMeters});
        plane.setPlacementMode(config.placementMode);
        plane.transform().position = {config.planeX, config.planeY, -config.planeDistanceMeters};

        const auto captureBackend = IC4Ext::D3D12BackendContext::FromCore(core);

        IC4Ext::CameraThreadOptions cameraThreadOptions;
        cameraThreadOptions.readTimeoutMs = config.cameraReadTimeoutMs;
        cameraThreadOptions.copyPerOutputQueue = false;
        cameraThreadOptions.stopOnReadError = false;

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
        leftCamera.addOutputQueue(0, inputQueue);
        rightCamera.addOutputQueue(1, inputQueue);

        IC4Ext::FrameSyncOptions syncOptions;
        syncOptions.policy = IC4Ext::FrameSyncPolicy::TimestampNearest;
        syncOptions.cameraIndices = {0, 1};
        syncOptions.maxTimestampDiffNs = static_cast<std::uint64_t>(
            std::llround(config.syncToleranceMs * 1'000'000.0));
        syncOptions.maxBufferedFramesPerCamera = config.syncBufferedFramesPerCamera;
        syncOptions.timestampSource = config.timestampSource;
        IC4Ext::D3D12FrameSyncThread syncThread(inputQueue, outputQueue, syncOptions);

        if (!syncThread.start()) {
            PrintError("FrameSyncThread", syncThread.lastError());
            return 1;
        }
        if (!leftCamera.start()) {
            PrintError("Left camera", leftCamera.lastError());
            syncThread.stopAndJoin();
            return 1;
        }
        if (config.cameraStartDelayMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(config.cameraStartDelayMs));
        }
        if (!rightCamera.start()) {
            PrintError("Right camera", rightCamera.lastError());
            leftCamera.stopAndJoin();
            syncThread.stopAndJoin();
            return 1;
        }

        auto stopThreads = [&]() {
            leftCamera.stopAndJoin();
            rightCamera.stopAndJoin();
            syncThread.stopAndJoin();
            inputQueue->close();
            outputQueue->close();
        };

        auto initialSet = outputQueue->waitPopLatestFor(
            std::chrono::milliseconds(config.initialFrameTimeoutMs));
        if (!initialSet) {
            const auto leftStats = leftCamera.stats();
            const auto rightStats = rightCamera.stats();
            const auto syncStats = syncThread.stats();
            stopThreads();
            std::cerr << "Timed out waiting for the first synchronized frame set.\n"
                      << "  left read=" << leftStats.readFrames << " errors=" << leftStats.readErrors << '\n'
                      << "  right read=" << rightStats.readFrames << " errors=" << rightStats.readErrors << '\n'
                      << "  sync input=" << syncStats.inputFrames << " emitted=" << syncStats.emittedSets
                      << " dropped=" << syncStats.droppedFrames << '\n';
            return 1;
        }

        DualIC4Varjo::ClockMapper clock;
        DualIC4Varjo::StereoDisplayTextureRing displayRing(core, d3dBackend, config.displayRingSize);
        DisplayedPairMetadata displayedMetadata;
        std::size_t activeSlot = 0;

        auto applySet = [&](IC4Ext::D3D12SyncedFrameSet& set) {
            const auto* left = FindCamera(set, 0);
            const auto* right = FindCamera(set, 1);
            if (!left || !right) {
                throw std::runtime_error("Synchronized set does not contain both camera indices 0 and 1");
            }
            const auto upload = displayRing.upload(left->frame, right->frame);
            activeSlot = upload.slotIndex;
            plane.setTexture(VarjoXR::Eye::Left, upload.left);
            plane.setTexture(VarjoXR::Eye::Right, upload.right);

            const float imageAspect = left->frame.format.height > 0
                ? static_cast<float>(left->frame.format.width) / static_cast<float>(left->frame.format.height)
                : 1.0f;
            const float heightMeters = config.planeHeightMeters > 0.0f
                ? config.planeHeightMeters
                : config.planeWidthMeters / std::max(0.001f, imageAspect);
            plane.setSize({config.planeWidthMeters, heightMeters});

            displayedMetadata = BuildPairMetadata(
                set, left->frame, right->frame, config.timestampSource, clock);
        };

        applySet(*initialSet);

        std::cout << "Rendering started. Press Esc or Ctrl+C to stop.\n";
        const auto runStart = std::chrono::steady_clock::now();
        std::uint64_t renderRowIndex = 0;
        bool firstRender = true;

        while (!gStopRequested.load()) {
            if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
                gStopRequested.store(true);
                break;
            }
            if (config.maxRuntimeSeconds > 0.0 &&
                std::chrono::duration<double>(std::chrono::steady_clock::now() - runStart).count() >= config.maxRuntimeSeconds) {
                break;
            }

            bool newFrameFromQueue = firstRender;
            firstRender = false;
            if (auto latest = outputQueue->tryPopLatest()) {
                applySet(*latest);
                newFrameFromQueue = true;
            }

            const auto submitSteady = std::chrono::steady_clock::now();
            const auto submitUnixUs = clock.unixMicroseconds(submitSteady);
            bool submitOk = false;
            try {
                space.update();
                submitOk = true;
                displayRing.markRendered(activeSlot);
            } catch (...) {
                DualIC4Varjo::RenderedFrameMetadataRow failedRow;
                failedRow.renderRowIndex = renderRowIndex++;
                failedRow.renderSubmitUnixUs = submitUnixUs;
                failedRow.renderSubmitLocalIso8601 = clock.localIso8601(submitUnixUs);
                failedRow.renderSubmitSteadyUs = clock.steadyMicroseconds(submitSteady);
                failedRow.newFrameFromQueue = newFrameFromQueue;
                failedRow.submitOk = false;
                failedRow.syncGroupId = displayedMetadata.syncGroupId;
                failedRow.syncEmittedSteadyUs = clock.steadyMicroseconds(displayedMetadata.emittedTime);
                failedRow.syncEmittedUnixUs = clock.unixMicroseconds(displayedMetadata.emittedTime);
                failedRow.syncTimestampSource = DualIC4Varjo::TimestampSourceName(config.timestampSource);
                failedRow.syncTimestampDiffNs = displayedMetadata.syncTimestampDiffNs;
                failedRow.syncTimestampDiffMs = displayedMetadata.syncTimestampDiffNs / 1'000'000.0;
                failedRow.hostReceivedDiffUs = displayedMetadata.hostReceivedDiffUs;
                failedRow.hostReceivedDiffMs = displayedMetadata.hostReceivedDiffUs / 1000.0;
                failedRow.left = displayedMetadata.left;
                failedRow.right = displayedMetadata.right;
                failedRow.displaySlotIndex = activeSlot;
                failedRow.syncStats = syncThread.stats();
                failedRow.syncedQueueStats = outputQueue->stats();
                failedRow.leftCameraStats = leftCamera.stats();
                failedRow.rightCameraStats = rightCamera.stats();
                logger.enqueue(std::move(failedRow));
                throw;
            }

            DualIC4Varjo::RenderedFrameMetadataRow row;
            row.renderRowIndex = renderRowIndex++;
            row.renderSubmitUnixUs = submitUnixUs;
            row.renderSubmitLocalIso8601 = clock.localIso8601(submitUnixUs);
            row.renderSubmitSteadyUs = clock.steadyMicroseconds(submitSteady);
            row.newFrameFromQueue = newFrameFromQueue;
            row.submitOk = submitOk;
            row.syncGroupId = displayedMetadata.syncGroupId;
            row.syncEmittedSteadyUs = clock.steadyMicroseconds(displayedMetadata.emittedTime);
            row.syncEmittedUnixUs = clock.unixMicroseconds(displayedMetadata.emittedTime);
            row.syncTimestampSource = DualIC4Varjo::TimestampSourceName(config.timestampSource);
            row.syncTimestampDiffNs = displayedMetadata.syncTimestampDiffNs;
            row.syncTimestampDiffMs = displayedMetadata.syncTimestampDiffNs / 1'000'000.0;
            row.hostReceivedDiffUs = displayedMetadata.hostReceivedDiffUs;
            row.hostReceivedDiffMs = displayedMetadata.hostReceivedDiffUs / 1000.0;
            row.left = displayedMetadata.left;
            row.right = displayedMetadata.right;
            row.displaySlotIndex = activeSlot;
            row.syncStats = syncThread.stats();
            row.syncedQueueStats = outputQueue->stats();
            row.leftCameraStats = leftCamera.stats();
            row.rightCameraStats = rightCamera.stats();
            if (!logger.enqueue(std::move(row))) {
                throw std::runtime_error("Metadata logger failed: " + logger.lastError());
            }
        }

        stopThreads();
        displayRing.waitIdle();
        logger.stop();

        const auto leftStats = leftCamera.stats();
        const auto rightStats = rightCamera.stats();
        const auto syncStats = syncThread.stats();
        std::cout << "Stopped.\n"
                  << "  left frames: " << leftStats.readFrames << '\n'
                  << "  right frames: " << rightStats.readFrames << '\n'
                  << "  synced sets: " << syncStats.emittedSets << '\n'
                  << "  sync drops: " << syncStats.droppedFrames << '\n'
                  << "  metadata: " << config.metadataCsv.string() << '\n';
        return 0;
    } catch (const std::exception& e) {
        gStopRequested.store(true);
        if (core) {
            try { core->WaitIdle(); } catch (...) {}
        }
        logger.stop();
        std::cerr << "Fatal error: " << e.what() << '\n';
        return 1;
    }
}
