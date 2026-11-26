// Stable pin keys and matching helpers for tray icons pinned onto the bar.
#pragma once

#include "shell/tray_model.h"
#include "win32_path.h"

#include <algorithm>
#include <string>
#include <vector>

namespace atrium {

inline constexpr size_t kMaxPinnedTrayIcons = 32;

// Stable identifier for persisting a pinned tray icon across restarts.
inline std::wstring TrayPinKey(const TrayIcon& icon)
{
    if (!icon.exe.empty()) {
        std::wstring key = L"exe:" + FoldCase(icon.exe);
        if (icon.exeOrdinal > 0)
            key += L"#" + std::to_wstring(icon.exeOrdinal);
        return key;
    }
    if (!icon.recordId.empty())
        return L"rec:" + FoldCase(icon.recordId);
    if (!icon.hostKey.empty())
        return L"host:" + FoldCase(icon.hostKey);
    const std::wstring title = TrayModel::Title(icon.tip);
    return title.empty() ? std::wstring() : L"tip:" + FoldCase(title);
}

inline bool TrayPinMatches(const std::wstring& pinKey, const std::vector<std::wstring>& pinned)
{
    if (pinKey.empty())
        return false;
    const std::wstring folded = FoldCase(pinKey);
    return std::ranges::any_of(pinned, [&](const std::wstring& entry) { return FoldCase(entry) == folded; });
}

inline bool TrayIconPinned(const TrayIcon& icon, const std::vector<std::wstring>& pinned)
{
    return TrayPinMatches(TrayPinKey(icon), pinned);
}

// Toggles `pinKey` in `pinned`; returns true when `pinKey` is now pinned.
inline bool ToggleTrayPin(std::vector<std::wstring>& pinned, const std::wstring& pinKey)
{
    if (pinKey.empty())
        return false;
    const std::wstring folded = FoldCase(pinKey);
    const auto it = std::ranges::find_if(pinned, [&](const std::wstring& entry) { return FoldCase(entry) == folded; });
    if (it != pinned.end()) {
        pinned.erase(it);
        return false;
    }
    if (pinned.size() >= kMaxPinnedTrayIcons)
        return false;
    pinned.push_back(folded);
    return true;
}

} // namespace atrium
