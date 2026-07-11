#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace DualIC4Varjo {

class ClockMapper {
public:
    ClockMapper();

    std::int64_t steadyMicroseconds(std::chrono::steady_clock::time_point value) const noexcept;
    std::int64_t unixMicroseconds(std::chrono::steady_clock::time_point value) const noexcept;
    std::string localIso8601(std::int64_t unixMicroseconds) const;

private:
    std::chrono::steady_clock::time_point steadyAnchor_;
    std::chrono::system_clock::time_point systemAnchor_;
};

// Non-template implementation used by the public wrapper below. Keeping the
// public name as a function template lets a translation unit provide a local
// non-template helper with the same name without creating an ambiguous call.
std::int64_t SignedDifferenceImpl(
    std::uint64_t lhs,
    std::uint64_t rhs) noexcept;

template <typename Dummy = void>
inline std::int64_t SignedDifference(
    std::uint64_t lhs,
    std::uint64_t rhs) noexcept
{
    return SignedDifferenceImpl(lhs, rhs);
}

} // namespace DualIC4Varjo
