#include "render/capture_policy.h"

#include <iostream>

using atrium::CapturePolicy;

namespace {

bool Require(bool condition, const char* message)
{
    if (condition)
        return true;
    std::cerr << message << '\n';
    return false;
}

bool CheckInitialSelection()
{
    CapturePolicy policy;
    if (!Require(policy.ChooseInitialSource(false, 0) == CapturePolicy::InitialSource::Duplication, "starts with Desktop Duplication") ||
        !Require(policy.ChooseInitialSource(true, 0) == CapturePolicy::InitialSource::Wallpaper, "remote-control mode uses wallpaper") ||
        !Require(!policy.ShouldExitBlackoutWallpaper(false, 100000), "remote wallpaper has no blackout expiry")) {
        return false;
    }

    policy.OnDuplicationUnavailable(1000);
    if (!Require(policy.ChooseInitialSource(false, 1000) == CapturePolicy::InitialSource::MonitorCapture,
                 "unavailable Desktop Duplication falls back to MonitorCapture") ||
        !Require(policy.ChooseInitialSource(true, 1000) == CapturePolicy::InitialSource::Wallpaper, "remote-control mode still overrides MonitorCapture") ||
        !Require(!policy.ShouldProbeDuplication(true, false, false, 0, 30999), "does not probe before 30 s backoff") ||
        !Require(!policy.ShouldProbeDuplication(false, false, false, 0, 31000), "does not probe while hidden") ||
        !Require(!policy.ShouldProbeDuplication(true, true, false, 0, 31000), "does not probe in remote mode") ||
        !Require(!policy.ShouldProbeDuplication(true, false, true, 0, 31000), "does not probe when capture already keeps own windows out") ||
        !Require(policy.ShouldProbeDuplication(true, false, false, 0, 31000), "probes once 30 s backoff elapses") ||
        !Require(!policy.ShouldProbeDuplication(true, false, false, 0, 90999), "failed probe arms 60 s backoff") ||
        !Require(policy.ShouldProbeDuplication(true, false, false, 0, 91000), "probes again after 60 s backoff")) {
        return false;
    }

    policy.OnDuplicationRecovered();
    return Require(policy.ChooseInitialSource(false, 61000) == CapturePolicy::InitialSource::Duplication,
                   "recovery restores Desktop Duplication on next session");
}

bool CheckBlackPollAndThresholds()
{
    CapturePolicy policy;
    if (!Require(!policy.PollBlackBackdrop(true, 0.f, 1000), "first dark frame starts the 1500 ms timer") ||
        !Require(!policy.PollBlackBackdrop(true, 0.01f, 2500), "exact 1500 ms and 0.01 luma boundary stays below threshold") ||
        !Require(policy.PollBlackBackdrop(true, 0.f, 2501), "sustained dark backdrop triggers ScreenLitAbove check beyond 1500 ms")) {
        return false;
    }

    // A genuinely dark screen never calls OnBlackOverLitScreen and must wait another 1500 ms.
    if (!Require(!policy.PollBlackBackdrop(true, 0.f, 2516), "next frame re-arms dark timer after a true dark screen") ||
        !Require(!policy.PollBlackBackdrop(true, 0.f, 4016), "second dark window holds through 1500 ms") ||
        !Require(policy.PollBlackBackdrop(true, 0.f, 4017), "second dark window triggers only after another 1500 ms") ||
        !Require(policy.ChooseInitialSource(false, 4017) == CapturePolicy::InitialSource::Duplication, "true dark screens never escalate capture fallback")) {
        return false;
    }

    // Bright backdrop or ineligibility resets the dark-duration timer.
    if (!Require(!policy.PollBlackBackdrop(true, 0.f, 5000), "dark timer starts") ||
        !Require(!policy.PollBlackBackdrop(true, 0.02f, 6400), "bright backdrop cancels pending dark timer") ||
        !Require(!policy.PollBlackBackdrop(true, 0.f, 6450), "dark timer restarts cleanly") ||
        !Require(!policy.PollBlackBackdrop(false, 0.f, 7900), "ineligible state cancels pending dark timer") ||
        !Require(!policy.PollBlackBackdrop(true, 0.f, 7950), "re-arm after ineligible state") ||
        !Require(!policy.PollBlackBackdrop(true, 0.f, 9450), "1500 ms boundary after re-arm") ||
        !Require(policy.PollBlackBackdrop(true, 0.f, 9451), "triggers 1501 ms after re-arm")) {
        return false;
    }

    // One lit-screen strike clears once the backdrop stays lit for more than 30 s.
    if (!Require(policy.OnBlackOverLitScreen(1, 10000) == CapturePolicy::BlackAction::RestartCapture, "first black-over-lit strike restarts capture") ||
        !Require(!policy.PollBlackBackdrop(true, 0.5f, 40000), "exactly 30 s lit does not clear restart count") ||
        !Require(!policy.PollBlackBackdrop(true, 0.5f, 40001), "more than 30 s lit clears restart count") ||
        !Require(policy.OnBlackOverLitScreen(1, 42000) == CapturePolicy::BlackAction::RestartCapture, "cleared count restarts capture instead of escalating")) {
        return false;
    }
    return true;
}

bool CheckEscalationAndRecovery()
{
    CapturePolicy policy;
    policy.BeginSession();
    if (!Require(!policy.PollBlackBackdrop(true, 0.f, 1000), "starts dark timer before session switch") ||
        !Require(policy.OnBlackOverLitScreen(1, 2600) == CapturePolicy::BlackAction::RestartCapture, "first strike restarts Desktop Duplication")) {
        return false;
    }

    policy.BeginSession();
    if (!Require(!policy.PollBlackBackdrop(true, 0.f, 2700), "BeginSession cleared any prior dark timer") ||
        !Require(!policy.PollBlackBackdrop(true, 0.f, 4200), "waits full 1500 ms in the new session") ||
        !Require(policy.PollBlackBackdrop(true, 0.f, 4201), "detects sustained black in second session") ||
        !Require(policy.OnBlackOverLitScreen(1, 4201) == CapturePolicy::BlackAction::SwitchToMonitorCapture,
                 "second strike switches to MonitorCapture and pins black generation")) {
        return false;
    }

    if (!Require(policy.ChooseInitialSource(false, 4201) == CapturePolicy::InitialSource::MonitorCapture, "next session starts on MonitorCapture") ||
        !Require(!policy.ShouldProbeDuplication(true, false, false, 1, 100000), "same display generation never probes Desktop Duplication again") ||
        !Require(!policy.ShouldProbeDuplication(true, false, false, 2, 34200), "new display generation still respects 30 s retry backoff") ||
        !Require(policy.ShouldProbeDuplication(true, false, false, 2, 34201), "new display generation probes once 30 s elapses")) {
        return false;
    }

    // Two black-over-lit strikes on MonitorCapture escalate to 30 s wallpaper blackout.
    if (!Require(policy.OnBlackOverLitScreen(2, 35000) == CapturePolicy::BlackAction::RestartCapture, "first MonitorCapture black strike restarts capture") ||
        !Require(policy.OnBlackOverLitScreen(2, 37000) == CapturePolicy::BlackAction::FallbackToWallpaper,
                 "second MonitorCapture black strike falls back to wallpaper for 30 s") ||
        !Require(policy.ChooseInitialSource(false, 37000) == CapturePolicy::InitialSource::Wallpaper, "blackout session selects wallpaper") ||
        !Require(!policy.ShouldExitBlackoutWallpaper(true, 66999), "wallpaper blackout holds until 30 s") ||
        !Require(policy.ShouldExitBlackoutWallpaper(true, 67000), "wallpaper blackout exits at 30 s") ||
        !Require(policy.ChooseInitialSource(false, 67000) == CapturePolicy::InitialSource::MonitorCapture,
                 "post-wallpaper session returns to MonitorCapture") ||
        !Require(policy.ShouldProbeDuplication(true, false, false, 2, 67000), "MonitorCapture can probe Desktop Duplication on the new display generation")) {
        return false;
    }

    policy.OnDuplicationRecovered();
    policy.OnDuplicationUnavailable(68000);
    return Require(policy.ChooseInitialSource(false, 68000) == CapturePolicy::InitialSource::MonitorCapture,
                   "recovered policy can still fall back cleanly on subsequent unavailability") &&
           Require(policy.ShouldProbeDuplication(true, false, false, 2, 98000),
                   "recovery cleared prior black generation so temporary unavailability can retry on generation 2");
}

bool CheckElsewhereSuppressionAndSwitch()
{
    constexpr uintptr_t kMonA = 0x1001;
    constexpr uintptr_t kMonB = 0x2002;
    constexpr uint64_t kHourMs = 3600ULL * 1000ULL;

    CapturePolicy policy;
    policy.BeginSession(kMonA, 1);
    policy.OnDuplicationUnavailable(1000, CapturePolicy::DuplicationFailure::Elsewhere, kMonA, 1);

    if (!Require(policy.ChooseInitialSource(false, 1000) == CapturePolicy::InitialSource::MonitorCapture, "Elsewhere failure falls back to MonitorCapture") ||
        !Require(!policy.ShouldProbeDuplication(true, false, false, kMonA, 1, 31000), "Elsewhere suppresses probe after 30 s on same monitor and generation") ||
        !Require(!policy.ShouldProbeDuplication(true, false, false, kMonA, 1, 1000 + kHourMs),
                 "Elsewhere suppresses probe after 1 hour on same monitor and generation") ||
        !Require(!policy.ShouldProbeDuplication(true, false, false, kMonA, 1, 1000 + 24 * kHourMs),
                 "Elsewhere suppresses probe after 24 hours on same monitor and generation")) {
        return false;
    }

    // Same monitor and generation on a new session stays on MonitorCapture.
    policy.BeginSession(kMonA, 1);
    if (!Require(policy.ChooseInitialSource(false, 1000 + 24 * kHourMs) == CapturePolicy::InitialSource::MonitorCapture,
                 "Elsewhere persists across sessions on the same monitor and generation")) {
        return false;
    }

    // Follow Mode moves to a different monitor in the same display generation.
    policy.BeginSession(kMonB, 1);
    if (!Require(policy.ChooseInitialSource(false, 2000 + 24 * kHourMs) == CapturePolicy::InitialSource::Duplication,
                 "moving to another monitor immediately retries Duplication")) {
        return false;
    }

    // Elsewhere on monitor A clears when display topology generation changes.
    policy.BeginSession(kMonA, 1);
    policy.OnDuplicationUnavailable(5000, CapturePolicy::DuplicationFailure::Elsewhere, kMonA, 1);
    policy.BeginSession(kMonA, 2);
    if (!Require(policy.ChooseInitialSource(false, 6000) == CapturePolicy::InitialSource::Duplication,
                 "display topology change immediately retries Duplication")) {
        return false;
    }

    // If MonitorCapture hits black-over-lit while Elsewhere is set on gen 1, keep pinned on gen 1 across monitor switch.
    policy.BeginSession(kMonA, 1);
    policy.OnDuplicationUnavailable(7000, CapturePolicy::DuplicationFailure::Elsewhere, kMonA, 1);
    return Require(policy.OnBlackOverLitScreen(1, 8000) == CapturePolicy::BlackAction::RestartCapture,
                   "first black strike on Elsewhere MonitorCapture restarts capture") &&
           Require(policy.OnBlackOverLitScreen(1, 9000) == CapturePolicy::BlackAction::FallbackToWallpaper,
                   "second black strike on Elsewhere MonitorCapture falls back to wallpaper") &&
           Require(!policy.ShouldProbeDuplication(true, false, false, kMonA, 1, 100000), "Elsewhere MonitorCapture still suppresses Duplication probes");
}

bool CheckTransientBackoffProgression()
{
    constexpr uintptr_t kMonA = 0x1001;
    CapturePolicy policy;
    policy.BeginSession(kMonA, 1);
    policy.OnDuplicationUnavailable(1000, CapturePolicy::DuplicationFailure::Transient, kMonA, 1);

    uint64_t t = 1000;
    const uint64_t expectedSteps[] = {30000, 60000, 120000, 240000, 300000, 300000};
    for (uint64_t step : expectedSteps) {
        if (!Require(!policy.ShouldProbeDuplication(true, false, false, kMonA, 1, t + step - 1), "does not probe before exponential backoff expires") ||
            !Require(policy.ShouldProbeDuplication(true, false, false, kMonA, 1, t + step), "probes once exponential backoff expires")) {
            return false;
        }
        t += step;
    }

    policy.OnDuplicationRecovered();
    policy.OnDuplicationUnavailable(t + 1000, CapturePolicy::DuplicationFailure::Transient, kMonA, 1);
    return Require(!policy.ShouldProbeDuplication(true, false, false, kMonA, 1, t + 1000 + 29999), "recovery reset backoff to 30 s (before boundary)") &&
           Require(policy.ShouldProbeDuplication(true, false, false, kMonA, 1, t + 1000 + 30000), "recovery reset backoff to 30 s (at boundary)");
}

bool CheckResetBlackTimer()
{
    CapturePolicy policy;
    if (!Require(!policy.PollBlackBackdrop(true, 0.f, 1000), "arms black timer before hide") ||
        !Require(!policy.PollBlackBackdrop(true, 0.f, 2400), "stays below 1500 ms before hide")) {
        return false;
    }

    policy.ResetBlackTimer();
    return Require(!policy.PollBlackBackdrop(true, 0.f, 2600), "ResetBlackTimer clears prior armed timer across hide/show") &&
           Require(!policy.PollBlackBackdrop(true, 0.f, 4100), "requires full 1500 ms after re-arm on show") &&
           Require(policy.PollBlackBackdrop(true, 0.f, 4101), "triggers 1501 ms after re-arm on show");
}

} // namespace

int main()
{
    if (!CheckInitialSelection() || !CheckBlackPollAndThresholds() || !CheckEscalationAndRecovery() || !CheckElsewhereSuppressionAndSwitch() ||
        !CheckTransientBackoffProgression() || !CheckResetBlackTimer()) {
        return 1;
    }
    std::cout << "capture policy: all checks passed\n";
    return 0;
}
