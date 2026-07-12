#pragma once

#include "GuiControlBridge.hpp"

#include <VarjoXR/VarjoXR.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace DualIC4Varjo::BlackCirclePlaneIntegration {
namespace detail {

inline constexpr float kCircleDepthOffsetMeters = 0.01f;

inline const char* CirclePixelShaderHlsl() noexcept
{
    return R"hlsl(
float4 main(float2 uv : TEXCOORD0) : SV_TARGET
{
    const float2 centered = uv - float2(0.5f, 0.5f);
    const float2 rectHalfSize = max(params0.xy, float2(0.0f, 0.0f));

    // The real video Plane is drawn by another VarjoXR Plane. Even if this
    // circle Plane is submitted after it, keep the middle rectangle transparent
    // so the visible result is a black filled circle with the video rectangle
    // cut out.
    if (abs(centered.x) <= rectHalfSize.x && abs(centered.y) <= rectHalfSize.y) {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    const float radius = 0.5f;
    if (dot(centered, centered) > radius * radius) {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    return float4(0.0f, 0.0f, 0.0f, saturate(tint.a));
}
)hlsl";
}

inline bool PlaneHasVisibleAlpha(const VarjoXR::XRPlane& plane) noexcept
{
    return plane.material(VarjoXR::Eye::Left).tint.a > 0.001f ||
           plane.material(VarjoXR::Eye::Right).tint.a > 0.001f;
}

inline void SetCircleAlpha(VarjoXR::XRPlane& circlePlane, float alpha) noexcept
{
    circlePlane.setTint({0.0f, 0.0f, 0.0f, std::clamp(alpha, 0.0f, 1.0f)});
}

} // namespace detail

inline void Initialize(VarjoXR::XRPlane& circlePlane)
{
    circlePlane.setPixelShaderHLSL(detail::CirclePixelShaderHlsl());
    detail::SetCircleAlpha(circlePlane, 0.0f);
    std::cout
        << "[BLACK_CIRCLE] VarjoXR circle Plane initialized; "
        << "circle is black, alpha outside circle/inside video rect is zero\n";
}

inline void SyncToVideoPlane(
    const VarjoXR::XRPlane& videoPlane,
    VarjoXR::XRPlane& circlePlane) noexcept
{
    try {
        const glm::vec2 videoSize = videoPlane.size();
        if (videoSize.x <= 0.0f || videoSize.y <= 0.0f) {
            detail::SetCircleAlpha(circlePlane, 0.0f);
            return;
        }

        const float diameter = std::hypot(videoSize.x, videoSize.y);
        if (!std::isfinite(diameter) || diameter <= 0.0f) {
            detail::SetCircleAlpha(circlePlane, 0.0f);
            return;
        }

        circlePlane.setPlacementMode(videoPlane.placementMode());
        circlePlane.setSize({diameter, diameter});
        circlePlane.transform() = videoPlane.transform();

        // The video Plane should be visually in front. Move the circle a little
        // farther along the Plane local -Z direction. Depth is not relied on for
        // the rectangular cutout; the shader also makes that area transparent.
        circlePlane.transform().position +=
            circlePlane.transform().rotation *
            glm::vec3(0.0f, 0.0f, -detail::kCircleDepthOffsetMeters);

        const glm::vec2 halfCutoutUv{
            std::clamp(videoSize.x / (2.0f * diameter), 0.0f, 0.5f),
            std::clamp(videoSize.y / (2.0f * diameter), 0.0f, 0.5f),
        };
        circlePlane.material(VarjoXR::Eye::Left).params0 = {
            halfCutoutUv.x,
            halfCutoutUv.y,
            0.0f,
            0.0f,
        };
        circlePlane.material(VarjoXR::Eye::Right).params0 = {
            halfCutoutUv.x,
            halfCutoutUv.y,
            0.0f,
            0.0f,
        };

        const bool visible = GuiControlBridge::PlaneVisible() &&
            detail::PlaneHasVisibleAlpha(videoPlane) &&
            !GuiControlBridge::ApplicationExitRequested();
        detail::SetCircleAlpha(circlePlane, visible ? 1.0f : 0.0f);
    } catch (...) {
        detail::SetCircleAlpha(circlePlane, 0.0f);
    }
}

} // namespace DualIC4Varjo::BlackCirclePlaneIntegration
