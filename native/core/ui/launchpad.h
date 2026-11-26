// Launchpad, as on macOS: the apps the user installed (and Windows' Settings), oldest first, then one
// folder holding Windows' own apps (the catalog's `system`), opened in place; a search finds every app.
// One window of the bar's glass (or that glass under a black veil: launchpadMaterial tint) in the middle
// of the display (or over the whole monitor, launchpadLayout fullscreen, where a click on the empty glass
// closes it), in pages of a grid (launchpad_model.h) with the page dots below and a search field
// above. It opens from its entry next to the start logo: the glass fades in and the apps settle from a
// little larger, all at once when every icon is read (a spinner meanwhile; they are read ahead at start),
// and it leaves the same way. Typing goes into the search (the input method's composition inline, the
// caret shown once the field is clicked or typed in); the wheel, a drag or the arrows turn the pages with
// a spring; a click launches, a right click shows the bar's menu for the app; Esc clears a search, then closes the folder, then Launchpad, as a click
// outside it does.
#pragma once

#include "motion.h"
#include "shell/app_catalog.h"
#include "ui/launchpad_model.h"
#include "ui/surface.h"

#include <windows.h>

#include <functional>
#include <map>
#include <unordered_map>
#include <string>
#include <utility>
#include <vector>

namespace atrium {

class Launchpad {
public:
    Launchpad(HINSTANCE instance, AppCatalog& catalog);

    // work: the display's work area (screen px), the window centred in it; previous: the window to
    // give the focus back to when closed without launching.
    void Open(RECT work, HWND previous);
    void SetRadius(float dip) { radius_ = dip; }
    void SetTint(bool tint) { tint_ = tint; }
    void SetFullscreen(bool on) { full_ = on; } // launchpadLayout fullscreen: the whole monitor, as macOS's
    void Close(bool launched);
    bool Visible() const { return surface_.Visible(); }
    ULONGLONG LastClosed() const { return lastClosed_; }
    void Refresh(); // the catalog changed: the apps, an icon
    // Reads every app's icon ahead, at the size a window on `work` would show them.
    void Prewarm(RECT work);
    HWND Hwnd() const { return surface_.Hwnd(); }
    // A right click on an app: its menu at `screen` (the bar's, main.cpp); done(picked) once it has closed.
    std::function<void(const CatalogApp& app, POINT screen, std::function<void(bool picked)> done)> onAppMenu;

private:
    static constexpr UINT_PTR kCloseTimer = 0x4C50, kCaretTimer = 0x4C51, kEdgeTimer = 0x4C53;
    static constexpr int kSearch = -1, kGlass = -1000; // what HitAt finds besides a tile (its index) and page dot n (-2 - n)
    struct Hit {
        D2D1_RECT_F rect;
        int index; // into Shown(), kSearch, or -2 - n for page dot n
    };

    // Painting (launchpad.cpp).
    void Paint(Canvas& c);
    void PaintSearch(Canvas& c, const Theme& t, float shown);
    void PaintPage(Canvas& c, const Theme& t, int page, float dx);
    void PaintTile(Canvas& c, const Theme& t, int index, D2D1_RECT_F cell);
    void PaintDots(Canvas& c, const Theme& t);
    void PaintSpinner(Canvas& c, const Theme& t, float shown);
    void PaintIcon(Canvas& c, const Theme& t, const std::wstring& id, D2D1_RECT_F r); // an app's icon, a quiet square until read
    void PaintFolder(Canvas& c, const Theme& t, D2D1_RECT_F r);                       // the system folder: its first nine apps
    void Split();                                                                      // tiles_ and system_ from apps_
    void ShowFolder(bool open);
    std::vector<std::wstring> Ids() const; // apps_' ids
    int IconPx(RECT work) const;
    void PublishGlass(double now);
    float Hover(Canvas& c, int id, bool on);
    D2D1_RECT_F Cell(int slot) const; // a slot of a page, its page at x 0
    D2D1_RECT_F SearchRect() const;
    float OpenAmount(double now) const; // 0 hidden .. 1 open, through the opening and the closing
    void Size(RECT work);               // w_, h_ and grid_ for a window on `work`
    static RECT Monitor(RECT work);     // the monitor `work` is on (fullscreen)
    std::vector<CatalogApp> LaunchableApps() const;
    float Radius() const;

    // Input (launchpad_input.cpp).
    bool Message(UINT m, WPARAM w, LPARAM l, LRESULT& result);
    bool Key(WPARAM key);
    void Type(wchar_t ch);
    void Press(POINT pt);
    void Drag(POINT pt);
    void Release(POINT pt);
    void AppMenu(POINT pt); // a right click
    void Wheel(int delta);
    int HitAt(POINT pt) const;
    void SetPage(int page);
    void Launch(int index);
    bool ImeMessage(UINT m, WPARAM w, LPARAM l, LRESULT& result);
    // Arranging (launchpad_drag.cpp): pressed and moved, an app lifts and follows the pointer (as a start menu
    // tile does); the others
    // make room, the page turns at the sheet's sides, and the order is kept (shell/launchpad_order.h).
    bool Arranging() const { return query_.empty() && !folderOpen_; }
    void Lift();
    void MoveLifted(POINT pt);
    void TurnAtEdge();
    void DropLifted();
    void ApplyOrder();                                       // tiles_ in the user's order
    D2D1_RECT_F Glide(const std::wstring& id, D2D1_RECT_F cell, int page, float dx); // where a tile is drawn as it makes room
    void PaintLifted(Canvas& c, const Theme& t);
    void PlaceIme();

    void Finish();          // the closing has played: the window goes, the focus goes back
    void Filter();          // shown_ from apps_ and query_
    int Pages() const;
    const std::vector<CatalogApp>& Shown() const { return !query_.empty() ? shown_ : folderOpen_ ? system_ : tiles_; }

    Surface surface_;
    AppCatalog& catalog_;
    static constexpr const wchar_t* kFolderId = L"#system"; // the system folder's tile among tiles_
    std::vector<CatalogApp> apps_, shown_;
    std::vector<CatalogApp> tiles_, system_; // the pages, the system folder's apps
    bool folderOpen_ = false;
    std::wstring query_;
    std::wstring composition_;
    bool searchFocused_ = false;
    float caretX_ = 0;
    LaunchpadGrid grid_;
    float w_ = 0, h_ = 0; // the window, DIP
    float radius_ = 24;   // the bar's popups' corner
    bool tint_ = false;   // launchpadMaterial tint: a black veil over the glass, light ink
    bool full_ = false;   // launchpadLayout fullscreen
    int iconPx_ = 0;      // the icons' size on the display it opened on
    bool loading_ = false;  // icons still being read: a spinner instead of the apps
    double revealedMs_ = 0; // when the apps began to settle in
    int page_ = 0;
    float pageShown_ = 0, pageSpeed_ = 0; // the page on screen (fractional while it glides), pages/s
    int sel_ = -1, hover_ = -1, pressed_ = -1;
    // A press: on a tile (moved, it lifts) or the glass (dragged sideways past a few DIP, it turns the pages).
    bool down_ = false, dragging_ = false;
    POINT downPt_{};
    float dragFrom_ = 0;     // pageShown_ when the drag began
    float dragSpeed_ = 0;    // pages/s, of the last moves
    double dragMs_ = 0;
    float lastDragX_ = 0;
    int wheel_ = 0;          // wheel notches not yet turned into a page
    bool tracking_ = false;
    bool menuOpen_ = false; // an app's menu is up: the focus going to it does not close Launchpad
    bool stale_ = false;    // the catalog changed while an app was lifted
    HWND previous_ = nullptr;
    double openedMs_ = 0, closedMs_ = 0, lastPaint_ = 0;
    bool closing_ = false, launched_ = false;
    ULONGLONG lastClosed_ = 0;
    std::pair<int, size_t> prewarmed_; // icon px and the apps' count last read ahead
    std::vector<Hit> hits_;
    std::vector<std::pair<std::wstring, int>> visibleIcons_, askedIcons_;
    std::map<int, motion::Follow> hoverAnim_;
    int lifted_ = -1;                // the app being moved, its place in tiles_ as it goes
    D2D1_POINT_2F liftAt_{};         // the pointer, DIP
    std::vector<std::wstring> order_; // the user's order, as read when it opened
    std::unordered_map<std::wstring, D2D1_POINT_2F> tilePos_; // the tiles' places on the sheet as they glide
    float glideMs_ = 0;              // this frame's time step for the places
    bool gliding_ = false;
};

} // namespace atrium
