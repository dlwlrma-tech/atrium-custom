// The now-playing entry's five bars: heights (0..1 of the icon) that move with the music. The
// playing app's peak level (linear amplitude) is read as loudness (decibels over a 48 dB range),
// followed quickly up and slowly down; each bar then swings on its own slow rhythm inside that
// level, the middle one tallest, so the five read as a live equaliser rather than one bar five
// times. Paused, the bars settle into a still waveform; settled, Step reports nothing moving and
// the bar's render thread can idle. Pure: tests drive it with made-up levels and time steps.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace atrium {

// Minimum interval between music-only presents. 13 ms keeps 60 Hz at 60 fps,
// 120 Hz and 240 Hz at 60 fps, and 144 Hz locked on 2-tick 72 fps (13.89 ms)
// instead of slipping to 3 ticks (48 fps) under a 14 ms cutoff.
inline constexpr double kMusicOnlyMinIntervalMs = 13.0;

inline bool ShouldThrottleMusicOnlyFrame(double elapsedMs) { return elapsedMs >= 0.0 && elapsedMs < kMusicOnlyMinIntervalMs; }

// What the bars move with, shared by the media watcher (which writes it, on its own thread) and
// whatever draws the bars (each bar's render thread, the now-playing card), which read it.
struct MusicFeed {
    std::atomic<float> peak{0};       // the playing app's level, linear 0..1
    std::atomic<bool> playing{false}; // the session is playing
    std::atomic<float> intensity{1};  // the motion setting: 0 still .. 1.5 vivid
    std::atomic<bool> loop{false};    // nowPlayingRhythm loop: a steady level, the bars on their own rhythms
    mutable std::atomic<bool> needWake{true}; // set when bars settle while playing so incoming audio wakes UI

    float EffectivePeak() const { return loop.load(std::memory_order_relaxed) ? 0.35f : peak.load(std::memory_order_relaxed); }
};

class MusicBars {
public:
    static constexpr size_t kCount = 5;
    // The silhouette: the middle bar tallest, as Apple's waveform glyph.
    static constexpr std::array<float, kCount> kShape{0.58f, 0.86f, 1.f, 0.80f, 0.52f};
    static constexpr float kRest = 0.22f;   // the shortest bar, playing or not
    static constexpr float kStill = 0.18f;  // what the silhouette adds while still (paused)
    static constexpr float kRangeDb = 48.f; // the loudness shown, below full scale

    // Advances by dtMs. peak: the app's level (0..1, linear); playing: its state; intensity: the
    // setting (0 still, 1 normal, up to 1.5 vivid). True while any bar still moves.
    bool Step(float peak, bool playing, float intensity, double dtMs)
    {
        const double safeDtMs = std::isfinite(dtMs) ? std::clamp(dtMs, 0.0, 100.0) : 0.0;
        const float safeIntensity = std::isfinite(intensity) ? std::max(0.f, intensity) : 0.f;
        const float dt = float(safeDtMs) / 1000.f;
        const bool live = playing && safeIntensity > 0.f;
        const float loud = live ? Loudness(peak) : 0.f;
        level_ += (loud - level_) * (1.f - std::exp(-dt * (loud > level_ ? kAttack : kRelease)));
        if (loud == 0.f && level_ < 0.001f)
            level_ = 0.f;
        bool moving = live && (loud > 0.f || level_ > 0.f);
        for (size_t i = 0; i < kCount; ++i) {
            phase_[i] = std::fmod(phase_[i] + dt * kRate[i] * (0.55f + 0.6f * level_), 1.f);
            const float target = Target(i, live, safeIntensity);
            const float rate = target > heights_[i] ? kRise : kFall;
            heights_[i] += (target - heights_[i]) * (1.f - std::exp(-dt * rate));
            moving = moving || std::abs(target - heights_[i]) > 0.002f;
        }
        return moving;
    }

    float Height(size_t i) const { return heights_[i]; }
    float Level() const { return level_; }

    // Linear amplitude to 0..1 loudness: kRangeDb below full scale is silence.
    static float Loudness(float peak)
    {
        if (!std::isfinite(peak) || peak <= 0.f)
            return 0.f;
        return std::clamp(1.f + 20.f * std::log10(peak) / kRangeDb, 0.f, 1.f);
    }

    // The still waveform (paused, or motion off).
    static float StillHeight(size_t i) { return kRest + kStill * kShape[i]; }

private:
    static constexpr float kAttack = 28.f, kRelease = 8.f; // the level, per second
    static constexpr float kRise = 26.f, kFall = 18.f;     // each bar towards its target, per second
    // Each bar's rhythm (cycles per second, about a beat's pace when loud): no two alike, none a
    // multiple of another, so the pattern does not visibly repeat.
    static constexpr std::array<float, kCount> kRate{0.92f, 1.27f, 1.71f, 1.13f, 0.81f};

    float Target(size_t i, bool live, float intensity) const
    {
        if (!live)
            return StillHeight(i);
        // Two sines a fifth apart: a swing that breathes rather than ticks.
        const float a = 2.f * std::numbers::pi_v<float> * phase_[i];
        const float swing = 0.5f + 0.32f * std::sin(a) + 0.18f * std::sin(1.5f * a + 1.7f * float(i));
        const float reach = std::min(1.f, level_ * intensity) * kShape[i];
        return std::clamp(kRest + (1.f - kRest) * reach * (0.38f + 0.62f * swing), kRest, 1.f);
    }

    float level_ = 0;
    std::array<float, kCount> phase_{0.f, 0.37f, 0.71f, 0.19f, 0.53f};
    std::array<float, kCount> heights_{StillHeight(0), StillHeight(1), StillHeight(2), StillHeight(3), StillHeight(4)};
};

} // namespace atrium
