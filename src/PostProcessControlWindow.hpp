#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "CalibrationRuntimeBridge.hpp"
#include "GuiControlBridge.hpp"

#include <Windows.h>
#include <CommCtrl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <thread>

#if defined(_MSC_VER)
#pragma comment(lib, "comctl32.lib")
#endif

namespace DualIC4Varjo::PostProcessControlWindow {
namespace detail {

constexpr int kIdMode = 100;
constexpr int kIdCenterX = 101;
constexpr int kIdCenterY = 102;
constexpr int kIdRadius = 103;
constexpr int kIdSoftness = 104;
constexpr int kIdDarkenStrength = 105;
constexpr int kIdBlurRadius = 106;
constexpr int kIdBlurStrength = 107;
constexpr int kIdDebugDot = 108;

struct Controls {
    HWND window = nullptr;
    HWND mode = nullptr;
    HWND debugDot = nullptr;

    HWND centerX = nullptr;
    HWND centerY = nullptr;
    HWND radius = nullptr;
    HWND softness = nullptr;
    HWND darkenStrength = nullptr;
    HWND blurRadius = nullptr;
    HWND blurStrength = nullptr;

    HWND centerXLabel = nullptr;
    HWND centerYLabel = nullptr;
    HWND radiusLabel = nullptr;
    HWND softnessLabel = nullptr;
    HWND darkenStrengthLabel = nullptr;
    HWND blurRadiusLabel = nullptr;
    HWND blurStrengthLabel = nullptr;
};

inline Controls& State() noexcept
{
    static Controls controls;
    return controls;
}

inline std::atomic_bool& Started() noexcept
{
    static std::atomic_bool started{false};
    return started;
}

inline void SetTrackRange(HWND track, int minValue, int maxValue) noexcept
{
    if (!track) return;
    SendMessageW(track, TBM_SETRANGE, TRUE, MAKELPARAM(minValue, maxValue));
    SendMessageW(track, TBM_SETPAGESIZE, 0, 25);
}

inline void SetTrackPos(HWND track, int value) noexcept
{
    if (!track) return;
    SendMessageW(track, TBM_SETPOS, TRUE, value);
}

inline int TrackPos(HWND track, int fallback = 0) noexcept
{
    if (!track) return fallback;
    return static_cast<int>(SendMessageW(track, TBM_GETPOS, 0, 0));
}

inline void SetLabel(HWND label, const wchar_t* name, double value, const wchar_t* suffix = L"") noexcept
{
    if (!label) return;
    wchar_t text[128]{};
    std::swprintf(text, std::size(text), L"%s: %.3f%s", name, value, suffix ? suffix : L"");
    SetWindowTextW(label, text);
}

inline HWND CreateLabel(HWND parent, int x, int y, int w, const wchar_t* text)
{
    return CreateWindowW(
        L"STATIC",
        text,
        WS_CHILD | WS_VISIBLE,
        x,
        y,
        w,
        22,
        parent,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr);
}

inline HWND CreateTrack(HWND parent, int id, int x, int y, int w, int minValue, int maxValue)
{
    HWND track = CreateWindowW(
        TRACKBAR_CLASSW,
        L"",
        WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
        x,
        y,
        w,
        32,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr),
        nullptr);
    SetTrackRange(track, minValue, maxValue);
    return track;
}

inline void UpdateLabels() noexcept
{
    auto& c = State();
    SetLabel(c.centerXLabel, L"Center X", TrackPos(c.centerX) / 1000.0);
    SetLabel(c.centerYLabel, L"Center Y", TrackPos(c.centerY) / 1000.0);
    SetLabel(c.radiusLabel, L"Clear radius", TrackPos(c.radius) / 1000.0);
    SetLabel(c.softnessLabel, L"Edge softness", TrackPos(c.softness) / 1000.0);
    SetLabel(c.darkenStrengthLabel, L"Darken strength", TrackPos(c.darkenStrength) / 1000.0);
    SetLabel(c.blurRadiusLabel, L"Blur radius", TrackPos(c.blurRadius) / 10.0, L" px");
    SetLabel(c.blurStrengthLabel, L"Blur strength", TrackPos(c.blurStrength) / 1000.0);
}

inline int ModeIndex(StereoPostProcessMode mode, bool enabled) noexcept
{
    if (!enabled || mode == StereoPostProcessMode::None) return 0;
    if (mode == StereoPostProcessMode::Darken) return 1;
    if (mode == StereoPostProcessMode::Blur) return 2;
    return 0;
}

inline void LoadRuntimeIntoControls() noexcept
{
    auto& c = State();
    const auto config = CalibrationRuntimeBridge::GetPostProcessRuntimeConfig();
    const auto& s = config.settings;

    SendMessageW(c.mode, CB_SETCURSEL, ModeIndex(s.mode, s.enabled), 0);
    SetTrackPos(c.centerX, static_cast<int>(std::lround(std::clamp(s.centerX01, 0.0f, 1.0f) * 1000.0f)));
    SetTrackPos(c.centerY, static_cast<int>(std::lround(std::clamp(s.centerY01, 0.0f, 1.0f) * 1000.0f)));
    SetTrackPos(c.radius, static_cast<int>(std::lround(std::clamp(s.radiusShortAxis01, 0.0f, 1.5f) * 1000.0f)));
    SetTrackPos(c.softness, static_cast<int>(std::lround(std::clamp(s.edgeSoftnessShortAxis01, 0.0f, 0.3f) * 1000.0f)));
    SetTrackPos(c.darkenStrength, static_cast<int>(std::lround((1.0f - std::clamp(s.outsideBrightness, 0.0f, 1.0f)) * 1000.0f)));
    SetTrackPos(c.blurRadius, static_cast<int>(std::lround(std::clamp(s.blurRadiusPixels, 1.0f, 64.0f) * 10.0f)));
    SetTrackPos(c.blurStrength, static_cast<int>(std::lround(std::clamp(s.blurStrength01, 0.0f, 1.0f) * 1000.0f)));
    SendMessageW(c.debugDot, BM_SETCHECK,
        GuiControlBridge::PostProcessDebugCenterDotVisible() ? BST_CHECKED : BST_UNCHECKED,
        0);
    UpdateLabels();
}

inline void ApplyControlsToRuntime() noexcept
{
    auto& c = State();
    auto config = CalibrationRuntimeBridge::GetPostProcessRuntimeConfig();
    auto settings = config.settings;

    const int modeIndex = static_cast<int>(SendMessageW(c.mode, CB_GETCURSEL, 0, 0));
    if (modeIndex <= 0) {
        settings.mode = StereoPostProcessMode::None;
        settings.enabled = false;
    } else if (modeIndex == 1) {
        settings.mode = StereoPostProcessMode::Darken;
        settings.enabled = true;
    } else {
        settings.mode = StereoPostProcessMode::Blur;
        settings.enabled = true;
    }

    settings.centerX01 = std::clamp(TrackPos(c.centerX) / 1000.0f, 0.0f, 1.0f);
    settings.centerY01 = std::clamp(TrackPos(c.centerY) / 1000.0f, 0.0f, 1.0f);
    settings.radiusShortAxis01 = std::clamp(TrackPos(c.radius) / 1000.0f, 0.0f, 1.5f);
    settings.edgeSoftnessShortAxis01 = std::clamp(TrackPos(c.softness) / 1000.0f, 0.0f, 0.3f);
    const float darkenStrength = std::clamp(TrackPos(c.darkenStrength) / 1000.0f, 0.0f, 1.0f);
    settings.outsideBrightness = 1.0f - darkenStrength;
    settings.blurRadiusPixels = std::clamp(TrackPos(c.blurRadius) / 10.0f, 1.0f, 64.0f);
    settings.blurSigmaPixels = std::max(0.01f, settings.blurRadiusPixels * 0.5f);
    settings.blurStrength01 = std::clamp(TrackPos(c.blurStrength) / 1000.0f, 0.0f, 1.0f);

    config.settings = settings;
    CalibrationRuntimeBridge::SetPostProcessRuntimeConfig(config);

    GuiControlBridge::SetPostProcessDebugCenterDotVisible(
        SendMessageW(c.debugDot, BM_GETCHECK, 0, 0) == BST_CHECKED);

    UpdateLabels();
}

inline void CreateControls(HWND window)
{
    auto& c = State();
    c.window = window;

    CreateLabel(window, 12, 12, 120, L"Mode");
    c.mode = CreateWindowW(
        L"COMBOBOX",
        L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
        150,
        8,
        220,
        140,
        window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdMode)),
        GetModuleHandleW(nullptr),
        nullptr);
    SendMessageW(c.mode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"None"));
    SendMessageW(c.mode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Darken"));
    SendMessageW(c.mode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Blur"));

    int y = 50;
    constexpr int labelX = 12;
    constexpr int trackX = 150;
    constexpr int labelW = 135;
    constexpr int trackW = 240;
    constexpr int stepY = 48;

    c.centerXLabel = CreateLabel(window, labelX, y, labelW, L"Center X");
    c.centerX = CreateTrack(window, kIdCenterX, trackX, y - 6, trackW, 0, 1000);
    y += stepY;

    c.centerYLabel = CreateLabel(window, labelX, y, labelW, L"Center Y");
    c.centerY = CreateTrack(window, kIdCenterY, trackX, y - 6, trackW, 0, 1000);
    y += stepY;

    c.radiusLabel = CreateLabel(window, labelX, y, labelW, L"Clear radius");
    c.radius = CreateTrack(window, kIdRadius, trackX, y - 6, trackW, 0, 1500);
    y += stepY;

    c.softnessLabel = CreateLabel(window, labelX, y, labelW, L"Edge softness");
    c.softness = CreateTrack(window, kIdSoftness, trackX, y - 6, trackW, 0, 300);
    y += stepY;

    c.darkenStrengthLabel = CreateLabel(window, labelX, y, labelW, L"Darken strength");
    c.darkenStrength = CreateTrack(window, kIdDarkenStrength, trackX, y - 6, trackW, 0, 1000);
    y += stepY;

    c.blurRadiusLabel = CreateLabel(window, labelX, y, labelW, L"Blur radius");
    c.blurRadius = CreateTrack(window, kIdBlurRadius, trackX, y - 6, trackW, 10, 640);
    y += stepY;

    c.blurStrengthLabel = CreateLabel(window, labelX, y, labelW, L"Blur strength");
    c.blurStrength = CreateTrack(window, kIdBlurStrength, trackX, y - 6, trackW, 0, 1000);
    y += stepY;

    c.debugDot = CreateWindowW(
        L"BUTTON",
        L"Show center debug dot",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        12,
        y,
        250,
        28,
        window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdDebugDot)),
        GetModuleHandleW(nullptr),
        nullptr);

    LoadRuntimeIntoControls();
}

inline LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE:
        CreateControls(window);
        return 0;
    case WM_HSCROLL:
        ApplyControlsToRuntime();
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        const int code = HIWORD(wParam);
        if ((id == kIdMode && code == CBN_SELCHANGE) ||
            (id == kIdDebugDot && code == BN_CLICKED)) {
            ApplyControlsToRuntime();
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

inline void ThreadMain() noexcept
{
    try {
        INITCOMMONCONTROLSEX commonControls{};
        commonControls.dwSize = sizeof(commonControls);
        commonControls.dwICC = ICC_BAR_CLASSES;
        InitCommonControlsEx(&commonControls);

        constexpr const wchar_t* className = L"DualIC4VarjoPostProcessControlWindow";
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = &WindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = className;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassW(&windowClass);

        HWND window = CreateWindowExW(
            WS_EX_TOOLWINDOW,
            className,
            L"Postprocess Controls",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            430,
            455,
            nullptr,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
        if (!window) {
            std::cerr << "[POSTPROCESS_UI] failed to create control window\n";
            return;
        }

        ShowWindow(window, SW_SHOWNORMAL);
        UpdateWindow(window);

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    } catch (const std::exception& exception) {
        std::cerr << "[POSTPROCESS_UI] failed: " << exception.what() << '\n';
    } catch (...) {
        std::cerr << "[POSTPROCESS_UI] failed: unknown error\n";
    }
}

} // namespace detail

inline void EnsureStarted()
{
    bool expected = false;
    if (!detail::Started().compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return;
    }

    std::thread(&detail::ThreadMain).detach();
}

} // namespace DualIC4Varjo::PostProcessControlWindow
