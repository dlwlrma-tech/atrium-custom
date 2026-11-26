// Stage Manager bringing windows back from the strip: picks, restore-all, and the restore bookkeeping
// that keeps a window on its way back out of re-adoption.
#include "ui/stage_manager.h"

#include <algorithm>

namespace atrium {

// Back without taking the foreground. Never a synchronous call into another app's window: one that
// hangs must not hang the bar. A window minimized from maximized comes back maximized:
// SW_SHOWNOACTIVATE alone restores its normal size, a maximized app back at half the screen (#19).
void StageManager::ShowQuietly(HWND h)
{
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (IsIconic(h) && GetWindowPlacement(h, &wp) && (wp.flags & WPF_RESTORETOMAXIMIZED)) {
        ShowWindowAsync(h, SW_SHOWMAXIMIZED);
        return;
    }
    ShowWindowAsync(h, SW_SHOWNOACTIVATE);
}

// The windows of `keys` that were put away come back, least recent first; `front` (by default the
// most recent of them) last, and in front, activated if asked (a pick in the strip).
void StageManager::Unpark(const Set& keys, HWND front, bool activate, const std::unordered_map<HWND, Pose>* fly, bool moveBeside)
{
    std::vector<std::pair<uint64_t, HWND>> mine;
    for (const auto& [h, p] : parked_)
        if (HasKey(keys, p.key))
            mine.emplace_back(p.seq, h);
    std::ranges::sort(mine);
    if (!front && !mine.empty())
        front = mine.back().second;
    // One of them comes back maximized: the bar goes with the flight, not after it lands.
    if (onCoverSoon && std::ranges::any_of(mine, [](const auto& m) {
            WINDOWPLACEMENT wp{sizeof(wp)};
            return IsZoomed(m.second) || (IsIconic(m.second) && GetWindowPlacement(m.second, &wp) && (wp.flags & WPF_RESTORETOMAXIMIZED));
        }))
        onCoverSoon(monitor_);
    // Kept out of the strip's band: they come back beside it.
    if (moveBeside)
        for (const auto& [seq, h] : mine)
            MoveBeside(h);
    // Picked in the strip: the pictures fly back to the windows' places, the windows come up as they land.
    bool flying = false;
    for (const auto& [seq, h] : mine) {
        const Pose* from = nullptr;
        if (fly)
            if (auto it = fly->find(h); it != fly->end())
                from = &it->second;
        std::wstring key;
        auto picture = from ? PictureOf(h, parked_[h].key, key) : nullptr;
        std::erase_if(flights_, [&](const Flight& f) { return f.window == h; });
        if (!picture || !IsWindow(h) || !key.starts_with(L"stage|w|"))
            continue;
        Flight f{h, picture, key, {}, {}, false, *from, PlacePose(parked_[h].frame), 0, 0, false, activate && h == front};
        SetLarge(f);
        flights_.push_back(std::move(f));
        flying = true;
    }
    if (flying)
        Launch();
    const ULONGLONG now = GetTickCount64();
    for (const auto& [seq, h] : mine) {
        parked_.erase(h);
        if (Flying(h))
            restoring_[h] = now; // wait out the flight until StepFlight triggers restore
        else
            UnparkDirect(h, h != front, now);
    }
    if (!front)
        return;
    parked_.erase(front);
    if (!Flying(front) && (activate || IsIconic(front)))
        EnqueueAction({activate ? Action::Kind::Activate : Action::Kind::ShowQuietly, front});
    if (!mine.empty()) {
        // The others come back asynchronously: `front` goes on top again once they have.
        raise_ = front;
        SetTimer(surface_.Hwnd(), kRaiseTimer, kRaiseMs, nullptr);
    }
}

void StageManager::RestoreAll()
{
    openIntent_.Cancel();
    std::unique_lock lk(actionLock_);
    actions_.clear();
    lk.unlock();
    ReleaseUp();
    // Pictures on their way back: their windows come up now.
    for (Flight& f : flights_)
        if (!f.toStrip && !f.restored && IsWindow(f.window)) {
            SetTransitions(f.window, true);
            ShowQuietly(f.window);
        }
    flights_.clear();
    flight_.Hide(false);
    for (const auto& [h, p] : parked_)
        if (IsWindow(h))
            SetTransitions(h, true);
    for (HWND h : ParkedInOrder())
        if (IsWindow(h) && IsIconic(h))
            ShowQuietly(h);
    parked_.clear();
    recent_.clear();
    stage_.clear();
    pending_.clear();
    for (const auto& [h, p] : pictures_)
        if (p.gen)
            ForgetImages(L"stage|w|" + std::to_wstring(reinterpret_cast<uintptr_t>(h)) + L"|" + std::to_wstring(p.gen), true);
    pictures_.clear();
    for (const auto& [app, ph] : placeholders_)
        ForgetImages(L"stage|p|" + app, false);
    placeholders_.clear();
    Save();
    Rebuild();
}

void StageManager::UnparkDirect(HWND h, bool showQuietly, ULONGLONG now)
{
    if (!minimizeToStrip_ || !HasPicture(h))
        SetTransitions(h, true);
    if (IsIconic(h))
        restoring_[h] = now;
    if (showQuietly && IsWindow(h))
        EnqueueAction({Action::Kind::ShowQuietly, h});
}

bool StageManager::IsRestoring(HWND window, ULONGLONG now) const
{
    const auto it = restoring_.find(window);
    const bool hasEntry = it != restoring_.end();
    const uint64_t ageMs = hasEntry && now >= it->second ? now - it->second : 0;
    return ShouldWaitStageRestore(hasEntry, ageMs, kSettleMs, FlyingFromStrip(window));
}

void StageManager::PruneRestoring(ULONGLONG now)
{
    std::erase_if(restoring_, [this, now](const auto& kv) {
        return !IsWindow(kv.first) || !IsWindowVisible(kv.first) || !IsIconic(kv.first) || (!FlyingFromStrip(kv.first) && now - kv.second > kSettleMs);
    });
}

} // namespace atrium
