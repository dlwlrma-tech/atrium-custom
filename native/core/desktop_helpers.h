// Small desktop helpers the bar's menus and entries use: Windows' own shortcuts, a window's program, and
// what kind of thing a jump list's path is.
#pragma once

#include <windows.h>

#include <string>

namespace atrium {

// Win + key (and Ctrl), as the keyboard would send it: the system's Show Desktop (D) and the like.
void SendWinShortcut(WORD key, bool ctrl = false);

// The full path of the program that owns `window`; empty when it cannot be read.
std::wstring ProgramOf(HWND window);

// A local folder (a network path is not asked: an offline share answers in tens of seconds).
bool IsDirectoryPath(const std::wstring& path);

// A project or workspace file an IDE opens (.sln, .code-workspace, ...).
bool IsWorkspaceFile(const std::wstring& path);

// Returns foreground focus from Explorer's hidden tray overflow window to `previous` (or Progman).
void RestoreHiddenOverflowFocus(HWND previous);

} // namespace atrium
