// The now-playing card, opened from the bar's now-playing entry: a card of the bar's glass in the
// spirit of the Dynamic Island's player, its glass the bar's own (nothing laid over it). The album art
// with rounded corners; the title and artist; the five bars, live, in the art's colour; a progress bar
// between its times that follows the track and, where the player allows, is dragged by its knob to
// seek; previous, play/pause and next. Commands leave through onCommand, the state comes back through Update;
// a press shows its result at once and the session's answer confirms it.
#pragma once

#include "motion.h"
#include "render/music_bars.h"
#include "shell/media_snapshot.h"
#include "ui/surface.h"

#include <windows.h>

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace atrium {

class NowPlayingCard {
public:
    explicit NowPlayingCard(HINSTANCE instance);

    // entry: the now-playing entry on the bar (screen px); bar: the bar's glass.
    void Open(RECT entry, RECT bar);
    void Close();
    bool Visible() const { return surface_.Visible(); }
    ULONGLONG LastClosed() const { return lastClosed_; }
    void Update(const MediaSnapshot& snapshot);
    void WakeBars();
    // The playing app's level and the motion setting, as the bar's entry reads them.
    void SetFeed(std::shared_ptr<const MusicFeed> feed) { feed_ = std::move(feed); }
    // The bar's popup corner, made rounder (the Dynamic Island's): at least kMinRadius.
    void SetRadius(float dip)
    {
        radius_ = std::max(dip, kMinRadius);
        surface_.SetRadius(radius_);
    }
    std::function<void(MediaCommand, int64_t positionUs)> onCommand;
    std::function<void()> onClose;

private:
    static constexpr float kMinRadius = 34;
    enum class Part { None, Track, Previous, Play, Next, Session, Art, Like, Lyrics };

    void Place(bool show);
    void Paint(Canvas& c);
    void PaintHeader(Canvas& c, const Theme& t);
    void PaintProgress(Canvas& c, const Theme& t);
    void PaintControls(Canvas& c, const Theme& t);
    D2D1_RECT_F BeginControl(Canvas& c, const Theme& t, Part part, int at, float r, bool enabled, float plateMin = 0.f);
    bool Message(UINT m, WPARAM w, LPARAM l, LRESULT& result);
    Part HitAt(POINT pt) const;
    void Send(MediaCommand command, int64_t positionUs = 0);
    void TogglePlayPause();
    void ActivatePart(Part part);
    void ReleaseDrag(bool releaseCapture);
    bool Playing() const; // as shown: a press flips it before the session answers
    int64_t ShownPositionUs(double now) const;
    float TrackFraction(float x) const; // a point along the progress bar, 0..1
    D2D1_RECT_F TrackRect() const;      // where the bar takes a press
    float Hover(Canvas& c, Part part, bool on);
    void TakeArtwork(); // the rounded art and its colour, once per picture

    Surface surface_;
    MediaSnapshot media_;
    RECT entry_{}, bar_{};
    float radius_ = 24;
    std::shared_ptr<const MusicFeed> feed_;
    MusicBars bars_;
    double lastBarsMs_ = 0;
    float trackL_ = 58, trackR_ = 302;   // the bar's ends as last painted (DIP): the times beside it decide them
    // The art as drawn: corners rounded; its colour (the bars, the progress).
    IconImage art_;
    uint64_t artVersion_ = 0;
    std::optional<D2D1_COLOR_F> tint_;
    bool liked_ = false, lyricsOn_ = false; // as pressed here, or the heart as the player tells it (NetEase)
    double artMs_ = 0; // a new picture fades in from then
    // Pressed play/pause, until the session agrees (or a second passes).
    std::optional<bool> playingShown_;
    double playingAt_ = 0;
    // Dragging along the progress bar; after the release the bar holds the asked position until the
    // session's timeline catches up.
    bool dragging_ = false;
    bool closing_ = false;
    float dragFraction_ = 0;
    std::optional<int64_t> seekTo_;
    double seekAt_ = 0;
    Part hover_ = Part::None, pressed_ = Part::None;
    bool tracking_ = false;
    ULONGLONG lastClosed_ = 0;
    std::map<Part, motion::Follow> hoverAnim_;
};

} // namespace atrium
