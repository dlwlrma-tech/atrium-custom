// Popup theme colors, monitor-edge placement helpers, and the compositor-paced frame pump.
#pragma once

#include <windows.h>
#include <d2d1_3.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace atrium {

// Ink and washes over the popup glass, which is the bar's clear material:
// dark ink where the glass shows something bright, light ink elsewhere (the
// render thread decides, WM_GLASS_INK). Text gets a soft halo, as on the bar.
// `panel` is only painted when the glass is unavailable (its own tone).
struct Theme {
    bool light = false; // dark ink
    D2D1_COLOR_F panel, border, text, subtext, faint, hover, press, accent, accentText, divider, card, cardHot, halo;
    static Theme For(bool darkInk);
    // Between the light (0) and the dark (1) ink, while the ink fades.
    static Theme Mix(float darkInk);
    // The plain stand-in panel: its tone, and the ink over it.
    static Theme Panel();
    // The bar's tint and transparency (ApplySettings), which decide the stand-in's tone.
    static void SetGlass(uint32_t tint, double transparency);
};

// Where every popup goes (D32): kPopupGapDip inside the bar's glass on whichever
// edge it sits on; Left aligns with the bar's start (left, or top on a side edge),
// Right with its end, Center with the work area's middle; inside the work area.
constexpr float kPopupGapDip = 12;
enum class PopupAlign { Left, Right, Center };
float PopupScale(RECT bar); // px per DIP on the bar's monitor
RECT PopupRect(RECT bar, D2D1_SIZE_F sizeDip, PopupAlign align);
POINT PopupAnchor(RECT bar, RECT rect);
POINT PopupOrigin(RECT bar, RECT popup, RECT anchor);

constexpr UINT WM_SURFACE_FRAME = WM_APP + 0x42;

// Frames for the popups' animations, in step with the compositor: a thread
// waits for each composition (DwmFlush) and tells the windows that want a
// frame. (A 10 ms timer really fires every 16-31 ms and stutters.)
class FramePump {
public:
    FramePump(const FramePump&) = delete;
    FramePump& operator=(const FramePump&) = delete;
    static FramePump& Get();
    void Want(HWND h);
    void Shutdown();

private:
    FramePump() = default;
    ~FramePump();
    void Run();

    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<HWND> wanting_;
    bool stop_ = false;
    std::thread thread_;
};

} // namespace atrium
