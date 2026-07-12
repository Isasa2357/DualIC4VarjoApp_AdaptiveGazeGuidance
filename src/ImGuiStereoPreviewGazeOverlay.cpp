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
    if (text.empty() || text == "nan" || text == "nullopt") return std::numeric_limits<float>::quiet_NaN();
    char* end = nullptr;
    const float value = std::strtof(text.c_str(), &end);
    return (!end || end == text.c_str()) ? std::numeric_limits<float>::quiet_NaN() : value;
}

std::int64_t ParseI64(const std::string& text)
{
    if (text.empty() || text == "nan" || text == "nullopt") return 0;
    char* end = nullptr;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    return (!end || end == text.c_str()) ? 0 : static_cast<std::int64_t>(value);
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
        for (std::size_t index = 0; index < columns.size(); ++index) header_[columns[index]] = index;
        headerParsed_ = true;
    }

    std::optional<std::size_t> column(const char* name) const
    {
        const auto it = header_.find(name);
        return it == header_.end() ? std::nullopt : std::optional<std::size_t>(it->second);
    }

    std::string value(const std::vector<std::string>& columns, const char* name) const
    {
        const auto index = column(name);
        if (!index || *index >= columns.size()) return {};
        return columns[*index];
    }

    SideSample parseSide(const std::vector<std::string>& columns, const char* prefix) const
    {
        SideSample side;
        const std::string xName = std::string(prefix) + "_camera_x01";
        const std::string yName = std::string(prefix) + "_camera_y01";
        const std::string insideName = std::string(prefix) + "_inside_frame";
        side.x01 = ParseFloat(value(columns, xName.c_str()));
        side.y01 = ParseFloat(value(columns, yName.c_str()));
        side.inside = ParseBool01(value(columns, insideName.c_str()));
        side.valid = side.inside && std::isfinite(side.x01) && std::isfinite(side.y01) &&
            side.x01 >= 0.0f && side.x01 <= 1.0f && side.y01 >= 0.0f && side.y01 <= 1.0f;
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
            if (!headerSkipped_) { headerSkipped_ = true; continue; }
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

std::filesystem::path FindFileBySuffix(const std::filesystem::path& directory, const std::string& suffix)
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
        if (name.size() < suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
        const auto time = entry.last_write_time(error);
        if (best.empty() || (!error && time > bestTime)) { best = entry.path(); bestTime = time; }
    }
    return best;
}

class RateHistory {
public:
    void push(float value)
    {
        values_.push_back(value);
        if (values_.size() > capacity_) values_.pop_front();
    }
    const std::deque<float>& values() const noexcept { return values_; }
    float latest() const noexcept { return values_.empty() ? 0.0f : values_.back(); }
private:
    std::deque<float> values_;
    std::size_t capacity_ = 90;
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
            const auto dir = VstLoadServiceHook::outputDirectory();
            codecLeftCounter_.setPath(FindFileBySuffix(dir, "_left_raw_metadata.csv"));
            codecRightCounter_.setPath(FindFileBySuffix(dir, "_right_raw_metadata.csv"));
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

        if (!hasPrevious_) { previous_ = current; previousTime_ = now; hasPrevious_ = true; return; }
        const double dt = std::chrono::duration<double>(now - previousTime_).count();
        if (dt < 0.5) return;

        cameraLeft.push(rate(previous_.cameraLeft, current.cameraLeft, dt));
        cameraRight.push(rate(previous_.cameraRight, current.cameraRight, dt));
        codecLeft.push(rate(previous_.codecLeft, current.codecLeft, dt));
        codecRight.push(rate(previous_.codecRight, current.codecRight, dt));
        vstLeft.push(rate(previous_.vstLeft, current.vstLeft, dt));
        vstRight.push(rate(previous_.vstRight, current.vstRight, dt));
        eye.push(rate(previous_.eye, current.eye, dt));
        imu.push(rate(previous_.imu, current.imu, dt));
        previous_ = current;
        previousTime_ = now;
    }

    RateHistory cameraLeft, cameraRight, codecLeft, codecRight, vstLeft, vstRight, eye, imu;

private:
    static float rate(std::uint64_t previous, std::uint64_t current, double dt) noexcept
    {
        if (current < previous || dt <= 0.0) return 0.0f;
        return static_cast<float>(static_cast<double>(current - previous) / dt);
    }
    LineTailCounter codecLeftCounter_;
    LineTailCounter codecRightCounter_;
    bool hasPrevious_ = false;
    CounterSnapshot previous_{};
    Clock::time_point previousTime_{};
    Clock::time_point lastFileScan_{};
};

CsvTailReader& Reader() { static CsvTailReader reader; return reader; }
PerformancePanelState& Performance() { static PerformancePanelState value; return value; }
int& ImageSideIndex() { static thread_local int value = 0; return value; }

void DrawGazePoint(const SideSample& side, const ImVec2& imageMin, const ImVec2& imageSize)
{
    if (!side.valid) return;
    const ImVec2 center(imageMin.x + side.x01 * imageSize.x, imageMin.y + side.y01 * imageSize.y);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    constexpr float radius = 8.0f;
    draw->AddCircleFilled(center, radius + 3.0f, IM_COL32(0, 0, 0, 220), 32);
    draw->AddCircle(center, radius + 3.0f, IM_COL32(255, 255, 255, 255), 32, 2.0f);
    draw->AddCircleFilled(center, radius, IM_COL32(0, 255, 255, 230), 32);
    draw->AddLine(ImVec2(center.x - 14.0f, center.y), ImVec2(center.x + 14.0f, center.y), IM_COL32(0, 0, 0, 240), 3.0f);
    draw->AddLine(ImVec2(center.x, center.y - 14.0f), ImVec2(center.x, center.y + 14.0f), IM_COL32(0, 0, 0, 240), 3.0f);
    draw->AddLine(ImVec2(center.x - 14.0f, center.y), ImVec2(center.x + 14.0f, center.y), IM_COL32(255, 255, 255, 255), 1.0f);
    draw->AddLine(ImVec2(center.x, center.y - 14.0f), ImVec2(center.x, center.y + 14.0f), IM_COL32(255, 255, 255, 255), 1.0f);
}

void RequestApplicationExitFromGui()
{
    GuiControlBridge::RequestApplicationExit();
    INPUT inputs[2]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_ESCAPE;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = VK_ESCAPE;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, inputs, sizeof(INPUT));
}

float MaxHistoryValue(const std::deque<float>& a, const std::deque<float>& b)
{
    float value = 1.0f;
    for (float item : a) value = std::max(value, item);
    for (float item : b) value = std::max(value, item);
    return value;
}

void DrawOneLine(ImDrawList* draw, const ImVec2& min, const ImVec2& size, const std::deque<float>& values, float maxValue, ImU32 color)
{
    if (!draw || values.size() < 2 || maxValue <= 0.0f) return;
    const std::size_t count = values.size();
    ImVec2 previous{};
    for (std::size_t index = 0; index < count; ++index) {
        const float x = min.x + size.x * (static_cast<float>(index) / static_cast<float>(count - 1));
        const float normalized = std::clamp(values[index] / maxValue, 0.0f, 1.0f);
        const float y = min.y + size.y * (1.0f - normalized);
        const ImVec2 current(x, y);
        if (index > 0) draw->AddLine(previous, current, color, 2.0f);
        previous = current;
    }
}

void DrawRateGraph(const char* title, const RateHistory& left, const char* leftLabel, const RateHistory* right, const char* rightLabel, float height)
{
    ImGui::Text("%s", title);
    const ImVec2 graphMin = ImGui::GetCursorScreenPos();
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const ImVec2 graphSize(width, height);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 frameColor = ImGui::GetColorU32(ImGuiCol_Border);
    const ImU32 leftColor = IM_COL32(80, 180, 255, 255);
    const ImU32 rightColor = IM_COL32(255, 190, 80, 255);
    draw->AddRect(graphMin, ImVec2(graphMin.x + graphSize.x, graphMin.y + graphSize.y), frameColor);
    const float maxValue = MaxHistoryValue(left.values(), right ? right->values() : std::deque<float>{});
    DrawOneLine(draw, graphMin, graphSize, left.values(), maxValue, leftColor);
    if (right) DrawOneLine(draw, graphMin, graphSize, right->values(), maxValue, rightColor);
    ImGui::Dummy(graphSize);
    if (right) ImGui::Text("%s %.1f fps   %s %.1f fps   max %.1f", leftLabel, left.latest(), rightLabel, right->latest(), maxValue);
    else ImGui::Text("%s %.1f /s   max %.1f", leftLabel, left.latest(), maxValue);
    ImGui::Spacing();
}

void DrawControlPanel()
{
    ImGui::TextUnformatted("Plane controls");
    if (ImGui::Button("Up", ImVec2(80, 28))) GuiControlBridge::RequestMoveUp(); ImGui::SameLine();
    if (ImGui::Button("Down", ImVec2(80, 28))) GuiControlBridge::RequestMoveDown(); ImGui::SameLine();
    if (ImGui::Button("Left", ImVec2(80, 28))) GuiControlBridge::RequestMoveLeft(); ImGui::SameLine();
    if (ImGui::Button("Right", ImVec2(80, 28))) GuiControlBridge::RequestMoveRight();
    if (ImGui::Button("Size +", ImVec2(80, 28))) GuiControlBridge::RequestSizeIncrease(); ImGui::SameLine();
    if (ImGui::Button("Size -", ImVec2(80, 28))) GuiControlBridge::RequestSizeDecrease(); ImGui::SameLine();
    if (ImGui::Button("Near", ImVec2(80, 28))) GuiControlBridge::RequestMoveNear(); ImGui::SameLine();
    if (ImGui::Button("Far", ImVec2(80, 28))) GuiControlBridge::RequestMoveFar();
    const bool visible = GuiControlBridge::PlaneVisible();
    if (ImGui::Button(visible ? "Hide Plane" : "Show Plane", ImVec2(170, 30))) GuiControlBridge::RequestTogglePlaneVisibility();
    ImGui::SameLine();
    bool locked = GuiControlBridge::KeyboardControlLocked();
    if (ImGui::Checkbox("Keyboard operation lock", &locked)) GuiControlBridge::SetKeyboardControlLocked(locked);
    ImGui::TextWrapped("When Plane is hidden, the VST postprocess mask becomes pass-through. F23 reveal remains available while keyboard operations are locked.");
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(160, 50, 50, 255));
    if (ImGui::Button("Exit application", ImVec2(180, 32))) RequestApplicationExitFromGui();
    ImGui::PopStyleColor();
}

void DrawPerformancePanel()
{
    auto& perf = Performance();
    ImGui::TextUnformatted("Performance");
    DrawRateGraph("Camera FPS", perf.cameraLeft, "Left", &perf.cameraRight, "Right", 80.0f);
    DrawRateGraph("Video codec FPS", perf.codecLeft, "Left", &perf.codecRight, "Right", 80.0f);
    DrawRateGraph("VST acquisition", perf.vstLeft, "Left", &perf.vstRight, "Right", 80.0f);
    DrawRateGraph("Eye tracker acquisition", perf.eye, "Samples", nullptr, nullptr, 80.0f);
    DrawRateGraph("IMU acquisition", perf.imu, "Samples", nullptr, nullptr, 80.0f);
}

void DrawGuiPanels()
{
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 workPos = viewport->WorkPos;
    const ImVec2 workSize = viewport->WorkSize;
    const float leftWidth = std::max(420.0f, workSize.x * 0.64f);
    const float videoHeight = std::max(240.0f, workSize.y * 0.68f);
    const float controlsHeight = std::max(160.0f, workSize.y - videoHeight);
    const float rightWidth = std::max(320.0f, workSize.x - leftWidth);
    const ImGuiWindowFlags panelFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;

    ImGui::SetNextWindowPos(ImVec2(workPos.x, workPos.y + videoHeight));
    ImGui::SetNextWindowSize(ImVec2(leftWidth, controlsHeight));
    ImGui::Begin("Controls", nullptr, panelFlags);
    DrawControlPanel();
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(workPos.x + leftWidth, workPos.y));
    ImGui::SetNextWindowSize(ImVec2(rightWidth, workSize.y));
    ImGui::Begin("Performance", nullptr, panelFlags);
    DrawPerformancePanel();
    ImGui::End();
}

} // namespace

void BeginFrame()
{
    ImageSideIndex() = 0;
    Reader().refresh();
    Performance().update();
}

void DrawImageWithLatestGaze(ImTextureRef texture, const ImVec2& imageSize)
{
    const ImVec2 imageMin = ImGui::GetCursorScreenPos();
    ImGui::Image(texture, imageSize);
    const auto& latest = Reader().latest();
    if (!latest) { ImageSideIndex() = (ImageSideIndex() + 1) % 2; return; }
    const int sideIndex = ImageSideIndex();
    ImageSideIndex() = (ImageSideIndex() + 1) % 2;
    DrawGazePoint(sideIndex == 0 ? latest->left : latest->right, imageMin, imageSize);
}

ImVec2 PreviewContentRegionAvail()
{
    const ImVec2 real = ImGui::GetContentRegionAvail();
    return ImVec2(std::max(1.0f, real.x * 0.64f), std::max(1.0f, real.y * 0.68f));
}

void RenderWithPanels()
{
    DrawGuiPanels();
    ImGui::Render();
}

BOOL GazeOverlayDestroyWindow(HWND window)
{
    RequestApplicationExitFromGui();
    return ::DestroyWindow(window);
}

} // namespace DualIC4Varjo::ImGuiGazeOverlay

namespace ImGui {

void GazeOverlayNewFrame()
{
    DualIC4Varjo::ImGuiGazeOverlay::BeginFrame();
    ImGui::NewFrame();
}

void GazeOverlayImage(ImTextureRef texture, const ImVec2& imageSize)
{
    DualIC4Varjo::ImGuiGazeOverlay::DrawImageWithLatestGaze(texture, imageSize);
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
#include "ImGuiStereoPreview.cpp"
#undef DestroyWindow
#undef Render
#undef GetContentRegionAvail
#undef Image
#undef NewFrame
