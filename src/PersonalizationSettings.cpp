#include "PersonalizationSettings.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>

namespace DualIC4Varjo::PersonalizationSettings {
namespace {

struct PlaneState {
    float x = 0.0f;
    float y = 0.0f;
    float distance = 1.6f;
    float width = 1.0f;
    float height = 0.0f;
};

std::mutex gMutex;
bool gConfigured = false;
bool gAutoSaveEnabled = false;
std::string gName = "DEFAULT";
std::filesystem::path gDirectory = "personalization";
std::filesystem::path gPath = gDirectory / "DEFAULT.json";
PlaneState gLatest{};
std::once_flag gAtexitFlag;

std::string NormalizeName(std::string value)
{
    if (value.empty()) value = "DEFAULT";
    return value;
}

bool IsSafeProfileName(const std::string& value) noexcept
{
    if (value.empty() || value == "." || value == "..") return false;
    const std::filesystem::path path(value);
    if (path.is_absolute() || path.has_parent_path() || path.has_extension()) return false;
    constexpr char invalid[] = "<>:\"/\\|?*";
    return value.find_first_of(invalid) == std::string::npos;
}

PlaneState PlaneStateFromConfig(const AppConfig& config) noexcept
{
    PlaneState state;
    state.x = config.planeX;
    state.y = config.planeY;
    state.distance = config.planeDistanceMeters;
    state.width = config.planeWidthMeters;
    state.height = config.planeHeightMeters;
    return state;
}

bool ReadFloat(
    const nlohmann::json& object,
    const char* key,
    float& output,
    std::string& error)
{
    if (!object.contains(key)) return true;
    const auto& value = object.at(key);
    if (!value.is_number()) {
        error = std::string("personalization value must be numeric: ") + key;
        return false;
    }
    output = value.get<float>();
    return true;
}

bool ApplyJsonToConfig(
    const nlohmann::json& document,
    AppConfig& config,
    std::string& error)
{
    const nlohmann::json* plane = nullptr;
    if (document.contains("plane")) {
        if (!document.at("plane").is_object()) {
            error = "personalization field 'plane' must be an object";
            return false;
        }
        plane = &document.at("plane");
    } else if (document.is_object()) {
        // Also accept a flat object for quick hand editing.
        plane = &document;
    }
    if (!plane) return true;

    if (!ReadFloat(*plane, "x_m", config.planeX, error)) return false;
    if (!ReadFloat(*plane, "y_m", config.planeY, error)) return false;
    if (!ReadFloat(*plane, "distance_m", config.planeDistanceMeters, error)) return false;
    if (!ReadFloat(*plane, "width_m", config.planeWidthMeters, error)) return false;
    if (!ReadFloat(*plane, "height_m", config.planeHeightMeters, error)) return false;

    // Backward-compatible aliases for manually created files.
    if (!ReadFloat(*plane, "x", config.planeX, error)) return false;
    if (!ReadFloat(*plane, "y", config.planeY, error)) return false;
    if (!ReadFloat(*plane, "distance", config.planeDistanceMeters, error)) return false;
    if (!ReadFloat(*plane, "width", config.planeWidthMeters, error)) return false;
    if (!ReadFloat(*plane, "height", config.planeHeightMeters, error)) return false;

    return true;
}

bool ValidatePlaneConfig(const AppConfig& config, std::string& error)
{
    if (config.planeWidthMeters <= 0.0f ||
        config.planeDistanceMeters <= 0.0f ||
        config.planeHeightMeters < 0.0f) {
        error =
            "personalization Plane values are invalid: width and distance must be positive; height must be zero or positive";
        return false;
    }
    return true;
}

void SaveAtExit()
{
    SaveNow();
}

} // namespace

bool LoadIntoConfig(AppConfig& config, std::string& error)
{
    const std::string name = NormalizeName(config.personalizationName);
    if (!IsSafeProfileName(name)) {
        error = "--name must be a single safe file stem without path, extension, or Windows-invalid characters";
        return false;
    }

    const std::filesystem::path directory = "personalization";
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        error = "failed to create personalization directory: " + ec.message();
        return false;
    }

    const std::filesystem::path path = directory / (name + ".json");
    config.personalizationName = name;

    {
        std::lock_guard<std::mutex> lock(gMutex);
        gConfigured = true;
        gName = name;
        gDirectory = directory;
        gPath = path;
        gLatest = PlaneStateFromConfig(config);
    }

    if (std::filesystem::is_regular_file(path, ec)) {
        try {
            std::ifstream input(path);
            if (!input) {
                error = "failed to open personalization file: " + path.string();
                return false;
            }
            nlohmann::json document;
            input >> document;
            if (!ApplyJsonToConfig(document, config, error)) return false;
            if (!ValidatePlaneConfig(config, error)) return false;
            {
                std::lock_guard<std::mutex> lock(gMutex);
                gLatest = PlaneStateFromConfig(config);
            }
            std::cout << "[PERSONALIZATION] loaded " << path.string() << '\n';
        } catch (const std::exception& exception) {
            error = "failed to parse personalization file " + path.string() + ": " + exception.what();
            return false;
        }
    } else {
        std::cout
            << "[PERSONALIZATION] " << path.string()
            << " not found; current defaults will be saved on exit\n";
    }

    return true;
}

void EnableAutoSave() noexcept
{
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (!gConfigured) return;
        gAutoSaveEnabled = true;
    }
    std::call_once(gAtexitFlag, []() { std::atexit(&SaveAtExit); });
}

void CapturePlaneState(const VarjoXR::XRPlane& plane) noexcept
{
    try {
        const auto position = plane.transform().position;
        const auto size = plane.size();
        PlaneState state;
        state.x = position.x;
        state.y = position.y;
        state.distance = (std::max)(0.0f, -position.z);
        state.width = size.x;
        state.height = size.y;
        std::lock_guard<std::mutex> lock(gMutex);
        if (gConfigured) gLatest = state;
    } catch (...) {
    }
}

void SaveNow() noexcept
{
    try {
        std::filesystem::path directory;
        std::filesystem::path path;
        std::string name;
        PlaneState state;
        {
            std::lock_guard<std::mutex> lock(gMutex);
            if (!gConfigured || !gAutoSaveEnabled) return;
            directory = gDirectory;
            path = gPath;
            name = gName;
            state = gLatest;
        }

        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        if (ec) {
            std::cerr
                << "[PERSONALIZATION] failed to create directory "
                << directory.string() << ": " << ec.message() << '\n';
            return;
        }

        nlohmann::json document;
        document["name"] = name;
        document["plane"] = {
            {"x_m", state.x},
            {"y_m", state.y},
            {"distance_m", state.distance},
            {"width_m", state.width},
            {"height_m", state.height},
        };

        std::ofstream output(path, std::ios::out | std::ios::trunc);
        if (!output) {
            std::cerr
                << "[PERSONALIZATION] failed to write "
                << path.string() << '\n';
            return;
        }
        output << std::setw(2) << document << '\n';
        std::cout << "[PERSONALIZATION] saved " << path.string() << '\n';
    } catch (const std::exception& exception) {
        std::cerr
            << "[PERSONALIZATION] save failed: "
            << exception.what() << '\n';
    } catch (...) {
        std::cerr << "[PERSONALIZATION] save failed: unknown error\n";
    }
}

std::filesystem::path ActivePath()
{
    std::lock_guard<std::mutex> lock(gMutex);
    return gPath;
}

std::string ActiveName()
{
    std::lock_guard<std::mutex> lock(gMutex);
    return gName;
}

} // namespace DualIC4Varjo::PersonalizationSettings
