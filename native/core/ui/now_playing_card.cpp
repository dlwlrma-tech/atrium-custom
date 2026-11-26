#include "ui/now_playing_card.h"

#include "bar_edge.h"
#include "clock.h"
#include "localization.h"
#include "render/corner_profile.h"

#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <tuple>

namespace atrium {

namespace {

// The card, DIP, long as the Dynamic Island playing music and sized from the panels' type (DESIGN
// §1.2: body 13.5 for the track and the artist, caption 11 for the times; the panel padding 14): the
// art with the two lines beside it, the five bars at the far end; the progress bar between its times;
// the three controls with the heart to their left and the lyrics to their right. A little wider than
// the control centre (340).
constexpr float kW = 360, kPad = 14, kBody = 13.5f, kCaption = 11;
constexpr float kArt = 52, kArtRadius = 12;
// The bars sit in from the card's side by their own width's air, as the art sits in its corner.
constexpr float kTextX = kPad + kArt + 12, kBarsW = 20, kBarsRight = kW - kPad - 6, kBarsY = kPad + kArt / 2;
constexpr float kTrackY = kPad + kArt + 16, kTrackH = 4, kTrackHot = 7, kTimeW = 38, kKnob = 6;
constexpr float kControlsY = kTrackY + 34, kControlStep = 58, kControlR = 16, kPlayR = 20;
constexpr float kH = kControlsY + kPlayR + kPad;
constexpr D2D1_COLOR_F kHeart{1.f, 0.216f, 0.373f, 1.f}; // a liked track's heart (#FF375F)
constexpr double kArtFadeMs = 260;
constexpr UINT_PTR kTickTimer = 0x4E50;
constexpr UINT kTickMs = 250; // the progress bar's pace while nothing else animates

D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { return D2D1::ColorF(c.r, c.g, c.b, c.a * a); }

D2D1_RECT_F ArtRect() { return D2D1::RectF(kPad, kPad, kPad + kArt, kPad + kArt); }
D2D1_RECT_F SessionRect() { return D2D1::RectF(kW - kPad - 40, kPad + kArt - 16, kW - kPad, kPad + kArt); }
D2D1_POINT_2F ControlCentre(int i) { return D2D1::Point2F(kW / 2 + float(i) * kControlStep, kControlsY); }

// "1:23", or "1:02:03" past an hour.
std::wstring Clock(int64_t us)
{
    const int64_t s = std::max<int64_t>(0, us / 1000000);
    wchar_t text[32];
    if (s >= 3600)
        swprintf_s(text, L"%lld:%02lld:%02lld", s / 3600, s / 60 % 60, s % 60);
    else
        swprintf_s(text, L"%lld:%02lld", s / 60, s % 60);
    return text;
}

// Rounds a premultiplied picture's corners with the G3 continuous corner every panel has
// (corner_profile.h): each pixel keeps the share of it inside the outline (`radius` a share of the side).
IconImage RoundCorners(const IconImage& src, float radius)
{
    IconImage out = src;
    const float w = float(src.width), h = float(src.height);
    const CornerProfile profile = CornerFor(w, h, radius * std::min(w, h));
    for (int y = 0; y < src.height; ++y)
        for (int x = 0; x < src.width; ++x) {
            const float cover = CornerCoverage(float(x) + 0.5f, float(y) + 0.5f, w, h, profile);
            if (cover >= 1)
                continue;
            uint8_t* p = &out.bgra[(size_t(y) * size_t(src.width) + size_t(x)) * 4];
            for (int c = 0; c < 4; ++c)
                p[c] = uint8_t(std::lround(float(p[c]) * cover));
        }
    return out;
}

// The picture's colour: its pixels weighted by how vivid they are, then lifted to read on glass.
// None for a grey picture.
std::optional<D2D1_COLOR_F> Tint(const IconImage& src)
{
    double r = 0, g = 0, b = 0, weight = 0;
    for (size_t i = 0; i + 3 < src.bgra.size(); i += 16) { // every fourth pixel is plenty
        const float a = float(src.bgra[i + 3]) / 255.f;
        if (a < 0.5f)
            continue;
        const float pb = float(src.bgra[i]) / 255.f / a, pg = float(src.bgra[i + 1]) / 255.f / a, pr = float(src.bgra[i + 2]) / 255.f / a;
        const float hi = std::max({pr, pg, pb}), lo = std::min({pr, pg, pb});
        const float vivid = (hi - lo) * hi; // saturated and bright counts most
        r += pr * vivid;
        g += pg * vivid;
        b += pb * vivid;
        weight += vivid;
    }
    if (weight < 1e-3 * (double(src.bgra.size()) / 16 + 1))
        return std::nullopt;
    const float k = float(weight), top = float(std::max({r, g, b}) / weight);
    const float lift = 0.95f / std::max(top, 0.05f);
    return D2D1::ColorF(std::min(1.f, float(r) / k * lift), std::min(1.f, float(g) / k * lift), std::min(1.f, float(b) / k * lift), 1.f);
}

void PaintTransportMark(Canvas& c, D2D1_POINT_2F o, bool isPlay, bool playing, int dir, D2D1_COLOR_F ink)
{
    // The marks (MainWindow.ExtrasScenes.cs draws the same): pause as two bars, play as a
    // triangle, previous and next as two triangles pointing the way the track goes.
    if (isPlay && playing) {
        c.Fill(D2D1::RectF(o.x - 7.5f, o.y - 8.5f, o.x - 2.5f, o.y + 8.5f), 1.8f, ink);
        c.Fill(D2D1::RectF(o.x + 2.5f, o.y - 8.5f, o.x + 7.5f, o.y + 8.5f), 1.8f, ink);
    } else if (isPlay) {
        const std::array<D2D1_POINT_2F, 3> play{{{o.x - 4.5f, o.y - 8}, {o.x + 8.5f, o.y}, {o.x - 4.5f, o.y + 8}}};
        c.Polygon(play, ink, 2);
    } else {
        const float d = float(dir);
        for (float base : {-9.5f, 0.f}) {
            const std::array<D2D1_POINT_2F, 3> tri{{{o.x + d * base, o.y - 6}, {o.x + d * (base + 8.5f), o.y}, {o.x + d * base, o.y + 6}}};
            c.Polygon(tri, ink, 1.5f);
        }
    }
}

} // namespace

NowPlayingCard::NowPlayingCard(HINSTANCE instance) : surface_(instance, L"Atrium Now Playing", true)
{
    surface_.paint = [this](Canvas& c) { Paint(c); };
    surface_.message = [this](UINT m, WPARAM w, LPARAM l, LRESULT& r) { return Message(m, w, l, r); };
    surface_.onDismiss = [this] { Close(); };
}

void NowPlayingCard::Open(RECT entry, RECT bar)
{
    entry_ = entry;
    bar_ = bar;
    hover_ = pressed_ = Part::None;
    dragging_ = closing_ = false;
    hoverAnim_.clear();
    Place(true);
    SetTimer(surface_.Hwnd(), kTickTimer, kTickMs, nullptr);
}

void NowPlayingCard::Close()
{
    if (closing_)
        return;
    if (const HWND h = surface_.Hwnd())
        KillTimer(h, kTickTimer);
    ReleaseDrag(true);
    hover_ = Part::None;
    if (!surface_.Visible())
        return;
    closing_ = true;
    surface_.Hide();
    lastClosed_ = GetTickCount64();
    if (onClose)
        onClose();
    closing_ = false;
}

// kPopupGapDip off the bar's glass: over a horizontal bar its right side lines up with the bar's,
// beside a vertical one it is centred on the entry; inside the work area.
void NowPlayingCard::Place(bool show)
{
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromRect(&bar_, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& work = mi.rcWork;
    const float s = PopupScale(bar_);
    const LONG w = std::lround(kW * s), h = std::lround(kH * s), gap = std::lround(kPopupGapDip * s);
    const LONG ex = (entry_.left + entry_.right) / 2, ey = (entry_.top + entry_.bottom) / 2;
    const BarEdge edge = EdgeOf(bar_);
    RECT r{};
    r.left = edge == BarEdge::Left ? bar_.right + gap : edge == BarEdge::Right ? bar_.left - gap - w : bar_.right - w;
    r.top = Vertical(edge) ? ey - h / 2 : edge == BarEdge::Top ? bar_.bottom + gap : bar_.top - gap - h;
    r.left = std::clamp(r.left, work.left + gap, std::max(work.left + gap, work.right - gap - w));
    r.top = std::clamp(r.top, work.top + gap, std::max(work.top + gap, work.bottom - gap - h));
    r.right = r.left + w;
    r.bottom = r.top + h;
    if (show)
        surface_.Show(r, true, POINT{ex, ey});
    else
        surface_.Move(r);
}

void NowPlayingCard::Update(const MediaSnapshot& snapshot)
{
    const double now = ClockMs();
    // Another track: its heart and lyrics are not known to be on, unless the player tells (NetEase's heart).
    if (snapshot.sessionId != media_.sessionId || snapshot.title != media_.title || snapshot.artist != media_.artist) {
        liked_ = snapshot.liked.value_or(false);
        seekTo_.reset();
        playingShown_.reset();
    }
    if (snapshot.appId != media_.appId || !snapshot.canLyrics)
        lyricsOn_ = false;
    if (snapshot.liked && snapshot.liked != media_.liked)
        liked_ = *snapshot.liked; // given or taken in the player (or here, once the player has it)
    media_ = snapshot;
    if (!media_.timeline.Seekable() || !media_.canSeek) {
        const Part keepPressed = pressed_ == Part::Track ? Part::None : pressed_;
        ReleaseDrag(true);
        pressed_ = keepPressed;
    }
    // The session agrees with the press (or never answered): its state shows again.
    if (playingShown_ && (*playingShown_ == (media_.state == MediaPlaybackState::Playing) || now - playingAt_ > 1500))
        playingShown_.reset();
    if (seekTo_ && (std::llabs(media_.timeline.PositionUs(now) - *seekTo_) < 1500000 || now - seekAt_ > 2500))
        seekTo_.reset();
    if (media_.artworkVersion != artVersion_)
        TakeArtwork();
    if (!media_.active)
        Close();
    else
        surface_.Invalidate();
}

void NowPlayingCard::WakeBars()
{
    if (surface_.Visible())
        surface_.Invalidate();
}

void NowPlayingCard::TakeArtwork()
{
    artVersion_ = media_.artworkVersion;
    surface_.ForgetImage(L"np|art");
    const IconImage* art = media_.artwork.get();
    if (!art || art->Empty() || art->bgra.size() < size_t(art->width) * size_t(art->height) * 4) {
        art_ = IconImage{};
        tint_.reset();
        return;
    }
    artMs_ = ClockMs();
    art_ = RoundCorners(*art, kArtRadius / kArt);
    tint_ = Tint(*art);
}

bool NowPlayingCard::Playing() const { return playingShown_ ? *playingShown_ : media_.state == MediaPlaybackState::Playing; }

int64_t NowPlayingCard::ShownPositionUs(double now) const
{
    const MediaTimeline& t = media_.timeline;
    if (dragging_)
        return t.startUs + int64_t(double(t.endUs - t.startUs) * dragFraction_);
    if (seekTo_)
        return *seekTo_;
    return t.PositionUs(now);
}

float NowPlayingCard::TrackFraction(float x) const { return std::clamp((x - trackL_) / (trackR_ - trackL_), 0.f, 1.f); }

D2D1_RECT_F NowPlayingCard::TrackRect() const { return D2D1::RectF(trackL_ - 6, kTrackY - 11, trackR_ + 6, kTrackY + 11); }

void NowPlayingCard::Send(MediaCommand command, int64_t positionUs)
{
    if (onCommand)
        onCommand(command, positionUs);
}

void NowPlayingCard::TogglePlayPause()
{
    playingShown_ = !Playing();
    playingAt_ = ClockMs();
    Send(MediaCommand::PlayPause);
    surface_.Invalidate();
}

void NowPlayingCard::ReleaseDrag(bool releaseCapture)
{
    if (releaseCapture) {
        if (const HWND h = surface_.Hwnd(); h && GetCapture() == h)
            ReleaseCapture();
    }
    dragging_ = false;
    pressed_ = Part::None;
}

void NowPlayingCard::ActivatePart(Part part)
{
    if (part == Part::Play && media_.canPlayPause) {
        TogglePlayPause();
    } else if (part == Part::Previous && media_.canPrevious) {
        Send(MediaCommand::Previous);
    } else if (part == Part::Next && media_.canNext) {
        Send(MediaCommand::Next);
    } else if (part == Part::Like && media_.canLike) {
        liked_ = !liked_;
        Send(MediaCommand::Like);
    } else if (part == Part::Lyrics && media_.canLyrics) {
        lyricsOn_ = !lyricsOn_;
        Send(MediaCommand::Lyrics);
    } else if (part == Part::Session) {
        Send(MediaCommand::NextSession);
    } else if (part == Part::Art) {
        Send(MediaCommand::ActivateApp);
        Close();
    }
}

float NowPlayingCard::Hover(Canvas& c, Part part, bool on)
{
    auto& f = hoverAnim_[part];
    const float v = f.Step(on, c.now);
    if (f.moving)
        c.Continue();
    return v;
}

void NowPlayingCard::Paint(Canvas& c)
{
    const Theme& t = c.theme;
    const D2D1_RECT_F card = D2D1::RectF(0, 0, kW, kH);
    if (c.panel)
        c.Fill(card, radius_, t.panel);
    // The art fades in over the glass (PaintHeader).
    const float artIn = motion::EaseOut(float((c.now - artMs_) / kArtFadeMs));
    if (!art_.Empty() && artIn < 1)
        c.Continue();
    PaintHeader(c, t);
    PaintProgress(c, t);
    PaintControls(c, t);
}

void NowPlayingCard::PaintHeader(Canvas& c, const Theme& t)
{
    const float artIn = motion::EaseOut(float((c.now - artMs_) / kArtFadeMs));
    const D2D1_RECT_F art = ArtRect();
    if (!art_.Empty()) {
        c.Image(L"np|art", art_, art, artIn);
    } else {
        c.Fill(art, kArtRadius, Alpha(t.hover, 1.6f));
        c.Glyph(L"", art, 22, t.subtext); // MusicNote
    }
    const bool several = media_.sessionCount > 1;
    const float right = kBarsRight - kBarsW - 10;
    const float artistRight = several ? std::min(right, SessionRect().left - 6) : right;
    const std::wstring title = media_.title.empty() ? std::wstring(Tr(L"Not playing")) : media_.title;
    // The track and the artist, centred together on the art.
    const float mid = kPad + kArt / 2;
    c.Text(title, D2D1::RectF(kTextX, mid - 19, right, mid), kBody, t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    c.Text(media_.artist, D2D1::RectF(kTextX, mid + 1, artistRight, mid + 20), kBody, t.subtext);
    if (several) {
        const D2D1_RECT_F s = SessionRect();
        const float hv = Hover(c, Part::Session, hover_ == Part::Session);
        c.Fill(s, (s.bottom - s.top) / 2, Alpha(t.hover, 0.5f + 0.5f * hv));
        const std::wstring badge = std::to_wstring(media_.sessionIndex + 1) + L"/" + std::to_wstring(media_.sessionCount) + L" ›";
        c.Text(badge, s, kCaption, t.text, DWRITE_FONT_WEIGHT_NORMAL, 1);
    }
    // The five bars, live, in the art's colour.
    const double dt = lastBarsMs_ == 0 ? 16 : c.now - lastBarsMs_;
    lastBarsMs_ = c.now;
    const float peak = feed_ ? feed_->EffectivePeak() : 0.f, intensity = feed_ ? feed_->intensity.load() : 1.f;
    if (bars_.Step(peak, Playing(), intensity, dt))
        c.Continue();
    else if (feed_ && Playing() && intensity > 0.f)
        feed_->needWake.store(true, std::memory_order_release);
    const float bw = kBarsW / 9, gap = (kBarsW - 5 * bw) / 4, cy = several ? kPad + 12 : kBarsY;
    const D2D1_COLOR_F ink = tint_ ? *tint_ : t.text;
    c.BeginInkArea(D2D1::RectF(kBarsRight - kBarsW, cy - kBarsW / 2, kBarsRight, cy + kBarsW / 2));
    for (size_t i = 0; i < MusicBars::kCount; ++i) {
        const float x = kBarsRight - kBarsW + float(i) * (bw + gap), h = std::max(bw, kBarsW * bars_.Height(i));
        c.Fill(D2D1::RectF(x, cy - h / 2, x + bw, cy + h / 2), bw / 2, ink);
    }
    c.EndInkArea();
}

void NowPlayingCard::PaintProgress(Canvas& c, const Theme& t)
{
    const MediaTimeline& tl = media_.timeline;
    const float hv = Hover(c, Part::Track, hover_ == Part::Track || dragging_);
    if (!tl.Seekable())
        return; // no length known (a stream, or a player that tells none): no track at all
    // The times either side of the bar, as the Dynamic Island has them, each flush with the card's side (and
    // with the art above the left one); past an hour they take the room they need from the bar.
    const int64_t pos = ShownPositionUs(c.now), length = tl.endUs - tl.startUs;
    const std::wstring done = Clock(pos - tl.startUs), left = L"-" + Clock(tl.endUs - pos);
    const float timeW = std::max({kTimeW, c.TextWidth(done, kCaption), c.TextWidth(left, kCaption)}) + 2;
    trackL_ = kPad + timeW + 6;
    trackR_ = kW - kPad - timeW - 6;
    c.Text(done, D2D1::RectF(kPad, kTrackY - 8, trackL_ - 6, kTrackY + 8), kCaption, t.subtext);
    c.Text(left, D2D1::RectF(trackR_ + 6, kTrackY - 8, kW - kPad, kTrackY + 8), kCaption, t.subtext, DWRITE_FONT_WEIGHT_NORMAL, 2);
    const float th = kTrackH + (kTrackHot - kTrackH) * hv;
    const D2D1_RECT_F track = D2D1::RectF(trackL_, kTrackY - th / 2, trackR_, kTrackY + th / 2);
    c.Fill(track, th / 2, Alpha(t.text, 0.16f));
    const float played = std::clamp(float(double(pos - tl.startUs) / double(length)), 0.f, 1.f);
    if (played > 0)
        c.Fill(D2D1::RectF(track.left, track.top, std::max(track.left + th, track.left + (track.right - track.left) * played), track.bottom), th / 2,
               tint_ && hv > 0.5f ? *tint_ : t.text);
    // Under the pointer (where the player can be told a position): a knob at the playhead, to drag.
    if (hv > 0.01f) {
        const float x = track.left + (track.right - track.left) * played, r = kKnob * motion::EaseOut(hv);
        c.Circle(x, kTrackY + 0.75f, r + 0.5f, D2D1::ColorF(0, 0, 0, 0.18f * hv)); // a hint of shadow
        c.Circle(x, kTrackY, r, tint_ ? *tint_ : t.text);
    }
}

D2D1_RECT_F NowPlayingCard::BeginControl(Canvas& c, const Theme& t, Part part, int at, float r, bool enabled, float plateMin)
{
    const D2D1_POINT_2F o = ControlCentre(at);
    const float hv = enabled ? Hover(c, part, hover_ == part) : 0.f;
    const float plate = std::max(hv, plateMin);
    const bool down = enabled && pressed_ == part;
    if (plate > 0.001f)
        c.Circle(o.x, o.y, r, Alpha(down ? t.press : t.hover, plate));
    c.BeginGroup(enabled ? 1.f : 0.35f, down ? 0.86f : 1.f, o.x, o.y);
    const D2D1_RECT_F box = D2D1::RectF(o.x - r, o.y - r, o.x + r, o.y + r);
    c.BeginInkArea(box); // one ink for the whole mark
    return box;
}

void NowPlayingCard::PaintControls(Canvas& c, const Theme& t)
{
    struct Control {
        Part part;
        int at;
        float r;
        bool enabled;
    };
    // The three light up in circles of one size, the play button's.
    const std::array<Control, 3> controls{
        {{Part::Previous, -1, kPlayR, media_.canPrevious}, {Part::Play, 0, kPlayR, media_.canPlayPause}, {Part::Next, 1, kPlayR, media_.canNext}}};
    for (const auto& k : controls) {
        BeginControl(c, t, k.part, k.at, k.r, k.enabled);
        PaintTransportMark(c, ControlCentre(k.at), k.part == Part::Play, Playing(), k.at, t.text);
        c.EndInkArea();
        c.EndGroup();
    }
    // Either side: the heart (the player's own like) and its lyrics, where the player takes them.
    for (const auto& [part, at, enabled] : {std::tuple{Part::Like, -2, media_.canLike}, {Part::Lyrics, 2, media_.canLyrics}}) {
        if (!enabled)
            continue;
        // The lyrics on: a plate behind the mark, as a pressed toggle (the mark stays in the ink).
        const float plateMin = part == Part::Lyrics && lyricsOn_ ? 1.f : 0.f;
        const D2D1_RECT_F box = BeginControl(c, t, part, at, kControlR, true, plateMin);
        if (part == Part::Like) {
            c.Glyph(liked_ ? L"\uEB52" : L"\uEB51", box, 16, liked_ ? kHeart : t.text); // HeartFill, Heart
        } else {
            const std::wstring lyrics = Tr(L"Lyrics", L"button"); // 词: one character where it reads so
            c.Text(lyrics, box, lyrics.size() > 2 ? 11.f : 16.5f, t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        }
        c.EndInkArea();
        c.EndGroup();
    }
}

NowPlayingCard::Part NowPlayingCard::HitAt(POINT pt) const
{
    const float s = surface_.Scale(), x = float(pt.x) / s, y = float(pt.y) / s;
    auto in = [&](const D2D1_RECT_F& r) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; };
    if (media_.timeline.Seekable() && media_.canSeek && in(TrackRect()))
        return Part::Track;
    if (media_.sessionCount > 1 && in(SessionRect()))
        return Part::Session;
    if (in(ArtRect()))
        return Part::Art;
    struct ControlHit {
        Part part;
        int at;
        bool enabled;
    };
    const std::array<ControlHit, 5> controls{{{Part::Like, -2, media_.canLike},
                                              {Part::Previous, -1, media_.canPrevious},
                                              {Part::Play, 0, media_.canPlayPause},
                                              {Part::Next, 1, media_.canNext},
                                              {Part::Lyrics, 2, media_.canLyrics}}};
    for (const auto& k : controls) {
        if (!k.enabled)
            continue;
        const D2D1_POINT_2F o = ControlCentre(k.at);
        if (std::hypot(x - o.x, y - o.y) <= kPlayR + 4)
            return k.part;
    }
    return Part::None;
}

bool NowPlayingCard::Message(UINT m, WPARAM w, LPARAM l, LRESULT&)
{
    const POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    const float x = float(pt.x) / surface_.Scale();
    switch (m) {
    case WM_TIMER:
        if (w != kTickTimer)
            return false;
        if (Playing())
            surface_.Invalidate(); // the progress bar and the time move on
        return true;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE)
            Close();
        return false;
    case WM_CAPTURECHANGED:
        if (reinterpret_cast<HWND>(l) != surface_.Hwnd()) {
            ReleaseDrag(false);
            surface_.Invalidate();
        }
        return false;
    case WM_CANCELMODE:
        ReleaseDrag(true);
        surface_.Invalidate();
        return false;
    case WM_MOUSEMOVE: {
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, surface_.Hwnd(), 0};
            tracking_ = TrackMouseEvent(&tme) != FALSE;
        }
        if (dragging_)
            dragFraction_ = TrackFraction(x);
        hover_ = dragging_ ? Part::Track : HitAt(pt);
        surface_.Invalidate();
        return true;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        if (!dragging_)
            hover_ = pressed_ = Part::None;
        surface_.Invalidate();
        return true;
    case WM_LBUTTONDOWN:
        pressed_ = HitAt(pt);
        if (pressed_ == Part::Track) {
            dragging_ = true;
            dragFraction_ = TrackFraction(x);
            SetCapture(surface_.Hwnd());
        }
        surface_.Invalidate();
        return true;
    case WM_LBUTTONUP: {
        const Part was = pressed_;
        const Part at = HitAt(pt);
        if (dragging_) {
            ReleaseDrag(true);
            const MediaTimeline& tl = media_.timeline;
            if (tl.Seekable() && media_.canSeek) {
                seekTo_ = tl.startUs + int64_t(double(tl.endUs - tl.startUs) * TrackFraction(x));
                seekAt_ = ClockMs();
                Send(MediaCommand::Seek, *seekTo_);
            }
        } else {
            pressed_ = Part::None;
            if (was == at)
                ActivatePart(at);
        }
        surface_.Invalidate();
        return true;
    }
    case WM_KEYDOWN:
        if (w == VK_ESCAPE)
            Close();
        else if (w == VK_SPACE && media_.canPlayPause)
            TogglePlayPause();
        else
            return false;
        return true;
    default: return false;
    }
}

} // namespace atrium
