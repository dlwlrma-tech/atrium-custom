#include "render/music_bars.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>

using namespace atrium;

namespace {

int g_failures = 0;

void Require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}

// Runs `bars` for `ms` in 16 ms frames; true if the last frame still moved.
bool Run(MusicBars& bars, float peak, bool playing, float intensity, int ms)
{
    bool moving = false;
    for (int t = 0; t < ms; t += 16)
        moving = bars.Step(peak, playing, intensity, 16);
    return moving;
}

void CheckMusicCadence()
{
    Require(!ShouldThrottleMusicOnlyFrame(-1.0), "negative elapsed time is never throttled");
    Require(ShouldThrottleMusicOnlyFrame(0.0) && ShouldThrottleMusicOnlyFrame(12.99), "elapsed time below 13 ms is throttled");
    Require(!ShouldThrottleMusicOnlyFrame(13.0), "exactly 13 ms is allowed");
    for (const auto& [hz, periodMs, expectedEveryTicks] :
         {std::tuple{60, 1000.0 / 60.0, 1}, std::tuple{120, 1000.0 / 120.0, 2}, std::tuple{144, 1000.0 / 144.0, 2}, std::tuple{240, 1000.0 / 240.0, 4}}) {
        double lastPresent = 0.0;
        int presents = 0;
        int ticksSincePresent = 0;
        for (int tick = 1; tick <= 24; ++tick) {
            const double now = tick * periodMs;
            ++ticksSincePresent;
            if (!ShouldThrottleMusicOnlyFrame(now - lastPresent)) {
                Require(ticksSincePresent == expectedEveryTicks,
                        hz == 144 ? "144 Hz presents on every 2nd tick (72 fps)" : "refresh rate locks onto expected integer tick divisor");
                lastPresent = now;
                ticksSincePresent = 0;
                ++presents;
            }
        }
        Require(presents == 24 / expectedEveryTicks, "full cadence cycle presents expected frame count");
    }
    Require(!ShouldThrottleMusicOnlyFrame(2.0 * (1000.0 / 144.0) - 0.5),
            "144 Hz two-tick cadence absorbs -0.5 ms compositor jitter without slipping to 48 fps");
}

} // namespace

int main()
{
    Require(MusicBars::Loudness(0.f) == 0.f, "silence is no loudness");
    Require(MusicBars::Loudness(1.f) == 1.f, "full scale is full loudness");
    Require(MusicBars::Loudness(0.0039f) == 0.f, "48 dB down is silence");
    const float half = MusicBars::Loudness(0.0631f); // -24 dB
    Require(half > 0.49f && half < 0.51f, "loudness is linear in decibels");

    MusicBars bars;
    for (size_t i = 0; i < MusicBars::kCount; ++i)
        Require(bars.Height(i) == MusicBars::StillHeight(i), "a new entry shows the still waveform");
    Require(!bars.Step(0.f, false, 1.f, 16), "paused from the start: nothing moves");
    Require(MusicBars::StillHeight(2) > MusicBars::StillHeight(0) && MusicBars::StillHeight(2) > MusicBars::StillHeight(4),
            "the still waveform is tallest in the middle");

    Require(Run(bars, 0.7f, true, 1.f, 600), "playing music keeps the bars moving");
    Require(bars.Level() > 0.8f, "the level reaches a loud peak within 0.6 s");
    float low = 1.f, high = 0.f;
    for (int t = 0; t < 3000; t += 16) {
        bars.Step(0.7f, true, 1.f, 16);
        for (size_t i = 0; i < MusicBars::kCount; ++i) {
            Require(bars.Height(i) >= MusicBars::kRest - 0.001f && bars.Height(i) <= 1.f, "bars stay inside the icon");
            low = std::min(low, bars.Height(2));
            high = std::max(high, bars.Height(2));
        }
    }
    Require(high - low > 0.2f, "the middle bar swings with the music");
    bool apart = false;
    for (int t = 0; t < 1000 && !apart; t += 16) {
        bars.Step(0.7f, true, 1.f, 16);
        apart = std::abs(bars.Height(1) - bars.Height(3)) > 0.08f;
    }
    Require(apart, "bars move on their own rhythms, not in step");

    MusicBars quiet;
    Run(quiet, 0.01f, true, 1.f, 2000);
    float quietTop = 0.f;
    for (size_t i = 0; i < MusicBars::kCount; ++i)
        quietTop = std::max(quietTop, quiet.Height(i));
    Require(quietTop < 0.6f, "quiet music moves the bars a little");

    Require(!Run(bars, 0.7f, false, 1.f, 2500), "paused: the bars settle and stop asking for frames");
    for (size_t i = 0; i < MusicBars::kCount; ++i)
        Require(std::abs(bars.Height(i) - MusicBars::StillHeight(i)) < 0.005f, "paused: the bars rest in the still waveform");
    Require(!Run(bars, 0.7f, true, 0.f, 2500), "motion off: playing music leaves the bars still");

    MusicBars jumpy;
    Require(jumpy.Step(0.9f, true, 1.f, 5000), "a long frame is clamped, never a jump past the target");
    for (size_t i = 0; i < MusicBars::kCount; ++i)
        Require(jumpy.Height(i) <= 1.f, "a long frame keeps the bars inside the icon");

    const float qnan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    Require(MusicBars::Loudness(qnan) == 0.f, "NaN peak produces zero loudness");
    Require(MusicBars::Loudness(inf) == 0.f, "+Inf peak produces zero loudness");
    Require(MusicBars::Loudness(-inf) == 0.f, "-Inf peak produces zero loudness");
    Require(MusicBars::Loudness(-0.5f) == 0.f, "negative peak produces zero loudness");

    MusicBars bad;
    bad.Step(qnan, true, 1.f, 16.0);
    bad.Step(inf, true, inf, inf);
    bad.Step(0.5f, true, qnan, qnan);
    Require(std::isfinite(bad.Level()), "level stays finite after NaN/Inf inputs");
    for (size_t i = 0; i < MusicBars::kCount; ++i)
        Require(std::isfinite(bad.Height(i)) && bad.Height(i) >= MusicBars::kRest && bad.Height(i) <= 1.f,
                "heights stay finite and bounded after NaN/Inf inputs");
    Require(!Run(bad, 0.f, false, 1.f, 2500), "bars settle normally after NaN/Inf inputs");

    MusicFeed feed;
    Require(feed.EffectivePeak() == 0.f, "EffectivePeak starts at zero");
    feed.peak.store(0.6f);
    Require(feed.EffectivePeak() == 0.6f, "EffectivePeak returns peak when loop is off");
    feed.peak.store(0.f);
    feed.loop.store(true);
    Require(feed.EffectivePeak() == 0.35f, "EffectivePeak returns 0.35 when loop is on even with zero peak");

    MusicBars silent;
    Require(Run(silent, 0.7f, true, 1.f, 600), "loud playback starts moving");
    Require(!Run(silent, 0.f, true, 1.f, 2500), "playing during silence settles and stops asking for frames");
    Require(silent.Level() == 0.f, "playing during silence snaps level to zero");
    for (size_t i = 0; i < MusicBars::kCount; ++i)
        Require(std::abs(silent.Height(i) - MusicBars::kRest) < 0.003f, "playing during silence settles heights to kRest");
    Require(silent.Step(0.5f, true, 1.f, 0.5), "sound returning from silence immediately moves even on a 0.5 ms step");

    MusicBars subtleQuiet;
    bool subtleMovedEveryFrame = true;
    for (int t = 0; t < 200; ++t)
        subtleMovedEveryFrame = subtleMovedEveryFrame && subtleQuiet.Step(0.00631f, true, 0.55f, 16);
    Require(subtleMovedEveryFrame && subtleQuiet.Step(0.00631f, true, 0.55f, 0.5),
            "quiet -44 dB playback with subtle motion keeps moving without dropping frames");

    MusicBars pausedAfterLoud;
    Require(Run(pausedAfterLoud, 0.8f, true, 1.f, 600), "loud playback animates before pausing");
    Require(!Run(pausedAfterLoud, 0.8f, false, 1.f, 2500), "pausing after loud playback settles and stops");
    for (size_t i = 0; i < MusicBars::kCount; ++i)
        Require(std::abs(pausedAfterLoud.Height(i) - MusicBars::StillHeight(i)) < 0.005f, "pausing after loud playback settles to StillHeight");

    CheckMusicCadence();

    std::cout << (g_failures ? "music_bars: FAILED" : "music_bars: ok") << '\n';
    return g_failures ? 1 : 0;
}
