// Launches from the bar that join the stage once their window arrives, not whichever app
// opens next. Several can wait at once (A, then B, added before either window shows); each
// lasts ten seconds, ends when its window has joined, and all end when the stage changes.
#pragma once

#include <climits>
#include <cstdint>
#include <map>
#include <string>

namespace atrium {

enum class StageAdoptAction { Up, Adopt, Wait, Drop };

// Pure decision for a stage window during AdoptMinimized:
// - non-iconic windows stay up only on the managed monitor;
// - iconic windows on the stage are adopted (even if Win32 moved them to -32000 off-monitor or
//   they missed up_), unless still restoring from an unpark without a picture.
inline StageAdoptAction DecideStageAdopt(bool parked, bool visible, bool iconic, bool onMonitor, bool inUp, bool restoring)
{
    (void)inUp;
    if (parked || !visible)
        return StageAdoptAction::Drop;
    if (!iconic)
        return onMonitor ? StageAdoptAction::Up : StageAdoptAction::Drop;
    if (restoring)
        return StageAdoptAction::Wait;
    return StageAdoptAction::Adopt;
}

// Restoring a picked strip window keeps it out of re-adoption while its outbound flight
// (`flyingFromStrip`) is active, and for `settleMs` after restore is dispatched.
inline bool ShouldWaitStageRestore(bool hasRestoringEntry, uint64_t ageMs, uint64_t settleMs, bool flyingFromStrip)
{
    return flyingFromStrip || (hasRestoringEntry && ageMs <= settleMs);
}

inline uint64_t StagePictureRetryMs(int misses)
{
    if (misses <= 0)
        return 5000;
    if (misses == 1)
        return 700;
    if (misses == 2)
        return 1500;
    return 5000;
}

// Whether KeepPicture should request a snapshot now:
// - once a picture exists, keep the steady-state 1500 ms settle and 1500 ms / 20 s refresh;
// - before the first picture exists, take it after 300 ms (200 ms near caption buttons) and
//   retry failed/blank captures with backoff.
inline bool ShouldTakeStagePicture(bool hasPicture, bool titleBar, int misses, uint64_t frontAgeMs, uint64_t pictureAgeMs)
{
    if (hasPicture)
        return frontAgeMs >= 1500 && pictureAgeMs >= (titleBar ? 1500ULL : 20000ULL);
    const uint64_t firstDelay = titleBar ? 200ULL : 300ULL;
    if (frontAgeMs < firstDelay)
        return false;
    if (pictureAgeMs == ULLONG_MAX)
        return true;
    return pictureAgeMs >= StagePictureRetryMs(misses);
}

inline bool ShouldParkStageWindow(bool parked, bool visible, bool iconic, bool onMonitor, bool transient, bool fillsScreen, bool enabled)
{
    return !parked && visible && !iconic && onMonitor && !transient && !fillsScreen && enabled;
}

class StageOpenIntent {
public:
    void Arm(const std::wstring& app, uint64_t now) { apps_[app] = now + 10000; }
    void Cancel() { apps_.clear(); }
    void Done(const std::wstring& app) { apps_.erase(app); }
    bool Matches(const std::wstring& app, uint64_t now)
    {
        std::erase_if(apps_, [&](const auto& item) { return now >= item.second; });
        return apps_.contains(app);
    }

private:
    std::map<std::wstring, uint64_t> apps_;
};

} // namespace atrium
