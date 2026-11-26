#include "ui/tray_menu_follow.h"

#include "log.h"
#include "shell/tray_click.h"
#include "shell/tray_model.h"
#include "ui/tray_panel.h"

#include <shlwapi.h>

#include <algorithm>
#include <cstdlib>
#include <iterator>

namespace atrium {

namespace {

// An app's menu opened through the hidden system tray: Explorer tells the app
// where its (hidden) button is, and some apps open the menu there, at the
// screen's left edge. A newly shown menu-shaped popup from the exact executable
// is moved to the click. Unknown programs and helper processes are excluded. An
// out-of-context event hook: nothing runs in the app or in Explorer.
struct MenuFollow {
    HWINEVENTHOOK hook = nullptr;
    POINT at{};
    RECT avoidBar{};
    std::wstring exe;
    UINT_PTR timer = 0;
    DWORD pid = 0;
    DWORD clickInput = 0;
    ULONGLONG until = 0;
    HWND menu = nullptr; // the menu moved: put back if the app moves it again meanwhile
    POINT placed{};
    int moves = 0;
} g_follow;

void CALLBACK FollowExpired(HWND, UINT, UINT_PTR timer, DWORD)
{
    if (timer == g_follow.timer && GetTickCount64() >= g_follow.until)
        StopTrayMenuFollow();
}

// Next to the click: above and to the right of it, else below or to the left; in the work area.
POINT MenuPlace(POINT at, int w, int h, const RECT& avoidBar)
{
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST), &mi);
    RECT area = mi.rcWork;
    if (!IsRectEmpty(&avoidBar)) {
        const LONG midBar = (avoidBar.top + avoidBar.bottom) / 2;
        const LONG midMon = (mi.rcMonitor.top + mi.rcMonitor.bottom) / 2;
        if (midBar >= midMon)
            area.bottom = std::min(area.bottom, avoidBar.top - 6);
        else
            area.top = std::max(area.top, avoidBar.bottom + 6);
    }
    return tray::PlaceMenu(at, w, h, area);
}

bool SameExeOrVendor(const std::wstring& a, const std::wstring& b)
{
    if (CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL)
        return true;
    auto containsCI = [](const std::wstring& s, const wchar_t* sub) {
        return StrStrIW(s.c_str(), sub) != nullptr;
    };
    if (containsCI(a, L"\\NVIDIA Corporation\\") && containsCI(b, L"\\NVIDIA Corporation\\"))
        return true;
    return false;
}

bool FollowCancelKeyDown()
{
    constexpr int kKeys[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_RETURN, VK_ESCAPE, VK_SPACE};
    return std::ranges::any_of(kKeys, [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; });
}

void CALLBACK MenuShown(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD)
{
    if (event != EVENT_OBJECT_SHOW && event != EVENT_OBJECT_LOCATIONCHANGE)
        return;
    if (GetTickCount64() > g_follow.until) {
        StopTrayMenuFollow();
        return;
    }
    LASTINPUTINFO input{sizeof(input)};
    POINT cursor{};
    const SIZE dblClick{GetSystemMetrics(SM_CXDOUBLECLK), GetSystemMetrics(SM_CYDOUBLECLK)};
    // The panel's click, or the real click the model sent for it; ignore small pointer jitter.
    if (!GetLastInputInfo(&input) || !GetCursorPos(&cursor) ||
        TrayFollowCancelled(input.dwTime, g_follow.clickInput, TrayModel::LastSyntheticInput(), TrayModel::SyntheticPointerInFlight(), g_follow.at, cursor,
                            dblClick, FollowCancelKeyDown())) {
        StopTrayMenuFollow(); // a later user action must not have its menu moved by this click
        return;
    }
    if (!hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF)
        return;
    if (!g_follow.menu && event != EVENT_OBJECT_SHOW)
        return;
    // Revalidate ownership even on subsequent location events: HWNDs can be reused.
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid || (g_follow.menu && (hwnd != g_follow.menu || pid != g_follow.pid)))
        return;
    std::wstring path;
    bool elevated = true; // an unqueryable token must not authorize a move
    if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[32768];
        DWORD size = DWORD(std::size(buf));
        if (QueryFullProcessImageNameW(process, 0, buf, &size))
            path.assign(buf, size);
        HANDLE token = nullptr;
        if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
            TOKEN_ELEVATION e{};
            DWORD got = 0;
            if (GetTokenInformation(token, TokenElevation, &e, sizeof(e), &got))
                elevated = e.TokenIsElevated != 0;
            CloseHandle(token);
        }
        CloseHandle(process);
    }
    if (g_follow.exe.empty() || path.empty() || elevated || !SameExeOrVendor(path, g_follow.exe))
        return;
    wchar_t cls[64]{};
    if (!GetClassNameW(hwnd, cls, 64))
        return;
    const bool nativeMenu = wcscmp(cls, L"#32768") == 0;
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE), ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    const bool customMenu = (style & WS_POPUP) && (style & WS_CAPTION) != WS_CAPTION && (ex & (WS_EX_TOOLWINDOW | WS_EX_TOPMOST)) && !(ex & WS_EX_APPWINDOW) &&
                            _wcsicmp(cls, L"tooltips_class32") != 0;
    if (!nativeMenu && !customMenu)
        return;
    const wchar_t* name = PathFindFileNameW(path.c_str());
    RECT r{};
    if (g_follow.menu) {
        // Moved already: some apps (WPF, a change of monitor's scale) place it once more themselves.
        if (hwnd != g_follow.menu || !IsWindowVisible(hwnd) || !GetWindowRect(hwnd, &r))
            return;
        // A pixel or two is the app rounding to its scale.
        if ((std::abs(r.left - g_follow.placed.x) > 2 || std::abs(r.top - g_follow.placed.y) > 2) && g_follow.moves < 3) {
            const POINT p = MenuPlace(g_follow.at, r.right - r.left, r.bottom - r.top, g_follow.avoidBar);
            g_follow.placed = p;
            ++g_follow.moves;
            SetWindowPos(hwnd, HWND_TOPMOST, p.x, p.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
            Log(L"tray: the menu went to %ld,%ld again: back to %ld,%ld", r.left, r.top, p.x, p.y);
        }
        return;
    }
    // Only a newly shown, menu-shaped popup from the exact executable is eligible.
    if (GetAncestor(hwnd, GA_ROOT) != hwnd || !IsWindowVisible(hwnd) || !GetWindowRect(hwnd, &r))
        return;
    const int w = r.right - r.left, h = r.bottom - r.top;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(g_follow.at, MONITOR_DEFAULTTONEAREST), &mi);
    if (w < 40 || h < 20 || w > (mi.rcWork.right - mi.rcWork.left) * 3 / 5 || h > (mi.rcWork.bottom - mi.rcWork.top) * 19 / 20)
        return; // a mark, a balloon, a window: not a menu
    const POINT at = g_follow.at;
    RECT barOverlap{};
    const bool hitsBar = !IsRectEmpty(&g_follow.avoidBar) && IntersectRect(&barOverlap, &r, &g_follow.avoidBar) != FALSE;
    if (!hitsBar && !tray::ShouldMoveFollowedMenu(r, at, mi.rcMonitor, mi.rcWork, nativeMenu)) {
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        StopTrayMenuFollow();
        return; // the app put it by the pointer itself and it fits
    }
    const POINT p = MenuPlace(at, w, h, g_follow.avoidBar);
    if (!SetWindowPos(hwnd, HWND_TOPMOST, p.x, p.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE)) {
        Log(L"tray: %s's menu (%s) at %ld,%ld could not be moved (%lu)", name, cls, r.left, r.top, GetLastError());
        StopTrayMenuFollow();
        return;
    }
    Log(L"tray: %s's menu (%s) moved from %ld,%ld to %ld,%ld", name, cls, r.left, r.top, p.x, p.y);
    g_follow.menu = hwnd;
    g_follow.pid = pid;
    g_follow.placed = p;
    g_follow.moves = 0;
    g_follow.until = GetTickCount64() + 800;
    if (g_follow.timer)
        KillTimer(nullptr, g_follow.timer);
    g_follow.timer = SetTimer(nullptr, 0, 800, FollowExpired);
    if (!g_follow.timer)
        StopTrayMenuFollow();
}

} // namespace

void StopTrayMenuFollow()
{
    if (g_follow.hook)
        UnhookWinEvent(g_follow.hook);
    g_follow.hook = nullptr;
    g_follow.menu = nullptr;
    if (g_follow.timer)
        KillTimer(nullptr, g_follow.timer);
    g_follow.timer = 0;
    g_follow.pid = 0;
}

void FollowTrayMenu(POINT at, const std::wstring& exe, RECT avoidBar)
{
    StopTrayMenuFollow();
    if (exe.empty() || _wcsicmp(PathFindFileNameW(exe.c_str()), L"explorer.exe") == 0)
        return;
    LASTINPUTINFO input{sizeof(input)};
    if (!GetLastInputInfo(&input))
        return;
    g_follow.clickInput = input.dwTime;
    g_follow.at = at;
    g_follow.avoidBar = avoidBar;
    g_follow.exe = exe;
    g_follow.until = GetTickCount64() + 3000; // slow apps take a while to show it
    g_follow.hook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_LOCATIONCHANGE, nullptr, MenuShown, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (g_follow.hook)
        g_follow.timer = SetTimer(nullptr, 0, 3000, FollowExpired);
    if (!g_follow.timer)
        StopTrayMenuFollow();
}

} // namespace atrium
