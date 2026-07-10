#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "AppConfig.hpp"
#include "ExperimentOutput.hpp"
#include "RenderedFrameMetadataLogger.hpp"
#include "StereoCalibrationSupport.hpp"
#include "StereoDisplayTextureRing.hpp"
#include "TimeUtil.hpp"
#include "VarjoServiceLogging.hpp"

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
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {

constexpr float kPlaneMoveStepMeters = 0.01f;
constexpr float kPlaneResizeStepMeters = 0.01f;
constexpr float kMinimumPlaneWidthMeters = 0.05f;

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

struct PlaneKeyboardUpdate {
    bool moved = false;
    bool resized = false;
};

struct ActiveCalibrationInfo {
    std::string source = "none";
    std::string profile = "none";
    std::uint64_t revision = 0;
};

bool IsShiftDown()
{
    return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 ||
           (GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0 ||
           (GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0;
}

const char* PlacementModeName(VarjoXR::PlacementMode mode) noexcept
{
    return mode == VarjoXR::PlacementMode::HeadRelative ? "head" : "world";
}

PlaneKeyboardUpdate UpdatePlaneFromKeyboard(
    VarjoXR::XRPlane& plane,
    ArrowKeyEdgeTracker& keys)
{
    const bool shiftDown = IsShiftDown();
    const bool left = keys.leftPressed();
    const bool right = keys.rightPressed();
    const bool up = keys.upPressed();
    const bool down = keys.downPressed();

    PlaneKeyboardUpdate update;
    auto& position = plane.transform().position;

    if (shiftDown) {
        float widthDelta = 0.0f;
        if (left) widthDelta -= kPlaneResizeStepMeters;
        if (right) widthDelta += kPlaneResizeStepMeters;
        if (widthDelta != 0.0f) {
            const auto currentSize = plane.size();
            if (currentSize.x > 0.0f && currentSize.y > 0.0f) {
                const float newWidth = std::max(
                    kMinimumPlaneWidthMeters,
                    currentSize.x + widthDelta);
                if (std::abs(newWidth - currentSize.x) > 0.000001f) {
                    const float heightPerWidth = currentSize.y / currentSize.x;
                    plane.setSize({newWidth, newWidth * heightPerWidth});
                    update.resized = true;
                }
            }
        }
    } else {
        if (left) {
            position.x -= kPlaneMoveStepMeters;
            update.moved = true;
        }
        if (right) {
            position.x += kPlaneMoveStepMeters;
            update.moved = true;
        }
    }

    if (up) {
        if (shiftDown) position.z -= kPlaneMoveStepMeters;
        else position.y += kPlaneMoveStepMeters;
        update.moved = true;
    }
    if (down) {
        if (shiftDown) position.z += kPlaneMoveStepMeters;
        else position.y -= kPlaneMoveStepMeters;
        update.moved = true;
    }

    if (update.moved || update.resized) {
        const auto size = plane.size();
        std::cout << std::fixed << std::setprecision(3)
                  << "Plane: x=" << position.x
                  << " m, y=" << position.y
                  << " m, z=" << position.z
                  << " m, width=" << size.x
                  << " m, height=" << size.y << " m\n";
    }
    return update;
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
    const ActiveCalibrationInfo& calibration,
    const PlaneKeyboardUpdate& planeUpdate,
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
    row.calibrationSource = calibration.source;
    row.calibrationProfile = calibration.profile;
    row.calibrationRevision = calibration.revision;

    const auto& position = plane.transform().position;
    const auto size = plane.size();
    row.planeMoved = planeUpdate.moved;
    row.planeResized = planeUpdate.resized;
    row.planePlacementMode = PlacementModeName(placementMode);
    row.planeX = position.x;
    row.planeY = position.y;
    row.planeZ = position.z;
    row.planeWidth = size.x;
    row.planeHeight = size.y;

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
    namespace Calibration = Vdca::StereoCalibration;

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

    std::optional<Calibration::CalibrationDocument> loadedCalibration;
    bool runLiveCalibration = false;
    std::optional<std::filesystem::path> calibrationOutputPath;
    std::string selectedCalibrationProfile = config.calibration.profile;
    if (config.calibration.enabled) {
        calibrationOutputPath = config.calibration.jsonPath;
        if (calibrationOutputPath && std::filesystem::exists(*calibrationOutputPath)) {
            loadedCalibration = Calibration::CalibrationDocument::loadJson(
                std::filesystem::absolute(*calibrationOutputPath));
            if (!config.calibration.profileExplicit) {
                selectedCalibrationProfile = loadedCalibration->defaultProfile;
            }
            std::cout << "Loaded calibration JSON: "
                      << std::filesystem::absolute(*calibrationOutputPath).string() << '\n';
        } else {
            runLiveCalibration = true;
        }
    }

    std::cout << "DualIC4VarjoApp\n"
              << "  IC4Ext: v1.0.1\n"
              << "  VarjoXR: v0.1.0\n"
              << "  VarjoToolkit: v0.4.0\n"
              << "  D3D12Helper: v1.13.0\n"
              << "  Backend: D3D12\n"
              << "  Calibration: "
              << (config.calibration.enabled
                      ? (runLiveCalibration ? "live" : "JSON")
                      : "disabled")
              << '\n';

    DualIC4Varjo::RenderedFrameMetadataLogger logger;
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
            throw std::runtime_error(
                "Varjo session initialization failed: " + session->lastError());
        }

        auto backend = VarjoXR::Backends::D3D12::CreateBackend(core);
        VarjoXR::XRSpace space({session, std::move(backend)});
        auto& d3dBackend =
            static_cast<VarjoXR::Backends::D3D12::D3D12Backend&>(space.backend());

        auto& plane = space.createPlane(
            {config.planeWidthMeters, config.planeWidthMeters});
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
        syncOptions.maxBufferedFramesPerCamera =
            config.syncBufferedFramesPerCamera;
        syncOptions.timestampSource = config.timestampSource;
        IC4Ext::D3D12FrameSyncThread syncThread(
            inputQueue,
            outputQueue,
            syncOptions);

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
            core,
            d3dBackend,
            config.displayRingSize);
        DisplayedPairMetadata displayedMetadata;
        std::size_t activeSlot = 0;
        bool planeSizeInitialized = false;
        std::uint32_t inputWidth = 0;
        std::uint32_t inputHeight = 0;

        auto applySet = [&](const std::shared_ptr<IC4Ext::D3D12SyncedFrameSet>& owner) {
            if (!owner) {
                throw std::invalid_argument("applySet received null frame set");
            }
            const auto* left = FindCamera(*owner, 0);
            const auto* right = FindCamera(*owner, 1);
            if (!left || !right) {
                throw std::runtime_error(
                    "Synchronized set does not contain both camera indices 0 and 1");
            }

            const auto upload = displayRing.upload(left->frame, right->frame);
            activeSlot = upload.slotIndex;
            plane.setTexture(VarjoXR::Eye::Left, upload.left);
            plane.setTexture(VarjoXR::Eye::Right, upload.right);

            inputWidth = static_cast<std::uint32_t>(
                std::max(0, left->frame.format.width));
            inputHeight = static_cast<std::uint32_t>(
                std::max(0, left->frame.format.height));
            if (!planeSizeInitialized) {
                const float imageAspect = inputHeight > 0
                    ? static_cast<float>(inputWidth) /
                        static_cast<float>(inputHeight)
                    : 1.0f;
                const float heightMeters = config.planeHeightMeters > 0.0f
                    ? config.planeHeightMeters
                    : config.planeWidthMeters / std::max(0.001f, imageAspect);
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

        auto currentSet = std::make_shared<IC4Ext::D3D12SyncedFrameSet>(
            std::move(*initialSet));
        applySet(currentSet);
        if (inputWidth == 0 || inputHeight == 0) {
            throw std::runtime_error("IC4 produced an invalid frame size");
        }

        ActiveCalibrationInfo activeCalibration;

        if (loadedCalibration) {
            DualIC4Varjo::ValidateCalibrationInputGeometry(
                *loadedCalibration,
                inputWidth,
                inputHeight);
            if (!loadedCalibration->hasProfile(selectedCalibrationProfile)) {
                throw std::invalid_argument(
                    "calibration JSON does not contain profile: " +
                    selectedCalibrationProfile);
            }
            DualIC4Varjo::ApplyCalibrationToPlane(
                plane,
                *loadedCalibration,
                selectedCalibrationProfile);
            DualIC4Varjo::UpdatePlaneAspectFromCalibration(
                plane,
                *loadedCalibration);
            activeCalibration.source = "json";
            activeCalibration.profile = selectedCalibrationProfile;
            activeCalibration.revision = 1;
        } else if (runLiveCalibration) {
            DualIC4Varjo::LiveStereoCalibrationOptions liveOptions;
            liveOptions.boardColumns = config.calibration.boardColumns;
            liveOptions.boardRows = config.calibration.boardRows;
            liveOptions.activeProfile = config.calibration.profile;
            liveOptions.maxObservationCount = config.calibration.maxObservations;
            liveOptions.minObservationCountForUpdate =
                config.calibration.minObservations;
            liveOptions.minMeanCornerMotionPx =
                config.calibration.minCornerMotionPx;
            liveOptions.fundamentalRansacThresholdPx =
                config.calibration.ransacThresholdPx;
            liveOptions.useFindChessboardCornersSB =
                config.calibration.useChessboardSb;

            DualIC4Varjo::LiveStereoCalibration liveCalibration(
                core,
                inputWidth,
                inputHeight,
                liveOptions);
            liveCalibration.start();
            liveCalibration.submitLatest(currentSet);

            std::uint64_t appliedRevision = 0;
            std::cout
                << "Live stereo calibration started.\n"
                << "Show a " << config.calibration.boardColumns << " x "
                << config.calibration.boardRows
                << " inner-corner checkerboard at varied positions and angles.\n"
                << "Press q after a valid live estimate is available.\n"
                << "Press Esc or Ctrl+C to abort the application.\n";

            std::shared_ptr<const Calibration::CalibrationSnapshot> finalSnapshot;
            std::uint64_t calibrationRenderFrames = 0;
            while (!gStopRequested.load()) {
                if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
                    gStopRequested.store(true);
                    break;
                }

                if (auto latest = outputQueue->tryPopLatest()) {
                    currentSet =
                        std::make_shared<IC4Ext::D3D12SyncedFrameSet>(
                            std::move(*latest));
                    applySet(currentSet);
                    liveCalibration.submitLatest(currentSet);
                }

                if (auto snapshot = liveCalibration.latestSnapshot();
                    snapshot && snapshot->revision != appliedRevision) {
                    DualIC4Varjo::ValidateCalibrationInputGeometry(
                        *snapshot->document,
                        inputWidth,
                        inputHeight);
                    DualIC4Varjo::ApplyCalibrationToPlane(
                        plane,
                        *snapshot->document,
                        snapshot->activeProfile);
                    DualIC4Varjo::UpdatePlaneAspectFromCalibration(
                        plane,
                        *snapshot->document);
                    appliedRevision = snapshot->revision;

                    const auto& quality =
                        snapshot->document->profile(snapshot->activeProfile).quality;
                    std::cout << "[CALIB] revision=" << snapshot->revision
                              << " profile=" << snapshot->activeProfile
                              << " observations=" << quality.usedPairs;
                    if (quality.medianAbsVerticalErrorPx) {
                        std::cout << " medianVerticalError="
                                  << std::fixed << std::setprecision(4)
                                  << *quality.medianAbsVerticalErrorPx << " px";
                    }
                    std::cout << '\n';
                }

                if ((GetAsyncKeyState('Q') & 0x1) != 0) {
                    auto snapshot = liveCalibration.latestSnapshot();
                    if (snapshot && DualIC4Varjo::IsLiveCalibrationReady(
                            *snapshot,
                            config.calibration.minObservations)) {
                        finalSnapshot = std::move(snapshot);
                        break;
                    }
                    const auto stats = liveCalibration.stats();
                    std::cout << "Calibration is not ready: accepted="
                              << stats.acceptedObservations
                              << ", required=" << config.calibration.minObservations
                              << ". Continue moving the checkerboard.\n";
                }

                space.frameContext().frameNumber = ++calibrationRenderFrames;
                space.update();
                displayRing.markRendered(activeSlot);

                if ((calibrationRenderFrames % 120u) == 0u) {
                    liveCalibration.rethrowWorkerExceptionIfAny();
                }
            }

            liveCalibration.stop();
            if (gStopRequested.load()) {
                stopThreads();
                displayRing.waitIdle();
                return 0;
            }
            if (!finalSnapshot || !finalSnapshot->document) {
                throw std::runtime_error(
                    "live calibration ended without a valid snapshot");
            }

            DualIC4Varjo::ApplyCalibrationToPlane(
                plane,
                *finalSnapshot->document,
                finalSnapshot->activeProfile);
            DualIC4Varjo::UpdatePlaneAspectFromCalibration(
                plane,
                *finalSnapshot->document);

            if (calibrationOutputPath) {
                const auto absolutePath =
                    std::filesystem::absolute(*calibrationOutputPath);
                finalSnapshot->document->saveJsonAtomically(absolutePath);
                std::cout << "Saved calibration JSON: "
                          << absolutePath.string() << '\n';
            } else {
                std::cout
                    << "Calibration was not saved because --calib - was used.\n";
            }

            activeCalibration.source = "live";
            activeCalibration.profile = finalSnapshot->activeProfile;
            activeCalibration.revision = finalSnapshot->revision;
        }

        const auto experimentOutput =
            DualIC4Varjo::CreateExperimentOutputLayout(
                config.outputBaseDirectory,
                config.projectName,
                config.metadataCsv);
        config.metadataCsv = experimentOutput.renderedFramesCsv;

        std::cout << "Experiment output directory: "
                  << experimentOutput.directory.string() << '\n';
        if (experimentOutput.resolvedProjectName !=
            experimentOutput.requestedProjectName) {
            std::cout << "Project name collision resolved as: "
                      << experimentOutput.resolvedProjectName << '\n';
        }

        DualIC4Varjo::VarjoServiceLogging serviceLogging(
            session->shared(),
            experimentOutput.directory);
        std::string serviceError;
        if (!serviceLogging.start(serviceError)) {
            throw std::runtime_error(serviceError);
        }
        if (!logger.start(config.metadataCsv)) {
            throw std::runtime_error(logger.lastError());
        }

        std::cout
            << "Experiment rendering and Varjo service logging started.\n"
            << "  Arrow Left/Right : move Plane left/right by 0.01 m\n"
            << "  Arrow Up/Down    : move Plane up/down by 0.01 m\n"
            << "  Shift + Up       : move Plane farther by 0.01 m\n"
            << "  Shift + Down     : move Plane closer by 0.01 m\n"
            << "  Shift + Left     : reduce Plane width by 0.01 m\n"
            << "  Shift + Right    : increase Plane width by 0.01 m\n"
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
                currentSet = std::make_shared<IC4Ext::D3D12SyncedFrameSet>(
                    std::move(*latest));
                applySet(currentSet);
                newFrameFromQueue = true;
            }

            serviceLogging.pump();

            const PlaneKeyboardUpdate planeUpdate =
                UpdatePlaneFromKeyboard(plane, planeKeys);
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
                    activeCalibration,
                    planeUpdate,
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
                    std::cerr
                        << "Metadata logger failed while recording a render error: "
                        << logger.lastError() << '\n';
                }
                throw;
            }

            auto row = BuildRenderedFrameMetadata(
                renderRowIndex++,
                submitUnixUs,
                newFrameFromQueue,
                submitOk,
                activeCalibration,
                planeUpdate,
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

        serviceLogging.stop();
        logger.stop();
        stopThreads();
        displayRing.waitIdle();

        const auto serviceStats = serviceLogging.summary();
        const auto leftStats = leftCamera.stats();
        const auto rightStats = rightCamera.stats();
        const auto syncStats = syncThread.stats();
        std::cout << "Stopped.\n"
                  << "  output directory: "
                  << experimentOutput.directory.string() << '\n'
                  << "  rendered frame CSV: "
                  << config.metadataCsv.string() << '\n'
                  << "  left camera frames: " << leftStats.readFrames << '\n'
                  << "  right camera frames: " << rightStats.readFrames << '\n'
                  << "  synchronized sets: " << syncStats.emittedSets << '\n'
                  << "  sync drops: " << syncStats.droppedFrames << '\n'
                  << "  gaze samples: " << serviceStats.gazeReceived
                  << " dropped from app queue=" << serviceStats.gazeDropped << '\n'
                  << "  IMU rows: " << serviceStats.imuRows << '\n'
                  << "  VST frames L/R: " << serviceStats.vstLeftFrames
                  << " / " << serviceStats.vstRightFrames
                  << " write failures=" << serviceStats.vstWriteFailures << '\n';
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
