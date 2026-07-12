#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Varjo.h>
#include <Varjo_mr.h>
#include <VarjoToolkit/MR/VarjoVideoPostProcessShader.hpp>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#if defined(_MSC_VER)
#pragma comment(lib, "d3dcompiler.lib")
#endif

namespace DualIC4Varjo {

class VarjoFadeOutPostProcess {
public:
    struct Config {
        float fadeSeconds = 2.0f;
    };

    struct PlaneMaskRect {
        float x0 = 2.0f;
        float y0 = 2.0f;
        float x1 = -1.0f;
        float y1 = -1.0f;
    };
    using PlaneMaskRects = std::array<PlaneMaskRect, 4>;

    VarjoFadeOutPostProcess(
        std::shared_ptr<varjo_Session> session,
        ID3D12CommandQueue* commandQueue,
        Config config = {})
        : session_(std::move(session))
        , commandQueue_(commandQueue)
        , config_(config)
    {
    }

    ~VarjoFadeOutPostProcess()
    {
        stop();
    }

    VarjoFadeOutPostProcess(const VarjoFadeOutPostProcess&) = delete;
    VarjoFadeOutPostProcess& operator=(const VarjoFadeOutPostProcess&) = delete;

    bool initialize()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (initialized_) return true;
        lastError_.clear();

        if (!session_) {
            lastError_ = "video postprocess requires a Varjo session";
            return false;
        }
        if (!commandQueue_) {
            lastError_ = "video postprocess requires a D3D12 command queue";
            return false;
        }

        shader_ = std::make_unique<VarjoVideoPostProcessShader>(session_, true);
        if (!shader_ || !shader_->valid()) {
            lastError_ = shader_ ? shader_->lastError() :
                "failed to create VarjoVideoPostProcessShader";
            shader_.reset();
            return false;
        }

        Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
        if (!compileShader(bytecode)) {
            shader_.reset();
            return false;
        }

        auto shaderConfig = VarjoVideoPostProcessShader::makeVideoPostProcessConfig(
            static_cast<int64_t>(sizeof(ShaderConstants)),
            8,
            0,
            varjo_ShaderFlag_VideoPostProcess_None);

        if (!shader_->configureD3D12(
                commandQueue_,
                shaderConfig,
                bytecode->GetBufferPointer(),
                static_cast<int32_t>(bytecode->GetBufferSize()))) {
            lastError_ = shader_->lastError();
            shader_.reset();
            return false;
        }

        constants_ = makePassThroughConstants();
        if (!shader_->submitConstantBuffer(constants_)) {
            lastError_ = shader_->lastError();
            shader_.reset();
            return false;
        }

        // Do not call varjo_MRSetShader(false) here. On some Varjo Runtime
        // versions this returns an opaque "Unknown error" before the video
        // postprocess path is actively used, even though the shader was locked,
        // configured, and supplied with constants successfully. A configured
        // shader is not applied until it is explicitly enabled.
        initialized_ = true;
        shaderEnabled_.store(false, std::memory_order_release);
        fadeStarted_.store(false, std::memory_order_release);
        completed_.store(false, std::memory_order_release);
        std::cout
            << "[VST_POSTPROCESS] shader configured; outside-plane blur+darken "
            << "will use blur radius " << kDefaultBlurRadiusPixels
            << " px, a Plane-centered black circle is cut by the Plane rect, "
            << "focus views map through sourceFocusRect, fade-out will take over on Esc\n";
        return true;
    }

    bool updateGreenOutsidePlane(const PlaneMaskRects& rects)
    {
        if (!initialize()) return false;

        std::lock_guard<std::mutex> lock(mutex_);
        if (fadeStarted_.load(std::memory_order_acquire)) return true;

        lastError_.clear();
        constants_ = makePassThroughConstants();
        constants_.mode = 1.0f;
        constants_.fadeAmount = 0.0f;
        constants_.blurRadiusPixels = kDefaultBlurRadiusPixels;
        constants_.darkenMultiplier = 0.5f;
        for (std::size_t view = 0; view < rects.size(); ++view) {
            constants_.planeRects[view][0] = rects[view].x0;
            constants_.planeRects[view][1] = rects[view].y0;
            constants_.planeRects[view][2] = rects[view].x1;
            constants_.planeRects[view][3] = rects[view].y1;
        }

        if (!shader_->submitConstantBuffer(constants_)) {
            lastError_ = shader_->lastError();
            return false;
        }
        if (!ensureShaderEnabledLocked("outside-plane blur+darken")) {
            return false;
        }

        if (!greenModeLogged_) {
            greenModeLogged_ = true;
            std::cout
                << "[VST_POSTPROCESS] outside-plane blur+darken enabled "
                << "(blur radius=" << constants_.blurRadiusPixels
                << " px; black circle diameter=Plane diagonal, Plane rect is cut out; "
                << "context views use their Plane rect; focus views map to context via sourceFocusRect)\n";
        }
        return true;
    }

    bool startFadeOut()
    {
        if (!initialize()) return false;

        std::lock_guard<std::mutex> lock(mutex_);
        if (fadeStarted_.load(std::memory_order_acquire)) return true;

        lastError_.clear();
        stopWorkerRequested_.store(false, std::memory_order_release);
        completed_.store(false, std::memory_order_release);

        if (!enableVideoRenderLocked()) return false;

        // Keep the current blur and darken settings from the normal Plane mask
        // mode. Once shutdown hides the Plane, the former Plane region should
        // also be blurred/darkened, then the darken multiplier fades to black.
        constants_.mode = 2.0f;
        constants_.fadeAmount = 0.0f;
        if (constants_.blurRadiusPixels <= 0.0f) {
            constants_.blurRadiusPixels = kDefaultBlurRadiusPixels;
        }
        if (constants_.darkenMultiplier <= 0.0f) {
            constants_.darkenMultiplier = 0.5f;
        }
        if (!shader_->submitConstantBuffer(constants_)) {
            lastError_ = shader_->lastError();
            return false;
        }
        if (!shaderEnabled_.load(std::memory_order_acquire)) {
            if (!shader_->setEnabled(true)) {
                lastError_ = shader_->lastError();
                return false;
            }
            shaderEnabled_.store(true, std::memory_order_release);
        }

        fadeStarted_.store(true, std::memory_order_release);
        worker_ = std::thread([this] { workerMain(); });
        std::cout
            << "[FADE] VST fade-out shader mode enabled; blur radius "
            << constants_.blurRadiusPixels
            << " px is kept and brightness fades to black\n";
        return true;
    }

    void waitForCompletion()
    {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void stop() noexcept
    {
        stopWorkerRequested_.store(true, std::memory_order_release);
        if (worker_.joinable()) {
            try { worker_.join(); } catch (...) {}
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (shader_ && shaderEnabled_.load(std::memory_order_acquire)) {
            try { shader_->setEnabled(false); } catch (...) {}
        }
        shader_.reset();
        initialized_ = false;
        shaderEnabled_.store(false, std::memory_order_release);
        fadeStarted_.store(false, std::memory_order_release);
        greenModeLogged_ = false;
    }

    bool started() const noexcept
    {
        return fadeStarted_.load(std::memory_order_acquire);
    }

    bool completed() const noexcept
    {
        return completed_.load(std::memory_order_acquire);
    }

    std::string lastError() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastError_;
    }

private:
    static constexpr float kDefaultBlurRadiusPixels = 8.0f;

    struct alignas(16) ShaderConstants {
        float mode = 0.0f;            // 0: passthrough, 1: blur+darken outside Plane, 2: full-screen blur+darken fade-out
        float fadeAmount = 0.0f;      // Used only when mode == 2. 0 -> darkenMultiplier, 1 -> black.
        float blurRadiusPixels = kDefaultBlurRadiusPixels;
        float darkenMultiplier = 0.5f;
        float planeRects[4][4]{}; // x0, y0, x1, y1 in context-view normalized coordinates
    };
    static_assert(sizeof(ShaderConstants) == 80, "shader constants must remain 16-byte aligned");

    static ShaderConstants makePassThroughConstants() noexcept
    {
        ShaderConstants constants{};
        constants.mode = 0.0f;
        constants.fadeAmount = 0.0f;
        constants.blurRadiusPixels = kDefaultBlurRadiusPixels;
        constants.darkenMultiplier = 0.5f;
        for (auto& rect : constants.planeRects) {
            rect[0] = 2.0f;
            rect[1] = 2.0f;
            rect[2] = -1.0f;
            rect[3] = -1.0f;
        }
        return constants;
    }

    static const char* safeVarjoErrorDesc(varjo_Error error) noexcept
    {
        const char* description = varjo_GetErrorDesc(error);
        return description ? description : "unknown Varjo error";
    }

    bool enableVideoRenderLocked()
    {
        varjo_GetError(session_.get());
        varjo_MRSetVideoRender(session_.get(), varjo_True);
        const varjo_Error videoRenderError = varjo_GetError(session_.get());
        if (videoRenderError != varjo_NoError) {
            lastError_ = std::string("varjo_MRSetVideoRender(true) failed: ") +
                safeVarjoErrorDesc(videoRenderError);
            return false;
        }
        return true;
    }

    bool ensureShaderEnabledLocked(const char* reason)
    {
        if (!enableVideoRenderLocked()) return false;
        if (shaderEnabled_.load(std::memory_order_acquire)) return true;
        if (!shader_->setEnabled(true)) {
            lastError_ = shader_->lastError();
            return false;
        }
        shaderEnabled_.store(true, std::memory_order_release);
        std::cout << "[VST_POSTPROCESS] shader enabled for "
                  << (reason ? reason : "postprocess") << '\n';
        return true;
    }

    static const char* hlslSource() noexcept
    {
        return R"hlsl(
#define BLOCK_SIZE 8

#define VIEW_CONTEXT_L 0
#define VIEW_CONTEXT_R 1
#define VIEW_FOCUS_L 2
#define VIEW_FOCUS_R 3

Texture2D<float4> inputTex : register(t0);
RWTexture2D<float4> outputTex : register(u0);
SamplerState SamplerLinearClamp : register(s0);
SamplerState SamplerLinearWrap : register(s1);

cbuffer VarjoVideoPostProcessConstants : register(b0)
{
    int2 sourceSize;
    float sourceTime;
    int viewIndex;
    int4 destRect;
    float4x4 projection;
    float4x4 inverseProjection;
    float4x4 view;
    float4x4 inverseView;
    int4 sourceFocusRect;
    int2 sourceContextSize;
    int2 varjoPadding;
};

cbuffer DualIC4VarjoPostProcessConstants : register(b1)
{
    float mode;
    float fadeAmount;
    float blurRadiusPixels;
    float darkenMultiplier;
    float4 planeRects[4];
};

bool IsInsideRect(float2 uv, float4 rect)
{
    if (rect.z <= rect.x || rect.w <= rect.y) {
        return false;
    }
    return uv.x >= rect.x && uv.x <= rect.z && uv.y >= rect.y && uv.y <= rect.w;
}

int ContextRectIndexForCurrentView()
{
    if (viewIndex == VIEW_CONTEXT_R || viewIndex == VIEW_FOCUS_R) {
        return 1;
    }
    return 0;
}

float2 ContextUvForCurrentView(int2 pixel, int2 localPixel)
{
    const float2 contextSize = max(
        float2((float)sourceContextSize.x, (float)sourceContextSize.y),
        float2(1.0f, 1.0f));

    if (viewIndex == VIEW_FOCUS_L || viewIndex == VIEW_FOCUS_R) {
        // Varjo focus views are sampled in focus-local coordinates, but the
        // postprocess decision should match the context view at the same real
        // VST location. sourceFocusRect gives the focus-view area inside the
        // context texture. In practice the focus local Y axis is flipped
        // relative to context, so flip Y before mapping into sourceFocusRect.
        const float2 c0 = float2((float)sourceFocusRect.x, (float)sourceFocusRect.y);
        const float2 c1 = float2((float)sourceFocusRect.z, (float)sourceFocusRect.w);
        const float2 cMin = min(c0, c1);
        const float2 cMax = max(c0, c1);
        const float2 focusRectSizeInContext = max(cMax - cMin, float2(1.0f, 1.0f));
        const float2 focusSize = max(
            float2((float)sourceSize.x, (float)sourceSize.y),
            float2(1.0f, 1.0f));

        const float focusX = clamp((float)localPixel.x, 0.0f, focusSize.x - 1.0f);
        const float focusY = clamp((float)localPixel.y, 0.0f, focusSize.y - 1.0f);
        const float focusYFlipped = (focusSize.y - 1.0f) - focusY;

        const float2 contextPixel = cMin +
            float2(focusX, focusYFlipped) * (focusRectSizeInContext / focusSize);
        return saturate((contextPixel + 0.5f) / contextSize);
    }

    // Context views already use context texture coordinates. Use pixel rather
    // than destRect-normalized coordinates so focus and context execute the
    // same mask decision in context space.
    return saturate((float2((float)pixel.x, (float)pixel.y) + 0.5f) / contextSize);
}

bool IsInsidePlaneDiagonalCircle(float2 contextUv, float4 rect)
{
    if (rect.z <= rect.x || rect.w <= rect.y) {
        return false;
    }

    const float2 contextSize = max(
        float2((float)sourceContextSize.x, (float)sourceContextSize.y),
        float2(1.0f, 1.0f));
    const float2 rectCenter = 0.5f * (rect.xy + rect.zw);
    const float2 rectSizePixels = abs(rect.zw - rect.xy) * contextSize;

    // The requested circle is centered on the Plane center. Its diameter is the
    // Plane diagonal, so the radius is half of the Plane-rect diagonal in context
    // pixels. The Plane rectangle is tested separately and returned unchanged,
    // which makes the visible VST shape a filled circle with a rectangular cutout.
    const float radiusPixels = 0.5f * length(rectSizePixels);
    const float2 deltaPixels = (contextUv - rectCenter) * contextSize;
    return dot(deltaPixels, deltaPixels) <= radiusPixels * radiusPixels;
}

int2 ClampCurrentViewPixel(int2 p)
{
    const int2 viewMin = destRect.xy;
    const int2 viewMax = destRect.xy + max(destRect.zw, int2(1, 1)) - int2(1, 1);
    return int2(
        clamp(p.x, viewMin.x, viewMax.x),
        clamp(p.y, viewMin.y, viewMax.y));
}

float4 LoadSourceClamped(int2 p)
{
    return inputTex.Load(int3(ClampCurrentViewPixel(p), 0));
}

float3 BoxBlurCurrentViewRgb(int2 pixel)
{
    const int radius = min(32, max(1, (int)(blurRadiusPixels + 0.5f)));
    float3 accum = float3(0.0f, 0.0f, 0.0f);
    float sampleCount = 0.0f;

    [loop]
    for (int dy = -radius; dy <= radius; ++dy) {
        [loop]
        for (int dx = -radius; dx <= radius; ++dx) {
            accum += LoadSourceClamped(pixel + int2(dx, dy)).rgb;
            sampleCount += 1.0f;
        }
    }

    return accum / max(sampleCount, 1.0f);
}

float3 BlurDarkenRgb(int2 pixel, float multiplier)
{
    const float3 blurredRgb = BoxBlurCurrentViewRgb(pixel);
    return blurredRgb * saturate(multiplier);
}

[numthreads(BLOCK_SIZE, BLOCK_SIZE, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const int2 localPixel = int2(dispatchThreadID.xy);
    const int2 pixel = localPixel + destRect.xy;
    if (pixel.x < destRect.x || pixel.y < destRect.y ||
        pixel.x >= destRect.x + destRect.z ||
        pixel.y >= destRect.y + destRect.w) {
        return;
    }

    const float4 source = inputTex.Load(int3(pixel, 0));

    if (mode > 1.5f) {
        // After shutdown the Plane is hidden. Apply the same blur everywhere,
        // including the former Plane area, and fade brightness from the current
        // darkenMultiplier down to full black over fadeSeconds.
        const float brightness = saturate(darkenMultiplier) * (1.0f - saturate(fadeAmount));
        outputTex[pixel] = float4(BlurDarkenRgb(pixel, brightness), source.a);
        return;
    }

    if (mode > 0.5f) {
        const float2 contextUv = ContextUvForCurrentView(pixel, localPixel);
        const int rectIndex = ContextRectIndexForCurrentView();
        const float4 planeRect = planeRects[rectIndex];
        if (IsInsideRect(contextUv, planeRect)) {
            outputTex[pixel] = source;
        } else if (IsInsidePlaneDiagonalCircle(contextUv, planeRect)) {
            outputTex[pixel] = float4(0.0f, 0.0f, 0.0f, source.a);
        } else {
            outputTex[pixel] = float4(BlurDarkenRgb(pixel, darkenMultiplier), source.a);
        }
        return;
    }

    outputTex[pixel] = source;
}
)hlsl";
    }

    bool compileShader(Microsoft::WRL::ComPtr<ID3DBlob>& bytecode)
    {
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        const char* source = hlslSource();
        const HRESULT hr = D3DCompile(
            source,
            std::strlen(source),
            "DualIC4Varjo_VideoPostProcessMaskAndFade.hlsl",
            nullptr,
            nullptr,
            "main",
            "cs_5_0",
            flags,
            0,
            &bytecode,
            &errors);
        if (FAILED(hr)) {
            std::ostringstream message;
            message << "D3DCompile failed for video postprocess mask/fade shader: HRESULT=0x"
                    << std::hex << static_cast<unsigned long>(hr);
            if (errors && errors->GetBufferPointer()) {
                message << "\n" << static_cast<const char*>(errors->GetBufferPointer());
            }
            lastError_ = message.str();
            return false;
        }
        return true;
    }

    void workerMain() noexcept
    {
        try {
            const auto begin = std::chrono::steady_clock::now();
            const float durationSeconds = std::max(0.001f, config_.fadeSeconds);
            for (;;) {
                if (stopWorkerRequested_.load(std::memory_order_acquire)) break;

                const auto now = std::chrono::steady_clock::now();
                const float fade = std::clamp(
                    std::chrono::duration<float>(now - begin).count() / durationSeconds,
                    0.0f,
                    1.0f);

                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    constants_.mode = 2.0f;
                    constants_.fadeAmount = fade;
                    if (shader_ && !shader_->submitConstantBuffer(constants_)) {
                        lastError_ = shader_->lastError();
                        break;
                    }
                }

                if (fade >= 1.0f) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                constants_.mode = 2.0f;
                constants_.fadeAmount = 1.0f;
                if (shader_) shader_->submitConstantBuffer(constants_);
            }
            completed_.store(true, std::memory_order_release);
            std::cout << "[FADE] VST fade-out reached black with blur preserved\n";
        } catch (const std::exception& exception) {
            std::lock_guard<std::mutex> lock(mutex_);
            lastError_ = exception.what();
            completed_.store(true, std::memory_order_release);
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            lastError_ = "unknown video postprocess worker failure";
            completed_.store(true, std::memory_order_release);
        }
    }

private:
    std::shared_ptr<varjo_Session> session_;
    ID3D12CommandQueue* commandQueue_ = nullptr;
    Config config_{};
    std::unique_ptr<VarjoVideoPostProcessShader> shader_;
    mutable std::mutex mutex_;
    std::thread worker_;
    ShaderConstants constants_{};
    std::atomic_bool initialized_{false};
    std::atomic_bool shaderEnabled_{false};
    std::atomic_bool fadeStarted_{false};
    std::atomic_bool completed_{false};
    std::atomic_bool stopWorkerRequested_{false};
    bool greenModeLogged_ = false;
    std::string lastError_;
};

} // namespace DualIC4Varjo
