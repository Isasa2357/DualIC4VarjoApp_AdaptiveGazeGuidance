#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Varjo.h>
#include <Varjo_mr.h>

#include <chrono>
#include <mutex>
#include <unordered_map>

namespace DualIC4Varjo::VstVideoRenderOnceHook {
namespace detail {

using Clock = std::chrono::steady_clock;

struct SessionState {
    bool videoRenderEnabled = false;
    bool waitingForEnableResult = false;
    bool suppressedByBackoff = false;
    varjo_Error lastEnableError = varjo_NoError;
    Clock::time_point nextRetry{};
};

inline std::mutex& Mutex() noexcept
{
    static std::mutex value;
    return value;
}

inline std::unordered_map<varjo_Session*, SessionState>& States()
{
    static std::unordered_map<varjo_Session*, SessionState> value;
    return value;
}

} // namespace detail

inline void SetVideoRender(varjo_Session* session, varjo_Bool enabled)
{
    if (!session) {
        ::varjo_MRSetVideoRender(session, enabled);
        return;
    }

    if (enabled != varjo_True) {
        {
            std::lock_guard<std::mutex> lock(detail::Mutex());
            auto& state = detail::States()[session];
            state.videoRenderEnabled = false;
            state.waitingForEnableResult = false;
            state.suppressedByBackoff = false;
            state.lastEnableError = varjo_NoError;
            state.nextRetry = detail::Clock::time_point{};
        }
        ::varjo_MRSetVideoRender(session, enabled);
        return;
    }

    const auto now = detail::Clock::now();
    {
        std::lock_guard<std::mutex> lock(detail::Mutex());
        auto& state = detail::States()[session];
        if (state.videoRenderEnabled) {
            state.suppressedByBackoff = false;
            return;
        }
        if (state.nextRetry != detail::Clock::time_point{} && now < state.nextRetry) {
            state.suppressedByBackoff = true;
            return;
        }
        state.waitingForEnableResult = true;
        state.suppressedByBackoff = false;
    }

    ::varjo_MRSetVideoRender(session, enabled);
}

inline varjo_Error GetError(varjo_Session* session)
{
    if (!session) {
        return ::varjo_GetError(session);
    }

    {
        std::lock_guard<std::mutex> lock(detail::Mutex());
        auto& state = detail::States()[session];
        if (state.suppressedByBackoff) {
            state.suppressedByBackoff = false;
            return state.lastEnableError != varjo_NoError
                ? state.lastEnableError
                : varjo_NoError;
        }
    }

    const varjo_Error error = ::varjo_GetError(session);

    {
        std::lock_guard<std::mutex> lock(detail::Mutex());
        auto& state = detail::States()[session];
        if (state.waitingForEnableResult) {
            state.waitingForEnableResult = false;
            if (error == varjo_NoError) {
                state.videoRenderEnabled = true;
                state.lastEnableError = varjo_NoError;
                state.nextRetry = detail::Clock::time_point{};
            } else {
                state.videoRenderEnabled = false;
                state.lastEnableError = error;
                state.nextRetry = detail::Clock::now() + std::chrono::seconds(2);
            }
        }
    }

    return error;
}

} // namespace DualIC4Varjo::VstVideoRenderOnceHook
