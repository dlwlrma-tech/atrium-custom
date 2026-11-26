#include "ui/window_peek.h"

#include "clock.h"

#include <algorithm>
#include <cmath>

namespace atrium {

namespace {

constexpr double kFadeMs = 180;
constexpr float kVeil = 0.62f; // how much of the other windows the veil hides

bool AppsLight()
{
    DWORD v = 1, size = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v,
                 &size);
    return v != 0;
}

} // namespace

WindowPeek::WindowPeek(HINSTANCE instance) : surface_(instance, L"Atrium Peek", false, 0, false, true)
{
    surface_.SetGlassless(true);
    surface_.paint = [this](Canvas& c) { Paint(c); };
}

WindowPeek::~WindowPeek() { Hide(); }

void WindowPeek::Show(HWND window)
{
    if (window == window_)
        return;
    RECT win{}, frame{};
    MONITORINFO mi{sizeof(mi)};
    if (!IsWindow(window) || IsIconic(window) || !GetWindowRect(window, &win) || win.right <= win.left || win.bottom <= win.top ||
        FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))) ||
        !GetMonitorInfoW(MonitorFromRect(&frame, MONITOR_DEFAULTTONEAREST), &mi)) {
        Hide();
        return;
    }
    if (thumb_) {
        DwmUnregisterThumbnail(thumb_);
        thumb_ = nullptr;
    }
    // From one window to the next on the same display the veil stays as it is.
    if (const RECT shown = surface_.Rect(); !surface_.Visible() || !EqualRect(&shown, &mi.rcMonitor)) {
        surface_.Show(mi.rcMonitor, false, POINT{mi.rcMonitor.left, mi.rcMonitor.top}, false);
        // Above the ordinary windows, under the topmost ones: the bar and the preview stay over the veil.
        SetWindowPos(surface_.Hwnd(), HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        light_ = AppsLight();
        since_ = ClockMs();
    }
    if (FAILED(DwmRegisterThumbnail(surface_.Hwnd(), window, &thumb_))) {
        thumb_ = nullptr;
        Hide();
        return;
    }
    SIZE source{};
    if (FAILED(DwmQueryThumbnailSourceSize(thumb_, &source)) || source.cx <= 0 || source.cy <= 0) {
        Hide();
        return;
    }
    // The window without its invisible borders, in the thumbnail's px (they differ when its DPI does).
    const double sx = double(source.cx) / double(win.right - win.left), sy = double(source.cy) / double(win.bottom - win.top);
    POINT at{frame.left, frame.top};
    ScreenToClient(surface_.Hwnd(), &at);
    DWM_THUMBNAIL_PROPERTIES p{};
    p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_VISIBLE | DWM_TNP_SOURCECLIENTAREAONLY;
    p.rcSource = RECT{std::clamp(LONG(std::lround(double(frame.left - win.left) * sx)), 0L, source.cx),
                      std::clamp(LONG(std::lround(double(frame.top - win.top) * sy)), 0L, source.cy),
                      std::clamp(LONG(std::lround(double(frame.right - win.left) * sx)), 0L, source.cx),
                      std::clamp(LONG(std::lround(double(frame.bottom - win.top) * sy)), 0L, source.cy)};
    if (p.rcSource.right <= p.rcSource.left || p.rcSource.bottom <= p.rcSource.top) {
        Hide();
        return;
    }
    p.rcDestination = RECT{at.x, at.y, at.x + frame.right - frame.left, at.y + frame.bottom - frame.top};
    p.fVisible = TRUE;
    p.fSourceClientAreaOnly = FALSE;
    DwmUpdateThumbnailProperties(thumb_, &p);
    window_ = window;
    surface_.Invalidate();
}

void WindowPeek::Hide()
{
    if (thumb_) {
        DwmUnregisterThumbnail(thumb_);
        thumb_ = nullptr;
    }
    window_ = nullptr;
    if (surface_.Visible())
        surface_.Hide(false);
}

void WindowPeek::Paint(Canvas& c)
{
    const float t = float(std::clamp((c.now - since_) / kFadeMs, 0.0, 1.0));
    if (t < 1)
        c.Continue();
    const float a = kVeil * (1 - (1 - t) * (1 - t)); // ease out
    const RECT r = surface_.Rect();
    const float v = light_ ? 0.96f : 0.08f;
    c.Fill(D2D1::RectF(0, 0, float(r.right - r.left) / c.scale, float(r.bottom - r.top) / c.scale), 0, D2D1::ColorF(v, v, v, a));
}

} // namespace atrium
