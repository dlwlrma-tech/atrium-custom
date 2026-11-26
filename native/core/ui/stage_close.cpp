// Stage Manager's close buttons (stageCloseButton): a card under the pointer shows a cross on its front
// picture's top right corner; a click on it closes the card's windows as their own close buttons do (the
// apps still ask to save, or go to the tray).
#include "ui/stage_manager.h"

#include "log.h"

#include <algorithm>
#include <cmath>

namespace atrium {

namespace {

constexpr float kCloseR = 11, kCloseHitR = 14; // DIP

} // namespace

void StageManager::SetCloseButton(bool on)
{
    if (on == closeButton_)
        return;
    closeButton_ = on;
    closeHot_ = closePressed_ = -1;
    surface_.Invalidate();
}

void StageManager::PaintClose(Canvas& c, Entry& e, int index, D2D1_POINT_2F corner, D2D1_POINT_2F at, float shown)
{
    e.closeAt = at;
    if (!closeButton_ || shown < 0.01f)
        return;
    const bool hot = index == closeHot_, down = index == closePressed_;
    const float a = std::clamp(shown, 0.f, 1.f);
    // White, as macOS's close buttons over pictures: a soft shadow under it, the cross dark; greyer pressed.
    const float face = down ? 0.84f : hot ? 0.93f : 1.f;
    c.Circle(corner.x, corner.y + 1, kCloseR + 1.5f, D2D1::ColorF(0, 0, 0, 0.16f * a));
    c.Circle(corner.x, corner.y, kCloseR, D2D1::ColorF(face, face, face, 0.97f * a));
    const bool halo = c.halo;
    c.halo = false;
    c.Glyph(L"\uE711", D2D1::RectF(corner.x - kCloseR, corner.y - kCloseR, corner.x + kCloseR, corner.y + kCloseR), 9,
            D2D1::ColorF(0.11f, 0.11f, 0.13f, 0.9f * a));
    c.halo = halo;
}

int StageManager::CloseAt(POINT pt) const
{
    if (!closeButton_ || hover_ < 0 || hover_ >= int(entries_.size()))
        return -1;
    const float s = surface_.Scale();
    const D2D1_POINT_2F at = entries_[size_t(hover_)].closeAt;
    return std::hypot(float(pt.x) / s - at.x, float(pt.y) / s - at.y) <= kCloseHitR ? hover_ : -1;
}

// The pointer over a close button: true when the message is the button's.
bool StageManager::CloseInput(UINT m, POINT pt)
{
    const int at = CloseAt(pt);
    if (m == WM_MOUSEMOVE) {
        if (at != closeHot_) {
            closeHot_ = at;
            surface_.Invalidate();
        }
        return at >= 0;
    }
    if (m == WM_LBUTTONDOWN && at >= 0) {
        closePressed_ = at;
        pressed_ = -1;
        surface_.Invalidate();
        return true;
    }
    if (m == WM_LBUTTONUP && closePressed_ >= 0) {
        const bool same = at == closePressed_;
        closePressed_ = -1;
        surface_.Invalidate();
        if (same)
            CloseEntry(at);
        return true;
    }
    return false;
}

void StageManager::CloseEntry(int index)
{
    if (index < 0 || index >= int(entries_.size()))
        return;
    const Set keys = entries_[size_t(index)].keys;
    int closed = 0;
    for (const TaskItem& t : items_)
        if (std::ranges::find(keys, t.key) != keys.end())
            for (HWND h : t.windows)
                closed += TaskModel::CloseWindow(h) ? 1 : 0;
    Log(L"stage: %s closed from the strip (%d window(s))", keys.empty() ? L"" : keys.front().c_str(), closed);
    hover_ = closeHot_ = -1;
}

} // namespace atrium
