// Bar stacking policy: decides which foreign topmost windows the bar stays behind.
#pragma once

#include <windows.h>

#include <string_view>

namespace atrium {

// True when a foreign topmost window overlapping the bar is a menu, tooltip,
// shell surface, or click-through overlay that the bar must stay behind.
bool ShouldIgnoreTopmostCover(std::wstring_view cls, LONG_PTR style, LONG_PTR exStyle, bool isForeground, bool ownerIsForeground);

} // namespace atrium
