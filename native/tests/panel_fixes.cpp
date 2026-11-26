#include "render/panel_glass_host.h"
#include "ui/calendar_panel.h"
#include "ui/genie_minimize.h"
#include "ui/surface.h"
#include "ui/tray_panel.h"
#include "ui/window_peek.h"
#include "ui/window_preview.h"

#include <dwmapi.h>

#include <algorithm>
#include <iostream>

using namespace atrium;

namespace {

struct TimerPumpStats {
    int timerTicks = 0;
    int messages = 0;
};

TimerPumpStats PumpTimersWithHiddenOverlay(HINSTANCE inst, const std::function<void(Surface&, Surface&)>& exercise)
{
    Surface regular(inst, L"Atrium Timer Check Regular", false, 0);
    Surface overlay(inst, L"Atrium Timer Check Overlay", false, 0, false, true);
    if (exercise)
        exercise(regular, overlay);

    constexpr UINT_PTR kTimerId = 0x7711;
    TimerPumpStats stats{};
    regular.message = [&](UINT m, WPARAM w, LPARAM, LRESULT&) {
        if (m == WM_TIMER && w == kTimerId) {
            ++stats.timerTicks;
            return true;
        }
        return false;
    };
    if (!regular.Hwnd() || !overlay.Hwnd() || !SetTimer(regular.Hwnd(), kTimerId, 20, nullptr))
        return stats;

    const ULONGLONG deadline = GetTickCount64() + 500;
    while (GetTickCount64() < deadline && stats.timerTicks < 4 && stats.messages < 400) {
        MSG msg{};
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ++stats.messages;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            continue;
        }
        if (stats.timerTicks >= 4)
            break;
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline)
            break;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>(std::min<ULONGLONG>(50, deadline - now)), QS_ALLINPUT);
    }
    KillTimer(regular.Hwnd(), kTimerId);
    return stats;
}

void CheckClickThroughAndPeek(auto& check)
{
    const HINSTANCE inst = GetModuleHandleW(nullptr);
    Surface regular(inst, L"Atrium Regular Test", false, 0);
    Surface overlay(inst, L"Atrium ClickThrough Test", false, 0, false, true);
    const LONG_PTR regEx = GetWindowLongPtrW(regular.Hwnd(), GWL_EXSTYLE);
    const LONG_PTR ovEx = GetWindowLongPtrW(overlay.Hwnd(), GWL_EXSTYLE);
    check((regEx & (WS_EX_TRANSPARENT | WS_EX_LAYERED)) == 0, "regular surface is not click-through layered");
    check((ovEx & (WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP)) == (WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP),
          "click-through surface sets layered transparent + no redirection bitmap");
    check(SendMessageW(overlay.Hwnd(), WM_NCHITTEST, 0, 0) == HTTRANSPARENT, "click-through surface returns HTTRANSPARENT");

    // Verify DWM thumbnail registration and update on a click-through layered Surface.
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = inst;
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"AtriumPeekDummyTarget";
    RegisterClassExW(&wc);
    HWND target =
        CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"Target", WS_POPUP, -4000, -4000, 240, 160, nullptr, nullptr, inst, nullptr);
    ShowWindow(target, SW_SHOWNOACTIVATE);
    UpdateWindow(target);
    DwmFlush();
    HTHUMBNAIL thumb = nullptr;
    const HRESULT regHr = DwmRegisterThumbnail(overlay.Hwnd(), target, &thumb);
    check(SUCCEEDED(regHr) && thumb != nullptr, "DwmRegisterThumbnail succeeds on click-through layered Surface");
    if (thumb) {
        SIZE src{};
        const HRESULT szHr = DwmQueryThumbnailSourceSize(thumb, &src);
        DWM_THUMBNAIL_PROPERTIES p{};
        p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_VISIBLE | DWM_TNP_SOURCECLIENTAREAONLY;
        p.rcSource = RECT{0, 0, std::max<LONG>(1, src.cx), std::max<LONG>(1, src.cy)};
        p.rcDestination = RECT{0, 0, 240, 160};
        p.fVisible = TRUE;
        p.fSourceClientAreaOnly = FALSE;
        const HRESULT updHr = DwmUpdateThumbnailProperties(thumb, &p);
        check(SUCCEEDED(szHr) && SUCCEEDED(updHr), "DwmUpdateThumbnailProperties succeeds on click-through layered Surface");
        DwmUnregisterThumbnail(thumb);
    }
    WindowPeek peek(inst);
    peek.Show(nullptr);
    check(peek.Window() == nullptr, "WindowPeek ignores null window");
    peek.Show(target);
    peek.Hide();
    check(peek.Window() == nullptr, "WindowPeek clears window on Hide");
    DestroyWindow(target);

    GenieMinimize genie(inst, [](HWND) -> std::optional<RECT> { return std::nullopt; }, [](HWND) { return false; });
    genie.SetEnabled(true);
    genie.SetEnabled(false);

    const TimerPumpStats hiddenStats = PumpTimersWithHiddenOverlay(inst, {});
    check(hiddenStats.timerTicks >= 4 && hiddenStats.messages < 400, "hidden click-through layered Surface does not spin WM_PAINT or starve WM_TIMER");

    const TimerPumpStats showHideStats = PumpTimersWithHiddenOverlay(inst, [](Surface&, Surface& overlay) {
        overlay.Show(RECT{10, 10, 110, 110}, false, POINT{10, 10}, false);
        overlay.Hide(false);
    });
    check(showHideStats.timerTicks >= 4 && showHideStats.messages < 400,
          "click-through layered Surface after Show/Hide does not spin WM_PAINT or starve WM_TIMER");
}

} // namespace

int main()
{
    int fail = 0;
    auto check = [&](bool ok, const char* msg) {
        if (!ok) {
            std::cerr << "FAIL: " << msg << '\n';
            ++fail;
        }
    };
    check(CalendarCapsuleDate(2026, 11, 18, 2, 2026, false) == L"11月18日 星期三", "Chinese same-year date");
    check(CalendarCapsuleDate(2027, 1, 1, 4, 2026, false) == L"2027年1月1日 星期五", "Chinese cross-year date");
    check(CalendarCapsuleDate(2026, 11, 18, 2, 2026, true) == L"Wed, November 18", "English same-year date");
    check(CalendarCapsuleDate(2027, 1, 1, 4, 2026, true) == L"Fri, January 1, 2027", "English cross-year date");

    const POINT a{500, 900};
    const SIZE d96{4, 4}, d200{8, 8};
    check(!TrayFollowCancelled(100, 100, 90, false, a, {800, 800}, d96, false), "matching click input");
    check(!TrayFollowCancelled(110, 100, 110, false, a, {800, 800}, d96, false), "matching synthetic input");
    check(!TrayFollowCancelled(120, 100, 90, false, a, {501, 901}, d96, false), "1-pixel mouse jitter");
    check(!TrayFollowCancelled(120, 100, 90, false, a, {502, 898}, d96, false), "inside double-click rect");
    check(TrayFollowCancelled(120, 100, 90, false, a, {503, 900}, d96, false), "outside double-click rect horizontally");
    check(TrayFollowCancelled(120, 100, 90, false, a, {500, 897}, d96, false), "outside double-click rect vertically");
    check(!TrayFollowCancelled(120, 100, 90, false, a, {504, 904}, d200, false), "DPI-scaled double-click rect allows 4 px");
    check(!TrayFollowCancelled(120, 100, 90, true, a, {1200, 20}, d96, false), "synthetic pointer in flight ignores cursor distance");
    check(!TrayFollowCancelled(120, 100, 90, true, a, {1200, 20}, d96, true), "the fallback's own button press does not cancel it");
    check(ValidThumbnailSource(S_OK, {1280, 800}), "valid thumbnail source size");
    check(!ValidThumbnailSource(S_OK, {0, 0}) && !ValidThumbnailSource(S_OK, {0, 800}) && !ValidThumbnailSource(S_OK, {1280, 0}),
          "zero thumbnail source size is rejected");
    check(!ValidThumbnailSource(S_OK, {-1, 800}), "negative thumbnail source size is rejected");
    check(!ValidThumbnailSource(E_FAIL, {1280, 800}), "failed DwmQueryThumbnailSourceSize is rejected");
    check(BackupKeepIntervalMs(false, 2000.0) == kBackupIdleIntervalMs, "only desktop widgets use the 250 ms backup tier");
    check(BackupKeepIntervalMs(true, 2000.0) == kBackupActiveIntervalMs, "visible screen panels on the capture monitor use the 100 ms backup tier");
    check(BackupKeepIntervalMs(false, 500.0) == 0.0 && BackupKeepIntervalMs(true, 1000.0) == 0.0,
          "recent screen panel movement keeps every frame up to 1000 ms");
    CheckClickThroughAndPeek(check);
    if (fail)
        return 1;
    std::cout << "PASS: panel_fixes\n";
    return 0;
}
