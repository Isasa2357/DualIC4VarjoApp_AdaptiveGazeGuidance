#pragma once

#include <atomic>
#include <cstdint>

namespace DualIC4Varjo::GuiPerformanceStats {

struct StereoCounterSnapshot {
    std::uint64_t left = 0;
    std::uint64_t right = 0;
};

inline std::atomic<std::uint64_t>& CameraLeftReadFrames() noexcept
{
    static std::atomic<std::uint64_t> value{0};
    return value;
}

inline std::atomic<std::uint64_t>& CameraRightReadFrames() noexcept
{
    static std::atomic<std::uint64_t> value{0};
    return value;
}

inline void SubmitCameraReadFrames(
    std::uint64_t leftReadFrames,
    std::uint64_t rightReadFrames) noexcept
{
    CameraLeftReadFrames().store(leftReadFrames, std::memory_order_release);
    CameraRightReadFrames().store(rightReadFrames, std::memory_order_release);
}

inline StereoCounterSnapshot CameraReadFrames() noexcept
{
    return {
        CameraLeftReadFrames().load(std::memory_order_acquire),
        CameraRightReadFrames().load(std::memory_order_acquire)};
}

} // namespace DualIC4Varjo::GuiPerformanceStats
