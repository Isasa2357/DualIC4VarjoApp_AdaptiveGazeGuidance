#pragma once

#include <VarjoToolkit/Core/VarjoFrameInfo.hpp>
#include <VarjoXR/Core/XRPlane.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cmath>
#include <mutex>
#include <optional>

namespace DualIC4Varjo {

// One measured binocular ray for rendering. The logging queue retains its own samples.
class BlurWaveGaze {
public:
    void publish(const varjo_Gaze& gaze)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!latest_ || gaze.captureTime >= latest_->captureTime) latest_ = gaze;
    }

    std::optional<glm::vec2> planeUv(
        const VarjoXR::XRPlane& plane, const VarjoFrameInfoSnapshot& frame) const
    {
        std::optional<varjo_Gaze> sample;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sample = latest_;
        }
        // captureTime and predicted displayTime use the same Varjo nanosecond clock.
        constexpr varjo_Nanoseconds maxAgeNs = 100'000'000;
        if (!sample || !frame.valid || sample->status != varjo_GazeStatus_Valid ||
            sample->captureTime <= 0 || sample->captureTime > frame.displayTime ||
            frame.displayTime - sample->captureTime > maxAgeNs) return std::nullopt;

        const auto& ray = sample->gaze;
        // Varjo gaze is +Z forward, while XRPlane uses -Z forward.
        glm::dvec4 origin(ray.origin[0], ray.origin[1], -ray.origin[2], 1.0);
        glm::dvec4 direction(ray.forward[0], ray.forward[1], -ray.forward[2], 0.0);
        if (plane.placementMode() == VarjoXR::PlacementMode::World) {
            if (!frame.centerPoseValid) return std::nullopt;
            const auto headToWorld = glm::make_mat4(frame.centerPose.value);
            origin = headToWorld * origin;
            direction = headToWorld * direction;
        }
        const auto toLocal = glm::inverse(glm::dmat4(plane.transform().matrix()));
        origin = toLocal * origin;
        direction = toLocal * direction;
        if (std::abs(direction.z) < 1e-9) return std::nullopt;
        const double t = -origin.z / direction.z;
        const glm::vec2 size = plane.size();
        if (!(t > 0.0 && size.x > 0.f && size.y > 0.f)) return std::nullopt;
        const auto hit = origin + t * direction;
        const glm::vec2 uv(0.5 + hit.x / size.x, 0.5 - hit.y / size.y);
        if (!std::isfinite(uv.x) || !std::isfinite(uv.y)) return std::nullopt;
        return uv;
    }

private:
    mutable std::mutex mutex_;
    std::optional<varjo_Gaze> latest_;
};

} // namespace DualIC4Varjo
