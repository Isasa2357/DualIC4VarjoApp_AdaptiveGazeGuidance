#include "TimeUtil.hpp"

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>

namespace DualIC4Varjo {

ClockMapper::ClockMapper()
    : steadyAnchor_(std::chrono::steady_clock::now())
    , systemAnchor_(std::chrono::system_clock::now())
{
}

std::int64_t ClockMapper::steadyMicroseconds(std::chrono::steady_clock::time_point value) const noexcept
{
    return std::chrono::duration_cast<std::chrono::microseconds>(value.time_since_epoch()).count();
}

std::int64_t ClockMapper::unixMicroseconds(std::chrono::steady_clock::time_point value) const noexcept
{
    const auto mapped = systemAnchor_ + (value - steadyAnchor_);
    return std::chrono::duration_cast<std::chrono::microseconds>(mapped.time_since_epoch()).count();
}

std::string ClockMapper::localIso8601(std::int64_t unixMicroseconds) const
{
    std::int64_t seconds = unixMicroseconds / 1'000'000;
    std::int64_t micros = unixMicroseconds % 1'000'000;
    if (micros < 0) {
        micros += 1'000'000;
        --seconds;
    }

    const std::time_t timeValue = static_cast<std::time_t>(seconds);
    std::tm local{};
    localtime_s(&local, &timeValue);

    std::ostringstream stream;
    stream << std::put_time(&local, "%Y-%m-%dT%H:%M:%S")
           << '.' << std::setw(6) << std::setfill('0') << micros;
    return stream.str();
}

std::int64_t SignedDifferenceImpl(
    std::uint64_t lhs,
    std::uint64_t rhs) noexcept
{
    constexpr auto maxSigned =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (lhs >= rhs) {
        return static_cast<std::int64_t>(std::min(lhs - rhs, maxSigned));
    }
    return -static_cast<std::int64_t>(std::min(rhs - lhs, maxSigned));
}

} // namespace DualIC4Varjo
