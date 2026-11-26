// Launchpad's arranging: an app pressed and moved (as in the start menu) lifts and follows the pointer, the others glide to make
// room, the page turns while it waits at the sheet's side, and where it is let go it stays (launchpad.json).
#include "ui/launchpad.h"

#include "log.h"
#include "shell/launchpad_order.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace atrium {

namespace {

constexpr float kEdge = 40;      // DIP from the sheet's side where a lifted app turns the page
constexpr UINT kEdgeMs = 650;    // ... after waiting there this long
constexpr float kGlideRate = 16; // the tiles' approach to their places, per second
constexpr float kLiftScale = 1.12f;

} // namespace

void Launchpad::ApplyOrder()
{
    if (order_.empty())
        return;
    std::unordered_map<std::wstring, size_t> rank;
    for (size_t i = 0; i < order_.size(); ++i)
        rank.try_emplace(order_[i], i);
    // Those the user placed, as placed; new ones after them, as they came.
    std::ranges::stable_sort(tiles_, {}, [&](const CatalogApp& a) {
        const auto it = rank.find(a.id);
        return it == rank.end() ? order_.size() : it->second;
    });
}

void Launchpad::Lift()
{
    if (!down_ || dragging_ || pressed_ < 0 || !Arranging() || pressed_ >= int(tiles_.size()))
        return;
    lifted_ = pressed_;
    pressed_ = hover_ = -1;
    POINT p{};
    GetCursorPos(&p);
    p = surface_.ScreenToContent(p);
    liftAt_ = D2D1::Point2F(float(p.x) / surface_.Scale(), float(p.y) / surface_.Scale());
    surface_.Invalidate();
}

void Launchpad::MoveLifted(POINT pt)
{
    const float s = surface_.Scale();
    liftAt_ = D2D1::Point2F(float(pt.x) / s, float(pt.y) / s);
    // The cell under the pointer on the page shown: the app takes that place, the others shift.
    const int col = std::clamp(int((liftAt_.x - grid_.gridLeft) / grid_.cellW), 0, grid_.cols - 1);
    const int row = std::clamp(int((liftAt_.y - grid_.gridTop) / grid_.cellH), 0, grid_.rows - 1);
    const int to = std::clamp(page_ * grid_.perPage + row * grid_.cols + col, 0, int(tiles_.size()) - 1);
    if (to != lifted_) {
        const auto from = tiles_.begin() + lifted_, there = tiles_.begin() + to;
        if (to > lifted_)
            std::rotate(from, from + 1, there + 1);
        else
            std::rotate(there, from, from + 1);
        lifted_ = to;
    }
    if (liftAt_.x < kEdge || liftAt_.x > w_ - kEdge)
        SetTimer(surface_.Hwnd(), kEdgeTimer, kEdgeMs, nullptr);
    else
        KillTimer(surface_.Hwnd(), kEdgeTimer);
    surface_.Invalidate();
}

void Launchpad::TurnAtEdge()
{
    KillTimer(surface_.Hwnd(), kEdgeTimer);
    if (lifted_ < 0)
        return;
    const int page = page_ + (liftAt_.x < kEdge ? -1 : liftAt_.x > w_ - kEdge ? 1 : 0);
    if (page == page_ || page < 0 || page >= Pages())
        return;
    SetPage(page);
    SetTimer(surface_.Hwnd(), kEdgeTimer, kEdgeMs, nullptr); // and on, while it waits there
}

void Launchpad::DropLifted()
{
    KillTimer(surface_.Hwnd(), kEdgeTimer);
    if (lifted_ < 0)
        return;
    // It settles from under the pointer into its place.
    tilePos_[tiles_[size_t(lifted_)].id] = D2D1::Point2F(liftAt_.x - grid_.cellW / 2 + float(page_) * w_, liftAt_.y - grid_.cellH / 2);
    lifted_ = -1;
    order_.clear();
    for (const CatalogApp& app : tiles_)
        order_.push_back(app.id);
    SaveLaunchpadOrder(order_);
    Log(L"launchpad: arranged (%zu tiles)", order_.size());
    if (stale_) {
        stale_ = false;
        Refresh();
    } else {
        surface_.Invalidate();
    }
}

// The tile's place on the sheet glides to its cell's; drawn where it is now on this page.
D2D1_RECT_F Launchpad::Glide(const std::wstring& id, D2D1_RECT_F cell, int page, float dx)
{
    const D2D1_POINT_2F goal = D2D1::Point2F(cell.left + float(page) * w_, cell.top);
    auto [it, added] = tilePos_.try_emplace(id, goal);
    D2D1_POINT_2F& p = it->second;
    if (!added && (std::abs(p.x - goal.x) > 0.3f || std::abs(p.y - goal.y) > 0.3f)) {
        p.x = motion::Approach(p.x, goal.x, glideMs_, kGlideRate);
        p.y = motion::Approach(p.y, goal.y, glideMs_, kGlideRate);
        gliding_ = true;
    } else {
        p = goal;
    }
    const float x = p.x - float(page) * w_ + dx;
    return D2D1::RectF(x, p.y, x + (cell.right - cell.left), p.y + (cell.bottom - cell.top));
}

void Launchpad::PaintLifted(Canvas& c, const Theme& t)
{
    if (lifted_ < 0 || lifted_ >= int(tiles_.size()) || !Arranging())
        return;
    const float w = grid_.cellW, h = grid_.cellH;
    const D2D1_RECT_F cell = D2D1::RectF(liftAt_.x - w / 2, liftAt_.y - h / 2, liftAt_.x + w / 2, liftAt_.y + h / 2);
    c.BeginGroup(0.92f, kLiftScale, liftAt_.x, liftAt_.y);
    PaintTile(c, t, lifted_, cell);
    c.EndGroup();
    // Where it is lifted from is where it will be: its place follows it.
    tilePos_.erase(tiles_[size_t(lifted_)].id);
}

} // namespace atrium
