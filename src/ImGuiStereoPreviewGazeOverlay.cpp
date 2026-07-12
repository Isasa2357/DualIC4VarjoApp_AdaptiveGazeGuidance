#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "EyeTrackerLoadServiceHook.hpp"
#include "GazeOnCameraFrameService.hpp"
#include "GuiControlBridge.hpp"
#include "GuiPerformanceStats.hpp"
#include "ImuLoadServiceHook.hpp"
#include "VstLoadServiceHook.hpp"

#include <imgui.h>

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
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
    return (!end || end == text.c_str())
        ? std::numeric_limits<float>::quiet_NaN()
        : value;
}

std::int64_t ParseI64(const std::string& text)
{
    if (text.empty() || text == "nan" || text == "nullopt") return 0;
    char* end = nullptr;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    return (!end || end == text.c_str())
        ? 0
        : static_cast<std::int64_t>(value);
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
        if (now - lastRefresh_ < std::chrono::milliseconds(20)) return;
        lastRefresh_ = now;

        const auto path = GazeOnCameraFrameHook::outputPath();
        if (path.empty()) return;
        if (path != path_) reset(path);

        std::ifstream input(path_, std::ios::binary);
        if (!input) return;
        input.seekg(0, std::ios::end);
        const std::streamoff end = static_cast<std::streamoff>(input.tellg());
        if (end < 0) return;
        if (offset_ > end) reset(path_);
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

class LineTailCounter {
public:
    void setPath(const std::filesystem::path& path)
    {
        if (path == path_) return;
        path_ = path;
        offset_ = 0;
        pending_.clear();
        headerSkipped_ = false;
        count_ = 0;
    }

    void refresh()
    {
        if (path_.empty()) return;
        std::ifstream input(path_, std::ios::binary);
        if (!input) return;
        input.seekg(0, std::ios::end);
        const std::streamoff end = static_cast<std::streamoff>(input.tellg());
        if (end < 0) return;
        if (offset_ > end) {
            offset_ = 0;
            pending_.clear();
            headerSkipped_ = false;
            count_ = 0;
        }
        if (end == offset_) return;

        input.seekg(offset_, std::ios::beg);
        std::string chunk(static_cast<std::size_t>(end - offset_), '\0');
        input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        offset_ = end;
        pending_ += chunk;

        for (;;) {
            const std::size_t newline = pending_.find('\n');
            if (newline == std::string::npos) break;
            std::string line = pending_.substr(0, newline);
            pending_.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if (!headerSkipped_) {
                headerSkipped_ = true;
                continue;
            }
            ++count_;
        }
    }

    std::uint64_t count() const noexcept { return count_; }

private:
    std::filesystem::path path_;
    std::streamoff offset_ = 0;
    std::string pending_;
    bool headerSkipped_ = false;
    std::uint64_t count_ = 0;
};

std::filesystem::path FindFileBySuffix(
    const std::filesystem::path& directory,
    const std::string& suffix)
{
    if (directory.empty()) return {};
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) return {};

    std::filesystem::path best;
    std::filesystem::file_time_type bestTime{};
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (error) break;
        if (!entry.is_regular_file(error)) continue;
        const auto name = entry.path().filename().string();
        if (name.size() < suffix.size() ||
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
            continue;
        }
        const auto time = entry.last_write_time(error);
        if (best.empty() || (!error && time > bestTime)) {
            best = entry.path();
            bestTime = time;
        }
    }
    return best;
}

class RateHistory {
public:
    void push(float value)
    {
        values_.push_back(std::isfinite(value) ? std::max(0.0f, value) : 0.0f);
        if (values_.size() > capacity_) values_.pop_front();
    }

    const std::deque<float>& values() const noexcept { return values_; }
    float latest() const noexcept { return values_.empty() ? 0.0f : values_.back(); }

private:
    std::deque<float> values_;
    std::size_t capacity_ = 120;
};

struct CounterSnapshot {
    std::uint64_t cameraLeft = 0;
    std::uint64_t cameraRight = 0;
    std::uint64_t codecLeft = 0;
    std::uint64_t codecRight = 0;
    std::uint64_t vstLeft = 0;
    std::uint64_t vstRight = 0;
    std::uint64_t eye = 0;
    std::uint64_t imu = 0;
};

class PerformancePanelState {
public:
    void update()
    {
        const auto now = Clock::now();
        if (now - lastFileScan_ > std::chrono::seconds(1)) {
            lastFileScan_ = now;
            const auto directory = VstLoadServiceHook::outputDirectory();
            codecLeftCounter_.setPath(
                FindFileBySuffix(directory, "_left_raw_metadata.csv"));
            codecRightCounter_.setPath(
                FindFileBySuffix(directory, "_right_raw_metadata.csv"));
        }
        codecLeftCounter_.refresh();
        codecRightCounter_.refresh();

        const auto camera = GuiPerformanceStats::CameraReadFrames();
        CounterSnapshot current{};
        current.cameraLeft = camera.left;
        current.cameraRight = camera.right;
        current.codecLeft = codecLeftCounter_.count();
        current.codecRight = codecRightCounter_.count();
        current.vstLeft = VstLoadServiceHook::leftReceivedCount();
        current.vstRight = VstLoadServiceHook::rightReceivedCount();
        current.eye = EyeTrackerLoadServiceHook::receivedSampleCount();
        current.imu = ImuLoadServiceHook::receivedCount();

        if (!hasPrevious_) {
            previous_ = current;
            previousTime_ = now;
            hasPrevious_ = true;
            return;
        }

        const double elapsed =
            std::chrono::duration<double>(now - previousTime_).count();
        if (elapsed < 0.5) return;

        cameraLeft.push(rate(previous_.cameraLeft, current.cameraLeft, elapsed));
        cameraRight.push(rate(previous_.cameraRight, current.cameraRight, elapsed));
        codecLeft.push(rate(previous_.codecLeft, current.codecLeft, elapsed));
        codecRight.push(rate(previous_.codecRight, current.codecRight, elapsed));
        vstLeft.push(rate(previous_.vstLeft, current.vstLeft, elapsed));
        vstRight.push(rate(previous_.vstRight, current.vstRight, elapsed));
        eye.push(rate(previous_.eye, current.eye, elapsed));
        imu.push(rate(previous_.imu, current.imu, elapsed));

        previous_ = current;
        previousTime_ = now;
    }

    RateHistory cameraLeft;
    RateHistory cameraRight;
    RateHistory codecLeft;
    RateHistory codecRight;
    RateHistory vstLeft;
    RateHistory vstRight;
    RateHistory eye;
    RateHistory imu;

private:
    static float rate(
        std::uint64_t previous,
        std::uint64_t current,
        double elapsed) noexcept
    {
        if (current < previous || elapsed <= 0.0) return 0.0f;
        return static_cast<float>(
            static_cast<double>(current - previous) / elapsed);
    }

    LineTailCounter codecLeftCounter_;
    LineTailCounter codecRightCounter_;
    bool hasPrevious_ = false;
    CounterSnapshot previous_{};
    Clock::time_point previousTime_{};
    Clock::time_point lastFileScan_{};
};

struct PanelLayout {
    float margin = 12.0f;
    float gap = 12.0f;
    float leftWidth = 1.0f;
    float rightWidth = 1.0f;
    float videoHeight = 1.0f;
    float controlsHeight = 1.0f;
};

PanelLayout CalculateLayout()
{
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 workSize = viewport ? viewport->WorkSize : ImVec2(1600.0f, 1020.0f);

    PanelLayout layout;
    const float usableWidth = std::max(
        640.0f,
        workSize.x - layout.margin * 2.0f);
    float rightWidth = std::clamp(usableWidth * 0.36f, 400.0f, 620.0f);
    if (usableWidth - layout.gap - rightWidth < 520.0f) {
        rightWidth = std::max(320.0f, usableWidth - layout.gap - 520.0f);
    }
    layout.rightWidth = rightWidth;
    layout.leftWidth = std::max(320.0f, usableWidth - layout.gap - rightWidth);

    float controlsHeight = std::clamp(workSize.y * 0.36f, 340.0f, 430.0f);
    float videoHeight = workSize.y - layout.margin * 2.0f - layout.gap - controlsHeight;
    if (videoHeight < 260.0f) {
        videoHeight = 260.0f;
        controlsHeight = std::max(
            260.0f,
            workSize.y - layout.margin * 2.0f - layout.gap - videoHeight);
    }
    layout.videoHeight = videoHeight;
    layout.controlsHeight = controlsHeight;
    return layout;
}

CsvTailReader& Reader()
{
    static CsvTailReader value;
    return value;
}

PerformancePanelState& Performance()
{
    static PerformancePanelState value;
    return value;
}

int& ImageSideIndex()
{
    static thread_local int value = 0;
    return value;
}

void ApplySpaciousStyleOnce()
{
    static bool applied = false;
    if (applied || !ImGui::GetCurrentContext()) return;

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(14.0f, 12.0f);
    style.FramePadding = ImVec2(10.0f, 7.0f);
    style.ItemSpacing = ImVec2(12.0f, 10.0f);
    style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    style.IndentSpacing = 20.0f;
    style.ScrollbarSize = 16.0f;
    style.GrabMinSize = 12.0f;
    style.WindowRounding = 5.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 4.0f;
    applied = true;
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
        IM_COL32(255, 255, 255, 255),
        2.0f);
    draw->AddLine(
        ImVec2(center.x, center.y - 14.0f),
        ImVec2(center.x, center.y + 14.0f),
        IM_COL32(255, 255, 255, 255),
        2.0f);
}

void RequestApplicationExitFromGui()
{
    GuiControlBridge::RequestApplicationExit();
}

float MaxHistoryValue(
    const std::deque<float>& first,
    const std::deque<float>* second)
{
    float value = 1.0f;
    for (float item : first) value = std::max(value, item);
    if (second) {
        for (float item : *second) value = std::max(value, item);
    }
    return value;
}

void DrawOneLine(
    ImDrawList* draw,
    const ImVec2& minimum,
    const ImVec2& size,
    const std::deque<float>& values,
    float maximum,
    ImU32 color)
{
    if (!draw || values.size() < 2 || maximum <= 0.0f) return;
    const std::size_t count = values.size();
    ImVec2 previous{};
    for (std::size_t index = 0; index < count; ++index) {
        const float x = minimum.x + size.x *
            (static_cast<float>(index) / static_cast<float>(count - 1));
        const float normalized = std::clamp(values[index] / maximum, 0.0f, 1.0f);
        const float y = minimum.y + size.y * (1.0f - normalized);
        const ImVec2 current(x, y);
        if (index > 0) draw->AddLine(previous, current, color, 2.0f);
        previous = current;
    }
}

void DrawRateGraph(
    const char* title,
    const RateHistory& left,
    const char* leftLabel,
    const RateHistory* right,
    const char* rightLabel,
    float graphHeight)
{
    ImGui::TextUnformatted(title);
    ImGui::Spacing();

    const ImVec2 graphMin = ImGui::GetCursorScreenPos();
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const ImVec2 graphSize(width, graphHeight);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 border = ImGui::GetColorU32(ImGuiCol_Border);
    const ImU32 grid = IM_COL32(90, 90, 90, 90);
    const ImU32 leftColor = IM_COL32(80, 180, 255, 255);
    const ImU32 rightColor = IM_COL32(255, 190, 80, 255);

    draw->AddRectFilled(
        graphMin,
        ImVec2(graphMin.x + graphSize.x, graphMin.y + graphSize.y),
        IM_COL32(12, 12, 12, 180));
    for (int line = 1; line < 4; ++line) {
        const float y = graphMin.y + graphSize.y * (static_cast<float>(line) / 4.0f);
        draw->AddLine(
            ImVec2(graphMin.x, y),
            ImVec2(graphMin.x + graphSize.x, y),
            grid,
            1.0f);
    }
    draw->AddRect(
        graphMin,
        ImVec2(graphMin.x + graphSize.x, graphMin.y + graphSize.y),
        border);

    const float maximum = MaxHistoryValue(
        left.values(),
        right ? &right->values() : nullptr);
    DrawOneLine(draw, graphMin, graphSize, left.values(), maximum, leftColor);
    if (right) {
        DrawOneLine(draw, graphMin, graphSize, right->values(), maximum, rightColor);
    }
    ImGui::Dummy(graphSize);

    ImGui::TextColored(
        ImVec4(0.31f, 0.71f, 1.0f, 1.0f),
        "%s %.1f%s",
        leftLabel,
        left.latest(),
        right ? " fps" : " /s");
    if (right) {
        ImGui::SameLine(0.0f, 24.0f);
        ImGui::TextColored(
            ImVec4(1.0f, 0.75f, 0.31f, 1.0f),
            "%s %.1f fps",
            rightLabel,
            right->latest());
    }
    ImGui::SameLine(0.0f, 24.0f);
    ImGui::TextDisabled("scale %.1f", maximum);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
}

float FourButtonWidth()
{
    const float available = ImGui::GetContentRegionAvail().x;
    return std::clamp((available - 36.0f) / 4.0f, 86.0f, 122.0f);
}

void DrawControlPanel()
{
    ImGui::TextUnformatted("Plane controls");
    ImGui::SameLine(0.0f, 18.0f);
    ImGui::TextDisabled("one click = 0.01 m");
    ImGui::Separator();
    ImGui::Spacing();

    const float buttonWidth = FourButtonWidth();
    const ImVec2 buttonSize(buttonWidth, 38.0f);

    ImGui::TextDisabled("Position");
    if (ImGui::Button("Up", buttonSize)) GuiControlBridge::RequestMoveUp();
    ImGui::SameLine();
    if (ImGui::Button("Down", buttonSize)) GuiControlBridge::RequestMoveDown();
    ImGui::SameLine();
    if (ImGui::Button("Left", buttonSize)) GuiControlBridge::RequestMoveLeft();
    ImGui::SameLine();
    if (ImGui::Button("Right", buttonSize)) GuiControlBridge::RequestMoveRight();

    ImGui::Spacing();
    ImGui::TextDisabled("Size and depth");
    if (ImGui::Button("Size +", buttonSize)) GuiControlBridge::RequestSizeIncrease();
    ImGui::SameLine();
    if (ImGui::Button("Size -", buttonSize)) GuiControlBridge::RequestSizeDecrease();
    ImGui::SameLine();
    if (ImGui::Button("Near", buttonSize)) GuiControlBridge::RequestMoveNear();
    ImGui::SameLine();
    if (ImGui::Button("Far", buttonSize)) GuiControlBridge::RequestMoveFar();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    bool showPlane = GuiControlBridge::PlaneVisible();
    if (ImGui::Checkbox("Show Plane", &showPlane)) {
        GuiControlBridge::RequestTogglePlaneVisibility();
    }
    ImGui::SameLine(0.0f, 32.0f);
    bool locked = GuiControlBridge::KeyboardControlLocked();
    if (ImGui::Checkbox("Keyboard operation lock", &locked)) {
        GuiControlBridge::SetKeyboardControlLocked(locked);
    }

    ImGui::Spacing();
    ImGui::TextDisabled(
        "Plane hidden: VST pass-through. Keyboard lock keeps only the postprocess reveal key active.");

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(150, 45, 45, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(185, 60, 60, 255));
    if (ImGui::Button("Exit application", ImVec2(190.0f, 42.0f))) {
        RequestApplicationExitFromGui();
    }
    ImGui::PopStyleColor(2);
}

void DrawPerformancePanel()
{
    auto& performance = Performance();
    ImGui::TextUnformatted("Live rates");
    ImGui::SameLine(0.0f, 16.0f);
    ImGui::TextDisabled("0.5 s sampling / recent history");
    ImGui::Separator();
    ImGui::Spacing();

    const float availableHeight = std::max(360.0f, ImGui::GetContentRegionAvail().y);
    const float graphHeight = std::clamp(
        availableHeight / 5.0f - 54.0f,
        72.0f,
        108.0f);

    ImGui::BeginChild("PerformanceGraphs", ImVec2(0.0f, 0.0f), false);
    DrawRateGraph(
        "Camera FPS",
        performance.cameraLeft,
        "Left",
        &performance.cameraRight,
        "Right",
        graphHeight);
    DrawRateGraph(
        "Video codec FPS",
        performance.codecLeft,
        "Left",
        &performance.codecRight,
        "Right",
        graphHeight);
    DrawRateGraph(
        "VST acquisition",
        performance.vstLeft,
        "Left",
        &performance.vstRight,
        "Right",
        graphHeight);
    DrawRateGraph(
        "Eye tracker acquisition",
        performance.eye,
        "Samples",
        nullptr,
        nullptr,
        graphHeight);
    DrawRateGraph(
        "IMU acquisition",
        performance.imu,
        "Samples",
        nullptr,
        nullptr,
        graphHeight);
    ImGui::EndChild();
}

void DrawGuiPanels()
{
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (!viewport) return;

    const PanelLayout layout = CalculateLayout();
    const ImVec2 workPos = viewport->WorkPos;
    constexpr ImGuiWindowFlags panelFlags =
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoCollapse;

    ImGui::SetNextWindowPos(
        ImVec2(
            workPos.x + layout.margin,
            workPos.y + layout.margin + layout.videoHeight + layout.gap),
        ImGuiCond_Always);
    ImGui::SetNextWindowSize(
        ImVec2(layout.leftWidth, layout.controlsHeight),
        ImGuiCond_Always);
    ImGui::Begin("Controls", nullptr, panelFlags);
    DrawControlPanel();
    ImGui::End();

    ImGui::SetNextWindowPos(
        ImVec2(
            workPos.x + layout.margin + layout.leftWidth + layout.gap,
            workPos.y + layout.margin),
        ImGuiCond_Always);
    ImGui::SetNextWindowSize(
        ImVec2(
            layout.rightWidth,
            layout.videoHeight + layout.gap + layout.controlsHeight),
        ImGuiCond_Always);
    ImGui::Begin("Performance", nullptr, panelFlags);
    DrawPerformancePanel();
    ImGui::End();
}

} // namespace

void BeginFrame()
{
    ApplySpaciousStyleOnce();
    ImageSideIndex() = 0;
    Reader().refresh();
    Performance().update();
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
    DrawGazePoint(
        sideIndex == 0 ? latest->left : latest->right,
        imageMin,
        imageSize);
}

ImVec2 PreviewContentRegionAvail()
{
    const PanelLayout layout = CalculateLayout();
    return ImVec2(
        std::max(1.0f, layout.leftWidth - 20.0f),
        std::max(1.0f, layout.videoHeight - 18.0f));
}

void RenderWithPanels()
{
    DrawGuiPanels();
    ImGui::Render();
}

BOOL GazeOverlayDestroyWindow(HWND window)
{
    // WM_CLOSE reaches DestroyWindow in ImGuiStereoPreview.cpp. Publish the same
    // graceful-exit request as the GUI button before destroying only the desktop
    // preview window. The Varjo render thread remains alive for the VST fade.
    RequestApplicationExitFromGui();
    return ::DestroyWindow(window);
}

HWND WINAPI GazeOverlayCreateWindowW(
    LPCWSTR className,
    LPCWSTR windowName,
    DWORD style,
    int x,
    int y,
    int width,
    int height,
    HWND parent,
    HMENU menu,
    HINSTANCE instance,
    LPVOID parameter)
{
    // The integrated preview now contains video, controls, and performance plots.
    // Keep the user-specified width but enforce a taller parent window so the
    // Controls panel is visible without immediate scrolling at the old 1600x800
    // default.
    const int tallerHeight = std::max(height, 1080);
    return ::CreateWindowW(
        className,
        windowName,
        style,
        x,
        y,
        width,
        tallerHeight,
        parent,
        menu,
        instance,
        parameter);
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

ImVec2 GazeOverlayGetContentRegionAvail()
{
    return DualIC4Varjo::ImGuiGazeOverlay::PreviewContentRegionAvail();
}

void GazeOverlayRender()
{
    DualIC4Varjo::ImGuiGazeOverlay::RenderWithPanels();
}

} // namespace ImGui

#define NewFrame GazeOverlayNewFrame
#define Image GazeOverlayImage
#define GetContentRegionAvail GazeOverlayGetContentRegionAvail
#define Render GazeOverlayRender
#define DestroyWindow DualIC4Varjo::ImGuiGazeOverlay::GazeOverlayDestroyWindow
#define CreateWindowW DualIC4Varjo::ImGuiGazeOverlay::GazeOverlayCreateWindowW
#include "ImGuiStereoPreview.cpp"
#undef CreateWindowW
#undef DestroyWindow
#undef Render
#undef GetContentRegionAvail
#undef Image
#undef NewFrame
