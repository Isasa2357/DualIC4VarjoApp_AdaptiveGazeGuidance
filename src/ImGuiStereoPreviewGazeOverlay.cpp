#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "GazeOnCameraFrameService.hpp"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace DualIC4Varjo::ImGuiGazeOverlay {
namespace {

using Clock = std::chrono::steady_clock;

struct SideSample {
    bool valid = false;
    bool inside = false;
    float x01 = std::numeric_limits<float>::quiet_NaN();
    float y01 = std::numeric_limits<float>::quiet_NaN();
};

struct GazeSample {
    SideSample left;
    SideSample right;
    std::uint64_t rowCount = 0;
    std::int64_t rowUnixUs = 0;
};

std::vector<std::string> SplitComma(const std::string& line)
{
    std::vector<std::string> result;
    std::size_t start = 0;
    for (;;) {
        const std::size_t comma = line.find(',', start);
        if (comma == std::string::npos) {
            result.push_back(line.substr(start));
            return result;
        }
        result.push_back(line.substr(start, comma - start));
        start = comma + 1;
    }
}

float ParseFloat(const std::string& text)
{
    if (text.empty() || text == "nan" || text == "nullopt") {
        return std::numeric_limits<float>::quiet_NaN();
    }
    char* end = nullptr;
    const float value = std::strtof(text.c_str(), &end);
    if (!end || end == text.c_str()) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    return value;
}

std::int64_t ParseI64(const std::string& text)
{
    if (text.empty() || text == "nan" || text == "nullopt") return 0;
    char* end = nullptr;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if (!end || end == text.c_str()) return 0;
    return static_cast<std::int64_t>(value);
}

bool ParseBool01(const std::string& text)
{
    return text == "1" || text == "true" || text == "TRUE";
}

class CsvTailReader {
public:
    void refresh()
    {
        const auto now = Clock::now();
        if (now - lastRefresh_ < std::chrono::milliseconds(15)) return;
        lastRefresh_ = now;

        const auto path = GazeOnCameraFrameHook::outputPath();
        if (path.empty()) return;
        if (path != path_) reset(path);

        std::ifstream input(path_, std::ios::binary);
        if (!input) return;
        input.seekg(0, std::ios::end);
        const std::streamoff end = static_cast<std::streamoff>(input.tellg());
        if (end < 0) return;
        if (offset_ > end) {
            offset_ = 0;
            pending_.clear();
            header_.clear();
            headerParsed_ = false;
        }
        if (end == offset_) return;

        input.seekg(offset_, std::ios::beg);
        std::string chunk(static_cast<std::size_t>(end - offset_), '\0');
        input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        offset_ = end;
        pending_ += chunk;
        consumeCompleteLines();
    }

    const std::optional<GazeSample>& latest() const noexcept { return latest_; }

private:
    void reset(std::filesystem::path path)
    {
        path_ = std::move(path);
        offset_ = 0;
        pending_.clear();
        header_.clear();
        headerParsed_ = false;
        latest_.reset();
        rowCount_ = 0;
    }

    void consumeCompleteLines()
    {
        for (;;) {
            const std::size_t newline = pending_.find('\n');
            if (newline == std::string::npos) return;
            std::string line = pending_.substr(0, newline);
            pending_.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if (!headerParsed_) parseHeader(line);
            else parseRow(line);
        }
    }

    void parseHeader(const std::string& line)
    {
        const auto columns = SplitComma(line);
        header_.clear();
        for (std::size_t index = 0; index < columns.size(); ++index) {
            header_[columns[index]] = index;
        }
        headerParsed_ = true;
    }

    std::optional<std::size_t> column(const char* name) const
    {
        const auto it = header_.find(name);
        if (it == header_.end()) return std::nullopt;
        return it->second;
    }

    std::string value(
        const std::vector<std::string>& columns,
        const char* name) const
    {
        const auto index = column(name);
        if (!index || *index >= columns.size()) return {};
        return columns[*index];
    }

    SideSample parseSide(
        const std::vector<std::string>& columns,
        const char* prefix) const
    {
        SideSample side;
        const std::string xName = std::string(prefix) + "_camera_x01";
        const std::string yName = std::string(prefix) + "_camera_y01";
        const std::string insideName = std::string(prefix) + "_inside_frame";
        side.x01 = ParseFloat(value(columns, xName.c_str()));
        side.y01 = ParseFloat(value(columns, yName.c_str()));
        side.inside = ParseBool01(value(columns, insideName.c_str()));
        side.valid = side.inside &&
            std::isfinite(side.x01) && std::isfinite(side.y01) &&
            side.x01 >= 0.0f && side.x01 <= 1.0f &&
            side.y01 >= 0.0f && side.y01 <= 1.0f;
        return side;
    }

    void parseRow(const std::string& line)
    {
        const auto columns = SplitComma(line);
        GazeSample sample;
        sample.rowUnixUs = ParseI64(value(columns, "row_unix_us"));
        sample.left = parseSide(columns, "gaze_left");
        sample.right = parseSide(columns, "gaze_right");
        sample.rowCount = ++rowCount_;
        latest_ = sample;
    }

    std::filesystem::path path_;
    std::streamoff offset_ = 0;
    std::string pending_;
    std::unordered_map<std::string, std::size_t> header_;
    bool headerParsed_ = false;
    std::optional<GazeSample> latest_;
    std::uint64_t rowCount_ = 0;
    Clock::time_point lastRefresh_{};
};

CsvTailReader& Reader()
{
    static CsvTailReader reader;
    return reader;
}

int& ImageSideIndex()
{
    static thread_local int value = 0;
    return value;
}

void DrawGazePoint(
    const SideSample& side,
    const ImVec2& imageMin,
    const ImVec2& imageSize)
{
    if (!side.valid) return;
    const ImVec2 center(
        imageMin.x + side.x01 * imageSize.x,
        imageMin.y + side.y01 * imageSize.y);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    constexpr float radius = 8.0f;
    draw->AddCircleFilled(center, radius + 3.0f, IM_COL32(0, 0, 0, 220), 32);
    draw->AddCircle(center, radius + 3.0f, IM_COL32(255, 255, 255, 255), 32, 2.0f);
    draw->AddCircleFilled(center, radius, IM_COL32(0, 255, 255, 230), 32);
    draw->AddLine(
        ImVec2(center.x - 14.0f, center.y),
        ImVec2(center.x + 14.0f, center.y),
        IM_COL32(0, 0, 0, 240),
        3.0f);
    draw->AddLine(
        ImVec2(center.x, center.y - 14.0f),
        ImVec2(center.x, center.y + 14.0f),
        IM_COL32(0, 0, 0, 240),
        3.0f);
    draw->AddLine(
        ImVec2(center.x - 14.0f, center.y),
        ImVec2(center.x + 14.0f, center.y),
        IM_COL32(255, 255, 255, 255),
        1.0f);
    draw->AddLine(
        ImVec2(center.x, center.y - 14.0f),
        ImVec2(center.x, center.y + 14.0f),
        IM_COL32(255, 255, 255, 255),
        1.0f);
}

} // namespace

void BeginFrame()
{
    ImageSideIndex() = 0;
    Reader().refresh();
}

void DrawImageWithLatestGaze(
    ImTextureRef texture,
    const ImVec2& imageSize)
{
    const ImVec2 imageMin = ImGui::GetCursorScreenPos();
    ImGui::Image(texture, imageSize);

    const auto& latest = Reader().latest();
    if (!latest) {
        ImageSideIndex() = (ImageSideIndex() + 1) % 2;
        return;
    }

    const int sideIndex = ImageSideIndex();
    ImageSideIndex() = (ImageSideIndex() + 1) % 2;
    DrawGazePoint(sideIndex == 0 ? latest->left : latest->right, imageMin, imageSize);
}

} // namespace DualIC4Varjo::ImGuiGazeOverlay

namespace ImGui {

void GazeOverlayNewFrame()
{
    DualIC4Varjo::ImGuiGazeOverlay::BeginFrame();
    ImGui::NewFrame();
}

void GazeOverlayImage(
    ImTextureRef texture,
    const ImVec2& imageSize)
{
    DualIC4Varjo::ImGuiGazeOverlay::DrawImageWithLatestGaze(
        texture,
        imageSize);
}

} // namespace ImGui

#define NewFrame GazeOverlayNewFrame
#define Image GazeOverlayImage
#include "ImGuiStereoPreview.cpp"
#undef Image
#undef NewFrame
