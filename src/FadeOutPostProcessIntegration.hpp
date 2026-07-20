#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "CalibrationRuntimeBridge.hpp"
#include "GuiControlBridge.hpp"
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
    for (int index = 0; index < 16; ++index) {
        glm::value_ptr(out)[index] = static_cast<float>(values[index]);
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

inline VarjoFadeOutPostProcess::PlaneMaskRects FullScreenPassThroughRects() noexcept
{
    VarjoFadeOutPostProcess::PlaneMaskRects rects{};
    for (auto& rect : rects) {
        rect.x0 = 0.0f;
        rect.y0 = 0.0f;
        rect.x1 = 1.0f;
        rect.y1 = 1.0f;
    }
    return rects;
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

    const std::size_t viewCount =
        std::min<std::size_t>(snapshot.views.size(), rects.size());
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

        // The Plane mask intentionally has no extra margin. Its four projected
        // corners must match the actual Plane bounds in context/focus views.
        rects[viewIndex].x0 = std::clamp(minX, 0.0f, 1.0f);
        rects[viewIndex].y0 = std::clamp(minY, 0.0f, 1.0f);
        rects[viewIndex].x1 = std::clamp(maxX, 0.0f, 1.0f);
        rects[viewIndex].y1 = std::clamp(maxY, 0.0f, 1.0f);
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
        std::cout << "[VST_POSTPROCESS] runtime registered\n";
    }

    bool initializeWhenPlaneAvailable()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (postProcess_) return true;
        if (!session_) {
            lastError_ = "video postprocess runtime session was not registered";
            return false;
        }
        if (!commandQueue_) {
            lastError_ = "video postprocess runtime D3D12 queue was not registered";
            return false;
        }

        std::cout
            << "[VST_POSTPROCESS] initializing when the Plane becomes visible\n";
        VarjoFadeOutPostProcess::Config config{};
        config.fadeSeconds = 2.0f;
        auto postProcess = std::make_unique<VarjoFadeOutPostProcess>(
            session_,
            commandQueue_,
            config);
        if (!postProcess->initialize()) {
            lastError_ = postProcess->lastError();
            std::cerr
                << "[VST_POSTPROCESS] initialization failed: "
                << lastError_ << '\n';
            return false;
        }

        postProcess_ = std::move(postProcess);
        lastError_.clear();
        std::cout
            << "[VST_POSTPROCESS] Plane-time blur+darken shader ready; "
            << "Esc/GUI/Ctrl+C switches to full-screen fade-out\n";
        return true;
    }

    void suspendPlaneMaskForCalibration()
    {
        if (fadeStarted_.load(std::memory_order_acquire)) return;

        std::unique_ptr<VarjoFadeOutPostProcess> postProcess;
        bool shouldLog = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            postProcess = std::move(postProcess_);
            planeMaskWarningPrinted_ = false;
            planeMaskRectPrinted_ = false;
            hiddenPassThroughLogged_ = false;
            if (!calibrationSuspendedLogged_) {
                calibrationSuspendedLogged_ = true;
                shouldLog = true;
            }
        }

        if (postProcess) postProcess->stop();
        if (shouldLog) {
            std::cout
                << "[VST_POSTPROCESS] suspended during checkerboard calibration; "
                << "Plane-time VST blur/darken will resume after calibration\n";
        }
    }

    void updatePlaneMask(const VarjoFadeOutPostProcess::PlaneMaskRects& rects)
    {
        if (fadeStarted_.load(std::memory_order_acquire)) return;

        if (!initializeWhenPlaneAvailable()) {
            printMaskWarningOnce();
            return;
        }

        VarjoFadeOutPostProcess* postProcess = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            postProcess = postProcess_.get();
        }
        if (!postProcess) return;

        const bool visible = GuiControlBridge::PlaneVisible();
        const auto appliedRects = visible
            ? rects
            : detail::FullScreenPassThroughRects();

        if (!postProcess->updateGreenOutsidePlane(appliedRects)) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                lastError_ = postProcess->lastError();
            }
            printMaskWarningOnce();
            return;
        }

        if (!planeMaskRectPrinted_ && visible) {
            planeMaskRectPrinted_ = true;
            const auto& left = appliedRects[0];
            const auto& right = appliedRects[1];
            std::cout
                << "[VST_POSTPROCESS] initial Plane mask rects "
                << "view0=(" << left.x0 << ',' << left.y0 << ','
                << left.x1 << ',' << left.y1 << ") "
                << "view1=(" << right.x0 << ',' << right.y0 << ','
                << right.x1 << ',' << right.y1 << ")\n";
        }

        if (!visible && !hiddenPassThroughLogged_) {
            hiddenPassThroughLogged_ = true;
            std::cout
                << "[VST_POSTPROCESS] Plane hidden; VST postprocess is pass-through\n";
        } else if (visible) {
            hiddenPassThroughLogged_ = false;
        }
    }

    bool startFadeOut(const char* trigger)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (shutdownReady_.load(std::memory_order_acquire)) return true;
            if (fadeStarted_.load(std::memory_order_acquire)) return true;
        }

        if (CalibrationRuntimeBridge::gCalibrationActive.load(
                std::memory_order_acquire)) {
            lastError_ = "VST fade-out is disabled during checkerboard calibration";
            shutdownReady_.store(true, std::memory_order_release);
            std::cerr
                << "[FADE] " << (trigger ? trigger : "shutdown")
                << " requested during calibration; VST fade-out skipped\n";
            return false;
        }

        if (!initializeWhenPlaneAvailable()) {
            shutdownReady_.store(true, std::memory_order_release);
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdownReady_.load(std::memory_order_acquire)) return true;
        if (fadeStarted_.load(std::memory_order_acquire)) return true;
        if (!postProcess_) {
            lastError_ = "video postprocess was not initialized";
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
            << "[FADE] " << (trigger ? trigger : "shutdown")
            << " requested; Plane hidden and VST blur+darken fade-out started (2.0 s)\n";
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

    bool shutdownReady()
    {
        if (shutdownReady_.load(std::memory_order_acquire)) return true;
        if (!fadeStarted_.load(std::memory_order_acquire)) return false;

        bool completed = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            completed = postProcess_ && postProcess_->completed();
        }
        if (completed) {
            const bool wasReady = shutdownReady_.exchange(
                true,
                std::memory_order_acq_rel);
            if (!wasReady) {
                std::cout << "[FADE] fade-out completed; shutdown may proceed\n";
            }
            return true;
        }
        return false;
    }

    SHORT handleEscapeGetAsyncKeyState()
    {
        const SHORT physicalState = ::GetAsyncKeyState(VK_ESCAPE);
        const bool physicalEscDown = (physicalState & 0x8000) != 0;
        const bool externalExitRequested =
            GuiControlBridge::ApplicationExitRequested();

        if (shutdownReady()) {
            return static_cast<SHORT>(0x8000);
        }

        if ((physicalEscDown || externalExitRequested) &&
            !fadeStarted_.load(std::memory_order_acquire)) {
            const char* trigger = physicalEscDown
                ? "Esc"
                : "GUI/console";
            if (!startFadeOut(trigger)) {
                std::cerr
                    << "[FADE] failed to start fade-out: "
                    << lastError()
                    << " ; falling back to immediate shutdown\n";
                return static_cast<SHORT>(0x8000);
            }
            return 0;
        }

        if (fadeStarted_.load(std::memory_order_acquire)) {
            return shutdownReady() ? static_cast<SHORT>(0x8000) : 0;
        }

        return physicalState;
    }

    void waitForFadeCompletion()
    {
        std::lock_guard<std::mutex> waitLock(waitMutex_);
        if (!fadeStarted_.load(std::memory_order_acquire)) return;

        VarjoFadeOutPostProcess* postProcess = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            postProcess = postProcess_.get();
        }
        if (postProcess) postProcess->waitForCompletion();
        shutdownReady();
    }

    void stop() noexcept
    {
        std::unique_ptr<VarjoFadeOutPostProcess> postProcess;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            postProcess = std::move(postProcess_);
        }
        if (postProcess) postProcess->stop();
    }

    std::string lastError() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastError_;
    }

private:
    Manager() = default;

    void printMaskWarningOnce()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (planeMaskWarningPrinted_) return;
        planeMaskWarningPrinted_ = true;
        std::cerr
            << "[VST_POSTPROCESS] failed to update Plane blur+darken mask: "
            << lastError_ << '\n';
    }

    mutable std::mutex mutex_;
    std::mutex waitMutex_;
    std::shared_ptr<varjo_Session> session_;
    ID3D12CommandQueue* commandQueue_ = nullptr;
    std::unique_ptr<VarjoFadeOutPostProcess> postProcess_;
    std::atomic_bool fadeStarted_{false};
    std::atomic_bool shutdownReady_{false};
    std::atomic_bool planeTransparentRequested_{false};
    bool planeMaskWarningPrinted_ = false;
    bool planeMaskRectPrinted_ = false;
    bool hiddenPassThroughLogged_ = false;
    bool calibrationSuspendedLogged_ = false;
    std::string lastError_;
};

inline void RegisterRuntime(
    std::shared_ptr<varjo_Session> session,
    ID3D12CommandQueue* commandQueue)
{
    Manager::instance().registerRuntime(std::move(session), commandQueue);
}

inline bool InitializeWhenPlaneAvailable()
{
    return Manager::instance().initializeWhenPlaneAvailable();
}

// Compatibility alias for the logger integration. The render thread normally
// initializes the shader earlier, on the first Plane frame.
inline bool InitializeAfterCalibration()
{
    return InitializeWhenPlaneAvailable();
}

inline void UpdatePlaneMaskFromFrame(
    const VarjoXR::XRPlane& plane,
    const VarjoFrameInfoSnapshot& snapshot)
{
    if (CalibrationRuntimeBridge::gCalibrationActive.load(
            std::memory_order_acquire)) {
        Manager::instance().suspendPlaneMaskForCalibration();
        return;
    }

    Manager::instance().updatePlaneMask(detail::ProjectPlaneRects(plane, snapshot));
}

inline void ApplyPlaneFadeVisibility(VarjoXR::XRPlane& plane)
{
    Manager::instance().applyPlaneVisibility(plane);
}

inline bool RequestGracefulShutdown(const char* trigger)
{
    return Manager::instance().startFadeOut(trigger);
}

inline bool ShutdownReady()
{
    return Manager::instance().shutdownReady();
}

inline void WaitForFadeCompletion()
{
    Manager::instance().waitForFadeCompletion();
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
        if (!InitializeWhenPlaneAvailable()) {
            startupWarning_ = Manager::instance().lastError();
            std::cerr
                << "[VST_POSTPROCESS] warning: postprocess is not ready yet: "
                << startupWarning_
                << " ; rendering continues and the Plane-mask hook will retry\n";
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
} // namespace DualIC4Varjo
