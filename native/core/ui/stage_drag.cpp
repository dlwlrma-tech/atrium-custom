#include "ui/stage_manager.h"

#include <algorithm>
#include <unordered_map>

namespace atrium {

void StageManager::SetDragMerge(bool on)
{
    dragMerge_ = on;
    SyncMoveHook();
}

int StageManager::EntryAt(POINT pt) const
{
    std::vector<D2D1_RECT_F> boxes;
    boxes.reserve(entries_.size());
    for (const Entry& e : entries_)
        boxes.push_back(Box(e));
    const float s = surface_.Scale();
    return StageEntryAt(boxes, float(pt.x) / s, float(pt.y) / s);
}

void StageManager::BeginDrag(HWND window)
{
    CURSORINFO ci{sizeof(ci)};
    const PCWSTR resize[] = {IDC_SIZEWE, IDC_SIZENS, IDC_SIZENWSE, IDC_SIZENESW};
    if (!enabled_ || !dragMerge_ || !IsWindow(window) || !IsWindowVisible(window) || IsIconic(window) || Transient(window) ||
        (GetCursorInfo(&ci) && ci.hCursor && std::ranges::any_of(resize, [&](PCWSTR id) { return ci.hCursor == LoadCursorW(nullptr, id); })))
        return;
    auto it = std::ranges::find_if(items_, [window](const TaskItem& t) { return std::ranges::find(t.windows, window) != t.windows.end(); });
    if (it == items_.end() || StageAppExcluded(excluded_, it->key))
        return;
    GetWindowRect(window, &dragStart_);
    dragWindow_ = window;
    dragKey_ = it->key;
    coveredAt_ = 0;
    Watch();
}

void StageManager::StepDrag(POINT screen)
{
    if (!dragWindow_)
        return;
    if (!enabled_ || !dragMerge_ || !IsWindow(dragWindow_) || !IsWindowVisible(dragWindow_) || IsIconic(dragWindow_) || StageAppExcluded(excluded_, dragKey_)) {
        CancelDrag();
        return;
    }
    const int hit = shown_ && surface_.Visible() ? EntryAt(surface_.ScreenToContent(screen)) : -1;
    if (hit == dragHover_)
        return;
    if (hit >= 0 && dragHover_ < 0)
        Foresee();
    dragHover_ = hit;
    surface_.Invalidate();
}

void StageManager::FinishDrag(HWND window)
{
    if (window != dragWindow_)
        return;
    const std::wstring key = dragKey_;
    const RECT startRect = dragStart_;
    CancelDrag();
    RECT nowRect{};
    POINT pt{};
    if (!enabled_ || !dragMerge_ || !IsWindow(window) || !IsWindowVisible(window) || IsIconic(window) || !GetWindowRect(window, &nowRect) ||
        EqualRect(&nowRect, &startRect) || (GetAsyncKeyState(VK_ESCAPE) & 0x8000) || !shown_ || !surface_.Visible() || !GetCursorPos(&pt))
        return;
    const int hit = EntryAt(surface_.ScreenToContent(pt));
    if (hit < 0 || size_t(hit) >= entries_.size())
        return;
    parked_.erase(window);
    Prune();
    Set nextStage = stage_;
    std::vector<Set> nextRecent = recent_;
    if (!StageMergeGroups(nextStage, nextRecent, excluded_, key, entries_[size_t(hit)].keys))
        return;
    std::unordered_map<HWND, Pose> from;
    for (HWND w : entries_[size_t(hit)].windows)
        if (Pose pose; StripPose(w, pose))
            from[w] = pose;
    MoveBeside(window);
    std::erase(stage_, key);
    pending_ = nextStage;
    pendingUntil_ = GetTickCount64() + kPendingMs + ULONGLONG(kFlightMs);
    hover_ = pressed_ = -1;
    Stage(nextStage, false, window, true, &from);
    if (!flights_.empty()) {
        raise_ = window;
        SetTimer(surface_.Hwnd(), kRaiseTimer, UINT(kFlightMs) + 80, nullptr);
    }
    AdoptMinimized();
    Rebuild();
}

void StageManager::CancelDrag()
{
    dragWindow_ = nullptr;
    dragKey_.clear();
    coveredAt_ = 0;
    if (std::exchange(dragHover_, -1) >= 0)
        surface_.Invalidate();
}

} // namespace atrium
