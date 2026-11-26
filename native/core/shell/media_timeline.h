// Pure media timeline interpolation, app allow/block filtering, and session selection.
#pragma once

#include "win32_path.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace atrium {

enum class MediaPlaybackState { Closed, Opened, Changing, Stopped, Playing, Paused };

struct MediaTimeline {
    int64_t startUs = 0;
    int64_t endUs = 0;
    int64_t anchorPosUs = 0;
    double anchorMs = 0.0;
    double rate = 1.0;
    bool playing = false;

    bool Seekable() const { return endUs > startUs; }

    int64_t PositionUs(double nowMs) const
    {
        int64_t pos = anchorPosUs;
        if (playing && std::isfinite(nowMs) && std::isfinite(anchorMs) && std::isfinite(rate) && rate > 0.0) {
            const double deltaUs = (nowMs - anchorMs) * 1000.0 * rate;
            if (std::isfinite(deltaUs)) {
                constexpr double kMaxStepUs = 86400.0 * 1e6 * 365.0;
                const int64_t step = static_cast<int64_t>(std::llround(std::clamp(deltaUs, -kMaxStepUs, kMaxStepUs)));
                if (step > 0 && pos > std::numeric_limits<int64_t>::max() - step)
                    pos = std::numeric_limits<int64_t>::max();
                else if (step < 0 && pos < std::numeric_limits<int64_t>::min() - step)
                    pos = std::numeric_limits<int64_t>::min();
                else
                    pos += step;
            } else if (deltaUs > 0.0) {
                pos = Seekable() ? endUs : std::numeric_limits<int64_t>::max();
            } else {
                pos = startUs;
            }
        }
        if (Seekable())
            return std::clamp(pos, startUs, endUs);
        return std::max(startUs, pos);
    }
};

inline bool TimelineChanged(const MediaTimeline& a, const MediaTimeline& b)
{
    if (a.startUs != b.startUs || a.endUs != b.endUs || a.anchorPosUs != b.anchorPosUs || a.rate != b.rate || a.playing != b.playing)
        return true;
    return a.playing && !(std::abs(a.anchorMs - b.anchorMs) < 250.0);
}

// A timeline for a player that reports none (NetEase Cloud Music): its length from the player's own
// files, its position counted from when the track was seen to start, through pauses. A track already
// under way when first seen has no known position: no timeline until the next one. Worker thread.
class TimelineEstimate {
public:
    // `track` names the track (its title and artist); lengthUs 0: unknown. Fills `t` when known.
    void Update(const std::wstring& track, bool playing, int64_t lengthUs, double nowMs, MediaTimeline& t)
    {
        if (!seen_ && (track.empty() || track.front() == L'\n'))
            return;
        if (track != track_) {
            fromStart_ = seen_; // a change seen happen: the new track starts at 0
            seen_ = true;
            track_ = track;
            posUs_ = 0;
            anchorMs_ = nowMs;
            playing_ = playing;
        } else if (playing != playing_) {
            if (playing_)
                posUs_ += ElapsedUs(nowMs);
            anchorMs_ = nowMs;
            playing_ = playing;
        }
        // Past its end on the same track: played again (repeat one), from the start.
        if (lengthUs > 0 && Position(nowMs) > lengthUs + 3'000'000) {
            posUs_ = 0;
            anchorMs_ = nowMs;
        }
        if (!fromStart_ || lengthUs <= 0)
            return;
        t.startUs = 0;
        t.endUs = lengthUs;
        t.anchorPosUs = posUs_;
        t.anchorMs = playing_ ? anchorMs_ : nowMs;
        t.rate = 1.0;
        t.playing = playing_;
    }

private:
    int64_t ElapsedUs(double nowMs) const
    {
        if (!std::isfinite(nowMs) || !std::isfinite(anchorMs_))
            return 0;
        constexpr double kMaxStepUs = 86400.0 * 1e6 * 365.0;
        const double us = std::clamp(std::max(0.0, nowMs - anchorMs_) * 1000.0, 0.0, kMaxStepUs);
        return static_cast<int64_t>(std::llround(us));
    }
    int64_t Position(double nowMs) const { return posUs_ + (playing_ ? ElapsedUs(nowMs) : 0); }

    std::wstring track_;
    bool seen_ = false, fromStart_ = false, playing_ = false;
    int64_t posUs_ = 0;
    double anchorMs_ = 0;
};

// Smooths GSMTC timeline updates: freezes on pause, resumes without pause time, anchors once when
// LastUpdatedTime is 0, clamps negative skew to 0, and takes any raw change at once.
struct GsmtcTimelineTracker {
    bool initialized = false;
    int64_t rawStartUs = 0, rawEndUs = 0, rawPosUs = 0, rawUpdated100Ns = 0;
    MediaTimeline current;

    void Reset() { *this = {}; }

    MediaTimeline Update(int64_t startUs, int64_t endUs, int64_t posUs, int64_t updated100Ns, int64_t fileTime100Ns, double nowMs, double rate, bool playing)
    {
        startUs = std::max<int64_t>(0, startUs);
        if (endUs <= startUs || endUs - startUs > 86400LL * 1'000'000 * 30)
            endUs = 0;
        posUs = std::max<int64_t>(0, posUs);
        if (!initialized || startUs != rawStartUs || endUs != rawEndUs || posUs != rawPosUs || updated100Ns != rawUpdated100Ns) {
            rawStartUs = startUs;
            rawEndUs = endUs;
            rawPosUs = posUs;
            rawUpdated100Ns = updated100Ns;
            initialized = true;
            double anchorMs = nowMs;
            if (updated100Ns > 0) {
                const double ageMs = std::max(0.0, static_cast<double>(fileTime100Ns - updated100Ns) / 10000.0);
                if (ageMs < 86400000.0)
                    anchorMs = nowMs - ageMs;
            }
            return current = {startUs, endUs, posUs, anchorMs, rate, playing};
        }
        if (current.playing && (!playing || rate != current.rate)) {
            current.anchorPosUs = current.PositionUs(nowMs);
            current.anchorMs = nowMs;
        } else if (!current.playing && playing) {
            current.anchorMs = nowMs;
        }
        current.playing = playing;
        current.rate = rate;
        return current;
    }
};

struct MediaSessionSummary {
    std::wstring id;
    std::wstring appId;
    std::wstring title;
    MediaPlaybackState state = MediaPlaybackState::Closed;
    bool isSystemCurrent = false;
    double lastPlayingMs = 0.0;
};

namespace media_detail {

inline bool MatchesAppEntry(const std::wstring& appLower, const std::wstring& fileLower, const std::wstring& stemLower, const std::wstring& rawEntry)
{
    const std::wstring entry = FoldCase(rawEntry);
    if (entry.empty())
        return false;
    if (appLower == entry || fileLower == entry)
        return true;
    const wchar_t last = entry.back();
    if ((last == L'\\' || last == L'/' || last == L'_' || last == L'!') && appLower.starts_with(entry))
        return true;
    if (entry.find_first_of(L"\\/") == std::wstring::npos && last != L'_' && last != L'!') {
        if (stemLower == ExeStemFolded(entry))
            return true;
        if (appLower.starts_with(entry + L'_') || appLower.starts_with(entry + L'!'))
            return true;
    }
    return false;
}

} // namespace media_detail

inline bool MediaAppAllowed(const std::wstring& appId, const std::wstring& mode, const std::vector<std::wstring>& list)
{
    const std::wstring m = FoldCase(mode);
    if (m != L"allow" && m != L"block")
        return true;
    const std::wstring app = FoldCase(appId);
    if (app.empty())
        return m == L"block";
    const size_t slash = app.find_last_of(L"\\/");
    const std::wstring file = slash == std::wstring::npos ? app : app.substr(slash + 1);
    const std::wstring stem = ExeStemFolded(file);
    const bool matched = std::ranges::any_of(list, [&](const std::wstring& entry) { return media_detail::MatchesAppEntry(app, file, stem, entry); });
    return m == L"allow" ? matched : !matched;
}

inline int PickMediaSession(const std::vector<MediaSessionSummary>& sessions, bool showPaused, const std::wstring& mode, const std::vector<std::wstring>& apps,
                            const std::wstring& pinnedId)
{
    int pinnedIdx = -1;
    int currentPlayingIdx = -1;
    int recentPlayingIdx = -1;
    int recentPausedIdx = -1;

    for (int i = 0; i < static_cast<int>(sessions.size()); ++i) {
        const auto& s = sessions[i];
        if (s.state == MediaPlaybackState::Closed)
            continue;
        if (s.title.empty() && s.state != MediaPlaybackState::Playing)
            continue;
        const bool isPlaying = s.state == MediaPlaybackState::Playing;
        const bool isPaused = showPaused && s.state == MediaPlaybackState::Paused;
        if (!isPlaying && !isPaused)
            continue;
        if (!MediaAppAllowed(s.appId, mode, apps))
            continue;

        if (!pinnedId.empty() && s.id == pinnedId)
            pinnedIdx = i;
        if (isPlaying) {
            if (s.isSystemCurrent && currentPlayingIdx < 0)
                currentPlayingIdx = i;
            if (recentPlayingIdx < 0 || s.lastPlayingMs > sessions[recentPlayingIdx].lastPlayingMs)
                recentPlayingIdx = i;
        } else if (recentPausedIdx < 0 || s.lastPlayingMs > sessions[recentPausedIdx].lastPlayingMs) {
            recentPausedIdx = i;
        }
    }

    if (pinnedIdx >= 0)
        return pinnedIdx;
    if (currentPlayingIdx >= 0)
        return currentPlayingIdx;
    if (recentPlayingIdx >= 0)
        return recentPlayingIdx;
    return recentPausedIdx;
}

} // namespace atrium
