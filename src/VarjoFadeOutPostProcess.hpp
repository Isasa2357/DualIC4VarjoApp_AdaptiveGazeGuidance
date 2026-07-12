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
            lastError_ = "fade-out postprocess requires a Varjo session";
            return false;
        }
        if (!commandQueue_) {
            lastError_ = "fade-out postprocess requires a D3D12 command queue";
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
            static_cast<int64_t>(sizeof(FadeConstants)),
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

        FadeConstants constants{};
        constants.fadeAmount = 0.0f;
        if (!shader_->submitConstantBuffer(constants)) {
            lastError_ = shader_->lastError();
            shader_.reset();
            return false;
        }

        // Do not call varjo_MRSetShader(false) here. On some Varjo Runtime
        // versions this returns an opaque "Unknown error" before the video
        // postprocess path is actively used, even though the shader was locked,
        // configured, and supplied with constants successfully. A configured
        // shader is not applied until it is explicitly enabled, so the initial
        // false transition is unnecessary. Enable it only when Esc starts the
        // fade-out, after varjo_MRSetVideoRender(true).
        initialized_ = true;
        started_.store(false, std::memory_order_release);
        completed_.store(false, std::memory_order_release);
        std::cout << "[FADE] VST fade-out shader configured; will enable on Esc\n";
        return true;
    }

    bool startFadeOut()
    {
        if (!initialize()) return false;

        std::lock_guard<std::mutex> lock(mutex_);
        if (started_.load(std::memory_order_acquire)) return true;

        lastError_.clear();
        stopWorkerRequested_.store(false, std::memory_order_release);
        completed_.store(false, std::memory_order_release);

        varjo_GetError(session_.get());
        varjo_MRSetVideoRender(session_.get(), varjo_True);
        const varjo_Error videoRenderError = varjo_GetError(session_.get());
        if (videoRenderError != varjo_NoError) {
            lastError_ = std::string("varjo_MRSetVideoRender(true) failed: ") +
                safeVarjoErrorDesc(videoRenderError);
            return false;
        }

        FadeConstants constants{};
        constants.fadeAmount = 0.0f;
        if (!shader_->submitConstantBuffer(constants)) {
            lastError_ = shader_->lastError();
            return false;
        }
        if (!shader_->setEnabled(true)) {
            lastError_ = shader_->lastError();
            return false;
        }

        started_.store(true, std::memory_order_release);
        worker_ = std::thread([this] { workerMain(); });
        std::cout << "[FADE] VST fade-out shader enabled\n";
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
        if (shader_ && started_.load(std::memory_order_acquire)) {
            try { shader_->setEnabled(false); } catch (...) {}
        }
        shader_.reset();
        initialized_ = false;
        started_.store(false, std::memory_order_release);
    }

    bool started() const noexcept
    {
        return started_.load(std::memory_order_acquire);
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
    struct alignas(16) FadeConstants {
        float fadeAmount = 0.0f;
        float padding0 = 0.0f;
        float padding1 = 0.0f;
        float padding2 = 0.0f;
    };
    static_assert(sizeof(FadeConstants) == 16, "fade constants must stay 16 bytes");

    static const char* safeVarjoErrorDesc(varjo_Error error) noexcept
    {
        const char* description = varjo_GetErrorDesc(error);
        return description ? description : "unknown Varjo error";
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

cbuffer FadeOutConstants : register(b1)
{
    float fadeAmount;
    float3 userPadding;
};

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
    const float fade = saturate(fadeAmount);
    const float3 fadedRgb = lerp(source.rgb, float3(0.0f, 0.0f, 0.0f), fade);
    outputTex[pixel] = float4(fadedRgb, source.a);
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
            "DualIC4Varjo_FadeOutVideoPostProcess.hlsl",
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
            message << "D3DCompile failed for fade-out video postprocess: HRESULT=0x"
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
                FadeConstants constants{};
                constants.fadeAmount = fade;

                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (shader_ && !shader_->submitConstantBuffer(constants)) {
                        lastError_ = shader_->lastError();
                        break;
                    }
                }

                if (fade >= 1.0f) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }

            FadeConstants finalConstants{};
            finalConstants.fadeAmount = 1.0f;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (shader_) shader_->submitConstantBuffer(finalConstants);
            }
            completed_.store(true, std::memory_order_release);
            std::cout << "[FADE] VST fade-out constants reached 1.0\n";
        } catch (const std::exception& exception) {
            std::lock_guard<std::mutex> lock(mutex_);
            lastError_ = exception.what();
            completed_.store(true, std::memory_order_release);
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            lastError_ = "unknown fade-out postprocess worker failure";
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
    std::atomic_bool initialized_{false};
    std::atomic_bool started_{false};
    std::atomic_bool completed_{false};
    std::atomic_bool stopWorkerRequested_{false};
    std::string lastError_;
};

} // namespace DualIC4Varjo
