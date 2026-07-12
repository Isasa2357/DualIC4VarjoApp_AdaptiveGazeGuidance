#pragma once

#include "AppConfig.hpp"

#include <VarjoXR/VarjoXR.hpp>

#include <filesystem>
#include <string>

namespace DualIC4Varjo::PersonalizationSettings {

// Loads personalization/<name>.json into AppConfig. If --name is omitted,
// AppConfig::personalizationName is DEFAULT. Missing JSON files are accepted;
// the current/default Plane state will be written at normal process exit.
bool LoadIntoConfig(AppConfig& config, std::string& error);

// Enables saving the most recently captured Plane state at normal process exit.
// Call only after command-line parsing and validation have succeeded.
void EnableAutoSave() noexcept;

// Called from the Varjo render thread after GUI/keyboard Plane input has been
// applied. This is intentionally lightweight and stores only the current Plane
// pose/size for the exit-time JSON write.
void CapturePlaneState(const VarjoXR::XRPlane& plane) noexcept;

// Explicit save hook. Normal execution uses the atexit hook installed by
// EnableAutoSave(), but this is useful for future direct shutdown paths.
void SaveNow() noexcept;

std::filesystem::path ActivePath();
std::string ActiveName();

} // namespace DualIC4Varjo::PersonalizationSettings
