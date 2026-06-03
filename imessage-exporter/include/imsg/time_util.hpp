// Apple ("Mac absolute time") timestamp conversion.
#pragma once

#include <ctime>
#include <string>

namespace imsg {

// Converts a Messages timestamp (offset from 2001-01-01 UTC, in nanoseconds on
// modern macOS or whole seconds on legacy databases) to epoch seconds.
// Returns false for zero/missing values.
bool apple_time_to_epoch(long long value, std::time_t& out);

// Formats epoch seconds as local "YYYY-MM-DD HH:MM:SS".
std::string format_timestamp(std::time_t t);

}  // namespace imsg
