#pragma once

#include <IC4Ext/IC4Ext.hpp>

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

// Application-local compatibility wrapper for robust two-camera startup.
//
// It provides the D3D12CameraCaptureThread API used by the application while:
//  1. retrying non-strict IC4 JSON state application after removing properties
//     explicitly reported by IC4 as unavailable, and
//  2. pausing acquisition on the first camera until the second camera has also
//     completed stream setup.
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
        rebuildUnderlying();
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
        return openWithUnavailablePropertyRetry();
    }

    bool start()
    {
        lastError_ = NoError();
        if (!openWithUnavailablePropertyRetry()) return false;
        if (!thread_->start()) {
            lastError_ = thread_->lastError();
            return false;
        }

        // IC4Ext uses Immediate stream setup because Deferred is unreliable on
        // the DFK 33UX252 pair. Stop acquisition immediately after each camera
        // has opened, then restart both cameras only when both are ready.
        if (!thread_->stopAcquisition()) {
            lastError_ = thread_->lastError();
            thread_->stopAndJoin();
            return false;
        }

        std::lock_guard<std::mutex> lock(coordinatorMutex());
        auto& pending = pendingCameras();
        pending.erase(
            std::remove(pending.begin(), pending.end(), this),
            pending.end());
        pending.push_back(this);

        std::cout
            << "[IC4][STARTUP] deviceIndex=" << selector_.deviceIndex
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
        if (!second->thread_->startAcquisition()) {
            second->lastError_ = second->thread_->lastError();
            first->thread_->stopAcquisition();
            lastError_ = MakeError(
                ErrorCode::IC4Error,
                "CoordinatedD3D12CameraCaptureThread::start / second acquisition",
                "second camera acquisition restart failed: " +
                    second->lastError_.where + ": " +
                    second->lastError_.message);
            return false;
        }

        std::cout
            << "[IC4][STARTUP] both camera streams configured; "
            << "acquisition started for both cameras\n";
        return true;
    }

    void requestStop()
    {
        if (thread_) thread_->requestStop();
    }

    void join()
    {
        if (thread_) thread_->join();
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
    }

    void addOutputQueue(
        std::uint32_t cameraIndex,
        std::shared_ptr<D3D12IndexedFrameQueue> queue)
    {
        if (!queue) return;
        outputs_.push_back({cameraIndex, queue});
        thread_->addOutputQueue(cameraIndex, std::move(queue));
    }

    std::size_t removeOutputQueue(
        std::uint32_t cameraIndex,
        const std::shared_ptr<D3D12IndexedFrameQueue>& queue)
    {
        outputs_.erase(
            std::remove_if(
                outputs_.begin(),
                outputs_.end(),
                [&](const OutputBinding& binding) {
                    return binding.cameraIndex == cameraIndex &&
                        binding.queue == queue;
                }),
            outputs_.end());
        return thread_->removeOutputQueue(cameraIndex, queue);
    }

    std::size_t clearOutputQueues()
    {
        outputs_.clear();
        return thread_->clearOutputQueues();
    }

    std::size_t outputQueueCount() const
    {
        return thread_ ? thread_->outputQueueCount() : 0;
    }

    bool startAcquisition()
    {
        const bool ok = thread_ && thread_->startAcquisition();
        if (!ok && thread_) lastError_ = thread_->lastError();
        return ok;
    }

    bool stopAcquisition()
    {
        const bool ok = thread_ && thread_->stopAcquisition();
        if (!ok && thread_) lastError_ = thread_->lastError();
        return ok;
    }

    bool isStreaming() const noexcept
    {
        return thread_ && thread_->isStreaming();
    }

    bool isAcquisitionActive() const noexcept
    {
        return thread_ && thread_->isAcquisitionActive();
    }

    CameraThreadStats stats() const
    {
        return thread_ ? thread_->stats() : CameraThreadStats{};
    }

    const ErrorInfo& lastError() const noexcept
    {
        return lastError_ ? lastError_ : thread_->lastError();
    }

    bool applyIC4StateJson(
        const std::filesystem::path& jsonPath,
        std::size_t deviceIndex = 0,
        bool strict = false,
        bool applyNestedSelectorStates = true)
    {
        config_.ic4StateJson.path = jsonPath;
        config_.ic4StateJson.deviceIndex = deviceIndex;
        config_.ic4StateJson.strict = strict;
        config_.ic4StateJson.applyNestedSelectorStates =
            applyNestedSelectorStates;
        const bool ok = thread_->applyIC4StateJson(
            jsonPath,
            deviceIndex,
            strict,
            applyNestedSelectorStates);
        if (!ok) lastError_ = thread_->lastError();
        return ok;
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
            text.find("not available") == std::string::npos &&
            text.find("INode::is_available() == false") ==
                std::string::npos) {
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
            std::to_string(selector_.deviceIndex) + "_" +
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
            << "[IC4][WARN] deviceIndex=" << selector_.deviceIndex
            << " skipped unavailable JSON property '" << property
            << "' and will retry camera setup\n";
        return true;
    }

    void rebuildUnderlying()
    {
        thread_ = std::make_unique<D3D12CameraCaptureThread>(
            selector_,
            config_,
            backend_,
            options_);
        for (const auto& binding : outputs_) {
            thread_->addOutputQueue(binding.cameraIndex, binding.queue);
        }
    }

    bool openWithUnavailablePropertyRetry()
    {
        constexpr int kMaximumRetries = 8;
        for (int attempt = 0; attempt <= kMaximumRetries; ++attempt) {
            if (thread_->open()) {
                lastError_ = NoError();
                return true;
            }

            lastError_ = thread_->lastError();
            if (config_.ic4StateJson.strict ||
                !config_.ic4StateJson.enabled()) {
                return false;
            }

            const auto property = unavailableProperty(lastError_);
            if (!property || !writeFilteredJson(*property)) {
                return false;
            }

            thread_->stopAndJoin();
            rebuildUnderlying();
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
    std::unique_ptr<D3D12CameraCaptureThread> thread_;
    std::vector<OutputBinding> outputs_;
    std::vector<std::filesystem::path> temporaryJsonFiles_;
    ErrorInfo lastError_;
};

} // namespace IC4Ext
