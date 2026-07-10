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
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

constexpr float kPlaneMoveStepMeters = 0.01f;

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

class ArrowKeyEdgeTracker {
public:
    bool leftPressed() { return pressed(VK_LEFT, leftDown_); }
    bool rightPressed() { return pressed(VK_RIGHT, rightDown_); }
    bool upPressed() { return pressed(VK_UP, upDown_); }
    bool downPressed() { return pressed(VK_DOWN, downDown_); }

private:
    static bool pressed(int virtualKey, bool& previousDown)
    {
        const bool currentDown = (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
        const bool pressedNow = currentDown && !previousDown;
        previousDown = currentDown;
        return pressedNow;
    }

    bool leftDown_ = false;
    bool rightDown_ = false;
    bool upDown_ = false;
    bool downDown_ = false;
};

const char* PlacementModeName(VarjoXR::PlacementMode mode) noexcept
{
    return mode == VarjoXR::PlacementMode::HeadRelative ? "head" : "world";
}

bool UpdatePlanePositionFromKeyboard(
    VarjoXR::XRPlane& plane,
    ArrowKeyEdgeTracker& keys)
{
    const bool shiftDown =
        (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0;

    const bool left = keys.leftPressed();
    const bool right = keys.rightPressed();
    const bool up = keys.upPressed();
    const bool down = keys.downPressed();

    auto& position = plane.transform().position;
    bool moved = false;

    if (left) {
        position.x -= kPlaneMoveStepMeters;
        moved = true;
    }
    if (right) {
        position.x += kPlaneMoveStepMeters;
        moved = true;
    }
    if (up) {
        if (shiftDown) {
            // The Plane starts at negative Z. More negative means farther away.
            position.z -= kPlaneMoveStepMeters;
        } else {
            position.y += kPlaneMoveStepMeters;
        }
        moved = true;
    }
    if (down) {
        if (shiftDown) {
            position.z += kPlaneMoveStepMeters;
        } else {
            position.y -= kPlaneMoveStepMeters;
        }
        moved = true;
    }

    if (moved) {
        std::cout << std::fixed << std::setprecision(3)
                  << "Plane position: x=" << position.x
                  << " m, y=" << position.y
                  << " m, z=" << position.z << " m\n";
    }
    return moved;
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
    row.hostReceivedUnixUs = clock.unixMicroseconds(frame.timing.hostReceivedTime);
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
        SyncTimestampNs(left, source), SyncTimestampNs(right, source));
    metadata.hostReceivedDiffUs = DualIC4Varjo::SignedDifference(
        HostTimestampNs(left), HostTimestampNs(right)) / 1000;
    metadata.left = BuildCameraMetadata(left, clock);
    metadata.right = BuildCameraMetadata(right, clock);
    return metadata;
}

DualIC4Varjo::RenderedFrameMetadataRow BuildRenderedFrameMetadata(
    std::uint64_t renderRowIndex,
    std::int64_t renderSubmitUnixUs,
    bool newFrameFromQueue,
    bool submitOk,
    bool planeMoved,
    const VarjoXR::XRPlane& plane,
    VarjoXR::PlacementMode placementMode,
    const DisplayedPairMetadata& displayedMetadata,
    const char* syncTimestampSource,
    std::size_t displaySlotIndex,
    const DualIC4Varjo::ClockMapper& clock,
    const IC4Ext::FrameSyncStats& syncStats,
    const ThreadKit::Queues::QueueStats& syncedQueueStats,
    const IC4Ext::CameraThreadStats& leftCameraStats,
    const IC4Ext::CameraThreadStats& rightCameraStats)
{
    DualIC4Varjo::RenderedFrameMetadataRow row;
    row.renderRowIndex = renderRowIndex;
    row.renderSubmitUnixUs = renderSubmitUnixUs;
    row.renderSubmitLocalIso8601 = clock.localIso8601(renderSubmitUnixUs);
    row.newFrameFromQueue = newFrameFromQueue;
    row.submitOk = submitOk;

    const auto& position = plane.transform().position;
    row.planeMoved = planeMoved;
    row.planePlacementMode = PlacementModeName(placementMode);
    row.planeX = position.x;
    row.planeY = position.y;
    row.planeZ = position.z;

    row.syncGroupId = displayedMetadata.syncGroupId;
    row.syncEmittedUnixUs = clock.unixMicroseconds(displayedMetadata.emittedTime);
    row.syncTimestampSource = syncTimestampSource ? syncTimestampSource : "unknown";
    row.syncTimestampDiffNs = displayedMetadata.syncTimestampDiffNs;
    row.hostReceivedDiffUs = displayedMetadata.hostReceivedDiffUs;

    row.left = displayedMetadata.left;
    row.right = displayedMetadata.right;
    row.displaySlotIndex = displaySlotIndex;
    row.syncStats = syncStats;
    row.syncedQueueStats = syncedQueueStats;
    row.leftCameraStats = leftCameraStats;
    row.rightCameraStats = rightCameraStats;
    return row;
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
        auto& d3dBackend =
            static_cast<VarjoXR::Backends::D3D12::D3D12Backend&>(space.backend());

        auto& plane = space.createPlane({config.planeWidthMeters, config.planeWidthMeters});
        plane.setPlacementMode(config.placementMode);
        plane.transform().position =
            {config.planeX, config.planeY, -config.planeDistanceMeters};

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
            std::this_thread::sleep_for(
                std::chrono::milliseconds(config.cameraStartDelayMs));
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
                      << "  left read=" << leftStats.readFrames
                      << " errors=" << leftStats.readErrors << '\n'
                      << "  right read=" << rightStats.readFrames
                      << " errors=" << rightStats.readErrors << '\n'
                      << "  sync input=" << syncStats.inputFrames
                      << " emitted=" << syncStats.emittedSets
                      << " dropped=" << syncStats.droppedFrames << '\n';
            return 1;
        }

        DualIC4Varjo::ClockMapper clock;
        DualIC4Varjo::StereoDisplayTextureRing displayRing(
            core, d3dBackend, config.displayRingSize);
        DisplayedPairMetadata displayedMetadata;
        std::size_t activeSlot = 0;

        auto applySet = [&](IC4Ext::D3D12SyncedFrameSet& set) {
            const auto* left = FindCamera(set, 0);
            const auto* right = FindCamera(set, 1);
            if (!left || !right) {
                throw std::runtime_error(
                    "Synchronized set does not contain both camera indices 0 and 1");
            }

            const auto upload = displayRing.upload(left->frame, right->frame);
            activeSlot = upload.slotIndex;
            plane.setTexture(VarjoXR::Eye::Left, upload.left);
            plane.setTexture(VarjoXR::Eye::Right, upload.right);

            const float imageAspect = left->frame.format.height > 0
                ? static_cast<float>(left->frame.format.width) /
                    static_cast<float>(left->frame.format.height)
                : 1.0f;
            const float heightMeters = config.planeHeightMeters > 0.0f
                ? config.planeHeightMeters
                : config.planeWidthMeters / std::max(0.001f, imageAspect);
            plane.setSize({config.planeWidthMeters, heightMeters});

            displayedMetadata = BuildPairMetadata(
                set, left->frame, right->frame, config.timestampSource, clock);
        };

        applySet(*initialSet);

        std::cout
            << "Rendering started.\n"
            << "  Arrow Left/Right : move Plane left/right by 0.01 m\n"
            << "  Arrow Up/Down    : move Plane up/down by 0.01 m\n"
            << "  Shift + Up       : move Plane farther by 0.01 m\n"
            << "  Shift + Down     : move Plane closer by 0.01 m\n"
            << "  Esc or Ctrl+C    : stop\n";

        const auto runStart = std::chrono::steady_clock::now();
        std::uint64_t renderRowIndex = 0;
        bool firstRender = true;
        ArrowKeyEdgeTracker planeKeys;

        while (!gStopRequested.load()) {
            if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
                gStopRequested.store(true);
                break;
            }
            if (config.maxRuntimeSeconds > 0.0 &&
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - runStart).count() >=
                    config.maxRuntimeSeconds) {
                break;
            }

            bool newFrameFromQueue = firstRender;
            firstRender = false;
            if (auto latest = outputQueue->tryPopLatest()) {
                applySet(*latest);
                newFrameFromQueue = true;
            }

            const bool planeMoved =
                UpdatePlanePositionFromKeyboard(plane, planeKeys);

            const auto submitSteady = std::chrono::steady_clock::now();
            const auto submitUnixUs = clock.unixMicroseconds(submitSteady);
            bool submitOk = false;
            try {
                space.update();
                submitOk = true;
                displayRing.markRendered(activeSlot);
            } catch (...) {
                auto failedRow = BuildRenderedFrameMetadata(
                    renderRowIndex++,
                    submitUnixUs,
                    newFrameFromQueue,
                    false,
                    planeMoved,
                    plane,
                    config.placementMode,
                    displayedMetadata,
                    DualIC4Varjo::TimestampSourceName(config.timestampSource),
                    activeSlot,
                    clock,
                    syncThread.stats(),
                    outputQueue->stats(),
                    leftCamera.stats(),
                    rightCamera.stats());
                if (!logger.enqueue(std::move(failedRow))) {
                    std::cerr << "Metadata logger failed while recording a render error: "
                              << logger.lastError() << '\n';
                }
                throw;
            }

            auto row = BuildRenderedFrameMetadata(
                renderRowIndex++,
                submitUnixUs,
                newFrameFromQueue,
                submitOk,
                planeMoved,
                plane,
                config.placementMode,
                displayedMetadata,
                DualIC4Varjo::TimestampSourceName(config.timestampSource),
                activeSlot,
                clock,
                syncThread.stats(),
                outputQueue->stats(),
                leftCamera.stats(),
                rightCamera.stats());
            if (!logger.enqueue(std::move(row))) {
                throw std::runtime_error(
                    "Metadata logger failed: " + logger.lastError());
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
            try {
                core->WaitIdle();
            } catch (...) {
            }
        }
        logger.stop();
        std::cerr << "Fatal error: " << e.what() << '\n';
        return 1;
    }
}
