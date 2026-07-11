#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "AppConfig.hpp"
#include "ExperimentOutput.hpp"
#include "ImGuiStereoPreview.hpp"
#include "RenderedFrameMetadataLogger.hpp"
#include "StereoCalibrationSupport.hpp"
#include "StereoDisplayTextureRing.hpp"
#include "TimeUtil.hpp"

#include <IC4Ext/IC4Ext.hpp>
#include <VarjoToolkit/Core/VarjoSession.hpp>
#include <VarjoXR/Backends/D3D12/D3D12Backend.hpp>
#include <VarjoXR/VarjoXR.hpp>

#include <D3D12Helper/D3D12Core/D3D12Core.hpp>
#include <ThreadKit/Queues/QueueCommon.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
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
#include <vector>

namespace {

constexpr std::size_t kSyncPipelineCount = 3;
constexpr std::size_t kVarjoPipelineIndex = 0;
constexpr std::size_t kPcPreviewPipelineIndex = 1;
constexpr std::size_t kDiscardPipelineIndex = 2;

// IC4Ext copies to all outputs except the last one. Keep Varjo last so the
// latency-sensitive display path receives the original camera resources.
constexpr std::array<std::size_t, kSyncPipelineCount> kCameraOutputOrder{
    kPcPreviewPipelineIndex,
    kDiscardPipelineIndex,
    kVarjoPipelineIndex,
};

constexpr float kPlaneMoveStepMeters = 0.01f;
constexpr float kPlaneResizeStepMeters = 0.01f;
constexpr float kMinimumPlaneWidthMeters = 0.05f;
constexpr float kMinimumPlaneDistanceMeters = 0.05f;

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

class ArrowKeyEdgeTracker {
public:
    bool leftPressed() { return pressed(VK_LEFT, leftDown_); }
    bool rightPressed() { return pressed(VK_RIGHT, rightDown_); }
    bool upPressed() { return pressed(VK_UP, upDown_); }
    bool downPressed() { return pressed(VK_DOWN, downDown_); }

private:
    static bool pressed(int virtualKey, bool& previousDown)
    {
        const bool currentDown =
            (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
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

struct DisplayedPairMetadata {
    std::uint64_t syncGroupId = 0;
    std::chrono::steady_clock::time_point emittedTime{};
    std::int64_t syncTimestampDiffNs = 0;
    std::int64_t hostReceivedDiffUs = 0;
    DualIC4Varjo::CameraFrameMetadataRow left;
    DualIC4Varjo::CameraFrameMetadataRow right;
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
                    const float heightPerWidth =
                        currentSize.y / currentSize.x;
                    plane.setSize({newWidth, newWidth * heightPerWidth});
                    update.resized = true;
                }
            }
        }

        if (up) {
            // Plane is in front of the HMD at negative Z. More negative is farther.
            position.z -= kPlaneMoveStepMeters;
            update.moved = true;
        }
        if (down) {
            const float closestZ = -kMinimumPlaneDistanceMeters;
            const float newZ = std::min(
                closestZ,
                position.z + kPlaneMoveStepMeters);
            if (std::abs(newZ - position.z) > 0.000001f) {
                position.z = newZ;
                update.moved = true;
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
        if (up) {
            position.y += kPlaneMoveStepMeters;
            update.moved = true;
        }
        if (down) {
            position.y -= kPlaneMoveStepMeters;
            update.moved = true;
        }
    }

    if (update.moved || update.resized) {
        const auto size = plane.size();
        const float distanceMeters = std::max(0.0f, -position.z);
        std::cout << std::fixed << std::setprecision(3)
                  << "[PLANE] x=" << position.x
                  << " m, y=" << position.y
                  << " m, z=" << position.z
                  << " m, distance=" << distanceMeters
                  << " m, width=" << size.x
                  << " m, height=" << size.y << " m\n";
    }

    return update;
}

void PrintError(const char* label, const IC4Ext::ErrorInfo& error)
{
    std::cerr << label << " failed: "
              << error.where << ": " << error.message << '\n';
}

void PrintCalibrationUsage(std::ostream& out)
{
    out << "\nStatic stereo calibration:\n"
        << "  --calib JSON_PATH               Apply JSON default_profile to the Varjo Plane\n";
}

bool ExtractCalibrationArgument(
    int argc,
    char** argv,
    std::vector<char*>& filteredArguments,
    std::optional<std::filesystem::path>& calibrationPath,
    std::string& error)
{
    filteredArguments.clear();
    filteredArguments.reserve(static_cast<std::size_t>(argc));
    if (argc > 0) filteredArguments.push_back(argv[0]);

    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        if (option != "--calib") {
            filteredArguments.push_back(argv[index]);
            continue;
        }

        if (index + 1 >= argc) {
            error = "Missing value for --calib";
            return false;
        }
        calibrationPath = std::filesystem::path(argv[++index]);
        if (calibrationPath->empty()) {
            error = "--calib requires a non-empty JSON path";
            return false;
        }
    }
    return true;
}

const char* PlacementModeName(VarjoXR::PlacementMode mode) noexcept
{
    return mode == VarjoXR::PlacementMode::HeadRelative ? "head" : "world";
}

const char* PipelineName(std::size_t index) noexcept
{
    switch (index) {
    case kVarjoPipelineIndex: return "varjo";
    case kPcPreviewPipelineIndex: return "imgui-preview";
    case kDiscardPipelineIndex: return "discard";
    default: return "unknown";
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
    row.hostReceivedUnixUs =
        clock.unixMicroseconds(frame.timing.hostReceivedTime);
    row.width = frame.format.width;
    row.height = frame.format.height;
    return row;
}

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
    for (std::size_t index = 0; index < kSyncPipelineCount; ++index) {
        const auto syncStats = syncThreads[index]->stats();
        const auto queueStats = outputQueues[index]->stats();
        std::cout << "[SYNC " << index << ' ' << PipelineName(index) << "]"
                  << " input=" << syncStats.inputFrames
                  << " emitted=" << syncStats.emittedSets
                  << " dropped=" << syncStats.droppedFrames
                  << " pushFailures=" << syncStats.pushFailures
                  << " outputDroppedOldest=" << queueStats.droppedOldest
                  << " outputDroppedByPopLatest="
                  << queueStats.droppedByPopLatest << '\n';
    }
}

} // namespace

int main(int argc, char** argv)
{
    using DualIC4Varjo::AppConfig;

    gStopRequested.store(false, std::memory_order_release);
    SetConsoleCtrlHandler(ConsoleControlHandler, TRUE);

    AppConfig config;
    std::vector<char*> filteredArguments;
    std::optional<std::filesystem::path> calibrationPath;
    std::string argumentError;

    if (!ExtractCalibrationArgument(
            argc,
            argv,
            filteredArguments,
            calibrationPath,
            argumentError)) {
        std::cerr << "Argument error: " << argumentError << "\n\n";
        DualIC4Varjo::PrintUsage(std::cerr);
        PrintCalibrationUsage(std::cerr);
        return 2;
    }

    if (!DualIC4Varjo::ParseArguments(
            static_cast<int>(filteredArguments.size()),
            filteredArguments.data(),
            config,
            argumentError)) {
        std::cerr << "Argument error: " << argumentError << "\n\n";
        DualIC4Varjo::PrintUsage(std::cerr);
        PrintCalibrationUsage(std::cerr);
        return 2;
    }
    config.calibrationJson = calibrationPath;

    if (config.showHelp) {
        DualIC4Varjo::PrintUsage(std::cout);
        PrintCalibrationUsage(std::cout);
        return 0;
    }

    std::optional<DualIC4Varjo::StereoCalibrationDocument> calibration;
    ActiveCalibrationInfo activeCalibration;
    if (config.calibrationJson) {
        const auto absolutePath =
            std::filesystem::absolute(*config.calibrationJson);
        calibration = DualIC4Varjo::LoadStereoCalibrationJson(absolutePath);
        activeCalibration.source = "json";
        activeCalibration.profile = calibration->defaultProfile;
        activeCalibration.revision = 1;
        std::cout << "Loaded stereo calibration JSON: "
                  << absolutePath.string() << '\n'
                  << "  profile: " << activeCalibration.profile << '\n'
                  << "  source: "
                  << calibration->sourceSize.width << 'x'
                  << calibration->sourceSize.height << '\n'
                  << "  output: "
                  << calibration->rectifiedOutputSize.width << 'x'
                  << calibration->rectifiedOutputSize.height << '\n';
    }

    std::cout << "DualIC4VarjoApp Plane keyboard-control stage\n"
              << "  Capture FPS            : " << config.fps << '\n'
              << "  FrameSyncThread count  : " << kSyncPipelineCount << '\n'
              << "  Pipeline 0             : Varjo stereo display\n"
              << "  Pipeline 1             : ImGui raw stereo preview\n"
              << "  Pipeline 2             : DropOldest discard\n"
              << "  Calibration            : "
              << (calibration ? activeCalibration.profile : "disabled") << '\n'
              << "  PC preview             : "
              << (config.pcPreviewEnabled ? "enabled" : "disabled") << '\n'
              << "  Varjo render thread    : dedicated std::thread\n"
              << "  Frame selection        : latest after beginFrame\n"
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
                "Varjo session initialization failed: " + session->lastError());
        }

        auto backend = VarjoXR::Backends::D3D12::CreateBackend(core);
        VarjoXR::XRSpace space({session, std::move(backend)});
        auto& d3dBackend =
            static_cast<VarjoXR::Backends::D3D12::D3D12Backend&>(
                space.backend());

        auto& plane = space.createPlane(
            {config.planeWidthMeters, config.planeWidthMeters});
        plane.setPlacementMode(config.placementMode);
        plane.transform().position = {
            config.planeX,
            config.planeY,
            -config.planeDistanceMeters,
        };

        IC4Ext::FrameSyncOptions syncOptions;
        syncOptions.policy = IC4Ext::FrameSyncPolicy::TimestampNearest;
        syncOptions.cameraIndices = {0, 1};
        syncOptions.maxTimestampDiffNs = static_cast<std::uint64_t>(
            std::llround(config.syncToleranceMs * 1'000'000.0));
        syncOptions.maxBufferedFramesPerCamera =
            config.syncBufferedFramesPerCamera;
        syncOptions.timestampSource = config.timestampSource;

        for (std::size_t index = 0;
             index < kSyncPipelineCount;
             ++index) {
            inputQueues[index] =
                std::make_shared<IC4Ext::D3D12IndexedFrameQueue>(
                    MakeQueueOptions(config.inputQueueSize));
            const std::size_t outputSize =
                index == kVarjoPipelineIndex ? config.outputQueueSize : 1;
            outputQueues[index] =
                std::make_shared<IC4Ext::D3D12SyncedFrameQueue>(
                    MakeQueueOptions(outputSize));
            syncThreads[index] =
                std::make_unique<IC4Ext::D3D12FrameSyncThread>(
                    inputQueues[index],
                    outputQueues[index],
                    syncOptions);
        }

        std::size_t startedSyncThreadCount = 0;
        for (std::size_t index = 0;
             index < kSyncPipelineCount;
             ++index) {
            if (!syncThreads[index]->start()) {
                PrintError("FrameSyncThread", syncThreads[index]->lastError());
                for (std::size_t stopIndex = 0;
                     stopIndex < startedSyncThreadCount;
                     ++stopIndex) {
                    syncThreads[stopIndex]->stopAndJoin();
                }
                return 1;
            }
            ++startedSyncThreadCount;
            std::cout << "[THREAD] FrameSyncThread " << index
                      << " (" << PipelineName(index) << ") started\n";
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

        auto initialSet = outputQueues[kVarjoPipelineIndex]->waitPopLatestFor(
            std::chrono::milliseconds(config.initialFrameTimeoutMs));
        if (!initialSet) {
            PrintSyncStats(syncThreads, outputQueues);
            const auto leftStats = leftCamera.stats();
            const auto rightStats = rightCamera.stats();
            stopCaptureAndSync();
            std::cerr << "Timed out waiting for the first synchronized Varjo frame.\n"
                      << "  left read=" << leftStats.readFrames
                      << " errors=" << leftStats.readErrors << '\n'
                      << "  right read=" << rightStats.readFrames
                      << " errors=" << rightStats.readErrors << '\n';
            return 1;
        }

        DualIC4Varjo::ClockMapper clock;
        DualIC4Varjo::StereoDisplayTextureRing displayRing(
            core,
            d3dBackend,
            config.displayRingSize);
        auto currentSet = std::make_shared<IC4Ext::D3D12SyncedFrameSet>(
            std::move(*initialSet));
        DisplayedPairMetadata displayedMetadata;
        std::size_t activeSlot = 0;
        bool planeSizeInitialized = false;
        std::uint32_t inputWidth = 0;
        std::uint32_t inputHeight = 0;

        auto applySet = [&](const std::shared_ptr<
                                IC4Ext::D3D12SyncedFrameSet>& owner) {
            if (!owner) {
                throw std::invalid_argument("applySet received null frame set");
            }
            const auto* left = FindCamera(*owner, 0);
            const auto* right = FindCamera(*owner, 1);
            if (!left || !right) {
                throw std::runtime_error(
                    "Synchronized frame set does not contain camera 0 and 1");
            }

            const auto upload = displayRing.upload(left->frame, right->frame);
            activeSlot = upload.slotIndex;
            plane.setTexture(VarjoXR::Eye::Left, upload.left);
            plane.setTexture(VarjoXR::Eye::Right, upload.right);

            const int leftWidth = std::max(0, left->frame.format.width);
            const int leftHeight = std::max(0, left->frame.format.height);
            const int rightWidth = std::max(0, right->frame.format.width);
            const int rightHeight = std::max(0, right->frame.format.height);
            if (leftWidth == 0 || leftHeight == 0 ||
                rightWidth == 0 || rightHeight == 0) {
                throw std::runtime_error("IC4 produced an invalid frame size");
            }
            if (leftWidth != rightWidth || leftHeight != rightHeight) {
                throw std::runtime_error(
                    "left and right IC4 output sizes do not match");
            }

            inputWidth = static_cast<std::uint32_t>(leftWidth);
            inputHeight = static_cast<std::uint32_t>(leftHeight);

            if (!planeSizeInitialized) {
                const float imageAspect =
                    static_cast<float>(inputWidth) /
                    static_cast<float>(inputHeight);
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

        if (calibration) {
            DualIC4Varjo::ValidateCalibrationInputGeometry(
                *calibration,
                inputWidth,
                inputHeight);
            DualIC4Varjo::ApplyCalibrationToPlane(
                plane,
                *calibration,
                activeCalibration.profile);
            DualIC4Varjo::UpdatePlaneAspectFromCalibration(
                plane,
                *calibration);
            std::cout << "Static stereo calibration applied to Varjo Plane.\n";
        }

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

        std::cout << "Experiment output directory: "
                  << experimentOutput.directory.string() << '\n'
                  << "Rendered-frame CSV: "
                  << config.metadataCsv.string() << '\n';

        std::unique_ptr<DualIC4Varjo::ImGuiStereoPreview> pcPreview;
        if (config.pcPreviewEnabled) {
            DualIC4Varjo::ImGuiStereoPreviewConfig previewConfig;
            previewConfig.windowWidth = config.pcPreviewWidth;
            previewConfig.windowHeight = config.pcPreviewHeight;
            previewConfig.vsync = config.pcPreviewVsync;
            previewConfig.windowTitle = "Dual IC4 Raw Left + Right Preview";
            pcPreview =
                std::make_unique<DualIC4Varjo::ImGuiStereoPreview>(
                    core,
                    outputQueues[kPcPreviewPipelineIndex],
                    std::move(previewConfig));
            if (!pcPreview->start()) {
                throw std::runtime_error(
                    "PC ImGui preview failed to start: " +
                    pcPreview->lastError());
            }
        }

        std::cout
            << "Plane keyboard controls (one 0.01 m step per key press):\n"
            << "  Left/Right          : move left/right\n"
            << "  Up/Down             : move up/down\n"
            << "  Shift + Left/Right  : decrease/increase size\n"
            << "  Shift + Up/Down     : farther/closer\n"
            << "  Esc or Ctrl+C       : stop\n";

        const auto renderStart = std::chrono::steady_clock::now();
        std::atomic<bool> renderRunning{true};
        std::exception_ptr renderException;
        std::uint64_t renderRowIndex = 0;

        std::thread renderThread([&]() noexcept {
            if (!SetThreadPriority(
                    GetCurrentThread(),
                    THREAD_PRIORITY_HIGHEST)) {
                std::cerr
                    << "[THREAD] Failed to set Varjo render thread priority.\n";
            }
            std::cout << "[THREAD] Varjo render thread id="
                      << GetCurrentThreadId() << '\n';

            ArrowKeyEdgeTracker planeKeys;

            try {
                while (!gStopRequested.load(std::memory_order_acquire)) {
                    bool frameBegun = false;
                    try {
                        space.beginFrame();
                        frameBegun = true;

                        // This is the active-frame snapshot boundary. Frames arriving
                        // after this pop are not used until the next beginFrame().
                        bool newFrameFromQueue = false;
                        if (auto latest =
                                outputQueues[kVarjoPipelineIndex]
                                    ->tryPopLatest()) {
                            currentSet =
                                std::make_shared<
                                    IC4Ext::D3D12SyncedFrameSet>(
                                    std::move(*latest));
                            applySet(currentSet);
                            newFrameFromQueue = true;
                        }

                        const PlaneKeyboardUpdate planeUpdate =
                            UpdatePlaneFromKeyboard(plane, planeKeys);

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
                        row.calibrationSource = activeCalibration.source;
                        row.calibrationProfile = activeCalibration.profile;
                        row.calibrationRevision = activeCalibration.revision;
                        row.planeMoved = planeUpdate.moved;
                        row.planeResized = planeUpdate.resized;
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
                            syncThreads[kVarjoPipelineIndex]->stats();
                        row.syncedQueueStats =
                            outputQueues[kVarjoPipelineIndex]->stats();
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
        std::exception_ptr mainLoopException;
        try {
            while (renderRunning.load(std::memory_order_acquire) &&
                   !gStopRequested.load(std::memory_order_acquire)) {
                if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
                    gStopRequested.store(true, std::memory_order_release);
                    break;
                }
                if (pcPreview) {
                    pcPreview->rethrowWorkerExceptionIfAny();
                }
                if (config.maxRuntimeSeconds > 0.0) {
                    const double elapsed = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() -
                        mainWaitStart).count();
                    if (elapsed >= config.maxRuntimeSeconds) {
                        gStopRequested.store(true, std::memory_order_release);
                        break;
                    }
                }
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(10));
            }
        } catch (...) {
            mainLoopException = std::current_exception();
            gStopRequested.store(true, std::memory_order_release);
        }

        gStopRequested.store(true, std::memory_order_release);
        if (renderThread.joinable()) renderThread.join();
        if (pcPreview) {
            pcPreview->stopAndJoin();
            if (!mainLoopException) {
                try {
                    pcPreview->rethrowWorkerExceptionIfAny();
                } catch (...) {
                    mainLoopException = std::current_exception();
                }
            }
        }

        logger.stop();
        displayRing.waitIdle();
        PrintSyncStats(syncThreads, outputQueues);
        stopCaptureAndSync();

        if (renderException) std::rethrow_exception(renderException);
        if (mainLoopException) std::rethrow_exception(mainLoopException);

        std::cout << "Stopped cleanly.\n";
        return 0;
    } catch (const std::exception& exception) {
        gStopRequested.store(true, std::memory_order_release);
        if (core) {
            try {
                core->WaitIdle();
            } catch (...) {
            }
        }
        logger.stop();
        std::cerr << "Fatal error: " << exception.what() << '\n';
        return 1;
    }
}
