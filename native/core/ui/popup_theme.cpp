#include "ui/popup_theme.h"

#include "bar_edge.h"
#include "render/glass_panel.h"

#include <dwmapi.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace atrium {

namespace {

D2D1_COLOR_F Rgba(uint32_t rgb, float a) { return D2D1::ColorF(rgb, a); }

uint32_t g_tint = 0xFFFFFF;
double g_transparency = 0.95;

} // namespace

float PopupScale(RECT bar)
{
    HMONITOR mon = MonitorFromRect(&bar, MONITOR_DEFAULTTOPRIMARY);
    UINT dpi = 96, dpiY = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpi, &dpiY);
    return float(dpi) / 96.f;
}

RECT PopupRect(RECT bar, D2D1_SIZE_F sizeDip, PopupAlign align)
{
    HMONITOR mon = MonitorFromRect(&bar, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    const RECT& wa = mi.rcWork;
    const float s = PopupScale(bar);
    const int w = int(std::lround(sizeDip.width * s));
    const int h = int(std::lround(sizeDip.height * s));
    const int gap = int(std::lround(kPopupGapDip * s));
    const BarEdge edge = EdgeOf(bar);
    if (Vertical(edge)) {
        int top = int(wa.top + wa.bottom - h) / 2;
        if (align == PopupAlign::Left)
            top = int(bar.top);
        else if (align == PopupAlign::Right)
            top = int(bar.bottom) - h;
        top = std::max(int(wa.top) + gap, std::min(top, int(wa.bottom) - gap - h));
        const int bottom = std::min(int(wa.bottom) - gap, top + h);
        if (edge == BarEdge::Left) {
            const int left = std::max(int(wa.left) + gap, int(bar.right) + gap);
            return RECT{left, top, std::min(int(wa.right) - gap, left + w), bottom};
        }
        const int right = std::min(int(wa.right) - gap, int(bar.left) - gap);
        return RECT{std::max(int(wa.left) + gap, right - w), top, right, bottom};
    }
    int left = align == PopupAlign::Left ? int(bar.left) : align == PopupAlign::Right ? int(bar.right) - w : int(wa.left + wa.right - w) / 2;
    left = std::max(int(wa.left) + gap, std::min(left, int(wa.right) - gap - w));
    const int right = std::min(int(wa.right) - gap, left + w);
    if (edge == BarEdge::Top) {
        const int top = std::max(int(wa.top) + gap, int(bar.bottom) + gap);
        return RECT{left, top, right, std::min(int(wa.bottom) - gap, top + h)};
    }
    const int bottom = std::min(int(wa.bottom) - gap, int(bar.top) - gap);
    return RECT{left, std::max(int(wa.top) + gap, bottom - h), right, bottom};
}

POINT PopupAnchor(RECT bar, RECT rect)
{
    switch (EdgeOf(bar)) {
    case BarEdge::Top: return {(rect.left + rect.right) / 2, rect.bottom};
    case BarEdge::Left: return {rect.right, (rect.top + rect.bottom) / 2};
    case BarEdge::Right: return {rect.left, (rect.top + rect.bottom) / 2};
    default: return {(rect.left + rect.right) / 2, rect.top};
    }
}

POINT PopupOrigin(RECT bar, RECT popup, RECT anchor)
{
    const BarEdge edge = EdgeOf(bar);
    const POINT a = PopupAnchor(bar, anchor);
    if (Vertical(edge)) {
        const LONG y = anchor.bottom > anchor.top ? a.y : popup.top + (popup.bottom - popup.top) / 6;
        return {edge == BarEdge::Left ? popup.left : popup.right, y};
    }
    const LONG x = anchor.right > anchor.left ? a.x : popup.left + (popup.right - popup.left) / 6;
    return {x, edge == BarEdge::Top ? popup.top : popup.bottom};
}

FramePump& FramePump::Get()
{
    static FramePump pump;
    return pump;
}

FramePump::~FramePump() { Shutdown(); }

void FramePump::Want(HWND h)
{
    std::lock_guard g(mutex_);
    if (std::ranges::find(wanting_, h) == wanting_.end())
        wanting_.push_back(h);
    if (!thread_.joinable())
        thread_ = std::thread([this] { Run(); });
    cv_.notify_one();
}

void FramePump::Shutdown()
{
    {
        std::lock_guard g(mutex_);
        stop_ = true;
        wanting_.clear();
    }
    cv_.notify_one();
    if (thread_.joinable())
        thread_.join();
    std::lock_guard g(mutex_);
    stop_ = false;
}

void FramePump::Run()
{
    std::vector<HWND> due;
    for (;;) {
        {
            std::unique_lock g(mutex_);
            cv_.wait(g, [&] { return stop_ || !wanting_.empty(); });
            if (stop_)
                return;
        }
        const bool flushed = SUCCEEDED(DwmFlush());
        {
            std::unique_lock g(mutex_);
            if ((!flushed && cv_.wait_for(g, std::chrono::milliseconds(8), [&] { return stop_; })) || stop_)
                return;
            due.swap(wanting_);
        }
        for (HWND h : due)
            PostMessageW(h, WM_SURFACE_FRAME, 0, 0);
        due.clear();
    }
}

void Theme::SetGlass(uint32_t tint, double transparency)
{
    g_tint = tint;
    g_transparency = transparency;
}

Theme Theme::For(bool darkInk)
{
    // The bar's inks and washes (BarPainter::DrawContent).
    Theme t;
    t.light = darkInk;
    PanelTone tone = PanelToneFor(g_tint, g_transparency);
    t.panel = Rgba(tone.tint, 0.96f);
    const uint32_t wash = darkInk ? 0x000000 : 0xFFFFFF, textRgb = darkInk ? 0x111317 : 0xFFFFFF;
    t.border = Rgba(wash, darkInk ? 0.10f : 0.30f);
    t.text = Rgba(textRgb, darkInk ? 0.92f : 0.96f);
    t.subtext = Rgba(textRgb, darkInk ? 0.66f : 0.76f);
    t.faint = Rgba(textRgb, darkInk ? 0.46f : 0.56f);
    t.hover = Rgba(wash, darkInk ? 0.10f : 0.20f);
    t.press = Rgba(wash, darkInk ? 0.16f : 0.28f);
    t.divider = Rgba(wash, darkInk ? 0.12f : 0.24f);
    t.card = Rgba(wash, darkInk ? 0.05f : 0.08f);
    t.cardHot = Rgba(wash, darkInk ? 0.10f : 0.20f);
    t.halo = Rgba(darkInk ? 0xFFFFFF : 0x000000, darkInk ? 0.35f : 0.32f);
    t.accent = Rgba(0x0A84FF, 1);
    t.accentText = Rgba(0xFFFFFF, 1);
    return t;
}

Theme Theme::Mix(float darkInk)
{
    if (darkInk <= 0.001f || darkInk >= 0.999f)
        return For(darkInk >= 0.5f);
    Theme a = For(false), b = For(true), t = b;
    auto mix = [&](D2D1_COLOR_F Theme::* c) {
        const D2D1_COLOR_F &x = a.*c, &y = b.*c;
        t.*c = D2D1::ColorF(x.r + (y.r - x.r) * darkInk, x.g + (y.g - x.g) * darkInk, x.b + (y.b - x.b) * darkInk, x.a + (y.a - x.a) * darkInk);
    };
    for (auto c : {&Theme::border, &Theme::text, &Theme::subtext, &Theme::faint, &Theme::hover, &Theme::press, &Theme::divider, &Theme::card, &Theme::cardHot,
                   &Theme::halo})
        mix(c);
    t.light = darkInk >= 0.5f;
    return t;
}

Theme Theme::Panel()
{
    Theme t = For(PanelToneFor(g_tint, g_transparency).light);
    t.halo = D2D1::ColorF(0, 0, 0, 0); // an opaque panel needs none
    return t;
}

} // namespace atrium
