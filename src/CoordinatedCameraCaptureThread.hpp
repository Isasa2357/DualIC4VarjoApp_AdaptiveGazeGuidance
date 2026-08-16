#pragma once

#include "IC4ExtV2SharedPipeline.hpp"

#include <nlohmann/json.hpp>

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace IC4Ext {

// Application-local wrapper retaining the existing application lifecycle while
// using the IC4Ext v2 immutable-frame capture path. Every camera publishes to
// the one central ReadOnly ingress queue owned by AppV2Bridge; addOutputQueue()
// now records only logical consumer bindings and never causes a GPU texture copy.
class CoordinatedD3D12CameraCaptureThread {
public:
    CoordinatedD3D12CameraCaptureThread(
        IC4DeviceSelector selector,
        CameraCaptureConfig config,
        D3D12BackendContext backend,
        CameraThreadOptions options = {})
        : selector_(std::move(selector))
        , config_(std::move(config))
        , backend_(backend)
        , options_(options)
    {
        // Serial-only selection is enforced by AppConfig. Keep this defensive
        // guard here so a direct construction cannot silently fall back to an
        // IC4 device index or unique name either.
        selector_.deviceIndex = -1;
        selector_.uniqueName.clear();
        applyHardwareTriggerSyncOverride();
    }

    ~CoordinatedD3D12CameraCaptureThread()
    {
        stopAndJoin();
        cleanupTemporaryJsonFiles();
    }

    CoordinatedD3D12CameraCaptureThread(
        const CoordinatedD3D12CameraCaptureThread&) = delete;
    CoordinatedD3D12CameraCaptureThread& operator=(
        const CoordinatedD3D12CameraCaptureThread&) = delete;

    bool open()
    {
        lastError_ = NoError();
        if (!ensureUnderlying()) return false;
        return openWithUnavailablePropertyRetry();
    }

    bool start()
    {
        lastError_ = NoError();
        if (!ensureUnderlying()) return false;
        if (!openWithUnavailablePropertyRetry()) return false;

        const auto ingress =
            AppV2Bridge::SharedReadOnlyPipelineCoordinator::instance().ingress();
        if (!ingress) {
            lastError_ = MakeError(
                ErrorCode::InvalidArgument,
                "CoordinatedD3D12CameraCaptureThread::start",
                "central IC4Ext v2 FrameSync ingress is not initialized; start the sync output before the cameras");
            return false;
        }
        thread_->setOutputQueue(ingress);

        if (!thread_->start()) {
            lastError_ = thread_->lastError();
            return false;
        }
        acquisitionActive_ = true;

        // Preserve the existing paired startup behavior: both streams are fully
        // opened before acquisition is allowed to run. The resulting frames are
        // still published through one shared IC4Ext v2 ingress queue.
        if (!thread_->stopAcquisition()) {
            lastError_ = thread_->lastError();
            thread_->stopAndJoin();
            acquisitionActive_ = false;
            return false;
        }
        acquisitionActive_ = false;

        std::lock_guard<std::mutex> lock(coordinatorMutex());
        auto& pending = pendingCameras();
        pending.erase(
            std::remove(pending.begin(), pending.end(), this),
            pending.end());
        pending.push_back(this);

        std::cout
            << "[IC4][STARTUP] serial=" << selector_.serial
            << " stream configured; acquisition paused ("
            << pending.size() << "/2 ready)\n";

        if (pending.size() < 2) return true;

        CoordinatedD3D12CameraCaptureThread* first = pending[0];
        CoordinatedD3D12CameraCaptureThread* second = pending[1];
        pending.clear();

        if (!first->thread_->startAcquisition()) {
            first->lastError_ = first->thread_->lastError();
            lastError_ = MakeError(
                ErrorCode::IC4Error,
                "CoordinatedD3D12CameraCaptureThread::start / first acquisition",
                "first camera acquisition restart failed: " +
                    first->lastError_.where + ": " +
                    first->lastError_.message);
            return false;
        }
        first->acquisitionActive_ = true;

        if (!second->thread_->startAcquisition()) {
            second->lastError_ = second->thread_->lastError();
            first->thread_->stopAcquisition();
            first->acquisitionActive_ = false;
            lastError_ = MakeError(
                ErrorCode::IC4Error,
                "CoordinatedD3D12CameraCaptureThread::start / second acquisition",
                "second camera acquisition restart failed: " +
                    second->lastError_.where + ": " +
                    second->lastError_.message);
            return false;
        }
        second->acquisitionActive_ = true;

        std::cout
            << "[IC4][STARTUP] both camera streams configured; "
            << "acquisition started for both cameras on shared ReadOnly ingress\n";
        return true;
    }

    void requestStop()
    {
        if (thread_) thread_->requestStop();
    }

    void join()
    {
        if (thread_) thread_->join();
        acquisitionActive_ = false;
    }

    void stopAndJoin()
    {
        {
            std::lock_guard<std::mutex> lock(coordinatorMutex());
            auto& pending = pendingCameras();
            pending.erase(
                std::remove(pending.begin(), pending.end(), this),
                pending.end());
        }
        if (thread_) thread_->stopAndJoin();
        acquisitionActive_ = false;
    }

    void addOutputQueue(
        std::uint32_t cameraIndex,
        std::shared_ptr<D3D12IndexedFrameQueue> queue)
    {
        if (!queue) return;
        if (!cameraId_) {
            cameraId_ = cameraIndex;
        } else if (*cameraId_ != cameraIndex) {
            lastError_ = MakeError(
                ErrorCode::InvalidArgument,
                "CoordinatedD3D12CameraCaptureThread::addOutputQueue",
                "one physical camera cannot be bound to multiple logical camera IDs");
            return;
        }

        const auto duplicate = std::find_if(
            outputs_.begin(),
            outputs_.end(),
            [&](const OutputBinding& binding) {
                return binding.cameraIndex == cameraIndex &&
                       binding.queue == queue;
            });
        if (duplicate == outputs_.end()) {
            outputs_.push_back({cameraIndex, std::move(queue)});
        }
    }

    std::size_t removeOutputQueue(
        std::uint32_t cameraIndex,
        const std::shared_ptr<D3D12IndexedFrameQueue>& queue)
    {
        const auto before = outputs_.size();
        outputs_.erase(
            std::remove_if(
                outputs_.begin(),
                outputs_.end(),
                [&](const OutputBinding& binding) {
                    return binding.cameraIndex == cameraIndex &&
                           binding.queue == queue;
                }),
            outputs_.end());
        return before - outputs_.size();
    }

    std::size_t clearOutputQueues()
    {
        const auto count = outputs_.size();
        outputs_.clear();
        return count;
    }

    std::size_t outputQueueCount() const
    {
        return outputs_.size();
    }

    bool startAcquisition()
    {
        const bool ok = thread_ && thread_->startAcquisition();
        if (!ok && thread_) lastError_ = thread_->lastError();
        acquisitionActive_ = ok;
        return ok;
    }

    bool stopAcquisition()
    {
        const bool ok = thread_ && thread_->stopAcquisition();
        if (!ok && thread_) lastError_ = thread_->lastError();
        if (ok) acquisitionActive_ = false;
        return ok;
    }

    bool isStreaming() const noexcept
    {
        return thread_ && thread_->isRunning();
    }

    bool isAcquisitionActive() const noexcept
    {
        return acquisitionActive_;
    }

    CameraThreadStats stats() const
    {
        CameraThreadStats result;
        if (!thread_) return result;
        const auto native = thread_->stats();
        result.readFrames = native.readFrames;
        result.readTimeouts = native.readTimeouts;
        result.readErrors = native.readErrors;
        result.pushedFrames = native.pushedFrames;
        result.pushFailures = native.pushFailures;
        result.copiedFrames = 0;
        result.copyFailures = 0;
        result.noOutputDrops = native.noOutputDrops;
        return result;
    }

    const ErrorInfo& lastError() const noexcept
    {
        return lastError_;
    }

    bool applyIC4StateJson(
        const std::filesystem::path& jsonPath,
        std::size_t deviceIndex = 0,
        bool strict = false,
        bool applyNestedSelectorStates = true)
    {
        if (thread_ && thread_->isRunning()) {
            lastError_ = MakeError(
                ErrorCode::InvalidArgument,
                "CoordinatedD3D12CameraCaptureThread::applyIC4StateJson",
                "camera state cannot be replaced while the v2 capture worker is running");
            return false;
        }
        config_.ic4StateJson.path = jsonPath;
        config_.ic4StateJson.deviceIndex = deviceIndex;
        config_.ic4StateJson.strict = strict;
        config_.ic4StateJson.applyNestedSelectorStates =
            applyNestedSelectorStates;
        thread_.reset();
        return ensureUnderlying();
    }

private:
    struct OutputBinding {
        std::uint32_t cameraIndex = 0;
        std::shared_ptr<D3D12IndexedFrameQueue> queue;
    };

    static std::mutex& coordinatorMutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    static std::vector<CoordinatedD3D12CameraCaptureThread*>& pendingCameras()
    {
        static std::vector<CoordinatedD3D12CameraCaptureThread*> cameras;
        return cameras;
    }

    static std::optional<std::string> unavailableProperty(
        const ErrorInfo& error)
    {
        const std::string text = error.where + " " + error.message;
        if (text.find("not available") == std::string::npos &&
            text.find("INode::is_available() == false") == std::string::npos) {
            return std::nullopt;
        }

        const auto open = text.find('(');
        const auto close = open == std::string::npos
            ? std::string::npos
            : text.find(')', open + 1);
        if (open == std::string::npos ||
            close == std::string::npos ||
            close <= open + 1) {
            return std::nullopt;
        }
        return text.substr(open + 1, close - open - 1);
    }

    static bool erasePropertyRecursive(
        nlohmann::json& value,
        const std::string& property)
    {
        bool erased = false;
        if (value.is_object()) {
            erased = value.erase(property) != 0;
            for (auto& item : value.items()) {
                erased = erasePropertyRecursive(item.value(), property) || erased;
            }
        } else if (value.is_array()) {
            for (auto& item : value) {
                erased = erasePropertyRecursive(item, property) || erased;
            }
        }
        return erased;
    }

    bool writeFilteredJson(const std::string& property)
    {
        if (!config_.ic4StateJson.enabled()) return false;

        std::ifstream input(config_.ic4StateJson.path);
        if (!input) return false;
        nlohmann::json root;
        try {
            input >> root;
        } catch (...) {
            return false;
        }

        if (!erasePropertyRecursive(root, property)) return false;

        const auto unique = std::to_string(GetCurrentProcessId()) + "_" +
            std::to_string(cameraId_.value_or(0)) + "_" +
            std::to_string(temporaryJsonFiles_.size());
        const auto path = std::filesystem::temp_directory_path() /
            ("DualIC4Varjo_filtered_state_" + unique + ".json");
        std::ofstream output(path, std::ios::out | std::ios::trunc);
        if (!output) return false;
        output << root.dump(2) << '\n';
        if (!output) return false;

        temporaryJsonFiles_.push_back(path);
        config_.ic4StateJson.path = path;
        std::cout
            << "[IC4][WARN] serial=" << selector_.serial
            << " skipped unavailable JSON property '" << property
            << "' and will retry camera setup\n";
        return true;
    }

    void applyHardwareTriggerSyncOverride()
    {
        constexpr const char* triggerSource = "Line1";
        constexpr const char* triggerSelector = "FrameStart";
        constexpr const char* triggerActivation = "RisingEdge";

        ConfigureHardwareTriggerSync(
            config_,
            triggerSource,
            triggerSelector,
            triggerActivation);

        std::cout
            << "[IC4][TRIGGER] serial=" << selector_.serial
            << " hardware trigger requested: TriggerSelector="
            << triggerSelector
            << ", TriggerMode=On, TriggerSource=" << triggerSource
            << ", TriggerActivation=" << triggerActivation << '\n';
    }

    bool ensureUnderlying()
    {
        if (thread_) return true;
        if (selector_.serial.empty()) {
            lastError_ = MakeError(
                ErrorCode::InvalidArgument,
                "CoordinatedD3D12CameraCaptureThread::ensureUnderlying",
                "camera serial ID is required");
            return false;
        }
        if (!cameraId_) {
            lastError_ = MakeError(
                ErrorCode::InvalidArgument,
                "CoordinatedD3D12CameraCaptureThread::ensureUnderlying",
                "logical camera ID is unknown; addOutputQueue must be called before camera startup");
            return false;
        }

        D3D12::CameraCaptureOptions captureOptions;
        captureOptions.initialFramePoolCapacity = 8;
        captureOptions.maxFramePoolCapacity = 32;
        captureOptions.framePoolExhaustionPolicy =
            D3D12::FramePoolExhaustionPolicy::DropNewest;
        captureOptions.framePoolWaitTimeout = std::chrono::milliseconds(5);

        D3D12::CameraCaptureThreadOptions threadOptions;
        threadOptions.readTimeoutMs = options_.readTimeoutMs;
        threadOptions.stopOnReadError = options_.stopOnReadError;

        thread_ = std::make_unique<D3D12::CameraCaptureThread>(
            *cameraId_,
            selector_,
            config_,
            backend_,
            captureOptions,
            threadOptions);
        return true;
    }

    bool openWithUnavailablePropertyRetry()
    {
        constexpr int kMaximumRetries = 8;
        for (int attempt = 0; attempt <= kMaximumRetries; ++attempt) {
            if (!thread_ && !ensureUnderlying()) return false;
            if (thread_->open()) {
                lastError_ = NoError();
                return true;
            }

            lastError_ = thread_->lastError();
            if (config_.ic4StateJson.strict || !config_.ic4StateJson.enabled()) {
                return false;
            }

            const auto property = unavailableProperty(lastError_);
            if (!property || !writeFilteredJson(*property)) {
                return false;
            }

            thread_->stopAndJoin();
            thread_.reset();
            if (!ensureUnderlying()) return false;
        }
        return false;
    }

    void cleanupTemporaryJsonFiles() noexcept
    {
        for (const auto& path : temporaryJsonFiles_) {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
        temporaryJsonFiles_.clear();
    }

    IC4DeviceSelector selector_;
    CameraCaptureConfig config_;
    D3D12BackendContext backend_;
    CameraThreadOptions options_;
    std::optional<D3D12::CameraId> cameraId_;
    std::unique_ptr<D3D12::CameraCaptureThread> thread_;
    std::vector<OutputBinding> outputs_;
    std::vector<std::filesystem::path> temporaryJsonFiles_;
    ErrorInfo lastError_;
    bool acquisitionActive_ = false;
};

} // namespace IC4Ext
