// The app behind a media session (its AUMID, or its exe for a desktop player).
#pragma once

#include <string>
#include <unordered_map>

namespace atrium {

// Brings the player's main window forward (restored if minimised), or starts it by its AUMID.
void ActivatePlayerApp(const std::wstring& appId);

// The player's name as the shell shows it (its AppsFolder entry), else its exe's, capitalised; cached.
std::wstring ResolveAppName(const std::wstring& appId, std::unordered_map<std::wstring, std::wstring>& cache);

} // namespace atrium
