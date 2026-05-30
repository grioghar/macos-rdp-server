#include "imsg/attributed_body.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace imsg {
namespace {

// Class markers that precede the inline string payload, most specific first.
const std::array<std::string, 3> kMarkers = {
    "NSMutableString", "NSString", "NSAttributedString"};

// Candidate counts of archiver header bytes between the marker and the length
// prefix. Real databases use 5; nearby values are tried for robustness.
constexpr std::array<std::size_t, 3> kHeaderOffsets = {5, 6, 4};

// Reject runs containing low control characters (excluding tab/newline), which
// indicate the payload start was mis-located.
bool looks_like_garbage(const std::string& s) {
    for (unsigned char c : s) {
        if (c < 0x09) return true;
    }
    return false;
}

// Validates that the bytes form well-formed UTF-8.
bool is_valid_utf8(const std::string& s) {
    std::size_t i = 0;
    const std::size_t n = s.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t extra;
        if (c < 0x80) {
            extra = 0;
        } else if ((c >> 5) == 0x06) {
            extra = 1;
        } else if ((c >> 4) == 0x0E) {
            extra = 2;
        } else if ((c >> 3) == 0x1E) {
            extra = 3;
        } else {
            return false;
        }
        if (i + extra >= n) return false;
        for (std::size_t k = 1; k <= extra; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        }
        i += extra + 1;
    }
    return true;
}

// Parses the length-prefixed UTF-8 string that follows a class marker.
std::string read_inline_string(const std::string& payload) {
    for (std::size_t offset : kHeaderOffsets) {
        if (offset >= payload.size()) continue;

        unsigned char marker = static_cast<unsigned char>(payload[offset]);
        std::size_t length;
        std::size_t body_start;
        if (marker == 0x81) {
            if (offset + 2 >= payload.size()) continue;
            length = static_cast<unsigned char>(payload[offset + 1]) |
                     (static_cast<unsigned char>(payload[offset + 2]) << 8);
            body_start = offset + 3;
        } else {
            length = marker;
            body_start = offset + 1;
        }
        if (length == 0) continue;
        if (body_start + length > payload.size()) continue;

        std::string text = payload.substr(body_start, length);
        if (!text.empty() && is_valid_utf8(text) && !looks_like_garbage(text)) {
            return text;
        }
    }
    return std::string();
}

}  // namespace

std::string decode_attributed_body(const unsigned char* data, std::size_t len) {
    if (data == nullptr || len == 0) return std::string();
    std::string blob(reinterpret_cast<const char*>(data), len);
    return decode_attributed_body(blob);
}

std::string decode_attributed_body(const std::string& blob) {
    if (blob.empty()) return std::string();

    std::string data = blob;
    // Trailing attribute runs follow an NSDictionary/NSNumber; dropping from the
    // first such marker avoids decoding archiver metadata as text.
    for (const char* tail : {"NSDictionary", "NSNumber"}) {
        std::size_t idx = data.find(tail);
        if (idx != std::string::npos) data = data.substr(0, idx);
    }

    for (const std::string& marker : kMarkers) {
        std::size_t idx = data.find(marker);
        if (idx == std::string::npos) continue;
        std::string payload = data.substr(idx + marker.size());
        std::string text = read_inline_string(payload);
        if (!text.empty()) return text;
    }
    return std::string();
}

}  // namespace imsg
