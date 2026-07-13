#pragma once

#include "CalibrationRuntimeBridge.hpp"
#include "GuiControlBridge.hpp"

#include <VarjoXR/VarjoXR.hpp>

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace DualIC4Varjo::DebugCenterDotPlaneIntegration {
namespace detail {

inline constexpr float kDotDepthOffsetMeters = 0.005f;
inline constexpr float kDotSizeShortAxisRatio = 0.035f;
inline constexpr float kMinimumDotSizeMeters = 0.008f;
inline constexpr float kMaximumDotSizeMeters = 0.050f;

inline const char* DotPixelShaderHlsl() noexcept
{
    return R"hlsl(
float4 main(float2 uv : TEXCOORD0) : SV_TARGET
{
    const float2 centered = uv - float2(0.5f, 0.5f);
    const float radius = 0.5f;
    if (dot(centered, centered) > radius * radius) {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    // High-contrast red debug dot. The alpha is still controlled by tint.a.
    return float4(1.0f, 0.05f, 0.05f, saturate(tint.a));
}
)hlsl";
}

inline bool PlaneHasVisibleAlpha(const VarjoXR::XRPlane& plane) noexcept
{
    return plane.material(VarjoXR::Eye::Left).tint.a > 0.001f ||
           plane.material(VarjoXR::Eye::Right).tint.a > 0.001f;
}

inline void SetDotAlpha(VarjoXR::XRPlane& dotPlane, float alpha) noexcept
{
    dotPlane.setTint({1.0f, 0.0f, 0.0f, std::clamp(alpha, 0.0f, 1.0f)});
}

inline float FiniteClamp(float value, float fallback, float minValue, float maxValue) noexcept
{
    if (!std::isfinite(value)) value = fallback;
    return std::clamp(value, minValue, maxValue);
}

} // namespace detail

inline void Initialize(VarjoXR::XRPlane& dotPlane)
{
    dotPlane.setPixelShaderHLSL(detail::DotPixelShaderHlsl());
    detail::SetDotAlpha(dotPlane, 0.0f);
}

inline void SyncToPostProcessCenter(
    const VarjoXR::XRPlane& videoPlane,
    VarjoXR::XRPlane& dotPlane) noexcept
{
    try {
        if (!GuiControlBridge::PostProcessDebugCenterDotVisible() ||
            !GuiControlBridge::PlaneVisible() ||
            GuiControlBridge::ApplicationExitRequested() ||
            !detail::PlaneHasVisibleAlpha(videoPlane)) {
            detail::SetDotAlpha(dotPlane, 0.0f);
            return;
        }

        const glm::vec2 videoSize = videoPlane.size();
        if (videoSize.x <= 0.0f || videoSize.y <= 0.0f) {
            detail::SetDotAlpha(dotPlane, 0.0f);
            return;
        }

        const auto runtimeConfig = CalibrationRuntimeBridge::GetPostProcessRuntimeConfig();
        const auto& settings = runtimeConfig.settings;
        const float centerX01 = detail::FiniteClamp(settings.centerX01, 0.5f, 0.0f, 1.0f);
        const float centerY01 = detail::FiniteClamp(settings.centerY01, 0.5f, 0.0f, 1.0f);

        const float shortAxis = std::min(videoSize.x, videoSize.y);
        const float dotSize = std::clamp(
            shortAxis * detail::kDotSizeShortAxisRatio,
            detail::kMinimumDotSizeMeters,
            detail::kMaximumDotSizeMeters);

        // Convert texture UV to Plane-local meters. In VarjoXR's quad convention,
        // uv.y=0 is local +Y and uv.y=1 is local -Y.
        const float localX = (centerX01 - 0.5f) * videoSize.x;
        const float localY = (0.5f - centerY01) * videoSize.y;

        dotPlane.setPlacementMode(videoPlane.placementMode());
        dotPlane.setSize({dotSize, dotSize});
        dotPlane.transform() = videoPlane.transform();
        dotPlane.transform().position +=
            dotPlane.transform().rotation *
            glm::vec3(localX, localY, detail::kDotDepthOffsetMeters);

        detail::SetDotAlpha(dotPlane, 1.0f);
    } catch (...) {
        detail::SetDotAlpha(dotPlane, 0.0f);
    }
}

} // namespace DualIC4Varjo::DebugCenterDotPlaneIntegration
