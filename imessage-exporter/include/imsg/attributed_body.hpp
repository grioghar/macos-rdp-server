// Best-effort decoder for the Messages `attributedBody` column.
#pragma once

#include <cstddef>
#include <string>

namespace imsg {

// On modern macOS the plain `message.text` column is often NULL and the visible
// text lives in `attributedBody`: an NSAttributedString serialized with the
// legacy NeXTSTEP "typedstream" archiver. The string payload sits a few header
// bytes after the NSString/NSMutableString class marker as a length-prefixed
// UTF-8 run. This extracts that payload heuristically, returning "" when the
// layout is unexpected so export never fails on an unusual blob.
std::string decode_attributed_body(const unsigned char* data, std::size_t len);
std::string decode_attributed_body(const std::string& blob);

}  // namespace imsg
