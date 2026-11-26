// The bar's own entries (the logo, Launchpad, the tray, the status, the clock, ...): their keys, shared by
// bar.cpp and bar_entries.cpp.
#pragma once

#include <string>

namespace atrium {

inline constexpr wchar_t kClockKey[] = L"#clock", kStartKey[] = L"#start", kNotifyKey[] = L"#notify", kTrayKey[] = L"#tray", kImeKey[] = L"#ime",
                         kTrayItemPrefix[] = L"#tray:",
                         kStatusKey[] = L"#status", kMoreKey[] = L"#more", kDesktopKey[] = L"#desktop", kLaunchpadKey[] = L"#launchpad",
                         kMusicKey[] = L"#music", kStacksKey[] = L"#stacks";

// Bar entries and the clock start with '#'; app keys are AUMIDs or paths.
inline bool IsEntry(const std::wstring& key) { return !key.empty() && key[0] == L'#'; }
inline bool IsTrayItemKey(const std::wstring& key) { return key.starts_with(kTrayItemPrefix); }
inline std::wstring TrayItemPinKey(const std::wstring& key) { return IsTrayItemKey(key) ? key.substr(6) : std::wstring(); }

} // namespace atrium
