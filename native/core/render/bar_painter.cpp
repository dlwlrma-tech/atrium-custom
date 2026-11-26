#include "bar_painter.h"
#include "bar_painter_shared.h"
#include "continuous_corners.h"

#include "ime_glyph.h"
#include "round_stroke.h"
#include "ui_font.h"
#include "wifi_glyph.h"
#include "../log.h"
#include "../motion.h"
#include "../settings.h"
#include "glass_renderer.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>

namespace atrium {

namespace {

// Positions (items, the bar's edges) ease towards new places at this rate per second (about 0.3 s).
constexpr float kSlideRate = 14;
// An app asking for attention: its plate pulses kAlertPulseMs (once each kAlertPeriodMs), then stays lit.
constexpr double kAlertPulseMs = 4000;
// One hop: launch feedback, and kAlertHops of them for an app asking for attention.
constexpr double kHopMs = 480;
constexpr int kAlertHops = 3;

// motion::Approach, landing on `target` once the rest is invisible: 0..1 values.
float Approach(float v, float target, double dtMs, float rate)
{
    v = motion::Approach(v, target, dtMs, rate);
    return std::fabs(target - v) < 0.002f ? target : v;
}

// The same for positions in px.
float ApproachPx(float v, float target, double dtMs, float rate)
{
    v = motion::Approach(v, target, dtMs, rate);
    return std::fabs(target - v) < 0.25f ? target : v;
}

D2D1_COLOR_F BarMixColor(float dark, D2D1_COLOR_F d, D2D1_COLOR_F l)
{
    return D2D1::ColorF(l.r + (d.r - l.r) * dark, l.g + (d.g - l.g) * dark, l.b + (d.b - l.b) * dark, l.a + (d.a - l.a) * dark);
}

D2D1_COLOR_F BarHaloColor(float dark) { return BarMixColor(dark, D2D1::ColorF(1, 1, 1, 0.35f), D2D1::ColorF(0, 0, 0, 0.32f)); }

D2D1_COLOR_F BarWashColor(float dark) { return BarMixColor(dark, D2D1::ColorF(0, 0, 0, 0.10f), D2D1::ColorF(1, 1, 1, 0.20f)); }

bool StepItemHop(const BarScene& scene, const SceneItem& item, double alertSince, double nowMs, float& hop)
{
    hop = 0;
    bool moving = false;
    // Launch: one soft hop (v0.9.8; it was up to four, each lower). It leaves and lands
    // without a jolt (sin², no speed at either end), whatever the app does meanwhile.
    if (item.bounceMs > 0) {
        const double t = nowMs - item.bounceMs;
        if (t >= 0 && t < kHopMs) {
            const float sn = std::sin(std::numbers::pi_v<float> * float(t / kHopMs));
            hop = sn * sn;
            moving = true;
        }
    }
    // Asking for attention: a few hops of the same kind when it starts (not under the pointer).
    if (item.attention && scene.alertBounce && alertSince >= 0 && scene.hoverKey != item.key && scene.dragKey != item.key) {
        const double t = nowMs - alertSince;
        if (t >= 0 && t < kAlertHops * kHopMs) {
            const float sn = std::sin(std::numbers::pi_v<float> * float(std::fmod(t, kHopMs) / kHopMs));
            hop = std::max(hop, sn * sn);
            moving = true;
        }
    }
    return moving;
}

template<class StatusMotionT> bool StepStatusRing(StatusMotionT& s, const SystemStatus& st, double nowMs, double dt)
{
    constexpr double kLevelShowMs = 1500;
    bool moving = false;
    if (s.seen && st.audio && (st.volume != s.volume || st.muted != s.muted)) {
        s.volumeUntilMs = nowMs + kLevelShowMs;
        s.brightUntilMs = 0;
    }
    if (s.seen && st.brightness >= 0 && s.brightness >= 0 && st.brightness != s.brightness) {
        s.brightUntilMs = nowMs + kLevelShowMs;
        s.volumeUntilMs = 0;
    }
    s.volume = st.volume;
    s.muted = st.muted;
    s.brightness = st.brightness;
    const bool showVolume = st.audio && nowMs < s.volumeUntilMs, showBright = st.brightness >= 0 && nowMs < s.brightUntilMs;
    moving |= showVolume || showBright;
    const float mix = Approach(s.volumeMix, showVolume ? 1.f : 0.f, dt, 12);
    const float bmix = Approach(s.brightMix, showBright ? 1.f : 0.f, dt, 12);
    moving |= mix != s.volumeMix || bmix != s.brightMix;
    s.volumeMix = mix;
    s.brightMix = bmix;
    const float volume = st.muted ? 0.f : float(st.volume) / 100.f;
    const float target = showBright                  ? float(st.brightness) / 100.f :
                         (showVolume || !st.battery) ? volume :
                                                       float(std::clamp(st.batteryPercent, 0, 100)) / 100.f;
    if (!s.seen) {
        s.fill = target;
        s.seen = true;
    }
    motion::kSmooth.Step(s.fill, s.speed, target, float(dt) / 1000.f);
    if (motion::Spring::AtRest(s.fill, s.speed, target)) {
        s.fill = target;
        s.speed = 0;
        return moving;
    }
    return true;
}

} // namespace

BarPainter::BarPainter(ID2D1DeviceContext2* d2d, IDWriteFactory3* dwrite)
{
    dwrite_.copy_from(dwrite);
    winrt::check_hresult(d2d->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), contentBrush_.put()));
    winrt::check_hresult(d2d->CreateEffect(CLSID_D2D1Shadow, shadow_.put()));
}

IDWriteTextFormat* BarPainter::Format(float px, DWRITE_FONT_WEIGHT weight, const wchar_t* family)
{
    auto key = std::make_tuple(int(std::lround(px * 4)), int(weight), std::wstring(family ? family : L""));
    auto& f = formats_[key];
    if (!f) {
        if (family)
            dwrite_->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px, L"", f.put());
        else
            UiTextFormat(dwrite_.get(), px, weight, f.put());
        if (f) {
            f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    }
    return f.get();
}

ID2D1Bitmap1* BarPainter::BitmapFor(ID2D1DeviceContext2* d2d, const IconImage* img, uint64_t id)
{
    if (!img || img->Empty() || !id)
        return nullptr;
    auto& b = bitmaps_[id];
    if (!b) {
        D2D1_BITMAP_PROPERTIES1 bp =
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        d2d->CreateBitmap(D2D1::SizeU(UINT32(img->width), UINT32(img->height)), img->bgra.data(), UINT32(img->width * 4), &bp, b.put());
    }
    return b.get();
}

ID2D1Bitmap1* BarPainter::BitmapFor(ID2D1DeviceContext2* d2d, const SceneItem& item)
{
    return BitmapFor(d2d, item.image.get(), item.imageId);
}

float BarPainter::MapX(float u) const { return wave_.MapX(u); }

void BarPainter::StepWave(const BarScene& scene, double dt, BarAnimation& anim)
{
    const float prevRise = wave_.Height(1.f);
    const float prevCentre = wave_.Centre();
    const float prevWidth = wave_.Width();
    const float prevHeight = wave_.Height(scene.magnifyLift);
    const float prevMag = wave_.Scale(prevCentre, 1.f);
    if (!wave_.Update(scene, dt))
        return;
    const float curRise = wave_.Height(1.f);
    const float curCentre = wave_.Centre();
    const float curWidth = wave_.Width();
    const float curHeight = wave_.Height(scene.magnifyLift);
    const float curMag = wave_.Scale(curCentre, 1.f);
    if (curRise != prevRise || curHeight != prevHeight || curMag != prevMag || (curRise > 0.f && (curCentre != prevCentre || curWidth != prevWidth)))
        anim.glass = true;
    if (curMag != prevMag || curMag > 1.0001f)
        anim.body = true;
}

bool BarPainter::StepSingleItemMotion(const BarScene& scene, const std::wstring& key, const SceneItem* item, double nowMs, double dt, bool snap)
{
    Motion& m = motion_[key];
    m.inScene = true;
    const float h = Approach(m.hover, scene.hoverKey == key || scene.dragKey == key ? 1.f : 0.f, dt, 18);
    const float p = Approach(m.press, scene.pressedKey == key && scene.dragKey != key ? 1.f : 0.f, dt, 30);
    const float a = Approach(m.appear, 1.f, dt, 10);
    const float l = Approach(m.lift, scene.dragKey == key ? 1.f : 0.f, dt, 22);
    const float d = Approach(m.dot, item && item->running ? 1.f : 0.f, dt, 12);
    const bool asks = item && item->attention;
    // Not under the pointer: the icon grows there and would cover part of the lit plate.
    const bool lit = asks && scene.alertLight && scene.hoverKey != key && scene.dragKey != key;
    const float al = Approach(m.alert, lit ? 1.f : 0.f, dt, 8);
    if (asks && m.alertSince < 0)
        m.alertSince = nowMs;
    else if (!asks)
        m.alertSince = -1;
    // The lit plate pulses softly for its first seconds, then stays lit until the app is opened.
    bool moving = lit && nowMs - m.alertSince < kAlertPulseMs;
    moving |= h != m.hover || p != m.press || a != m.appear || l != m.lift || d != m.dot || al != m.alert;
    m.alert = al;
    m.hover = h;
    m.press = p;
    m.appear = a;
    m.lift = l;
    m.dot = d;
    m.leaving = false; // back again
    if (!item)
        return moving;
    const float x = (item->plate.left + item->plate.right) / 2;
    if (!m.placed || snap || scene.dragKey == key) {
        m.x = x;
        m.placed = true;
    } else {
        const float nx = ApproachPx(m.x, x, dt, kSlideRate);
        moving |= nx != m.x;
        m.x = nx;
    }
    moving |= StepItemHop(scene, *item, m.alertSince, nowMs, m.hop);
    return moving;
}

bool BarPainter::StepItemMotions(const BarScene& scene, double nowMs, double dt, bool snap)
{
    for (auto& [key, m] : motion_)
        m.inScene = false;
    bool moving = false;
    for (const auto& it : scene.items)
        moving |= StepSingleItemMotion(scene, it.key, &it, nowMs, dt, snap);
    for (const auto& it : scene.extras)
        moving |= StepSingleItemMotion(scene, it.key, &it, nowMs, dt, snap);
    moving |= StepSingleItemMotion(scene, L"#clock", nullptr, nowMs, dt, snap);
    return moving;
}

bool BarPainter::PruneLeavingItems(const BarScene& scene, double dt, bool snap)
{
    bool moving = false;
    for (auto it = motion_.begin(); it != motion_.end();) {
        Motion& m = it->second;
        if (m.inScene) {
            ++it;
            continue;
        }
        auto last = lastApps_.find(it->first);
        if (!m.leaving && last != lastApps_.end() && !snap) {
            m.leaving = true;
            m.ghost = last->second;
            m.hover = m.press = m.lift = 0;
            m.hop = 0;
        }
        lastApps_.erase(it->first);
        if (m.leaving) {
            m.appear = Approach(m.appear, 0.f, dt, 11);
            m.dot = Approach(m.dot, 0.f, dt, 14);
        }
        if (!m.leaving || m.appear < 0.02f || snap) {
            it = motion_.erase(it);
        } else {
            moving = true;
            ++it;
        }
    }
    for (const auto& it : scene.items)
        lastApps_[it.key] = it;
    return moving;
}

bool BarPainter::StepRipple(const BarScene& scene, double nowMs)
{
    frame_.rippleA = 0;
    if (scene.rippleMs <= 0)
        return false;
    constexpr double kRippleMs = 720;
    const double t = nowMs - scene.rippleMs;
    if (t < 0 || t >= kRippleMs)
        return false;
    const float u = float(t / kRippleMs);
    frame_.rippleX = MapX(scene.rippleX);
    frame_.rippleY = scene.rippleY + frame_.slideY;
    frame_.rippleR = (scene.bar.bottom - scene.bar.top) * 1.5f * motion::EaseOut(u);
    frame_.rippleA = 0.9f * (1 - u) * (1 - u);
    return true;
}

BarAnimation BarPainter::Animate(const BarScene& scene, double nowMs)
{
    const double dt = lastMs_ == 0 ? 16 : std::clamp(nowMs - lastMs_, 0.0, 100.0);
    lastMs_ = nowMs;
    BarAnimation anim;
    // A new window geometry: items jump to their places instead of sliding
    // across from coordinates of the old window. (The window keeps its width
    // when apps come and go; then the bar's edges and the items slide.)
    const bool snap = scene.edge != lastEdge_ || scene.windowW != lastWindowW_ || scene.bar.top != lastTop_ || scene.bar.bottom != lastBottom_;
    lastEdge_ = scene.edge;
    lastWindowW_ = scene.windowW;
    lastTop_ = scene.bar.top;
    lastBottom_ = scene.bar.bottom;
    auto slide = [&](float& v, float target, bool& channel) {
        if (snap) {
            v = target;
            return;
        }
        const float n = ApproachPx(v, target, dt, kSlideRate);
        channel |= n != v;
        v = n;
    };
    slide(barL_, scene.bar.left, anim.glass);
    slide(barR_, scene.bar.right, anim.glass);
    if (scene.separatorX < 0 || sepX_ < 0)
        sepX_ = scene.separatorX;
    else
        slide(sepX_, scene.separatorX, anim.body);
    slide(clockX_, (scene.clock.left + scene.clock.right) / 2, anim.body);

    // Magnification: follows the pointer at once, grows and settles smoothly. A pointer
    // resting just above the bar is answered ahead of time too, each on its own switch:
    // the glass rises to it (riseNear, with hoverRise) and the icons magnify (magnifyNear).
    StepWave(scene, dt, anim);

    anim.body |= StepItemMotions(scene, nowMs, dt, snap);
    anim.body |= PruneLeavingItems(scene, dt, snap);
    anim.body |= StepStatusRing(statusMotion_, scene.status, nowMs, dt);

    constexpr double kImeFadeMs = 180;
    const float prevImeFade = imeFade_;
    imeFade_ = scene.imeMs > 0 ? float(std::clamp((nowMs - scene.imeMs) / kImeFadeMs, 0.0, 1.0)) : 1.f;
    anim.body |= imeFade_ < 1.f || prevImeFade < 1.f;

    anim.glass |= AnimateSlide(scene, nowMs);
    anim.music |= AnimateMusic(scene, dt);

    // What the glass shows this frame: the stretched shape and the ripple.
    frame_.shape = D2D1::RectF(MapX(barL_), scene.bar.top + frame_.slideY, MapX(barR_), scene.bar.bottom + frame_.slideY);
    frame_.waveX = wave_.Centre();
    frame_.waveWidth = wave_.Width();
    frame_.waveHeight = wave_.Height(scene.magnifyLift);
    anim.glass |= StepRipple(scene, nowMs);
    return anim;
}

D2D1_ELLIPSE BarPainter::RunningDot(const BarScene& scene, const SceneItem& item, const Motion& motion) const
{
    float dx = motion.placed ? motion.x - (item.plate.left + item.plate.right) / 2 : 0;
    float x = MapX((item.icon.left + item.icon.right) / 2 + dx);
    float y = (item.icon.bottom + scene.bar.bottom) / 2;
    float radius = (item.active ? 2.3f : 1.9f) * scene.scale;
    return D2D1::Ellipse(D2D1::Point2F(x, y), radius, radius);
}

// The now-playing bars, while the bar shows them; true while they move.
bool BarPainter::AnimateMusic(const BarScene& scene, double dt)
{
    if (!scene.music || std::ranges::none_of(scene.extras, [](const SceneItem& it) { return it.key == L"#music"; }))
        return false;
    const float peak = scene.music->EffectivePeak();
    const bool playing = scene.music->playing.load();
    const float intensity = scene.music->intensity.load();
    const bool moving = music_.Step(peak, playing, intensity, dt);
    if (!moving && playing && intensity > 0.f)
        scene.music->needWake.store(true, std::memory_order_release);
    return moving;
}

// Auto-hide: the slide down off the screen and back up; true while it moves.
bool BarPainter::AnimateSlide(const BarScene& scene, double nowMs)
{
    bool moving = false;
    if (scene.tucked != slideDown_) {
        slideDown_ = scene.tucked;
        slideFrom_ = slide_;
        slideStartMs_ = nowMs;
        Log(L"bar: slide %s", slideDown_ ? L"down" : L"up");
    }
    {
        const float target = slideDown_ ? 1.f : 0.f;
        if (slide_ != target) {
            double length = slideDown_ ? kSlideOutMs : kSlideInMs;
            float u = float(std::clamp((nowMs - slideStartMs_) / length, 0.0, 1.0));
            float e;
            if (slideDown_) {
                e = 0.5f - 0.5f * std::cos(std::numbers::pi_v<float> * u); // eases in and out
            } else {
                e = motion::EaseOutBack(u, 1.2f); // a slight overshoot
            }
            slide_ = u >= 1 ? target : slideFrom_ + (target - slideFrom_) * e;
            moving = true;
        }
    }
    frame_.slideY = slide_ * std::max(0.f, scene.windowH - scene.bar.top);
    return moving;
}

bool BarPainter::UpdateDotInk(const BarScene& scene, const Readback& backdrop, const Settings& settings, double nowMs)
{
    float mix = float(1 - settings.transparency);
    float tint = (0.2126f * float((settings.tint >> 16) & 255) + 0.7152f * float((settings.tint >> 8) & 255) + 0.0722f * float(settings.tint & 255)) / 255.f;
    const EdgeFrame frame = BarFrameFor(scene);
    bool moving = false;
    for (const auto& item : scene.items) {
        auto& m = motion_[item.key];
        if (!item.running && m.dot < 0.01f)
            continue;
        if (!backdrop.luma.empty()) {
            auto dot = RunningDot(scene, item, m);
            D2D1_POINT_2F p = frame.ToArea(D2D1::Point2F(dot.point.x, dot.point.y + std::round(frame_.slideY)));
            float x = p.x + GlassRenderer::kPad, y = p.y + GlassRenderer::kPad;
            float luma = backdrop.RegionLuma(x - dot.radiusX, y - dot.radiusY, x + dot.radiusX, y + dot.radiusY) * (1 - mix) + tint * mix;
            if (!m.darkDot && luma > 0.50f)
                m.darkDot = true;
            else if (m.darkDot && luma < 0.42f)
                m.darkDot = false;
        }
        bool dark = settings.textColor == L"dark" || (settings.textColor == L"auto" && m.darkDot);
        float before = m.dotInk.Value(nowMs);
        m.dotInk.Target(dark ? 1.f : 0.f, nowMs);
        moving |= before != m.dotInk.Value(nowMs) || m.dotInk.Moving(nowMs);
    }
    return moving;
}

bool BarPainter::TryDrawMusicClip(ID2D1DeviceContext2* d2d, const BarScene& scene, Ink ink, const std::wstring& contentKey, ID2D1Image* target,
                                  D2D1_SIZE_U size)
{
    const auto itExtra = std::ranges::find_if(scene.extras, [](const SceneItem& it) { return it.key == L"#music"; });
    if (itExtra == scene.extras.end() || itExtra->running || itExtra->attention || itExtra->badge > 0)
        return false;
    const auto itMotion = motion_.find(L"#music");
    if (itMotion == motion_.end())
        return false;
    const Motion& m = itMotion->second;
    const float plateCx = (itExtra->plate.left + itExtra->plate.right) / 2;
    const float plateW = std::max(1.f, itExtra->plate.right - itExtra->plate.left);
    if (m.hover != 0.f || m.press != 0.f || m.lift != 0.f || m.alert != 0.f || m.hop != 0.f || m.leaving || m.appear < 0.999f ||
        (m.placed && std::fabs(m.x - plateCx) > 0.01f))
        return false;
    if (wave_.Height(1.f) > 0.001f || std::fabs(wave_.MapX(plateCx) - plateCx) > 0.01f || std::fabs(wave_.Scale(plateCx, plateW) - 1.f) > 0.001f)
        return false;
    const D2D1_RECT_F areaRect = BarFrameFor(scene).ToArea(itExtra->plate);
    const D2D1_RECT_F clip =
        D2D1::RectF(std::max(0.f, std::floor(areaRect.left - 1.f)), std::max(0.f, std::floor(areaRect.top - 1.f)),
                    std::min(float(size.width), std::ceil(areaRect.right + 1.f)), std::min(float(size.height), std::ceil(areaRect.bottom + 1.f)));
    if (clip.right <= clip.left || clip.bottom <= clip.top)
        return false;
    d2d->SetTarget(layer_.get());
    d2d->SetTransform(D2D1::Matrix3x2F::Identity());
    d2d->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
    d2d->Clear(D2D1::ColorF(0, 0, 0, 0));
    d2d->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    d2d->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    const D2D1_COLOR_F rightInk = BarInkColor(ink.darkClock);
    contentBrush_->SetColor(rightInk);
    DrawItem(d2d, scene, *itExtra, contentBrush_.get(), rightInk, BarWashColor(ink.darkClock));
    d2d->PopAxisAlignedClip();
    d2d->SetTarget(target);
    layerKey_ = contentKey;
    layerEdge_ = scene.edge;
    return true;
}

void BarPainter::Draw(ID2D1DeviceContext2* d2d, const BarScene& scene, Ink ink, const std::wstring& time, const std::wstring& date,
                      const std::wstring& contentKey, BarAnimation anim, bool forceBody)
{
    winrt::com_ptr<ID2D1Image> target;
    d2d->GetTarget(target.put());
    const auto size = d2d->GetPixelSize();
    if (layer_ && (layer_->GetPixelSize().width != size.width || layer_->GetPixelSize().height != size.height))
        layer_ = nullptr;
    if (!layer_) {
        D2D1_BITMAP_PROPERTIES1 bp =
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(d2d->CreateBitmap(size, nullptr, 0, &bp, layer_.put()))) {
            DrawContent(d2d, scene, ink, time, date); // no layer: draw directly
            return;
        }
        layerKey_.clear();
    }
    const bool keyMismatch = contentKey != layerKey_ || scene.edge != layerEdge_;
    if (anim.body || anim.music || forceBody || keyMismatch) {
        const bool musicOnly = anim.music && !anim.body && !anim.glass && !forceBody && !keyMismatch;
        if (!musicOnly || !TryDrawMusicClip(d2d, scene, ink, contentKey, target.get(), size)) {
            d2d->SetTarget(layer_.get());
            d2d->Clear(D2D1::ColorF(0, 0, 0, 0));
            DrawContent(d2d, scene, ink, time, date);
            d2d->SetTarget(target.get());
            layerKey_ = anim.body ? std::wstring() : contentKey;
            layerEdge_ = scene.edge;
        }
    }
    d2d->SetTransform(D2D1::Matrix3x2F::Identity());
    const D2D1_POINT_2F at = BarVectorToArea(BarFrameFor(scene), 0, std::round(frame_.slideY));
    d2d->DrawImage(layer_.get(), &at, nullptr, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_SOURCE_OVER);
}

void BarPainter::DrawContent(ID2D1DeviceContext2* d2d, const BarScene& scene, Ink inks, const std::wstring& time, const std::wstring& date)
{
    const float s = scene.scale;
    // Ink colours: white on dark glass, near-black on light glass, per region.
    // `dark` is the amount of dark ink: colours in between while the ink fades.
    auto inkFor = BarInkColor;
    float darkText = inks.darkClock;
    D2D1_COLOR_F ink = inkFor(inks.darkApps);
    D2D1_COLOR_F halo = BarHaloColor(darkText);
    D2D1_COLOR_F wash = BarWashColor(inks.darkApps);
    const auto& brush = contentBrush_;
    brush->SetColor(ink);
    d2d->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    d2d->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE); // premultiplied target: no ClearType

    usedBitmaps_.clear();
    for (const auto& it : scene.extras) { // the right group follows the clock's ink, the others the apps'
        bool right = it.key == L"#notify" || it.key == L"#tray" || it.key.starts_with(L"#tray:") || it.key == L"#status" || it.key == L"#ime" ||
                     it.key == L"#desktop" || it.key == L"#music" || it.key == L"#stacks";
        DrawItem(d2d, scene, it, brush.get(), right ? inkFor(darkText) : ink, right ? BarWashColor(darkText) : wash);
    }
    for (const auto& [key, m] : motion_)
        if (m.leaving)
            DrawItem(d2d, scene, m.ghost, brush.get(), ink, wash);
    for (const auto& it : scene.items)
        if (it.key != scene.dragKey)
            DrawItem(d2d, scene, it, brush.get(), ink, wash);
    for (const auto& it : scene.items)
        if (it.key == scene.dragKey)
            DrawItem(d2d, scene, it, brush.get(), ink, wash); // on top of the others
    // Drop bitmaps of icons that are gone or were replaced.
    for (auto it = bitmaps_.begin(); it != bitmaps_.end();)
        it = std::ranges::find(usedBitmaps_, it->first) != usedBitmaps_.end() ? std::next(it) : bitmaps_.erase(it);

    const EdgeFrame frame = BarFrameFor(scene);
    if (sepX_ >= 0) {
        D2D1_COLOR_F c = inkFor(darkText);
        c.a = 0.30f + (0.18f - 0.30f) * darkText;
        brush->SetColor(c);
        float h = (scene.bar.bottom - scene.bar.top) * 0.46f, mid = (scene.bar.top + scene.bar.bottom) / 2, x = MapX(sepX_);
        d2d->FillRectangle(frame.ToArea(D2D1::RectF(x - 0.5f * s, mid - h / 2, x + 0.5f * s, mid + h / 2)), brush.get());
    }

    if (scene.clock.right > scene.clock.left) {
        const Motion& m = motion_[L"#clock"];
        ink = inkFor(darkText);
        // The clock does not grow; it moves with the stretched bar.
        float shift = MapX(clockX_) - (scene.clock.left + scene.clock.right) / 2;
        const D2D1_RECT_F clock = frame.ToArea(scene.clock);
        const D2D1_POINT_2F clockShift = BarVectorToArea(frame, shift, 0);
        d2d->SetTransform(D2D1::Matrix3x2F::Translation(clockShift.x, clockShift.y));
        if (m.hover > 0.001f) {
            D2D1_COLOR_F c = BarWashColor(darkText);
            c.a *= motion::EaseOut(m.hover);
            brush->SetColor(c);
            float r = scene.plateRadius;
            FillContinuousRect(d2d, clock, r, brush.get());
        }
        float timePx = scene.timePx, datePx = scene.datePx;
        if (Vertical(scene.edge)) {
            const float slot = clock.right - clock.left;
            if (float w = MeasureUiText(dwrite_.get(), time, timePx, DWRITE_FONT_WEIGHT_SEMI_BOLD); w > slot && slot > 0)
                timePx *= slot / w;
            if (float w = MeasureUiText(dwrite_.get(), date, datePx, DWRITE_FONT_WEIGHT_NORMAL); w > slot && slot > 0)
                datePx *= slot / w;
        }
        float lineT = timePx * 1.22f, lineD = datePx * 1.25f;
        float top = (clock.top + clock.bottom) / 2 - (lineT + lineD) / 2;
        auto text = [&](const std::wstring& str, IDWriteTextFormat* f, float y0, float y1) {
            if (!f)
                return;
            float o = std::max(1.0f, s * 0.6f);
            brush->SetColor(halo);
            DrawUiText(d2d, dwrite_.get(), str, f, D2D1::RectF(clock.left, y0 + o, clock.right, y1 + o), brush.get());
            brush->SetColor(ink);
            DrawUiText(d2d, dwrite_.get(), str, f, D2D1::RectF(clock.left, y0, clock.right, y1), brush.get());
        };
        text(time, Format(timePx, DWRITE_FONT_WEIGHT_SEMI_BOLD), top, top + lineT);
        text(date, Format(datePx, DWRITE_FONT_WEIGHT_NORMAL), top + lineT, top + lineT + lineD);
        d2d->SetTransform(D2D1::Matrix3x2F::Identity());
    }
}

} // namespace atrium
