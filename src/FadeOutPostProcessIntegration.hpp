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
#include <VarjoToolkit/Core/VarjoFrameInfo.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace DualIC4Varjo::FadeOutPostProcessIntegration {
namespace detail {

inline glm::mat4 Mat4FromArray(const double values[16]) noexcept
{
    glm::mat4 out(1.0f);
    for (int i = 0; i < 16; ++i) {
        glm::value_ptr(out)[i] = static_cast<float>(values[i]);
    }
    return out;
}

inline glm::mat4 ComputeHeadMatrix(const VarjoFrameInfoSnapshot& snapshot) noexcept
{
    glm::mat4 head(1.0f);
    glm::vec3 positionSum(0.0f);
    int count = 0;
    bool rotationSet = false;
    glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);

    for (const auto& view : snapshot.views) {
        const glm::mat4 eyePose = glm::inverse(Mat4FromArray(view.viewMatrix));
        positionSum += glm::vec3(eyePose[3]);
        if (!rotationSet) {
            rotation = glm::quat_cast(eyePose);
            rotationSet = true;
        }
        ++count;
    }

    if (count > 0) {
        const glm::vec3 position = positionSum / static_cast<float>(count);
        head = glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(rotation);
    }
    return head;
}

inline glm::mat4 PlaneLocalMatrix(const VarjoXR::XRPlane& plane)
{
    VarjoXR::Transform transform = plane.transform();
    transform.scale.x *= plane.size().x;
    transform.scale.y *= plane.size().y;
    return transform.matrix();
}

inline glm::mat4 PlaneWorldMatrix(
    const VarjoXR::XRPlane& plane,
    const VarjoFrameInfoSnapshot& snapshot)
{
    const glm::mat4 local = PlaneLocalMatrix(plane);
    if (plane.placementMode() == VarjoXR::PlacementMode::HeadRelative) {
        return ComputeHeadMatrix(snapshot) * local;
    }
    return local;
}

inline VarjoFadeOutPostProcess::PlaneMaskRects ProjectPlaneRects(
    const VarjoXR::XRPlane& plane,
    const VarjoFrameInfoSnapshot& snapshot)
{
    VarjoFadeOutPostProcess::PlaneMaskRects rects{};
    if (!snapshot.valid || snapshot.views.empty()) {
        return rects;
    }

    const glm::mat4 world = PlaneWorldMatrix(plane, snapshot);
    const glm::vec4 corners[] = {
        {-0.5f, -0.5f, 0.0f, 1.0f},
        {-0.5f,  0.5f, 0.0f, 1.0f},
        { 0.5f,  0.5f, 0.0f, 1.0f},
        { 0.5f, -0.5f, 0.0f, 1.0f},
    };

    const std::size_t viewCount = std::min<std::size_t>(snapshot.views.size(), rects.size());
    for (std::size_t viewIndex = 0; viewIndex < viewCount; ++viewIndex) {
        const auto& view = snapshot.views[viewIndex];
        const glm::mat4 viewMatrix = Mat4FromArray(view.viewMatrix);
        const glm::mat4 projectionMatrix = Mat4FromArray(view.projectionMatrix);
        const glm::mat4 mvp = projectionMatrix * viewMatrix * world;

        float minX = std::numeric_limits<float>::max();
        float minY = std::numeric_limits<float>::max();
        float maxX = -std::numeric_limits<float>::max();
        float maxY = -std::numeric_limits<float>::max();
        bool valid = true;

        for (const glm::vec4& corner : corners) {
            const glm::vec4 clip = mvp * corner;
            if (!std::isfinite(clip.w) || clip.w <= 0.0001f) {
                valid = false;
                break;
            }
            const float ndcX = clip.x / clip.w;
            const float ndcY = clip.y / clip.w;
            if (!std::isfinite(ndcX) || !std::isfinite(ndcY)) {
                valid = false;
                break;
            }

            const float u = ndcX * 0.5f + 0.5f;
            const float v = 0.5f - ndcY * 0.5f;
            minX = std::min(minX, u);
            minY = std::min(minY, v);
            maxX = std::max(maxX, u);
            maxY = std::max(maxY, v);
        }

        if (!valid) continue;

        // Keep the mask tight to the projected Plane corners. Earlier versions
        // used a 0.01 normalized margin, which was visibly too large in Varjo
        // focus/context postprocess output.
        constexpr float kMaskMargin01 = 0.0f;
        rects[viewIndex].x0 = std::clamp(minX - kMaskMargin01, 0.0f, 1.0f);
        rects[viewIndex].y0 = std::clamp(minY - kMaskMargin01, 0.0f, 1.0f);
        rects[viewIndex].x1 = std::clamp(maxX + kMaskMargin01, 0.0f, 1.0f);
        rects[viewIndex].y1 = std::clamp(maxY + kMaskMargin01, 0.0f, 1.0f);
    }

    return rects;
}

} // namespace detail

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
        std::cout << "[FADE] runtime registered for VST fade-out postprocess\n";
    }

    bool initializeAfterCalibration()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        postCalibrationActive_.store(true, std::memory_order_release);
        if (postProcess_) return true;
        if (!session_) {
            lastError_ = "video postprocess runtime session was not registered";
            std::cerr << "[VST_POSTPROCESS] initialization skipped: " << lastError_ << '\n';
            return false;
        }
        if (!commandQueue_) {
            lastError_ = "video postprocess runtime D3D12 queue was not registered";
            std::cerr << "[VST_POSTPROCESS] initialization skipped: " << lastError_ << '\n';
            return false;
        }

        std::cout << "[VST_POSTPROCESS] initializing video postprocess\n";
        VarjoFadeOutPostProcess::Config config{};
        config.fadeSeconds = 2.0f;
        auto postProcess = std::make_unique<VarjoFadeOutPostProcess>(
            session_,
            commandQueue_,
            config);
        if (!postProcess->initialize()) {
            lastError_ = postProcess->lastError();
            std::cerr << "[VST_POSTPROCESS] initialization failed: " << lastError_ << '\n';
            return false;
        }

        postProcess_ = std::move(postProcess);
        lastError_.clear();
        std::cout
            << "[VST_POSTPROCESS] post-calibration shader ready; "
            << "Plane outside region will become green, Esc switches to fade-out\n";
        return true;
    }

    void updatePlaneMask(const VarjoFadeOutPostProcess::PlaneMaskRects& rects)
    {
        if (!postCalibrationActive_.load(std::memory_order_acquire)) return;
        if (fadeStarted_.load(std::memory_order_acquire)) return;

        VarjoFadeOutPostProcess* postProcess = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            postProcess = postProcess_.get();
        }
        if (!postProcess) {
            if (!initializeAfterCalibration()) return;
            std::lock_guard<std::mutex> lock(mutex_);
            postProcess = postProcess_.get();
        }
        if (!postProcess) return;

        if (!postProcess->updateGreenOutsidePlane(rects)) {
            const std::string error = postProcess->lastError();
            std::lock_guard<std::mutex> lock(mutex_);
            lastError_ = error;
            if (!planeMaskWarningPrinted_) {
                planeMaskWarningPrinted_ = true;
                std::cerr
                    << "[VST_POSTPROCESS] failed to update green Plane mask: "
                    << lastError_ << '\n';
            }
            return;
        }

        if (!planeMaskRectPrinted_) {
            planeMaskRectPrinted_ = true;
            const auto& r0 = rects[0];
            const auto& r1 = rects[1];
            std::cout
                << "[VST_POSTPROCESS] initial Plane mask rects "
                << "view0=(" << r0.x0 << ',' << r0.y0 << ',' << r0.x1 << ',' << r0.y1 << ") "
                << "view1=(" << r1.x0 << ',' << r1.y0 << ',' << r1.x1 << ',' << r1.y1 << ")\n";
        }
    }

    bool startEscFadeOut()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (shutdownReady_.load(std::memory_order_acquire)) return true;
            if (fadeStarted_.load(std::memory_order_acquire)) return true;
        }

        if (!initializeAfterCalibration()) {
            shutdownReady_.store(true, std::memory_order_release);
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdownReady_.load(std::memory_order_acquire)) return true;
        if (fadeStarted_.load(std::memory_order_acquire)) return true;
        if (!postProcess_) {
            lastError_ = "video postprocess was not initialized after calibration";
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
        const SHORT physicalState = ::GetAsyncKeyState(VK_ESCAPE);
        const bool physicalEscDown = (physicalState & 0x8000) != 0;

        if (shutdownReady_.load(std::memory_order_acquire)) {
            return static_cast<SHORT>(0x8000);
        }

        if (physicalEscDown && !fadeStarted_.load(std::memory_order_acquire)) {
            if (!startEscFadeOut()) {
                std::cerr << "[FADE] failed to start fade-out: " << lastError()
                          << " ; falling back to immediate shutdown\n";
                return physicalState;
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

        return physicalState;
    }

    void waitForFadeCompletion()
    {
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
    std::atomic_bool postCalibrationActive_{false};
    std::atomic_bool fadeStarted_{false};
    std::atomic_bool shutdownReady_{false};
    std::atomic_bool planeTransparentRequested_{false};
    bool planeMaskWarningPrinted_ = false;
    bool planeMaskRectPrinted_ = false;
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

inline void UpdatePlaneMaskFromFrame(
    const VarjoXR::XRPlane& plane,
    const VarjoFrameInfoSnapshot& snapshot)
{
    Manager::instance().updatePlaneMask(detail::ProjectPlaneRects(plane, snapshot));
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
            startupWarning_ = Manager::instance().lastError();
            std::cerr
                << "[VST_POSTPROCESS] warning: postprocess is not ready yet: "
                << startupWarning_
                << " ; recording/rendering will continue and render thread will retry Plane mask updates\n";
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
        return inner_.lastError();
    }

private:
    RecordingRenderedFrameMetadataLogger inner_;
    std::string startupWarning_;
};

} // namespace DualIC4Varjo::FadeOutPostProcessIntegration

namespace DualIC4Varjo {
using FadeOutRenderedFrameMetadataLogger =
    FadeOutPostProcessIntegration::FadeOutRenderedFrameMetadataLogger;
}