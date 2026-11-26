// Peek, as the Windows taskbar's preview does: the pointer resting on a window's picture in the
// bar's preview shows that window where it is, the other windows of its display faded behind a
// veil (the bar and the preview stay above it). A layer over the display's ordinary windows, under
// every topmost one, with the window's live picture (a DWM thumbnail) in the window's own place.
#pragma once

#include "ui/surface.h"

#include <dwmapi.h>

namespace atrium {

class WindowPeek {
public:
    explicit WindowPeek(HINSTANCE instance);
    ~WindowPeek();
    WindowPeek(const WindowPeek&) = delete;
    WindowPeek& operator=(const WindowPeek&) = delete;

    // Shows `window` and fades the rest; a minimized window has no place to be shown in: nothing.
    void Show(HWND window);
    void Hide();
    HWND Window() const { return window_; }

private:
    void Paint(Canvas& c);

    Surface surface_;
    HWND window_ = nullptr;
    HTHUMBNAIL thumb_ = nullptr;
    bool light_ = true; // the apps' theme: the veil is of its tone
    double since_ = 0;  // ClockMs() the veil began to come
};

} // namespace atrium
