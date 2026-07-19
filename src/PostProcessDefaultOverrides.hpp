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
    if (config.settings.mode == StereoPostProcessMode::None) {
        return;
    }

    config.settings.radiusShortAxis01 = 0.2f;
    config.settings.radiusXShortAxis01 = 0.2f;
    config.settings.radiusYShortAxis01 = 0.15f;
    config.settings.outsideBrightness = 0.25f; // darken strength = 0.75
    config.settings.blurRadiusPixels = 6.0f;
    config.settings.blurSigmaPixels = 3.0f;

    // The command line selects which postprocess mode is available, but the GUI
    // checkbox controls whether it is applied. Start with it unchecked so the
    // first visible state is the unprocessed image.
    config.settings.enabled = false;

    CalibrationRuntimeBridge::SetPostProcessRuntimeConfig(config);

    std::cout
        << "[POSTPROCESS] defaults overridden: radius_x_short_axis=0.2, "
        << "radius_y_short_axis=0.15, darken_strength=0.75, "
        << "blur_radius_px=6.0, apply_at_start=0\n";
}

} // namespace DualIC4Varjo::PostProcessDefaultOverrides
