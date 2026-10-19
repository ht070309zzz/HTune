#pragma once

#include <cstddef>

namespace stage {

// Win32 long paths are limited to 32,767 UTF-16 code units including the
// terminator. Keep enough margin for full-path expansion and the unique
// sibling suffix used by atomic theme export.
inline constexpr std::size_t MaxLocalFilePathChars = 32700;

// A valid UTF-16 path needs at most three UTF-8 bytes per code unit (a
// surrogate pair uses four bytes for two units). JSON may additionally escape
// ASCII separators, so callers still apply their complete-payload limit.
inline constexpr std::size_t MaxLocalFilePathUtf8Bytes = MaxLocalFilePathChars * 3;

} // namespace stage
