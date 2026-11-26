// Follows an app's tray context menu opened through Explorer's hidden overflow
// window and moves it beside the pointer when the app places it off-position.
#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace atrium {

void FollowTrayMenu(POINT at, const std::wstring& exe, RECT avoidBar = {}, std::function<RECT()> currentBar = {});
void StopTrayMenuFollow();

} // namespace atrium
