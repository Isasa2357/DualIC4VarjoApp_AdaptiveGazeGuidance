#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "RawStereoNvencRecordingIntegration.hpp"
#include "VarjoFadeOutPostProcess.hpp"

#include <VarjoXR/VarjoXR.hpp>

#include <Windows.h>

#include <atomic>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace DualIC4Varjo::FadeOutPostProcessIntegration {

class Manager final {
public:
    static Manager& instance()
    {
        static Manager value;
        return value;
    }

    void registerRuntime(
        std::shared_ptr<varjo_Session> session,
        ID3D12CommandQueue* commandQueue)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        session_ = std::move(session);
        commandQueue_ = commandQueue;
    }

    bool initializeAfterCalibration()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (postProcess_) return true;
        if (!session_) {
            lastError_ = "fade-out postprocess runtime session was not registered";
            return false;
        }
        if (!commandQueue_) {
            lastError_ = "fade-out postprocess runtime D3D12 queue was not registered";
            return false;
        }

        VarjoFadeOutPostProcess::Config config{};
        config.fadeSeconds = 2.0f;
        auto postProcess = std::make_unique<VarjoFadeOutPostProcess>(
            session_,
            commandQueue_,
            config);
        if (!postProcess->initialize()) {
            lastError_ = postProcess->lastError();
            return false;
        }

        postProcess_ = std::move(postProcess);
        lastError_.clear();
        std::cout << "[FADE] VST fade-out postprocess created after calibration\n";
        return true;
    }

    bool startEscFadeOut()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdownReady_.load(std::memory_order_acquire)) return true;
        if (fadeStarted_.load(std::memory_order_acquire)) return true;
        if (!postProcess_) {
            lastError_ = "fade-out postprocess was not initialized after calibration";
            shutdownReady_.store(true, std::memory_order_release);
            return false;
        }

        planeTransparentRequested_.store(true, std::memory_order_release);
        if (!postProcess_->startFadeOut()) {
            lastError_ = postProcess_->lastError();
            shutdownReady_.store(true, std::memory_order_release);
            return false;
        }

        fadeStarted_.store(true, std::memory_order_release);
        std::cout
            << "[FADE] Esc detected; Plane hidden and VST fade-out started (2.0 s)\n";
        return true;
    }

    void applyPlaneVisibility(VarjoXR::XRPlane& plane)
    {
        if (!planeTransparentRequested_.load(std::memory_order_acquire)) return;
        static thread_local bool applied = false;
        if (applied) return;
        plane.setTint({1.0f, 1.0f, 1.0f, 0.0f});
        applied = true;
        std::cout << "[FADE] Varjo Plane made transparent for shutdown fade\n";
    }

    SHORT handleEscapeGetAsyncKeyState()
    {
        if (!postProcess_) {
            return ::GetAsyncKeyState(VK_ESCAPE);
        }

        if (shutdownReady_.load(std::memory_order_acquire)) {
            return static_cast<SHORT>(0x8000);
        }

        const bool physicalEscDown = (::GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        if (physicalEscDown && !fadeStarted_.load(std::memory_order_acquire)) {
            if (!startEscFadeOut()) {
                std::cerr << "[FADE] failed to start fade-out: " << lastError() << '\n';
            }
            return 0;
        }

        if (fadeStarted_.load(std::memory_order_acquire)) {
            bool completed = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                completed = postProcess_ && postProcess_->completed();
            }
            if (completed) {
                shutdownReady_.store(true, std::memory_order_release);
                std::cout << "[FADE] fade-out completed; shutdown may proceed\n";
                return static_cast<SHORT>(0x8000);
            }
            return 0;
        }

        return ::GetAsyncKeyState(VK_ESCAPE);
    }

    void waitForFadeCompletion()
    {
        std::unique_ptr<VarjoFadeOutPostProcess>* unused = nullptr;
        (void)unused;
        std::lock_guard<std::mutex> lock(waitMutex_);
        if (!fadeStarted_.load(std::memory_order_acquire)) return;
        VarjoFadeOutPostProcess* postProcess = nullptr;
        {
            std::lock_guard<std::mutex> lock2(mutex_);
            postProcess = postProcess_.get();
        }
        if (postProcess) postProcess->waitForCompletion();
        shutdownReady_.store(true, std::memory_order_release);
    }

    void stop() noexcept
    {
        std::unique_ptr<VarjoFadeOutPostProcess> postProcess;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            postProcess = std::move(postProcess_);
        }
        if (postProcess) {
            postProcess->stop();
        }
    }

    std::string lastError() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastError_;
    }

private:
    Manager() = default;

    mutable std::mutex mutex_;
    std::mutex waitMutex_;
    std::shared_ptr<varjo_Session> session_;
    ID3D12CommandQueue* commandQueue_ = nullptr;
    std::unique_ptr<VarjoFadeOutPostProcess> postProcess_;
    std::atomic_bool fadeStarted_{false};
    std::atomic_bool shutdownReady_{false};
    std::atomic_bool planeTransparentRequested_{false};
    std::string lastError_;
};

inline void RegisterRuntime(
    std::shared_ptr<varjo_Session> session,
    ID3D12CommandQueue* commandQueue)
{
    Manager::instance().registerRuntime(std::move(session), commandQueue);
}

inline bool InitializeAfterCalibration()
{
    return Manager::instance().initializeAfterCalibration();
}

inline void ApplyPlaneFadeVisibility(VarjoXR::XRPlane& plane)
{
    Manager::instance().applyPlaneVisibility(plane);
}

inline SHORT GetAsyncKeyState(int virtualKey)
{
    if (virtualKey == VK_ESCAPE) {
        return Manager::instance().handleEscapeGetAsyncKeyState();
    }
    return ::GetAsyncKeyState(virtualKey);
}

class FadeOutRenderedFrameMetadataLogger final {
public:
    bool start(const std::filesystem::path& path)
    {
        if (!inner_.start(path)) return false;
        if (!InitializeAfterCalibration()) {
            integrationError_ = Manager::instance().lastError();
            inner_.stop();
            return false;
        }
        return true;
    }

    bool enqueue(RenderedFrameMetadataRow row)
    {
        return inner_.enqueue(std::move(row));
    }

    void stop()
    {
        Manager::instance().waitForFadeCompletion();
        inner_.stop();
        Manager::instance().stop();
    }

    std::string lastError() const
    {
        if (!integrationError_.empty()) return integrationError_;
        return inner_.lastError();
    }

private:
    RecordingRenderedFrameMetadataLogger inner_;
    std::string integrationError_;
};

} // namespace DualIC4Varjo::FadeOutPostProcessIntegration

namespace DualIC4Varjo {
using FadeOutRenderedFrameMetadataLogger =
    FadeOutPostProcessIntegration::FadeOutRenderedFrameMetadataLogger;
}
