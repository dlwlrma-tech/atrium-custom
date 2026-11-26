// Guard process (`--guard`) that restores Windows' taskbar if Atrium hangs or exits unexpectedly.
#pragma once

#include <windows.h>

namespace atrium {

void EnsureGuard(HANDLE& guard);
int RunGuard(DWORD pid, const wchar_t* hostClass);
void WaitForGuard();

} // namespace atrium
