// The windows of an app, previewed over the bar (as the Windows taskbar does): the pointer rests on
// an app that has windows, and a piece of the bar's glass above it shows each window live (DWM
// thumbnails), with its title and a close button. A click brings that window to the front; a
// middle click closes it. The preview stays while the pointer moves from the app onto it, and goes
// a moment after the pointer has left both. The pointer resting on a picture peeks at that window
// (WindowPeek): it is shown where it is, the others faded.
#pragma once

#include "bar_edge.h"
#include "motion.h"
#include "shell/task_model.h"
#include "ui/surface.h"
#include "ui/window_peek.h"

#include <dwmapi.h>

#include <map>
#include <string>
#include <unordered_set>
#include <vector>

namespace atrium {

// True when DwmQueryThumbnailSourceSize succeeded and returned a non-empty source size.
inline bool ValidThumbnailSource(HRESULT hr, SIZE source) { return SUCCEEDED(hr) && source.cx > 0 && source.cy > 0; }

class WindowPreview {
public:
    explicit WindowPreview(HINSTANCE instance);
    ~WindowPreview();
    WindowPreview(const WindowPreview&) = delete;
    WindowPreview& operator=(const WindowPreview&) = delete;

    // The windows of app `key` on the inner side of `anchor` (screen px), using
    // `icon` when DWM has no thumbnail.
    void Show(const std::wstring& key, const std::vector<HWND>& windows, POINT anchor, RECT bar, const IconImage& icon);
    // Window changes still reach the open preview after the pointer leaves
    // the taskbar for a card. This does not restart its leave/fade timers.
    void Refresh(const std::vector<TaskItem>& items);
    // The pointer left the app on the bar: the preview goes soon, unless the pointer comes onto it.
    void Leave();
    void Close();
    bool Visible() const { return surface_.Visible(); }
    void SetPeek(bool on); // windowPeek

private:
    struct Card {
        HWND window = nullptr;
        std::wstring title;
        HTHUMBNAIL thumb = nullptr;
        SIZE source{};        // the thumbnail's source size, px
        RECT sourceRect{};    // the window without its invisible borders, in the source's px
        D2D1_RECT_F rect{};   // the card, content DIP
        D2D1_RECT_F image{};  // where the window's picture goes, content DIP
        D2D1_RECT_F close{};  // its close button, content DIP
        motion::Follow hover;
    };

    void Paint(Canvas& c);
    bool Message(UINT m, WPARAM w, LPARAM l, LRESULT& result);
    bool InClose(int index, POINT pt) const;
    bool OnTimer(WPARAM timerId);
    void OnMouseMove(POINT pt);
    void OnButtonUp(UINT message, POINT pt);
    void Layout(float scale, float maxSpanDip, bool vertical);
    void Place();          // the surface's rectangle for the cards, inside the anchor
    void UpdateThumbs();   // DWM: where the pictures go, how visible
    void DropThumbs();
    void SetWindows(const std::vector<HWND>& windows);
    int CardAt(POINT pt) const;

    Surface surface_;
    WindowPeek peek_;
    std::wstring key_;
    std::vector<Card> cards_;
    IconImage icon_;
    POINT anchor_{};
    BarEdge edge_ = BarEdge::Bottom;
    D2D1_SIZE_F size_{};   // DIP
    int hover_ = -1, pressed_ = -1;
    bool overClose_ = false;
    bool tracking_ = false, inside_ = false;
    bool peekOn_ = true;
    float thumbAlpha_ = 0; // the pictures fade in once the glass has popped out
    double fadeStart_ = 0;
    std::unordered_set<HWND> loggedFailures_;
};

} // namespace atrium
