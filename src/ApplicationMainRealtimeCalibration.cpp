#include "CheckerboardStereoCalibration.hpp"
#include "ExperimentOutput.hpp"
#include "EyeTrackerLoadServiceHook.hpp"
#include "GazeOnCameraFrameService.hpp"
#include "ImuLoadServiceHook.hpp"
#include "KeyInputService.hpp"
#include "TimestampLoadService.hpp"
#include "VstLoadServiceHook.hpp"

#define main DualIC4VarjoLegacyMain
#include "ApplicationMainPlaneControl.cpp"
#undef main

#include <condition_variable>
#include <mutex>

namespace DualIC4Varjo::RealtimeApp {
namespace {

struct ParsedArguments {
    std::vector<std::string> appArguments;
    bool calibrationSpecified = false;
    bool calibrationDash = false;
    std::filesystem::path calibrationPath;
    CheckerboardCalibrationOptions checkerboard;
};

int ParseInt(const std::string& value, const char* option)
{
    try {
        std::size_t consumed = 0;
        const int result = std::stoi(value, &consumed, 10);
        if (consumed != value.size()) throw std::invalid_argument("trailing");
        return result;
    } catch (...) {
        throw std::invalid_argument(
            std::string("invalid integer for ") + option + ": " + value);
    }
}

double ParseDouble(const std::string& value, const char* option)
{
    try {
        std::size_t consumed = 0;
        const double result = std::stod(value, &consumed);
        if (consumed != value.size() || !std::isfinite(result)) {
            throw std::invalid_argument("invalid");
        }
        return result;
    } catch (...) {
        throw std::invalid_argument(
            std::string("invalid number for ") + option + ": " + value);
    }
}

ParsedArguments ParseRealtimeArguments(int argc, char** argv)
{
    ParsedArguments result;
    result.appArguments.reserve(static_cast<std::size_t>(argc));
    if (argc > 0 && argv[0]) result.appArguments.emplace_back(argv[0]);

    auto requireValue = [&](int& index, const std::string& option) {
        if (index + 1 >= argc || !argv[index + 1]) {
            throw std::invalid_argument("missing value for " + option);
        }
        return std::string(argv[++index]);
    };

    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index] ? argv[index] : "";
        if (option == "--calib") {
            if (result.calibrationSpecified) {
                throw std::invalid_argument("--calib may be specified only once");
            }
            const std::string value = requireValue(index, option);
            result.calibrationSpecified = true;
            result.calibrationDash = value == "-";
            if (!result.calibrationDash) result.calibrationPath = value;
        } else if (option == "--calib-board-cols") {
            result.checkerboard.innerCornersColumns =
                ParseInt(requireValue(index, option), option.c_str());
        } else if (option == "--calib-board-rows") {
            result.checkerboard.innerCornersRows =
                ParseInt(requireValue(index, option), option.c_str());
        } else if (option == "--calib-min-samples") {
            const int value = ParseInt(requireValue(index, option), option.c_str());
            if (value < 3) {
                throw std::invalid_argument("--calib-min-samples must be at least 3");
            }
            result.checkerboard.minimumSamplePairs =
                static_cast<std::size_t>(value);
        } else if (option == "--calib-capture-interval-ms") {
            result.checkerboard.captureIntervalMs =
                ParseInt(requireValue(index, option), option.c_str());
        } else if (option == "--calib-min-motion-px") {
            result.checkerboard.minimumCornerMotionPixels =
                ParseDouble(requireValue(index, option), option.c_str());
        } else {
            result.appArguments.push_back(option);
        }
    }
    return result;
}

std::vector<char*> MutableArguments(std::vector<std::string>& values)
{
    std::vector<char*> result;
    result.reserve(values.size());
    for (auto& value : values) result.push_back(value.data());
    return result;
}

void PrintRealtimeUsage(std::ostream& output)
{
    output
        << "\nRealtime checkerboard calibration:\n"
        << "  --calib PATH|-                    Existing/new JSON, or '-' for experiment folder\n"
        << "  --calib-board-cols N              Inner corners horizontally (default 9)\n"
        << "  --calib-board-rows N              Inner corners vertically (default 6)\n"
        << "  --calib-min-samples N             Required stereo pairs (default 12)\n"
        << "  --calib-capture-interval-ms N     Auto-capture interval (default 500)\n"
        << "  --calib-min-motion-px N           Required checkerboard motion (default 20)\n";
}

int PromptExistingCalibration(const std::filesystem::path& path)
{
    const std::wstring message =
        L"Existing calibration JSON was found:\n\n" + path.wstring() +
        L"\n\nRun OpenCV checkerboard stereo calibration now?";
    return MessageBoxW(
        nullptr,
        message.c_str(),
        L"Realtime stereo calibration",
        MB_YESNOCANCEL | MB_ICONQUESTION | MB_SETFOREGROUND);
}

void ReorderOutputWithVarjoLast(
    IC4Ext::D3D12CameraCaptureThread& camera,
    std::uint32_t cameraIndex,
    const std::shared_ptr<IC4Ext::D3D12IndexedFrameQueue>& varjoQueue,
    const std::shared_ptr<IC4Ext::D3D12IndexedFrameQueue>& addedQueue)
{
    camera.removeOutputQueue(cameraIndex, varjoQueue);
    camera.addOutputQueue(cameraIndex, addedQueue);
    camera.addOutputQueue(cameraIndex, varjoQueue);
}

void PrintOneSyncStats(
    const char* name,
    const IC4Ext::D3D12FrameSyncThread& thread,
    const IC4Ext::D3D12SyncedFrameQueue& queue)
{
    const auto sync = thread.stats();
    const auto output = queue.stats();
    std::cout
        << "[SYNC " << name << "] input=" << sync.inputFrames
        << " emitted=" << sync.emittedSets
        << " dropped=" << sync.droppedFrames
        << " pushFailures=" << sync.pushFailures
        << " outputDroppedOldest=" << output.droppedOldest
        << " outputDroppedByPopLatest=" << output.droppedByPopLatest
        << '\n';
}

} // namespace
} // namespace DualIC4Varjo::RealtimeApp

int main(int argc, char** argv)
{
    using namespace DualIC4Varjo;
    using namespace DualIC4Varjo::RealtimeApp;

    gStopRequested.store(false, std::memory_order_release);
    SetConsoleCtrlHandler(ConsoleControlHandler, TRUE);

    ParsedArguments parsed;
    AppConfig config;
    std::string argumentError;
    try {
        parsed = ParseRealtimeArguments(argc, argv);
        auto appPointers = MutableArguments(parsed.appArguments);
        if (!ParseArguments(
                static_cast<int>(appPointers.size()),
                appPointers.data(),
                config,
                argumentError)) {
            throw std::invalid_argument(argumentError);
        }
    } catch (const std::exception& exception) {
        std::cerr << "Argument error: " << exception.what() << "\n\n";
        PrintUsage(std::cerr);
        PrintRealtimeUsage(std::cerr);
        return 2;
    }

    if (config.showHelp) {
        PrintUsage(std::cout);
        PrintRealtimeUsage(std::cout);
        return 0;
    }

    ExperimentOutputLayout outputLayout;
    try {
        outputLayout = ReserveExperimentOutputLayout(
            config.outputBaseDirectory,
            config.projectName,
            config.metadataCsv);
    } catch (const std::exception& exception) {
        std::cerr << "Output setup failed: " << exception.what() << '\n';
        return 1;
    }
    config.metadataCsv = outputLayout.renderedFramesCsv;

    std::optional<std::filesystem::path> calibrationPath;
    bool existingCalibration = false;
    if (parsed.calibrationSpecified) {
        if (parsed.calibrationDash) {
            calibrationPath = outputLayout.directory /
                (outputLayout.resolvedProjectName + "_stereo_calibration.json");
        } else {
            calibrationPath = std::filesystem::absolute(parsed.calibrationPath);
            existingCalibration =
                std::filesystem::is_regular_file(*calibrationPath);
        }
    }

    std::cout
        << "[OUTPUT] requested project : " << outputLayout.requestedProjectName << '\n'
        << "[OUTPUT] resolved project  : " << outputLayout.resolvedProjectName << '\n'
        << "[OUTPUT] directory         : " << outputLayout.directory.string() << '\n'
        << "[LIFECYCLE] initial SyncThreads: Varjo + ImGui (2)\n";

    RenderedFrameMetadataLogger logger;
    std::shared_ptr<D3D12CoreLib::D3D12Core> core;
    std::shared_ptr<VarjoSession> session;
    std::unique_ptr<ImGuiStereoPreview> pcPreview;

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
            -config.planeDistanceMeters};

        IC4Ext::FrameSyncOptions syncOptions;
        syncOptions.policy = IC4Ext::FrameSyncPolicy::TimestampNearest;
        syncOptions.cameraIndices = {0, 1};
        syncOptions.maxTimestampDiffNs = static_cast<std::uint64_t>(
            std::llround(config.syncToleranceMs * 1'000'000.0));
        syncOptions.maxBufferedFramesPerCamera =
            config.syncBufferedFramesPerCamera;
        syncOptions.timestampSource = config.timestampSource;

        auto varjoInput =
            std::make_shared<IC4Ext::D3D12IndexedFrameQueue>(
                MakeQueueOptions(config.inputQueueSize));
        auto imguiInput =
            std::make_shared<IC4Ext::D3D12IndexedFrameQueue>(
                MakeQueueOptions(config.inputQueueSize));
        auto varjoOutput =
            std::make_shared<IC4Ext::D3D12SyncedFrameQueue>(
                MakeQueueOptions(config.outputQueueSize));
        auto imguiOutput =
            std::make_shared<IC4Ext::D3D12SyncedFrameQueue>(
                MakeQueueOptions(1));

        IC4Ext::D3D12FrameSyncThread varjoSync(
            varjoInput, varjoOutput, syncOptions);
        IC4Ext::D3D12FrameSyncThread imguiSync(
            imguiInput, imguiOutput, syncOptions);
        if (!varjoSync.start()) {
            throw std::runtime_error(
                "Varjo FrameSyncThread failed: " +
                varjoSync.lastError().message);
        }
        if (!imguiSync.start()) {
            varjoSync.stopAndJoin();
            throw std::runtime_error(
                "ImGui FrameSyncThread failed: " +
                imguiSync.lastError().message);
        }

        const auto captureBackend =
            IC4Ext::D3D12BackendContext::FromCore(core);
        IC4Ext::CameraThreadOptions cameraOptions;
        cameraOptions.readTimeoutMs = config.cameraReadTimeoutMs;
        cameraOptions.copyPerOutputQueue = true;
        cameraOptions.stopOnReadError = false;
        cameraOptions.copiedOutputFrameStride = 1;
        cameraOptions.copiedOutputFrameBurst = 1;

        IC4Ext::D3D12CameraCaptureThread leftCamera(
            config.left.selector,
            MakeCaptureConfig(config, config.left),
            captureBackend,
            cameraOptions);
        IC4Ext::D3D12CameraCaptureThread rightCamera(
            config.right.selector,
            MakeCaptureConfig(config, config.right),
            captureBackend,
            cameraOptions);

        // ImGui copy first, Varjo original last.
        leftCamera.addOutputQueue(0, imguiInput);
        leftCamera.addOutputQueue(0, varjoInput);
        rightCamera.addOutputQueue(1, imguiInput);
        rightCamera.addOutputQueue(1, varjoInput);

        if (!leftCamera.start()) {
            throw std::runtime_error(
                "left camera failed: " + leftCamera.lastError().message);
        }
        if (config.cameraStartDelayMs > 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(config.cameraStartDelayMs));
        }
        if (!rightCamera.start()) {
            leftCamera.stopAndJoin();
            throw std::runtime_error(
                "right camera failed: " + rightCamera.lastError().message);
        }

        auto initialSet = varjoOutput->waitPopLatestFor(
            std::chrono::milliseconds(config.initialFrameTimeoutMs));
        if (!initialSet) {
            throw std::runtime_error(
                "timed out waiting for initial Varjo synchronized pair");
        }

        ClockMapper clock;
        StereoDisplayTextureRing displayRing(
            core,
            d3dBackend,
            config.displayRingSize);
        auto currentSet = std::make_shared<IC4Ext::D3D12SyncedFrameSet>(
            std::move(*initialSet));
        DisplayedPairMetadata displayedMetadata;
        std::size_t activeSlot = 0;
        std::uint32_t inputWidth = 0;
        std::uint32_t inputHeight = 0;
        bool planeSizeInitialized = false;

        auto applySet = [&](const std::shared_ptr<
                                IC4Ext::D3D12SyncedFrameSet>& owner) {
            const auto* left = FindCamera(*owner, 0);
            const auto* right = FindCamera(*owner, 1);
            if (!left || !right) {
                throw std::runtime_error("incomplete synchronized camera pair");
            }
            const auto upload = displayRing.upload(left->frame, right->frame);
            activeSlot = upload.slotIndex;
            plane.setTexture(VarjoXR::Eye::Left, upload.left);
            plane.setTexture(VarjoXR::Eye::Right, upload.right);

            const int width = std::max(0, left->frame.format.width);
            const int height = std::max(0, left->frame.format.height);
            if (width == 0 || height == 0 ||
                right->frame.format.width != width ||
                right->frame.format.height != height) {
                throw std::runtime_error("invalid stereo camera geometry");
            }
            inputWidth = static_cast<std::uint32_t>(width);
            inputHeight = static_cast<std::uint32_t>(height);
            if (!planeSizeInitialized) {
                const float aspect = static_cast<float>(inputWidth) /
                    static_cast<float>(inputHeight);
                const float planeHeight = config.planeHeightMeters > 0.0f
                    ? config.planeHeightMeters
                    : config.planeWidthMeters / std::max(0.001f, aspect);
                plane.setSize({config.planeWidthMeters, planeHeight});
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

        ImGuiStereoPreviewConfig previewConfig;
        previewConfig.windowWidth = config.pcPreviewWidth;
        previewConfig.windowHeight = config.pcPreviewHeight;
        previewConfig.vsync = config.pcPreviewVsync;
        previewConfig.windowTitle = "Dual IC4 raw stereo preview";
        pcPreview = std::make_unique<ImGuiStereoPreview>(
            core,
            imguiOutput,
            previewConfig);
        if (!pcPreview->start()) {
            throw std::runtime_error(
                "PC ImGui preview failed: " + pcPreview->lastError());
        }

        std::mutex calibrationMutex;
        std::optional<StereoCalibrationDocument> pendingCalibration;
        std::uint64_t pendingCalibrationRevision = 0;
        std::atomic<std::uint64_t> appliedCalibrationRevision{0};
        std::atomic<bool> loggingEnabled{false};
        ActiveCalibrationInfo renderCalibrationInfo;
        std::atomic<bool> renderRunning{true};
        std::exception_ptr renderException;
        std::uint64_t renderRowIndex = 0;
        const auto renderStart = std::chrono::steady_clock::now();

        std::thread renderThread([&]() noexcept {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            std::cout << "[THREAD] Varjo render thread id="
                      << GetCurrentThreadId() << '\n';
            ArrowKeyEdgeTracker planeKeys;
            std::uint64_t locallyAppliedRevision = 0;
            try {
                while (!gStopRequested.load(std::memory_order_acquire)) {
                    bool frameBegun = false;
                    try {
                        space.beginFrame();
                        frameBegun = true;
                        bool newFrameFromQueue = false;
                        if (auto latest = varjoOutput->tryPopLatest()) {
                            currentSet = std::make_shared<
                                IC4Ext::D3D12SyncedFrameSet>(
                                    std::move(*latest));
                            applySet(currentSet);
                            newFrameFromQueue = true;
                        }

                        {
                            std::lock_guard<std::mutex> lock(calibrationMutex);
                            if (pendingCalibration &&
                                pendingCalibrationRevision != locallyAppliedRevision) {
                                ValidateCalibrationInputGeometry(
                                    *pendingCalibration,
                                    inputWidth,
                                    inputHeight);
                                ApplyCalibrationToPlane(
                                    plane,
                                    *pendingCalibration,
                                    pendingCalibration->defaultProfile);
                                UpdatePlaneAspectFromCalibration(
                                    plane,
                                    *pendingCalibration);
                                locallyAppliedRevision = pendingCalibrationRevision;
                                renderCalibrationInfo.source = "json";
                                renderCalibrationInfo.profile =
                                    pendingCalibration->defaultProfile;
                                renderCalibrationInfo.revision = locallyAppliedRevision;
                                appliedCalibrationRevision.store(
                                    locallyAppliedRevision,
                                    std::memory_order_release);
                                std::cout
                                    << "[CALIB] applied to Varjo plane: "
                                    << renderCalibrationInfo.profile << '\n';
                            }
                        }

                        PlaneKeyboardUpdate planeUpdate;
                        if (loggingEnabled.load(std::memory_order_acquire)) {
                            planeUpdate = UpdatePlaneFromKeyboard(plane, planeKeys);
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

                        if (loggingEnabled.load(std::memory_order_acquire)) {
                            const auto snapshot = d3dBackend.frameInfoSnapshot();
                            const auto planeSize = plane.size();
                            const auto& planePosition = plane.transform().position;
                            GazeCameraPlaneSnapshot gazePlane{};
                            gazePlane.x = planePosition.x;
                            gazePlane.y = planePosition.y;
                            gazePlane.z = planePosition.z;
                            gazePlane.width = planeSize.x;
                            gazePlane.height = planeSize.y;
                            gazePlane.leftFrameWidth = inputWidth;
                            gazePlane.leftFrameHeight = inputHeight;
                            gazePlane.rightFrameWidth = inputWidth;
                            gazePlane.rightFrameHeight = inputHeight;
                            gazePlane.placementMode = config.placementMode;
                            ImuLoadServiceHook::submit(snapshot, session->shared());
                            EyeTrackerLoadServiceHook::submit(snapshot, session->shared());
                            GazeOnCameraFrameHook::submitFrameInfo(
                                snapshot,
                                gazePlane,
                                session->shared());
                            VstLoadServiceHook::ensureStarted(session->shared());

                            RenderedFrameMetadataRow row;
                            row.renderRowIndex = renderRowIndex++;
                            row.renderSubmitUnixUs =
                                clock.unixMicroseconds(frameSnapshotTime);
                            row.renderSubmitLocalIso8601 =
                                clock.localIso8601(row.renderSubmitUnixUs);
                            row.newFrameFromQueue = newFrameFromQueue;
                            row.submitOk = true;
                            row.calibrationSource = renderCalibrationInfo.source;
                            row.calibrationProfile = renderCalibrationInfo.profile;
                            row.calibrationRevision = renderCalibrationInfo.revision;
                            row.planeMoved = planeUpdate.moved;
                            row.planeResized = planeUpdate.resized;
                            row.planePlacementMode =
                                PlacementModeName(config.placementMode);
                            row.planeX = planePosition.x;
                            row.planeY = planePosition.y;
                            row.planeZ = planePosition.z;
                            row.planeWidth = planeSize.x;
                            row.planeHeight = planeSize.y;
                            row.syncGroupId = displayedMetadata.syncGroupId;
                            row.syncEmittedUnixUs =
                                clock.unixMicroseconds(
                                    displayedMetadata.emittedTime);
                            row.syncTimestampSource =
                                TimestampSourceName(config.timestampSource);
                            row.syncTimestampDiffNs =
                                displayedMetadata.syncTimestampDiffNs;
                            row.hostReceivedDiffUs =
                                displayedMetadata.hostReceivedDiffUs;
                            row.left = displayedMetadata.left;
                            row.right = displayedMetadata.right;
                            row.displaySlotIndex = activeSlot;
                            row.syncStats = varjoSync.stats();
                            row.syncedQueueStats = varjoOutput->stats();
                            row.leftCameraStats = leftCamera.stats();
                            row.rightCameraStats = rightCamera.stats();
                            if (!logger.enqueue(std::move(row))) {
                                throw std::runtime_error(
                                    "metadata logger failed: " + logger.lastError());
                            }
                        }
                    } catch (...) {
                        if (frameBegun) {
                            try { space.endFrame(); } catch (...) {}
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

        // Let raw Varjo and ImGui views become visible before asking.
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        bool runCalibration = false;
        std::optional<StereoCalibrationDocument> selectedCalibration;
        if (parsed.calibrationSpecified) {
            if (existingCalibration) {
                const int answer = PromptExistingCalibration(*calibrationPath);
                if (answer == IDCANCEL) {
                    gStopRequested.store(true, std::memory_order_release);
                } else if (answer == IDYES) {
                    runCalibration = true;
                    selectedCalibration = LoadStereoCalibrationJson(*calibrationPath);
                } else {
                    selectedCalibration = LoadStereoCalibrationJson(*calibrationPath);
                }
            } else {
                runCalibration = true;
            }
        }

        if (gStopRequested.load(std::memory_order_acquire)) {
            if (renderThread.joinable()) renderThread.join();
            pcPreview->stopAndJoin();
            leftCamera.stopAndJoin();
            rightCamera.stopAndJoin();
            varjoSync.stopAndJoin();
            imguiSync.stopAndJoin();
            return 0;
        }

        if (runCalibration) {
            auto calibrationInput =
                std::make_shared<IC4Ext::D3D12IndexedFrameQueue>(
                    MakeQueueOptions(config.inputQueueSize));
            auto calibrationOutput =
                std::make_shared<IC4Ext::D3D12SyncedFrameQueue>(
                    MakeQueueOptions(4));

            ReorderOutputWithVarjoLast(
                leftCamera, 0, varjoInput, calibrationInput);
            ReorderOutputWithVarjoLast(
                rightCamera, 1, varjoInput, calibrationInput);

            IC4Ext::D3D12FrameSyncThread calibrationSync(
                calibrationInput,
                calibrationOutput,
                syncOptions);
            if (!calibrationSync.start()) {
                throw std::runtime_error(
                    "calibration FrameSyncThread failed: " +
                    calibrationSync.lastError().message);
            }
            std::cout
                << "[LIFECYCLE] calibration SyncThread added (total 3)\n";

            CheckerboardCalibrationResult calibrationResult =
                RunCheckerboardStereoCalibration(
                    *calibrationOutput,
                    captureBackend,
                    parsed.checkerboard,
                    selectedCalibration);

            leftCamera.removeOutputQueue(0, calibrationInput);
            rightCamera.removeOutputQueue(1, calibrationInput);
            calibrationInput->close();
            calibrationOutput->close();
            calibrationSync.stopAndJoin();
            std::cout
                << "[LIFECYCLE] calibration SyncThread removed (total 2)\n";

            if (calibrationResult.aborted) {
                gStopRequested.store(true, std::memory_order_release);
            } else if (!calibrationResult.ok) {
                throw std::runtime_error(
                    "checkerboard calibration failed: " +
                    calibrationResult.error);
            } else {
                selectedCalibration = calibrationResult.document;
                SaveStereoCalibrationJson(*calibrationPath, *selectedCalibration);
                std::cout
                    << "[CALIB] saved " << calibrationResult.capturedSamplePairs
                    << " samples, avg vertical error "
                    << calibrationResult.averageEpipolarErrorPixels
                    << " px: " << calibrationPath->string() << '\n';
            }
        }

        if (gStopRequested.load(std::memory_order_acquire)) {
            if (renderThread.joinable()) renderThread.join();
            pcPreview->stopAndJoin();
            leftCamera.stopAndJoin();
            rightCamera.stopAndJoin();
            varjoSync.stopAndJoin();
            imguiSync.stopAndJoin();
            return 0;
        }

        if (selectedCalibration) {
            std::uint64_t revision = 0;
            {
                std::lock_guard<std::mutex> lock(calibrationMutex);
                pendingCalibration = *selectedCalibration;
                revision = ++pendingCalibrationRevision;
            }
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::seconds(5);
            while (appliedCalibrationRevision.load(
                       std::memory_order_acquire) < revision &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (appliedCalibrationRevision.load(
                    std::memory_order_acquire) < revision) {
                throw std::runtime_error(
                    "timed out applying calibration to Varjo plane");
            }
            config.calibrationJson = calibrationPath;
        }

        // Calibration has ended. Add the unused discard pipeline now.
        auto discardInput =
            std::make_shared<IC4Ext::D3D12IndexedFrameQueue>(
                MakeQueueOptions(config.inputQueueSize));
        auto discardOutput =
            std::make_shared<IC4Ext::D3D12SyncedFrameQueue>(
                MakeQueueOptions(1));
        ReorderOutputWithVarjoLast(
            leftCamera, 0, varjoInput, discardInput);
        ReorderOutputWithVarjoLast(
            rightCamera, 1, varjoInput, discardInput);
        IC4Ext::D3D12FrameSyncThread discardSync(
            discardInput,
            discardOutput,
            syncOptions);
        if (!discardSync.start()) {
            throw std::runtime_error(
                "discard FrameSyncThread failed: " +
                discardSync.lastError().message);
        }
        std::cout
            << "[LIFECYCLE] discard SyncThread created after calibration "
            << "(Varjo + ImGui + discard = 3)\n";

        // All logging and Varjo services begin only after calibration is complete.
        ImuLoadServiceHook::configure(argc, argv);
        VstLoadServiceHook::configure(argc, argv);
        EyeTrackerLoadServiceHook::configure(argc, argv);
        const auto gazeCameraPath = outputLayout.directory /
            (outputLayout.resolvedProjectName + "_gaze_on_camera_frame.csv");
        if (!GazeOnCameraFrameHook::configure(
                gazeCameraPath,
                config.calibrationJson)) {
            throw std::runtime_error(
                "gaze-on-camera-frame service failed: " +
                GazeOnCameraFrameHook::lastError());
        }

        KeyInputService keyInputService(
            outputLayout.directory /
            (outputLayout.resolvedProjectName + "_key_input.csv"));
        if (!keyInputService.start()) {
            throw std::runtime_error(
                "key input service failed: " + keyInputService.lastError());
        }

        TimestampLoadService timestampService(argc, argv);
        if (!timestampService.start()) {
            throw std::runtime_error(
                "timestamp service failed: " + timestampService.lastError());
        }

        if (!logger.start(config.metadataCsv)) {
            throw std::runtime_error(logger.lastError());
        }

        std::atomic<bool> gazeBridgeStop{false};
        std::thread gazeBridge([&]() {
            while (!gazeBridgeStop.load(std::memory_order_acquire)) {
                auto data = EyeTrackerLoadServiceHook::requestData();
                if (!data.empty()) {
                    GazeOnCameraFrameHook::submitGazeData(std::move(data));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            auto remaining = EyeTrackerLoadServiceHook::requestData();
            if (!remaining.empty()) {
                GazeOnCameraFrameHook::submitGazeData(std::move(remaining));
            }
        });

        loggingEnabled.store(true, std::memory_order_release);
        std::cout
            << "[RUN] calibration complete; CSV and VST recording started\n";
        const auto runtimeStart = std::chrono::steady_clock::now();

        while (renderRunning.load(std::memory_order_acquire) &&
               !gStopRequested.load(std::memory_order_acquire)) {
            if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
                gStopRequested.store(true, std::memory_order_release);
                break;
            }
            pcPreview->rethrowWorkerExceptionIfAny();
            if (config.maxRuntimeSeconds > 0.0) {
                const double elapsed = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - runtimeStart).count();
                if (elapsed >= config.maxRuntimeSeconds) {
                    gStopRequested.store(true, std::memory_order_release);
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        loggingEnabled.store(false, std::memory_order_release);
        gStopRequested.store(true, std::memory_order_release);
        if (renderThread.joinable()) renderThread.join();
        gazeBridgeStop.store(true, std::memory_order_release);
        if (gazeBridge.joinable()) gazeBridge.join();

        EyeTrackerLoadServiceHook::stop();
        GazeOnCameraFrameHook::stop();
        VstLoadServiceHook::stop();
        ImuLoadServiceHook::stop();
        timestampService.stop();
        keyInputService.stop();
        logger.stop();

        pcPreview->stopAndJoin();
        displayRing.waitIdle();
        leftCamera.stopAndJoin();
        rightCamera.stopAndJoin();
        discardInput->close();
        discardOutput->close();
        discardSync.stopAndJoin();
        varjoInput->close();
        imguiInput->close();
        varjoOutput->close();
        imguiOutput->close();
        varjoSync.stopAndJoin();
        imguiSync.stopAndJoin();

        PrintOneSyncStats("varjo", varjoSync, *varjoOutput);
        PrintOneSyncStats("imgui", imguiSync, *imguiOutput);
        PrintOneSyncStats("discard", discardSync, *discardOutput);

        if (renderException) std::rethrow_exception(renderException);

        std::cout
            << "[TIMESTAMP] samples=" << timestampService.sampleCount()
            << " failed=" << timestampService.failedSampleCount()
            << " CSV=" << timestampService.outputPath().string() << '\n'
            << "[IMU] received=" << ImuLoadServiceHook::receivedCount()
            << " processed=" << ImuLoadServiceHook::processedCount()
            << " written=" << ImuLoadServiceHook::writtenCount()
            << " dropped=" << ImuLoadServiceHook::droppedCount()
            << " CSV=" << ImuLoadServiceHook::outputPath().string() << '\n'
            << "[VST] leftReceived=" << VstLoadServiceHook::leftReceivedCount()
            << " rightReceived=" << VstLoadServiceHook::rightReceivedCount()
            << " leftProcessed=" << VstLoadServiceHook::leftProcessedCount()
            << " rightProcessed=" << VstLoadServiceHook::rightProcessedCount()
            << " dropped=" << VstLoadServiceHook::droppedCount()
            << " writeFailures=" << VstLoadServiceHook::writeFailureCount()
            << '\n'
            << "[EYETRACKER] received="
            << EyeTrackerLoadServiceHook::receivedSampleCount()
            << " written=" << EyeTrackerLoadServiceHook::writtenSampleCount()
            << " dropped=" << EyeTrackerLoadServiceHook::droppedSampleCount()
            << '\n'
            << "[GAZE_CAMERA] received="
            << GazeOnCameraFrameHook::receivedGazeCount()
            << " written=" << GazeOnCameraFrameHook::writtenRowCount()
            << " dropped=" << GazeOnCameraFrameHook::droppedGazeCount()
            << '\n'
            << "[KEYINPUT] events=" << keyInputService.eventCount()
            << " CSV=" << keyInputService.outputPath().string() << '\n';

        std::cout << "Stopped cleanly.\n";
        return 0;
    } catch (const std::exception& exception) {
        gStopRequested.store(true, std::memory_order_release);
        if (pcPreview) pcPreview->stopAndJoin();
        logger.stop();
        if (core) {
            try { core->WaitIdle(); } catch (...) {}
        }
        std::cerr << "Fatal error: " << exception.what() << '\n';
        return 1;
    }
}
