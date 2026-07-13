#pragma once

#include "CalibrationRuntimeBridge.hpp"

#include <iostream>

namespace DualIC4Varjo::PostProcessDefaultOverrides {

inline void ApplyOnce()
{
    static bool applied = false;
    if (applied) return;
    applied = true;

    auto config = CalibrationRuntimeBridge::GetPostProcessRuntimeConfig();
    if (!config.settings.enabled ||
        config.settings.mode == StereoPostProcessMode::None) {
        return;
    }

    config.settings.radiusShortAxis01 = 0.1f;
    config.settings.radiusXShortAxis01 = 0.1f;
    config.settings.radiusYShortAxis01 = 0.1f;

    if (config.settings.mode == StereoPostProcessMode::Blur) {
        // In the current HLSL, blurRadiusPixels is the parameter that directly
        // controls the visual blur spread. blurStrength01 is already saturated
        // at 1.0, so doubling the visible blur means doubling this radius.
        config.settings.blurRadiusPixels = 8.0f;
        config.settings.blurSigmaPixels = 4.0f;
        config.settings.blurStrength01 = 1.0f;
    }

    CalibrationRuntimeBridge::SetPostProcessRuntimeConfig(config);

    std::cout
        << "[POSTPROCESS] defaults overridden: radius_x_short_axis=0.1, radius_y_short_axis=0.1";
    if (config.settings.mode == StereoPostProcessMode::Blur) {
        std::cout
            << ", blur_radius_px=8.0, blur_sigma_px=4.0";
    }
    std::cout << '\n';
}

} // namespace DualIC4Varjo::PostProcessDefaultOverrides
