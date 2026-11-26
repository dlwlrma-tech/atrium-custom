// The tray host's callback protocol (shell/tray_protocol.h): the notifications the
// shell sends for each action and how they are packed, per NOTIFYICON_VERSION.
#include "shell/tray_host.h"
#include "shell/tray_protocol.h"

#include <cstdio>
#include <vector>

using namespace atrium::tray;

namespace {
int g_failures = 0;
void Check(bool ok, const char* what)
{
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

void TestYieldPolicy()
{
    using atrium::TrayHostYieldPolicy;
    using atrium::TrayHostYieldReason;

    // 1. OtherProcess yields while another tray host is first, and recovers immediately once Explorer is first again.
    {
        TrayHostYieldPolicy p;
        Check(!p.OnShellPid(100, false), "initial shell pid does not report prior yield");
        Check(p.OnOtherProcess() && p.Yielded() && p.Reason() == TrayHostYieldReason::OtherProcess, "other process yields");
        Check(!p.OnOtherProcess() && p.Yielded(), "repeat other process does not re-notify");

        bool newlyYielded = true;
        Check(p.OnExplorerFirst(1000, newlyYielded) && !newlyYielded && !p.Yielded() && p.Reason() == TrayHostYieldReason::None,
              "explorer first after other process exits clears yield and allows reclaim");
        Check(!p.OnFirst() && !p.Yielded() && p.rapidLosses == 0, "OnFirst resets rapidLosses after reclaim");
    }

    // 2. RapidLoss trips on the 6th loss within 5s, stays yielded < 60s, retries >= 60s, and resets on OnFirst().
    {
        TrayHostYieldPolicy p;
        p.OnShellPid(100, false);
        bool newlyYielded = false;

        // Losses spaced > 5s apart do not accumulate.
        Check(p.OnExplorerFirst(1000, newlyYielded) && !newlyYielded, "loss 1");
        Check(p.OnExplorerFirst(7000, newlyYielded) && !newlyYielded && p.rapidLosses == 1, "loss after >5s resets window");

        // Mid-burst OnFirst() resets rapidLosses.
        for (int i = 0; i < 4; ++i)
            Check(p.OnExplorerFirst(7100 + ULONGLONG(i) * 100, newlyYielded) && !newlyYielded, "burst before OnFirst");
        Check(p.rapidLosses == 5 && !p.Yielded(), "5 losses within 5s do not yield yet");
        Check(!p.OnFirst() && p.rapidLosses == 0, "OnFirst resets rapidLosses mid-burst");

        // 6 losses within 5s trips RapidLoss.
        for (int i = 0; i < 5; ++i)
            Check(p.OnExplorerFirst(10000 + ULONGLONG(i) * 500, newlyYielded) && !newlyYielded && !p.Yielded(), "losses 1..5 within 5s allow reclaim");
        Check(!p.OnExplorerFirst(15000, newlyYielded) && newlyYielded && p.Yielded() && p.Reason() == TrayHostYieldReason::RapidLoss,
              "6th loss within 5s trips RapidLoss");
        Check(!p.OnExplorerFirst(74999, newlyYielded) && !newlyYielded && p.Yielded() && p.Reason() == TrayHostYieldReason::RapidLoss,
              "stays yielded before 60s backoff expires");
        Check(p.OnExplorerFirst(75000, newlyYielded) && !newlyYielded && !p.Yielded() && p.Reason() == TrayHostYieldReason::None,
              "retries and clears yield once 60s backoff expires");
        Check(!p.OnFirst() && p.rapidLosses == 0, "OnFirst resets count after backoff recovery");
    }

    // 3. StartAllBack stays permanently yielded across > 60s, clearing only on a new shellPid without StartAllBack.
    {
        TrayHostYieldPolicy p;
        Check(!p.OnShellPid(200, true), "StartAllBack shell pid set");
        bool newlyYielded = false;
        Check(!p.OnExplorerFirst(1000, newlyYielded) && newlyYielded && p.Yielded() && p.Reason() == TrayHostYieldReason::StartAllBack,
              "StartAllBack yields on first Explorer check");
        Check(!p.OnExplorerFirst(100000, newlyYielded) && !newlyYielded && p.Yielded() && p.Reason() == TrayHostYieldReason::StartAllBack,
              "StartAllBack stays yielded past 60s");
        Check(!p.OnOtherProcess() && p.Reason() == TrayHostYieldReason::StartAllBack, "StartAllBack reason preserved across OnOtherProcess");
        Check(!p.OnShellPid(200, false) && p.Yielded(), "same shell pid keeps StartAllBack yield");
        Check(p.OnShellPid(300, false) && !p.Yielded() && p.Reason() == TrayHostYieldReason::None, "new shell pid without StartAllBack clears yield");
        Check(p.OnExplorerFirst(101000, newlyYielded) && !newlyYielded && !p.Yielded(), "reclaim allowed after shell restart");
    }
}

void TestDecideHostForwardResult()
{
    Check(DecideHostForwardResult(NIM_ADD, true, TRUE, false, false) == TRUE, "accepted forward is preserved");
    Check(DecideHostForwardResult(NIM_MODIFY, true, 42, false, false) == 42, "non-zero forward result is preserved");
    Check(DecideHostForwardResult(NIM_ADD, false, 0, false, false) == TRUE, "missing shell accepts NIM_ADD");
    Check(DecideHostForwardResult(NIM_DELETE, false, 0, false, false) == 0, "missing shell does not turn unknown NIM_DELETE into TRUE");
    Check(DecideHostForwardResult(NIM_DELETE, false, 0, true, false) == TRUE, "missing shell accepts NIM_DELETE for known icon");

    Check(DecideHostForwardResult(NIM_ADD, true, 0, false, true) == TRUE, "refused NIM_ADD from valid live window is accepted");
    Check(DecideHostForwardResult(NIM_ADD, true, 0, false, false) == 0, "refused NIM_ADD from invalid/shell window stays 0");
    Check(DecideHostForwardResult(NIM_MODIFY, true, 0, true, true) == TRUE, "refused NIM_MODIFY for known icon is accepted");
    Check(DecideHostForwardResult(NIM_MODIFY, true, 0, false, true) == 0, "refused NIM_MODIFY for unknown key stays 0 for probing");
    Check(DecideHostForwardResult(NIM_SETVERSION, true, 0, true, true) == TRUE, "refused NIM_SETVERSION for known icon is accepted");
    Check(DecideHostForwardResult(NIM_SETVERSION, true, 0, false, true) == 0, "refused NIM_SETVERSION for unknown key stays 0");
    Check(DecideHostForwardResult(NIM_DELETE, true, 0, true, true) == TRUE, "refused NIM_DELETE for known icon is accepted");
    Check(DecideHostForwardResult(NIM_DELETE, true, 0, false, true) == 0, "refused NIM_DELETE for unknown icon stays 0");
}

void TestIconFlash()
{
    constexpr uint64_t kPicA = 0x1111, kPicB = 0x2222, kPicC = 0x3333;

    // One-off icon update and repeated identical updates do not flash.
    {
        IconFlashState s;
        Check(!RecordIconPicture(s, 1000, kPicA), "initial picture is not a flash");
        Check(!RecordIconPicture(s, 2000, kPicA), "same picture again is not a flash");
        Check(!RecordIconPicture(s, 3000, kPicA), "repeated identical picture is not a flash");
        Check(!RecordIconPicture(s, 10000, kPicB), "one-off icon update is not a flash");
        Check(!RecordIconPicture(s, 10500, kPicC), "two updates alone are not a flash");
    }

    // 1000 +/- 30 ms jittered alternation: detected within ~2.2 s, never drops while ongoing, ends within ~4.5 s.
    {
        IconFlashState s;
        const int jitter[] = {+30, -30, +25, -20, +30, +30, -30, +15, -25, +30};
        ULONGLONG t = 10000;
        ULONGLONG detectedAt = 0;
        for (int i = 0; i < 60; ++i) {
            const uint64_t pic = (i % 2 == 0) ? kPicA : kPicB;
            if (detectedAt != 0)
                Check(s.until > t, "ongoing 1 Hz alternation never drops between updates");
            const bool flashing = RecordIconPicture(s, t, pic);
            if (flashing && detectedAt == 0)
                detectedAt = t;
            t += ULONGLONG(1000 + jitter[size_t(i) % std::size(jitter)]);
        }
        Check(detectedAt != 0 && detectedAt - 10000 <= 2200, "1000 +/- 30 ms alternation is detected within 2.2 s");
        Check(s.until > t - 1030 && s.until <= t - 970 + 4500, "flash ends within ~4.5 s after stopping");
        Check(s.until <= (t - 970) + 4500 && !(s.until > (t - 970) + 4500), "flash is off by +4.5 s after last change");
    }
}

void TestResolveHostIconRect()
{
    const RECT clicked{100, 200, 140, 240};
    const RECT hovered{-400, 100, -360, 140};
    const RECT anchor{1800, 1390, 1830, 1430};
    const RECT empty{};
    auto eq = [](RECT a, RECT b) { return EqualRect(&a, &b) != FALSE; };

    const RECT unknown = ResolveHostIconRect(L"", L"guid:a", clicked, L"guid:b", hovered, anchor);
    Check(IsRectEmpty(&unknown), "unknown icon returns empty rect for Explorer forwarding");
    Check(eq(ResolveHostIconRect(L"guid:a", L"guid:a", clicked, L"guid:b", hovered, anchor), clicked), "clicked icon gets its clicked cell rect");
    Check(eq(ResolveHostIconRect(L"guid:a", L"guid:a", clicked, L"guid:a", hovered, anchor), clicked), "clicked cell takes priority over hovered cell");
    Check(eq(ResolveHostIconRect(L"guid:a", L"guid:a", empty, L"guid:a", hovered, anchor), hovered), "empty clicked rect falls back to hovered cell");
    Check(eq(ResolveHostIconRect(L"guid:b", L"guid:a", clicked, L"guid:b", hovered, anchor), hovered),
          "hovered icon gets its hovered cell rect including negative monitor coords");
    Check(eq(ResolveHostIconRect(L"guid:c", L"guid:a", clicked, L"guid:b", hovered, anchor), anchor), "other known icon falls back to bar anchor");
}
} // namespace

int main()
{
    using V = std::vector<UINT>;
    Check(CallbackSequence(HostAction::Left, 0) == V{WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP}, "v0 left: no NIN_SELECT");
    Check(CallbackSequence(HostAction::Left, 3) == V{WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP, NIN_SELECT}, "v3 left ends in NIN_SELECT");
    Check(CallbackSequence(HostAction::Left, 4) == V{WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP, NIN_SELECT}, "v4 left ends in NIN_SELECT");
    Check(CallbackSequence(HostAction::Right, 0) == V{WM_MOUSEMOVE, WM_RBUTTONDOWN, WM_RBUTTONUP}, "v0 right: no WM_CONTEXTMENU");
    Check(CallbackSequence(HostAction::Right, 4) == V{WM_MOUSEMOVE, WM_RBUTTONDOWN, WM_RBUTTONUP, WM_CONTEXTMENU}, "v4 right ends in WM_CONTEXTMENU");
    Check(CallbackSequence(HostAction::Middle, 4) == V{WM_MOUSEMOVE, WM_MBUTTONDOWN, WM_MBUTTONUP}, "middle");
    Check(CallbackSequence(HostAction::DoubleContinuation, 4) == V{WM_LBUTTONDBLCLK, WM_LBUTTONUP},
          "the second click of a double click never repeats the first");
    Check(CallbackSequence(HostAction::Double, 4) == V{WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP, NIN_SELECT, WM_LBUTTONDBLCLK, WM_LBUTTONUP},
          "a whole double click is a click and its continuation");
    Check(CallbackSequence(HostAction::PopupOpen, 3).empty() && CallbackSequence(HostAction::PopupClose, 0).empty(), "popup requests are version 4 only");
    Check(CallbackSequence(HostAction::PopupOpen, 4) == V{NIN_POPUPOPEN} && CallbackSequence(HostAction::PopupClose, 4) == V{NIN_POPUPCLOSE},
          "v4 popup requests");
    Check(CallbackSequence(HostAction::HoverIn, 0) == V{WM_MOUSEMOVE}, "hover is a mouse move");
    Check(CallbackSequence(HostAction::Open, 0) == V{WM_LBUTTONDOWN, WM_LBUTTONUP} &&
              CallbackSequence(HostAction::Open, 4) == V{WM_LBUTTONDOWN, WM_LBUTTONUP, NIN_SELECT},
          "opening from the bar is a click without the pointer's arrival");

    WPARAM w = 0;
    LPARAM l = 0;
    EncodeCallback(0, 7, WM_LBUTTONUP, POINT{100, 200}, w, l);
    Check(w == 7 && l == WM_LBUTTONUP, "v0: the id in wParam, the notification in lParam");
    EncodeCallback(4, 7, WM_CONTEXTMENU, POINT{-1200, 2000}, w, l);
    Check(LOWORD(l) == WM_CONTEXTMENU && HIWORD(l) == 7, "v4: the notification and the id in lParam");
    Check(short(LOWORD(w)) == -1200 && short(HIWORD(w)) == 2000, "v4: the pointer in wParam, negative monitors too");

    TestYieldPolicy();
    TestDecideHostForwardResult();
    TestIconFlash();
    TestResolveHostIconRect();

    if (!g_failures)
        std::printf("tray protocol: all checks passed\n");
    return g_failures ? 1 : 0;
}
