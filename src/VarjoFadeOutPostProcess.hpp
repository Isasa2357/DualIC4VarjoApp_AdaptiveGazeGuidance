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
            << "[VST_POSTPROCESS] shader configured; green mask will enable after calibration, "
            << "fade-out will take over on Esc\n";
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
        if (!ensureShaderEnabledLocked("green outside-plane mask")) {
            return false;
        }

        if (!greenModeLogged_) {
            greenModeLogged_ = true;
            std::cout
                << "[VST_POSTPROCESS] green outside-plane mask enabled "
                << "(Plane rect is passed through)\n";
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

        constants_ = makePassThroughConstants();
        constants_.mode = 2.0f;
        constants_.fadeAmount = 0.0f;
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
        std::cout << "[FADE] VST fade-out shader mode enabled\n";
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
    struct alignas(16) ShaderConstants {
        float mode = 0.0f;       // 0: passthrough, 1: green outside Plane, 2: fade-out
        float fadeAmount = 0.0f; // Used only when mode == 2
        float reserved0 = 0.0f;
        float reserved1 = 0.0f;
        float planeRects[4][4]{}; // x0, y0, x1, y1 in current view normalized coordinates
    };
    static_assert(sizeof(ShaderConstants) == 80, "shader constants must remain 16-byte aligned");

    static ShaderConstants makePassThroughConstants() noexcept
    {
        ShaderConstants constants{};
        constants.mode = 0.0f;
        constants.fadeAmount = 0.0f;
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
    float reserved0;
    float reserved1;
    float4 planeRects[4];
};

bool IsInsideRect(float2 uv, float4 rect)
{
    if (rect.z <= rect.x || rect.w <= rect.y) {
        return false;
    }
    return uv.x >= rect.x && uv.x <= rect.z && uv.y >= rect.y && uv.y <= rect.w;
}

[numthreads(BLOCK_SIZE, BLOCK_SIZE, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const int2 pixel = int2(dispatchThreadID.xy) + destRect.xy;
    if (pixel.x < destRect.x || pixel.y < destRect.y ||
        pixel.x >= destRect.x + destRect.z ||
        pixel.y >= destRect.y + destRect.w) {
        return;
    }

    const float4 source = inputTex.Load(int3(pixel, 0));

    if (mode > 1.5f) {
        const float fade = saturate(fadeAmount);
        const float3 fadedRgb = lerp(source.rgb, float3(0.0f, 0.0f, 0.0f), fade);
        outputTex[pixel] = float4(fadedRgb, source.a);
        return;
    }

    if (mode > 0.5f) {
        const float2 destSize = max(
            float2((float)destRect.z, (float)destRect.w),
            float2(1.0f, 1.0f));
        const float2 uv = (
            float2((float)pixel.x, (float)pixel.y) + 0.5f -
            float2((float)destRect.x, (float)destRect.y)) / destSize;
        int rectIndex = viewIndex;
        if (rectIndex < 0) rectIndex = 0;
        if (rectIndex > 3) rectIndex = 3;
        if (IsInsideRect(uv, planeRects[rectIndex])) {
            outputTex[pixel] = source;
        } else {
            outputTex[pixel] = float4(0.0f, 1.0f, 0.0f, source.a);
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
                const float elapsed = std::chrono::duration<float>(now - begin).count();
                const float fade = std::clamp(elapsed / durationSeconds, 0.0f, 1.0f);

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
            std::cout << "[FADE] VST fade-out constants reached 1.0\n";
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
