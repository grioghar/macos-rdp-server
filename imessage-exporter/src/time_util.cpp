#include "imsg/time_util.hpp"

#include <cmath>
#include <cstdio>

namespace imsg {
namespace {
// Seconds between the Unix epoch (1970-01-01) and Apple's epoch (2001-01-01).
constexpr long long kAppleToUnixOffset = 978307200LL;
// Values at/above this magnitude are nanoseconds (macOS 10.13+); below, seconds.
constexpr long long kNanosecondThreshold = 100000000000LL;  // 1e11
}  // namespace

bool apple_time_to_epoch(long long value, std::time_t& out) {
    if (value == 0) return false;
    long long seconds = (std::llabs(value) >= kNanosecondThreshold)
                            ? value / 1000000000LL
                            : value;
    out = static_cast<std::time_t>(kAppleToUnixOffset + seconds);
    return true;
}

std::string format_timestamp(std::time_t t) {
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return std::string(buffer);
}

}  // namespace imsg
