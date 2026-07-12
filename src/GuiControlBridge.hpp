#pragma once

#include <atomic>
#include <cstdint>

namespace DualIC4Varjo::GuiControlBridge {

struct PlaneCommandCounts {
    std::uint64_t moveLeft = 0;
    std::uint64_t moveRight = 0;
    std::uint64_t moveUp = 0;
    std::uint64_t moveDown = 0;
    std::uint64_t sizeIncrease = 0;
    std::uint64_t sizeDecrease = 0;
    std::uint64_t moveNear = 0;
    std::uint64_t moveFar = 0;
    std::uint64_t visibilityToggles = 0;
};

inline std::atomic_bool& KeyboardControlLockedStorage() noexcept
{
    static std::atomic_bool value{false};
    return value;
}

inline std::atomic_bool& PlaneVisibleStorage() noexcept
{
    static std::atomic_bool value{true};
    return value;
}

inline std::atomic_bool& ApplicationExitRequestedStorage() noexcept
{
    static std::atomic_bool value{false};
    return value;
}

inline std::atomic<std::uint64_t>& MoveLeftCounter() noexcept
{
    static std::atomic<std::uint64_t> value{0};
    return value;
}

inline std::atomic<std::uint64_t>& MoveRightCounter() noexcept
{
    static std::atomic<std::uint64_t> value{0};
    return value;
}

inline std::atomic<std::uint64_t>& MoveUpCounter() noexcept
{
    static std::atomic<std::uint64_t> value{0};
    return value;
}

inline std::atomic<std::uint64_t>& MoveDownCounter() noexcept
{
    static std::atomic<std::uint64_t> value{0};
    return value;
}

inline std::atomic<std::uint64_t>& SizeIncreaseCounter() noexcept
{
    static std::atomic<std::uint64_t> value{0};
    return value;
}

inline std::atomic<std::uint64_t>& SizeDecreaseCounter() noexcept
{
    static std::atomic<std::uint64_t> value{0};
    return value;
}

inline std::atomic<std::uint64_t>& MoveNearCounter() noexcept
{
    static std::atomic<std::uint64_t> value{0};
    return value;
}

inline std::atomic<std::uint64_t>& MoveFarCounter() noexcept
{
    static std::atomic<std::uint64_t> value{0};
    return value;
}

inline std::atomic<std::uint64_t>& VisibilityToggleCounter() noexcept
{
    static std::atomic<std::uint64_t> value{0};
    return value;
}

inline bool KeyboardControlLocked() noexcept
{
    return KeyboardControlLockedStorage().load(std::memory_order_acquire);
}

inline void SetKeyboardControlLocked(bool locked) noexcept
{
    KeyboardControlLockedStorage().store(locked, std::memory_order_release);
}

inline bool PlaneVisible() noexcept
{
    return PlaneVisibleStorage().load(std::memory_order_acquire);
}

inline void SetPlaneVisible(bool visible) noexcept
{
    PlaneVisibleStorage().store(visible, std::memory_order_release);
}

inline bool ApplicationExitRequested() noexcept
{
    return ApplicationExitRequestedStorage().load(std::memory_order_acquire);
}

inline bool ConsumeApplicationExitRequested() noexcept
{
    return ApplicationExitRequestedStorage().exchange(false, std::memory_order_acq_rel);
}

inline void RequestApplicationExit() noexcept
{
    ApplicationExitRequestedStorage().store(true, std::memory_order_release);
}

inline void RequestMoveLeft() noexcept { MoveLeftCounter().fetch_add(1, std::memory_order_acq_rel); }
inline void RequestMoveRight() noexcept { MoveRightCounter().fetch_add(1, std::memory_order_acq_rel); }
inline void RequestMoveUp() noexcept { MoveUpCounter().fetch_add(1, std::memory_order_acq_rel); }
inline void RequestMoveDown() noexcept { MoveDownCounter().fetch_add(1, std::memory_order_acq_rel); }
inline void RequestSizeIncrease() noexcept { SizeIncreaseCounter().fetch_add(1, std::memory_order_acq_rel); }
inline void RequestSizeDecrease() noexcept { SizeDecreaseCounter().fetch_add(1, std::memory_order_acq_rel); }
inline void RequestMoveNear() noexcept { MoveNearCounter().fetch_add(1, std::memory_order_acq_rel); }
inline void RequestMoveFar() noexcept { MoveFarCounter().fetch_add(1, std::memory_order_acq_rel); }
inline void RequestTogglePlaneVisibility() noexcept { VisibilityToggleCounter().fetch_add(1, std::memory_order_acq_rel); }

inline PlaneCommandCounts ConsumePlaneCommands() noexcept
{
    PlaneCommandCounts commands{};
    commands.moveLeft = MoveLeftCounter().exchange(0, std::memory_order_acq_rel);
    commands.moveRight = MoveRightCounter().exchange(0, std::memory_order_acq_rel);
    commands.moveUp = MoveUpCounter().exchange(0, std::memory_order_acq_rel);
    commands.moveDown = MoveDownCounter().exchange(0, std::memory_order_acq_rel);
    commands.sizeIncrease = SizeIncreaseCounter().exchange(0, std::memory_order_acq_rel);
    commands.sizeDecrease = SizeDecreaseCounter().exchange(0, std::memory_order_acq_rel);
    commands.moveNear = MoveNearCounter().exchange(0, std::memory_order_acq_rel);
    commands.moveFar = MoveFarCounter().exchange(0, std::memory_order_acq_rel);
    commands.visibilityToggles = VisibilityToggleCounter().exchange(0, std::memory_order_acq_rel);
    return commands;
}

} // namespace DualIC4Varjo::GuiControlBridge
