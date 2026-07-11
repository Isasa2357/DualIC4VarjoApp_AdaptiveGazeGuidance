#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "RealtimeCalibrationBootstrap.hpp"

#include "AppConfig.hpp"
#include "ImGuiStereoPreview.hpp"
#include "StereoCalibrationSupport.hpp"
#include "StereoDisplayTextureRing.hpp"

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
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace DualIC4Varjo {
namespace {

using namespace std::chrono_literals;

struct ParsedCalibrationArgument {
    bool present = false;
    bool dash = false;
    std::filesystem::path path;
    std::vector<std::string> filtered;
};

ParsedCalibrationArgument ParseCalibrationArgument(int argc, char** argv)
{
    ParsedCalibrationArgument result;
    result.filtered.reserve(static_cast<std::size_t>(argc));
    if (argc > 0 && argv[0]) result.filtered.emplace_back(argv[0]);

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index] ? argv[index] : "";
        if (argument != "--calib") {
            result.filtered.push_back(argument);
            continue;
        }
        if (result.present) {
            throw std::invalid_argument("--calib may be specified only once");
        }
        if (index + 1 >= argc || !argv[index + 1]) {
            throw std::invalid_argument("--calib requires a path or '-'");
        }
        result.present = true;
        const std::string value = argv[++index];
        result.dash = value == "-";
        if (!result.dash) {
            result.path = std::filesystem::path(value);
            if (result.path.empty()) {
                throw std::invalid_argument("--calib path must not be empty");
            }
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

ThreadKit::Queues::QueueOptions QueueOptions(std::size_t size)
{
    ThreadKit::Queues::QueueOptions options;
    options.maxSize = size;
    options.overflowPolicy =
        ThreadKit::Queues::QueueOverflowPolicy::DropOldest;
    return options;
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

class RawPairAssembler {
public:
    RawPairAssembler(
        std::shared_ptr<IC4Ext::D3D12IndexedFrameQueue> input,
        std::shared_ptr<IC4Ext::D3D12SyncedFrameQueue> output)
        : input_(std::move(input)), output_(std::move(output))
    {
    }

    ~RawPairAssembler() { stop(); }

    void start()
    {
        stopRequested_.store(false, std::memory_order_release);
        worker_ = std::thread([this] { run(); });
    }

    void stop() noexcept
    {
        stopRequested_.store(true, std::memory_order_release);
        if (worker_.joinable()) {
            try { worker_.join(); } catch (...) {}
        }
    }

private:
    void run() noexcept
    {
        std::optional<IC4Ext::D3D12IndexedCameraFrame> left;
        std::optional<IC4Ext::D3D12IndexedCameraFrame> right;
        std::uint64_t group = 1;
        try {
            while (!stopRequested_.load(std::memory_order_acquire)) {
                auto item = input_->waitPopFor(5ms);
                if (!item) continue;
                if (item->cameraIndex == 0) left = std::move(*item);
                if (item->cameraIndex == 1) right = std::move(*item);
                if (!left || !right) continue;

                IC4Ext::D3D12SyncedFrameSet set;
                set.syncGroupId = group++;
                set.emittedTime = std::chrono::steady_clock::now();
                set.frames.reserve(2);
                set.frames.push_back(std::move(*left));
                set.frames.push_back(std::move(*right));
                left.reset();
                right.reset();
                output_->push(std::move(set));
            }
        } catch (...) {
        }
    }

    std::shared_ptr<IC4Ext::D3D12IndexedFrameQueue> input_;
    std::shared_ptr<IC4Ext::D3D12SyncedFrameQueue> output_;
    std::atomic<bool> stopRequested_{true};
    std::thread worker_;
};

StereoCalibrationDocument NewCalibrationDocument(
    std::uint32_t width,
    std::uint32_t height)
{
    StereoCalibrationDocument document;
    document.format = "vdca.stereo_rectification";
    document.version = 1;
    document.defaultProfile = "affine_full";
    document.sourceSize = {width, height};
    document.calibrationInputSize = {width, height};
    document.rectifiedOutputSize = {width, height};
    document.resizeMode = "none";
    document.rightOrder = "same";
    document.samplingFilter = "linear";
    document.borderMode = "constant";
    document.borderRgba = {0.0f, 0.0f, 0.0f, 1.0f};
    CalibrationProfile profile;
    profile.method = "affine_full";
    document.profiles.emplace(document.defaultProfile, std::move(profile));
    return document;
}

CalibrationHomography Multiply(
    const CalibrationHomography& a,
    const CalibrationHomography& b)
{
    CalibrationHomography result;
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            double value = 0.0;
            for (std::size_t k = 0; k < 3; ++k) {
                value += a.rows[row * 3 + k] * b.rows[k * 3 + column];
            }
            result.rows[row * 3 + column] = value;
        }
    }
    return result;
}

CalibrationHomography AroundCenter(
    const CalibrationHomography& transform,
    double centerX,
    double centerY)
{
    CalibrationHomography toOrigin;
    toOrigin.rows[2] = -centerX;
    toOrigin.rows[5] = -centerY;
    CalibrationHomography fromOrigin;
    fromOrigin.rows[2] = centerX;
    fromOrigin.rows[5] = centerY;
    return Multiply(fromOrigin, Multiply(transform, toOrigin));
}

bool KeyPressedEdge(int key, bool& previous)
{
    const bool current = (GetAsyncKeyState(key) & 0x8000) != 0;
    const bool pressed = current && !previous;
    previous = current;
    return pressed;
}

void PrintMatrix(const CalibrationHomography& value, const char* eye)
{
    std::cout << "[CALIB] " << eye << " inverse homography\n";
    for (int row = 0; row < 3; ++row) {
        std::cout << "  ";
        for (int column = 0; column < 3; ++column) {
            std::cout << value.rows[row * 3 + column]
                      << (column == 2 ? '\n' : ' ');
        }
    }
}

bool UpdateCalibrationFromKeyboard(
    StereoCalibrationDocument& document,
    bool& editRight,
    std::uint32_t width,
    std::uint32_t height)
{
    static bool leftState = false;
    static bool rightState = false;
    static bool upState = false;
    static bool downState = false;
    static bool tabState = false;
    static bool resetState = false;

    const bool left = KeyPressedEdge(VK_LEFT, leftState);
    const bool right = KeyPressedEdge(VK_RIGHT, rightState);
    const bool up = KeyPressedEdge(VK_UP, upState);
    const bool down = KeyPressedEdge(VK_DOWN, downState);
    const bool tab = KeyPressedEdge(VK_TAB, tabState);
    const bool reset = KeyPressedEdge('R', resetState);
    if (tab) {
        editRight = !editRight;
        std::cout << "[CALIB] editing " << (editRight ? "right" : "left")
                  << " eye\n";
    }

    auto& profile = document.profiles.at(document.defaultProfile);
    auto& matrix = editRight ? profile.rightInverse : profile.leftInverse;
    if (reset) {
        matrix = CalibrationHomography{};
        PrintMatrix(matrix, editRight ? "right" : "left");
        return true;
    }
    if (!left && !right && !up && !down) return false;

    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool control = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    CalibrationHomography adjustment;

    if (control) {
        const double degrees = left ? -0.05 : right ? 0.05 : 0.0;
        const double radians = degrees * 3.14159265358979323846 / 180.0;
        adjustment.rows[0] = std::cos(radians);
        adjustment.rows[1] = -std::sin(radians);
        adjustment.rows[3] = std::sin(radians);
        adjustment.rows[4] = std::cos(radians);
        adjustment = AroundCenter(
            adjustment,
            (static_cast<double>(width) - 1.0) * 0.5,
            (static_cast<double>(height) - 1.0) * 0.5);
        matrix = Multiply(adjustment, matrix);
    } else if (shift) {
        double scale = 1.0;
        if (up || right) scale = 1.001;
        if (down || left) scale = 0.999;
        adjustment.rows[0] = scale;
        adjustment.rows[4] = scale;
        adjustment = AroundCenter(
            adjustment,
            (static_cast<double>(width) - 1.0) * 0.5,
            (static_cast<double>(height) - 1.0) * 0.5);
        matrix = Multiply(adjustment, matrix);
    } else if (alt) {
        if (left) adjustment.rows[1] = -0.0005;
        if (right) adjustment.rows[1] = 0.0005;
        if (up) adjustment.rows[3] = -0.0005;
        if (down) adjustment.rows[3] = 0.0005;
        adjustment = AroundCenter(
            adjustment,
            (static_cast<double>(width) - 1.0) * 0.5,
            (static_cast<double>(height) - 1.0) * 0.5);
        matrix = Multiply(adjustment, matrix);
    } else {
        if (left) matrix.rows[2] -= 1.0;
        if (right) matrix.rows[2] += 1.0;
        if (up) matrix.rows[5] -= 1.0;
        if (down) matrix.rows[5] += 1.0;
    }

    PrintMatrix(matrix, editRight ? "right" : "left");
    return true;
}

int PromptExistingCalibration(const std::filesystem::path& path)
{
    const std::wstring message =
        L"Existing calibration JSON was found:\n\n" + path.wstring() +
        L"\n\nRun realtime stereo calibration now?";
    return MessageBoxW(
        nullptr,
        message.c_str(),
        L"Realtime stereo calibration",
        MB_YESNOCANCEL | MB_ICONQUESTION | MB_SETFOREGROUND);
}

void RewriteCalibrationArgument(
    int argc,
    char** argv,
    const std::filesystem::path& calibrationPath,
    std::vector<std::string>& output)
{
    output.clear();
    output.reserve(static_cast<std::size_t>(argc) + 2u);
    bool replaced = false;
    for (int index = 0; index < argc; ++index) {
        const std::string argument = argv[index] ? argv[index] : "";
        if (argument == "--calib" && index + 1 < argc) {
            output.push_back(argument);
            output.push_back(calibrationPath.string());
            ++index;
            replaced = true;
        } else {
            output.push_back(argument);
        }
    }
    if (!replaced) {
        output.push_back("--calib");
        output.push_back(calibrationPath.string());
    }
}

} // namespace

RealtimeCalibrationBootstrapResult RunRealtimeCalibrationBootstrap(
    int argc,
    char** argv,
    const ExperimentOutputLayout& outputLayout)
{
    RealtimeCalibrationBootstrapResult result;
    try {
        ParsedCalibrationArgument calibrationArgument =
            ParseCalibrationArgument(argc, argv);
        if (!calibrationArgument.present) {
            result.ok = true;
            for (int index = 0; index < argc; ++index) {
                result.forwardedArguments.emplace_back(argv[index] ? argv[index] : "");
            }
            return result;
        }

        AppConfig config;
        std::string parseError;
        auto filteredPointers = MutableArguments(calibrationArgument.filtered);
        if (!ParseArguments(
                static_cast<int>(filteredPointers.size()),
                filteredPointers.data(),
                config,
                parseError)) {
            throw std::invalid_argument(parseError);
        }

        std::filesystem::path calibrationPath;
        bool existingCalibration = false;
        if (calibrationArgument.dash) {
            calibrationPath = outputLayout.directory /
                (outputLayout.resolvedProjectName + "_stereo_calibration.json");
        } else {
            calibrationPath = std::filesystem::absolute(calibrationArgument.path);
            existingCalibration = std::filesystem::is_regular_file(calibrationPath);
        }

        D3D12CoreLib::D3D12CoreConfig coreConfig{};
        coreConfig.enableDebugLayer = config.enableD3D12DebugLayer;
        coreConfig.enableInfoQueue = config.enableD3D12DebugLayer;
        coreConfig.enableDred = true;
        coreConfig.createDirectQueue = true;
        coreConfig.createCopyQueue = true;
        auto core = D3D12CoreLib::D3D12Core::CreateShared(coreConfig);
        const auto captureBackend = IC4Ext::D3D12BackendContext::FromCore(core);

        auto rawInput = std::make_shared<IC4Ext::D3D12IndexedFrameQueue>(
            QueueOptions(8));
        auto rawPreviewOutput = std::make_shared<IC4Ext::D3D12SyncedFrameQueue>(
            QueueOptions(1));
        auto calibrationSyncInput =
            std::make_shared<IC4Ext::D3D12IndexedFrameQueue>(QueueOptions(64));

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
        leftCamera.addOutputQueue(0, rawInput);
        leftCamera.addOutputQueue(0, calibrationSyncInput);
        rightCamera.addOutputQueue(1, rawInput);
        rightCamera.addOutputQueue(1, calibrationSyncInput);

        RawPairAssembler rawAssembler(rawInput, rawPreviewOutput);
        rawAssembler.start();
        if (!leftCamera.start()) {
            throw std::runtime_error("left camera failed during calibration bootstrap");
        }
        if (config.cameraStartDelayMs > 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(config.cameraStartDelayMs));
        }
        if (!rightCamera.start()) {
            leftCamera.stopAndJoin();
            throw std::runtime_error("right camera failed during calibration bootstrap");
        }

        ImGuiStereoPreviewConfig previewConfig;
        previewConfig.windowWidth = config.pcPreviewWidth;
        previewConfig.windowHeight = config.pcPreviewHeight;
        previewConfig.vsync = config.pcPreviewVsync;
        previewConfig.windowTitle = "IC4 raw stereo preview - calibration bootstrap";
        ImGuiStereoPreview preview(core, rawPreviewOutput, previewConfig);
        if (!preview.start()) {
            throw std::runtime_error(
                "raw ImGui preview failed: " + preview.lastError());
        }

        std::cout
            << "[CALIB] Raw IC4 preview started before FrameSyncThread creation.\n";

        bool runCalibration = !existingCalibration || calibrationArgument.dash;
        if (existingCalibration) {
            const int answer = PromptExistingCalibration(calibrationPath);
            if (answer == IDCANCEL) {
                result.aborted = true;
            } else {
                runCalibration = answer == IDYES;
            }
        }

        if (result.aborted) {
            preview.stopAndJoin();
            rawAssembler.stop();
            leftCamera.stopAndJoin();
            rightCamera.stopAndJoin();
            result.ok = false;
            return result;
        }

        if (runCalibration) {
            IC4Ext::FrameSyncOptions syncOptions;
            syncOptions.policy = IC4Ext::FrameSyncPolicy::TimestampNearest;
            syncOptions.cameraIndices = {0, 1};
            syncOptions.maxTimestampDiffNs = static_cast<std::uint64_t>(
                std::llround(config.syncToleranceMs * 1'000'000.0));
            syncOptions.maxBufferedFramesPerCamera =
                config.syncBufferedFramesPerCamera;
            syncOptions.timestampSource = config.timestampSource;
            auto calibrationSyncOutput =
                std::make_shared<IC4Ext::D3D12SyncedFrameQueue>(QueueOptions(4));

            calibrationSyncInput->clear();
            IC4Ext::D3D12FrameSyncThread calibrationSyncThread(
                calibrationSyncInput,
                calibrationSyncOutput,
                syncOptions);
            if (!calibrationSyncThread.start()) {
                throw std::runtime_error(
                    "calibration FrameSyncThread failed: " +
                    calibrationSyncThread.lastError().message);
            }
            std::cout
                << "[CALIB] Realtime calibration selected; "
                << "FrameSyncThread created now.\n";

            auto initial = calibrationSyncOutput->waitPopLatestFor(
                std::chrono::milliseconds(config.initialFrameTimeoutMs));
            if (!initial) {
                calibrationSyncThread.stopAndJoin();
                throw std::runtime_error(
                    "timed out waiting for synchronized calibration frame");
            }
            auto current = std::make_shared<IC4Ext::D3D12SyncedFrameSet>(
                std::move(*initial));
            const auto* firstLeft = FindCamera(*current, 0);
            const auto* firstRight = FindCamera(*current, 1);
            if (!firstLeft || !firstRight) {
                calibrationSyncThread.stopAndJoin();
                throw std::runtime_error("incomplete initial calibration pair");
            }
            const std::uint32_t width = static_cast<std::uint32_t>(
                std::max(0, firstLeft->frame.format.width));
            const std::uint32_t height = static_cast<std::uint32_t>(
                std::max(0, firstLeft->frame.format.height));
            if (width == 0 || height == 0 ||
                firstRight->frame.format.width != static_cast<int>(width) ||
                firstRight->frame.format.height != static_cast<int>(height)) {
                calibrationSyncThread.stopAndJoin();
                throw std::runtime_error("invalid calibration frame geometry");
            }

            StereoCalibrationDocument document = existingCalibration
                ? LoadStereoCalibrationJson(calibrationPath)
                : NewCalibrationDocument(width, height);
            document.sourceSize = {width, height};
            document.calibrationInputSize = {width, height};
            document.rectifiedOutputSize = {width, height};

            auto session = std::make_shared<VarjoSession>();
            if (!session->valid() && !session->initialize()) {
                calibrationSyncThread.stopAndJoin();
                throw std::runtime_error(
                    "Varjo session failed for calibration: " +
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
            plane.transform().position = {
                config.planeX,
                config.planeY,
                -config.planeDistanceMeters,
            };
            StereoDisplayTextureRing displayRing(
                core,
                d3dBackend,
                config.displayRingSize);
            std::size_t activeSlot = 0;

            const auto applyPair = [&](const IC4Ext::D3D12SyncedFrameSet& pair) {
                const auto* left = FindCamera(pair, 0);
                const auto* right = FindCamera(pair, 1);
                if (!left || !right) {
                    throw std::runtime_error("incomplete synchronized calibration pair");
                }
                const auto upload = displayRing.upload(left->frame, right->frame);
                activeSlot = upload.slotIndex;
                plane.setTexture(VarjoXR::Eye::Left, upload.left);
                plane.setTexture(VarjoXR::Eye::Right, upload.right);
            };
            applyPair(*current);
            ApplyCalibrationToPlane(plane, document, document.defaultProfile);
            UpdatePlaneAspectFromCalibration(plane, document);

            std::cout
                << "Realtime calibration controls:\n"
                << "  Tab                 : select left/right eye\n"
                << "  Arrow keys          : translate 1 pixel\n"
                << "  Shift + arrows      : uniform scale\n"
                << "  Ctrl + Left/Right   : rotate\n"
                << "  Alt + arrows        : shear\n"
                << "  R                   : reset selected eye\n"
                << "  Q                   : save and finish calibration\n"
                << "  Esc                 : abort application\n";

            bool qState = false;
            bool escState = false;
            bool editRight = true;
            bool finished = false;
            while (!finished) {
                preview.rethrowWorkerExceptionIfAny();
                if (auto latest = calibrationSyncOutput->tryPopLatest()) {
                    current = std::make_shared<IC4Ext::D3D12SyncedFrameSet>(
                        std::move(*latest));
                    applyPair(*current);
                }

                const bool changed = UpdateCalibrationFromKeyboard(
                    document, editRight, width, height);
                if (changed) {
                    ApplyCalibrationToPlane(
                        plane, document, document.defaultProfile);
                }

                if (KeyPressedEdge('Q', qState)) {
                    finished = true;
                }
                if (KeyPressedEdge(VK_ESCAPE, escState)) {
                    result.aborted = true;
                    finished = true;
                }

                space.beginFrame();
                space.render();
                space.endFrame();
                displayRing.markRendered(activeSlot);
            }

            displayRing.waitIdle();
            calibrationSyncThread.stopAndJoin();
            if (!result.aborted) {
                SaveStereoCalibrationJson(calibrationPath, document);
                result.calibrationPerformed = true;
                std::cout << "[CALIB] saved: "
                          << calibrationPath.string() << '\n';
            }
        }

        preview.stopAndJoin();
        rawAssembler.stop();
        leftCamera.stopAndJoin();
        rightCamera.stopAndJoin();
        core->WaitIdle();

        if (result.aborted) {
            result.ok = false;
            return result;
        }

        result.calibrationPath = calibrationPath;
        RewriteCalibrationArgument(
            argc,
            argv,
            calibrationPath,
            result.forwardedArguments);
        result.ok = true;
        return result;
    } catch (const std::exception& exception) {
        result.error = exception.what();
        result.ok = false;
        return result;
    } catch (...) {
        result.error = "unknown realtime calibration bootstrap failure";
        result.ok = false;
        return result;
    }
}

} // namespace DualIC4Varjo
