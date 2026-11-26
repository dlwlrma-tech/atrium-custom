#include "ui/ime_bubble_model.h"

#include <algorithm>
#include <cmath>
#include <iostream>

using namespace atrium;

// The bubble's melt rides in the paint's own parts budget: one shape carries at
// most six parts (ime_bubble.cpp publishes the skin, and every group of drops,
// as such a shape), so the model must never ask for more.
static_assert(ImeBubbleModel::kMaxMeltParts == 6, "the bubble's melt fits the paint's parts");

namespace {

struct Runner {
    int failures = 0;
    const char* current = "";
    bool caseFailed = false;
};

void Require(Runner& r, bool condition, const char* message)
{
    if (condition || r.caseFailed)
        return;
    r.caseFailed = true;
    ++r.failures;
    std::cerr << "FAIL: " << r.current << ": " << message << '\n';
}

template<class Fn> void Run(Runner& r, const char* name, Fn&& test)
{
    r.current = name;
    r.caseFailed = false;
    test();
    if (!r.caseFailed)
        std::cout << "PASS: " << name << '\n';
}

void CheckFrameBounds(Runner& r, const ImeBubbleModel::Frame& f, bool checkLobes)
{
    const float rad = ImeBubbleModel::kRadius * f.scale;
    Require(r, std::isfinite(f.cx) && std::isfinite(f.cy) && std::isfinite(f.scale) && std::isfinite(f.opacity), "frame values are finite");
    Require(r, f.cy - rad >= 0 && f.cy + rad <= ImeBubbleModel::kH, "the skin stays in the window");
    Require(r, f.lobeCount <= ImeBubbleModel::kMaxFrameLobes, "a frame carries no more than it can draw");
    if (checkLobes) {
        for (int i = 0; i < f.lobeCount; ++i) {
            const auto& lobe = f.lobes[i];
            Require(r, std::isfinite(lobe.x) && std::isfinite(lobe.y) && std::isfinite(lobe.r) && lobe.r > 0, "every newcomer drawn is a real bubble");
            Require(r, lobe.u >= 0 && lobe.u < 1, "each has its own place in its absorb");
            Require(r, lobe.y - lobe.r >= 0, "no newcomer ever leaves the window through the top");
            if (lobe.y + lobe.r <= ImeBubbleModel::kH)
                Require(r, lobe.x - lobe.r >= 0 && lobe.x + lobe.r <= ImeBubbleModel::kW && lobe.y - lobe.r >= 0, "a newcomer inside stays inside");
        }
    }
    for (int i = 0; i < f.beadCount; ++i) {
        const auto& bead = f.beads[i];
        Require(r, std::isfinite(bead.x) && std::isfinite(bead.y) && std::isfinite(bead.r) && bead.r >= 0, "every bead is valid");
        Require(r, bead.x - bead.r >= 0 && bead.x + bead.r <= ImeBubbleModel::kW && bead.y - bead.r >= 0 && bead.y + bead.r <= ImeBubbleModel::kH,
                "the burst stays inside the window");
    }
}

ImeBubbleModel MakeMerged(double start, int count, double firstFuse, double step)
{
    ImeBubbleModel bubble;
    bubble.Begin(start);
    for (int i = 0; i < count; ++i)
        bubble.Fuse(firstFuse + i * step);
    return bubble;
}

void TestLifecycle(Runner& r)
{
    Run(r, "the bubble swells in from below with a springy overshoot", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        const auto emerging = bubble.Step(1030);
        Require(r, emerging.active && emerging.animating, "the announce is running");
        Require(r, emerging.opacity > 0 && emerging.opacity < 1, "the bubble fades in");
        Require(r, emerging.cy > ImeBubbleModel::kCyRest && emerging.scale < 1, "it starts below its rest, small");
        Require(r, bubble.Step(1000 + 300).scale > 1, "the emerge passes its end by a little before settling");
    });
    Run(r, "the float drifts up gently while fully visible", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        const auto rested = bubble.Step(1000 + ImeBubbleModel::kEmergeMs + 1);
        Require(r, std::abs(rested.cy - ImeBubbleModel::kCyRest) < 0.5f, "the emerge ends at the resting place");
        Require(r, rested.opacity == 1 && std::abs(rested.scale - 1) < 0.02f, "a settled bubble is whole");
        const auto floated = bubble.Step(1000 + ImeBubbleModel::kPopAt - 1);
        Require(r, floated.cy < ImeBubbleModel::kCyRest - 5 && floated.beadCount == 0, "the float rises without bursting early");
    });
    Run(r, "the pop tears the skin into foam and mist", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        const auto burst = bubble.Step(1000 + ImeBubbleModel::kPopAt + ImeBubbleModel::kSwellMs + 10);
        Require(r, burst.beadCount >= ImeBubbleModel::kMinBeads && burst.beadCount <= ImeBubbleModel::kMaxBeads, "every pop has its own number of drops");
        Require(r, burst.foamCount >= 5 && burst.foamCount < burst.beadCount, "drops lead, mist follows");
        Require(r, burst.beadOpacity > 0.9f && burst.opacity < 1 && burst.opacity > 0 && burst.scale > 1.15f, "skin leaves as foam starts");
        for (int i = 0; i < burst.beadCount; ++i)
            Require(r, burst.beads[i].r > 0, "every drop of the foam is a real one");
        const auto flying = bubble.Step(1000 + ImeBubbleModel::kPopAt + ImeBubbleModel::kSwellMs + 40);
        const float by = ImeBubbleModel::kCyRest - ImeBubbleModel::kFloatRise;
        int above = 0;
        for (int i = 0; i < flying.beadCount; ++i)
            above += flying.beads[i].y < by - 1;
        Require(r, above >= 3, "the foam scatters upwards, like a bubble breaking");
    });
    Run(r, "every pop of its own pattern", [&] {
        ImeBubbleModel first, second;
        first.Begin(1000);
        second.Begin(3100);
        const auto a = first.Step(1000 + ImeBubbleModel::kPopAt + ImeBubbleModel::kSwellMs + 60);
        const auto b = second.Step(3100 + ImeBubbleModel::kPopAt + ImeBubbleModel::kSwellMs + 60);
        bool same = a.beadCount == b.beadCount && a.foamCount == b.foamCount;
        for (int i = 0; same && i < a.beadCount; ++i)
            same = std::abs(a.beads[i].x - b.beads[i].x) < 0.5f && std::abs(a.beads[i].y - b.beads[i].y) < 0.5f;
        Require(r, !same, "two pops cannot share one frozen pattern");
    });
    Run(r, "the whole life stays inside the small fixed window", [&] {
        for (int seed = 0; seed < 3; ++seed) {
            const double begin = 1000 + seed * 7919;
            ImeBubbleModel bubble;
            bubble.Begin(begin);
            for (int ms = 8; begin + ms < bubble.EndMs(); ms += 8) {
                const auto f = bubble.Step(begin + ms);
                Require(r, f.active && f.animating && f.opacity >= 0 && f.opacity <= 1 && f.scale >= 0.4f && f.scale <= 1.7f,
                        "the bubble stays visible and bounded");
                CheckFrameBounds(r, f, false);
            }
        }
    });
    Run(r, "the announce ends bounded, and a restart starts clean", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        Require(r, !bubble.Step(1000 + ImeBubbleModel::kPopAt + ImeBubbleModel::kSwellMs + ImeBubbleModel::kBurstMs + 1).active,
                "the bubble is done after its total");
        Require(r, !bubble.Step(1000 + ImeBubbleModel::kPopAt + ImeBubbleModel::kSwellMs + ImeBubbleModel::kBurstMs + 500).animating,
                "it never animates again");
        bubble.Begin(3000);
        const auto fresh = bubble.Step(3050);
        Require(r, fresh.active && fresh.opacity < 1 && fresh.cy > ImeBubbleModel::kCyRest, "a restarted bubble emerges like the first");
        bubble.Cancel();
        Require(r, !bubble.Step(3100).active, "cancel removes it at once");
    });
    Run(r, "a broken clock never starts, merges or advances a bubble", [&] {
        const double nan = std::nan("");
        const double inf = INFINITY;
        ImeBubbleModel bubble;
        bubble.Begin(nan);
        Require(r, !bubble.Active() && !bubble.Step(1000).active, "a broken start never begins");
        bubble.Begin(inf);
        Require(r, !bubble.Active(), "and neither does an infinite one");
        bubble.Begin(0);
        Require(r, !bubble.Active() && !bubble.Mergeable(1) && !bubble.Fuse(1), "time nought is no clock either");
        bubble.Begin(1000);
        Require(r, !bubble.Fuse(nan) && !bubble.Fuse(inf) && bubble.Fusions() == 1, "a broken merge time changes nothing");
        Require(r, !bubble.Step(nan).active && !bubble.Step(inf).active, "a broken frame time draws nothing");
        Require(r, bubble.Step(1100).active, "while the real clock still runs it");
    });
}

void TestDeterminismAndConstants(Runner& r)
{
    Run(r, "a late frame lands where a punctual one would", [&] {
        ImeBubbleModel slow, jumped;
        slow.Begin(1000);
        jumped.Begin(1000);
        const int at = 1000 + 617;
        for (int t = 1008; t < at; t += 8)
            slow.Step(double(t));
        const auto a = slow.Step(double(at)), b = jumped.Step(double(at));
        Require(r,
                std::abs(a.cy - b.cy) < 0.01f && std::abs(a.scale - b.scale) < 0.01f && std::abs(a.opacity - b.opacity) < 0.01f && a.beadCount == b.beadCount,
                "the frame is a pure function of time");
        for (int i = 0; i < a.beadCount; ++i)
            Require(r, std::abs(a.beads[i].x - b.beads[i].x) < 0.01f && std::abs(a.beads[i].y - b.beads[i].y) < 0.01f,
                    "the drawn burst pattern is the same for late and punctual frames");
    });
    Run(r, "a late frame lands where a punctual one would, even mid-merge", [&] {
        ImeBubbleModel slow, jumped;
        slow.Begin(1000);
        jumped.Begin(1000);
        slow.Fuse(1200);
        jumped.Fuse(1200);
        slow.Fuse(1300);
        jumped.Fuse(1300);
        const int at = 1700;
        for (int t = 1008; t < at; t += 8)
            slow.Step(double(t));
        const auto a = slow.Step(double(at)), b = jumped.Step(double(at));
        Require(r, a.lobeCount == 2 && b.lobeCount == 2, "both rising newcomers are in the frame");
        for (int i = 0; i < a.lobeCount; ++i)
            Require(r,
                    std::abs(a.lobes[i].x - b.lobes[i].x) < 0.01f && std::abs(a.lobes[i].y - b.lobes[i].y) < 0.01f &&
                        std::abs(a.lobes[i].r - b.lobes[i].r) < 0.01f && std::abs(a.lobes[i].u - b.lobes[i].u) < 0.0001f,
                    "each rising newcomer is the same");
        Require(r, std::abs(a.grow - b.grow) < 0.01f && std::abs(a.wob - b.wob) < 0.01f && std::abs(a.scale - b.scale) < 0.01f && a.beadCount == b.beadCount,
                "size, wobble and drops are the same");
        for (int i = 0; i < a.beadCount; ++i)
            Require(r, std::abs(a.beads[i].x - b.beads[i].x) < 0.01f && std::abs(a.beads[i].y - b.beads[i].y) < 0.01f, "any drawn drops are the same");
    });
    Run(r, "merges grow the bubble logarithmically and stop at ten", [&] {
        const ImeBubbleModel defaults{};
        Require(r, std::abs(defaults.ScaleAt(1) - 1.f) < 1e-6f && std::abs(defaults.RadiusAt(1) - ImeBubbleModel::kRadius) < 1e-6f,
                "one bubble is the nominal size");
        float last = defaults.ScaleAt(1);
        for (int x = 2; x <= ImeBubbleModel::kMaxFusions; ++x) {
            const float s = defaults.ScaleAt(x);
            Require(r, s > last, "every merge is larger than the last");
            last = s;
        }
        Require(r,
                std::abs(defaults.RadiusAt(ImeBubbleModel::kMaxFusions) - 51) < 1e-4f &&
                    std::abs(defaults.ScaleAt(ImeBubbleModel::kMaxFusions) - 51.f / ImeBubbleModel::kRadius) < 1e-5f,
                "ten merges reach 51/32 of the base");
        Require(r, defaults.ScaleAt(14) == defaults.ScaleAt(ImeBubbleModel::kMaxFusions) && defaults.ScaleAt(0) == defaults.ScaleAt(1),
                "the count clamps at [1, 10]");
        Require(r,
                defaults.ScaleAt(2) - defaults.ScaleAt(1) > defaults.ScaleAt(ImeBubbleModel::kMaxFusions) - defaults.ScaleAt(ImeBubbleModel::kMaxFusions - 1),
                "the growth tapers, as a logarithmic size must");
    });
    Run(r, "the constants keep the old sizes, and the letter rides the frame's scale", [&] {
        static_assert(ImeBubbleModel::kStartRadius == 32.f && ImeBubbleModel::kEndRadius == 51.f, "the original sizes");
        static_assert(ImeBubbleModel::kStartRadius == ImeBubbleModel::kRadius, "the first bubble is the nominal one");
        static_assert(ImeBubbleModel::kEndRadius > ImeBubbleModel::kStartRadius, "a bubble grows or stays, never shrinks");
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        Require(r, std::abs(bubble.Step(1000 + ImeBubbleModel::kEmergeMs + 1).scale - 1.f) < 0.02f, "an unmerged frame rides the start size");
        for (int i = 0; i < ImeBubbleModel::kMaxFusions; ++i)
            Require(r, bubble.Fuse(1150 + i * 120.0), "every switch merges");
        Require(r, std::abs(bubble.Step(bubble.PopAt() - 1).grow - ImeBubbleModel::kEndRadius / ImeBubbleModel::kRadius) < 0.02f,
                "ten merges ride the end size");
    });
}

void TestFeelAndTimeline(Runner& r)
{
    Run(r, "the merge feel and the life are the model's own, and a restart keeps them", [&] {
        Require(r, std::abs(ImeBubbleModel::SoftBounce(196, ImeBubbleModel::kRebound) - 1.308f) < 0.01f, "the bounce matches the old one");
        Require(r, ImeBubbleModel::SoftBounce(900, ImeBubbleModel::kRebound) < 1.01f && ImeBubbleModel::SoftBounce(196, 0.05f) < 1.10f,
                "the bounce settles and scales with rebound");
        static_assert(ImeBubbleModel::kCompression == 0.10f && ImeBubbleModel::kRebound == 0.31f && ImeBubbleModel::kWobbleAmp == 0.875f &&
                          ImeBubbleModel::kWobbleHz == 4.3f && ImeBubbleModel::kLifeMs == 1060.f,
                      "the defaults are the original feel and life");
        ImeBubbleModel plain;
        plain.Begin(1000);
        Require(r, std::abs(plain.EndMs() - (1000 + 1060 + 46 + 440)) < 0.5, "the life is the old one");
        plain.Begin(2500);
        Require(r, std::abs(plain.EndMs() - (2500 + 1060 + 46 + 440)) < 0.5, "Begin keeps the life");
        plain.Cancel();
        plain.Begin(4000);
        Require(r, std::abs(plain.EndMs() - (4000 + 1060 + 46 + 440)) < 0.5, "Cancel keeps the life");

        ImeBubbleModel bubble;
        bubble.Begin(1000);
        Require(r, bubble.Fuse(1200), "it merges");
        const double hit = 1200 + bubble.TouchMs();
        const auto at = bubble.Step(hit);
        Require(r, std::abs(at.scale / at.grow - (1 - ImeBubbleModel::kCompression)) < 0.02f, "the compression is the constant's");
        Require(r, std::abs(bubble.Step(hit + 60).wob) <= 1.15f * ImeBubbleModel::kWobbleAmp, "and the wobble stays inside its own bound");
    });
    Run(r, "the birth dips and the newcomer's three acts are the model's", [&] {
        static_assert(ImeBubbleModel::kFirstBirthDip == 18 && ImeBubbleModel::kLobeBirthDip == 10 && ImeBubbleModel::kLobeRiseMs == 380 &&
                          ImeBubbleModel::kLobeAttachMs == 243 && ImeBubbleModel::kLobeSwallowMs == 137,
                      "the constants are the old birth points and acts");
        const ImeBubbleModel defaults{};
        Require(r,
                std::abs(defaults.RiseEndMs() - 380) < 1e-9 && std::abs(defaults.AbsorbMs() - 760) < 1e-9 && std::abs(defaults.AttachEndMs() - 623) < 1e-9 &&
                    std::abs(defaults.TouchMs() - 623) < 1e-9 && std::abs(defaults.TouchFrac() - 623.0 / 760.0) < 1e-9,
                "the derived timeline is the old one");
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        Require(r, std::abs(bubble.Step(1000).cy - (ImeBubbleModel::kCyRest + 18)) < 1e-4f, "the first bubble is born below its resting centre");
        Require(r, bubble.Fuse(1200), "it merges");
        const auto born = bubble.Step(1200);
        Require(r, std::abs(born.lobes[0].y - (ImeBubbleModel::kH + 10)) < 1e-4f, "the newcomer is born below the window's bottom edge");
        Require(r, std::abs(born.lobes[0].r - 0.30f * ImeBubbleModel::kStartRadius) < 0.01f, "it is born at the start of its own radius rhythm");
        const float rTouch = 0.55f * ImeBubbleModel::kStartRadius;
        Require(r, std::abs(bubble.Step(1200 + 379.5).lobes[0].r - 0.8f * rTouch) < 0.5f && bubble.Step(1200 + 622.5).lobes[0].r > 0.95f * rTouch,
                "rise and approach hit the rim radius");
        Require(r, std::abs(bubble.Step(1200 + 622).grow - 1.f) < 1e-4f && bubble.Step(1200 + 624).grow > 1.f, "size steps at the touch");
        Require(r,
                bubble.Step(1200 + 645).lobes[0].r < 0.65f * rTouch && bubble.Step(1200 + 684).lobes[0].r < 0.25f * rTouch &&
                    bubble.Step(1200 + 761).lobeCount == 0,
                "the swallow draws it in cleanly");
        bubble.Begin(2500);
        Require(r, std::abs(bubble.Step(2500).cy - (ImeBubbleModel::kCyRest + 18)) < 1e-4f, "Begin keeps the birth dip");
        bubble.Cancel();
        bubble.Begin(4000);
        Require(r, bubble.Fuse(4200) && std::abs(bubble.Step(4200).lobes[0].y - (ImeBubbleModel::kH + 10)) < 1e-4f,
                "a newcomer after restart keeps the birth dip");
    });
    Run(r, "the absorb's three acts stay ordered and its timeline stays finite", [&] {
        static_assert(ImeBubbleModel::kLobeRiseMs > 0 && ImeBubbleModel::kLobeAttachMs > 0 && ImeBubbleModel::kLobeSwallowMs > 0, "no act is zero");
        const ImeBubbleModel model{};
        Require(r, std::abs(model.AbsorbMs() - double(ImeBubbleModel::kLobeRiseMs + ImeBubbleModel::kLobeAttachMs + ImeBubbleModel::kLobeSwallowMs)) < 1e-9,
                "the absorb is exactly its three acts");
        Require(r,
                model.RiseEndMs() < model.TouchMs() && model.TouchMs() < model.AbsorbMs() && std::abs(model.AttachEndMs() - model.TouchMs()) < 1e-9 &&
                    model.TouchFrac() > 0 && model.TouchFrac() < 1 && std::abs(model.TouchFrac() - model.TouchMs() / model.AbsorbMs()) < 1e-12,
                "absorb milestones stay ordered");
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        Require(r, bubble.Fuse(1200), "it merges");
        double prevU = -1;
        for (int dt = 0; double(dt) < bubble.AbsorbMs(); dt += 8) {
            const auto f = bubble.Step(1200.0 + dt);
            Require(r, f.active && f.lobeCount == 1 && f.lobes[0].u > prevU, "its place advances");
            prevU = f.lobes[0].u;
            Require(r,
                    std::isfinite(f.lobes[0].x) && std::isfinite(f.lobes[0].y) && std::isfinite(f.lobes[0].r) && std::isfinite(f.cx) && std::isfinite(f.cy) &&
                        std::isfinite(f.scale) && std::isfinite(f.wob),
                    "every frame of it is finite");
        }
        Require(r, bubble.Step(1200 + bubble.AbsorbMs()).lobeCount == 0, "and is gone at the absorb's own end");
    });
}

void TestMergesAndFeelAnchor(Runner& r)
{
    Run(r, "the pop is one life out, and the burst follows it", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        Require(r, std::abs(bubble.PopAt() - (1000 + 1060)) < 0.5 && bubble.Mergeable(bubble.PopAt() - 5) && !bubble.Mergeable(bubble.PopAt() + 5),
                "the default life sets the pop");
        Require(r, std::abs(bubble.EndMs() - (bubble.PopAt() + 486)) < 0.5, "the swell and burst follow the pop");
        const double before = bubble.EndMs();
        Require(r, bubble.Fuse(bubble.PopAt() - 100) && bubble.EndMs() > before, "the newcomer is absorbed and the pop is pushed back");
    });
    Run(r, "a merge pushes the pop past its own life and its newcomer's swallow", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        const double fuseAt = bubble.PopAt() - 100;
        Require(r, bubble.Fuse(fuseAt), "a state just before the pop still merges");
        Require(r, std::abs(bubble.PopAt() - (fuseAt + ImeBubbleModel::kLifeMs)) < 0.5 && bubble.PopAt() >= fuseAt + bubble.AbsorbMs(),
                "pop moves past life and absorb");
        const auto lastFrame = bubble.Step(fuseAt + bubble.AbsorbMs() - 1);
        Require(r, lastFrame.lobeCount == 1 && lastFrame.lobes[0].r < 1.f, "its newcomer is all but swallowed when its absorb ends");
        const auto atPop = bubble.Step(bubble.PopAt() - 0.5);
        Require(r, atPop.active && atPop.lobeCount == 0 && !bubble.Mergeable(bubble.PopAt()), "swallowed long before burst");
    });
    Run(r, "a live bubble merges: soft, springy, alive, one step larger", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        const double fuseAt = 1000 + ImeBubbleModel::kPopAt - 100;
        const double end1 = bubble.EndMs();
        const auto before = bubble.Step(fuseAt);
        Require(r, bubble.Fusions() == 1 && before.active && bubble.Fuse(fuseAt) && bubble.Fusions() == 2 && bubble.EndMs() > end1,
                "a live state merges and extends life");
        const auto at = bubble.Step(fuseAt);
        Require(r, std::abs(at.scale - before.scale) < 0.01f && std::abs(at.grow - 1.f) < 1e-4f, "the size waits for the newcomer to touch");
        const double touch = fuseAt + bubble.TouchMs();
        Require(r, std::abs(bubble.Step(touch - 30).grow - 1.f) < 1e-3f && bubble.Step(touch + 60).grow > 1.f, "touch presses the skin wider");
        float peak = 0;
        for (int ms = 0; ms <= 900; ms += 10)
            peak = std::max(peak, bubble.Step(touch + ms).grow);
        Require(r, peak > bubble.ScaleAt(2) + 0.05f, "a real merge overshoots its new size");
        const auto nearPop = bubble.Step(bubble.PopAt() - 1);
        const auto settled = bubble.Step(touch + 920);
        Require(r,
                nearPop.active && std::abs(nearPop.grow - bubble.ScaleAt(2)) < 0.02f && std::abs(settled.grow - bubble.ScaleAt(2)) < 0.001f && settled.active,
                "it settles at the new size");
    });
    Run(r, "a lone merge keeps the old feel: size, squash and wobble", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        Require(r, bubble.Fuse(1200), "it merges");
        const double touch = 1200 + bubble.TouchMs();
        const float step = bubble.ScaleAt(2) - bubble.ScaleAt(1);
        for (int t = 1200; double(t) < bubble.PopAt(); t += 7) {
            const double td = double(t);
            const float want = td < touch ? bubble.ScaleAt(1) : bubble.ScaleAt(1) + step * ImeBubbleModel::SoftBounce(td - touch, ImeBubbleModel::kRebound);
            Require(r, std::abs(bubble.Step(td).grow - want) < 1e-5f, "the size is the base plus one step");
        }
        const double absorb = bubble.AbsorbMs();
        const float touchFrac = float(bubble.TouchFrac());
        const int startT = 1000 + int(ImeBubbleModel::kEmergeMs) + 40;
        for (int t = startT; double(t) < 1200 + absorb; t += 7) {
            const float u = float((double(t) - 1200) / absorb), d = (u - touchFrac) / float(ImeBubbleModel::kDipSpan);
            const float breath = 1.f + 0.012f * std::sin(6.2831853f * float((double(t) - 1000 - ImeBubbleModel::kEmergeMs) / ImeBubbleModel::kFloatMs));
            const float want = breath * (1 - ImeBubbleModel::kCompression * std::exp(-d * d));
            const auto f = bubble.Step(double(t));
            Require(r, std::abs(f.scale / f.grow - want) < 1e-4f, "the hit squashes the skin by the old amount");
        }
        for (int ms = 20; ms <= 420; ms += 20) {
            Require(r,
                    std::abs(ImeBubbleModel::WobbleAt(ms, 2 * ImeBubbleModel::kWobbleAmp, ImeBubbleModel::kWobbleHz) -
                             2 * ImeBubbleModel::WobbleAt(ms, ImeBubbleModel::kWobbleAmp, ImeBubbleModel::kWobbleHz)) < 1e-6f,
                    "the wobble's strength is its own");
            Require(r,
                    std::abs(ImeBubbleModel::WobbleAt(ms, ImeBubbleModel::kWobbleAmp, 2 * ImeBubbleModel::kWobbleHz) -
                             ImeBubbleModel::WobbleAt(2.0 * ms, ImeBubbleModel::kWobbleAmp, ImeBubbleModel::kWobbleHz) * float(std::exp(ms / 260.0))) < 1e-4f,
                    "its pace decays by the same spring");
        }
        Require(r,
                std::abs(ImeBubbleModel::WobbleAt(20, ImeBubbleModel::kWobbleAmp, ImeBubbleModel::kWobbleHz)) > 0.3f &&
                    std::abs(std::abs(bubble.Step(touch + 50).wob) -
                             std::abs(ImeBubbleModel::WobbleAt(50, ImeBubbleModel::kWobbleAmp, ImeBubbleModel::kWobbleHz))) < 0.2f,
                "the frame's rock matches WobbleAt");
    });
    Run(r, "merges chain, extend the life, and stop growing at ten", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        double now = 1200;
        for (int i = 0; i < 30; ++i, now += 100)
            Require(r, bubble.Fuse(now), "each state inside the life merges");
        Require(r,
                bubble.Fusions() > ImeBubbleModel::kMaxFusions && bubble.EndMs() > now &&
                    bubble.EndMs() - 1000 >= ImeBubbleModel::kPopAt + ImeBubbleModel::kSwellMs + ImeBubbleModel::kBurstMs - 1,
                "the last merge still owns the bubble");
        const auto grown = bubble.Step(bubble.PopAt() - 1);
        Require(r, std::abs(grown.grow - bubble.ScaleAt(ImeBubbleModel::kMaxFusions)) < 0.02f && grown.lobeCount == 0 && !bubble.Fuse(bubble.PopAt() + 100),
                "size stops growing past ten merges");
    });
}

void TestNewcomerMotion(Runner& r)
{
    Run(r, "a bubble merged to ten still lives inside the window", [&] {
        ImeBubbleModel bubble = MakeMerged(1000, 12, 1100, 100.0);
        for (int i = 0; i < 12; ++i) {
            const auto start = bubble.Step(1100 + i * 100.0);
            Require(r, start.active && start.lobeCount > 0, "each newcomer is a visible bubble");
            const auto& own = start.lobes[start.lobeCount - 1];
            Require(r,
                    own.r > 0.5f && std::abs(own.y - (ImeBubbleModel::kH + ImeBubbleModel::kLobeBirthDip)) < 0.01f &&
                        std::abs(own.r - 0.30f * ImeBubbleModel::kStartRadius) < 0.01f,
                    "newcomer is born below the window at first bubble size");
        }
        for (int t = 1008; double(t) < bubble.EndMs(); t += 8)
            if (const auto f = bubble.Step(double(t)); f.active)
                CheckFrameBounds(r, f, true);
    });
    Run(r, "a newcomer keeps the first bubble's size, never the grown skin's", [&] {
        ImeBubbleModel bubble = MakeMerged(1000, 4, 1150, 120.0);
        const double launch = 1150 + 4 * 120.0;
        Require(r, bubble.Fuse(launch), "one more switch merges");
        const auto atTouch = bubble.Step(launch + bubble.TouchMs());
        Require(r, atTouch.grow > 1.2f && atTouch.lobeCount >= 1, "grown skin with active newcomer");
        const auto& own = atTouch.lobes[atTouch.lobeCount - 1];
        Require(r, std::abs(own.r - 0.55f * ImeBubbleModel::kStartRadius) < 0.5f && ImeBubbleModel::kRadius * atTouch.grow > ImeBubbleModel::kStartRadius + 5,
                "touch radius stays the first bubble's rim");
    });
    Run(r, "an unmerged float is perfectly still, so its glass publishes once", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        for (int t = 1000 + int(ImeBubbleModel::kEmergeMs) + 40; double(t) < 1000 + ImeBubbleModel::kPopAt; t += 8) {
            const auto f = bubble.Step(double(t));
            Require(r, f.active && f.opacity == 1 && f.grow == 1 && f.lobeCount == 0 && f.beadCount == 0 && f.wob == 0, "the frame is still");
        }
        const double fuseAt = 1000 + ImeBubbleModel::kPopAt - 300;
        Require(r, bubble.Fuse(fuseAt) && std::abs(bubble.Step(fuseAt + bubble.TouchMs() + 50).wob) > 0.3f, "a merge gives wobble back");
    });
    Run(r, "a merge is born below the window, rises, wobbles and lengthens the life", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        const double fuseAt = 1200;
        Require(r, bubble.Mergeable(fuseAt) && bubble.Fuse(fuseAt) && !bubble.Mergeable(bubble.PopAt()), "mergeable until pop");
        const double touch = fuseAt + bubble.TouchMs();
        const auto first = bubble.Step(fuseAt);
        Require(r, first.lobeCount == 1 && first.lobes[0].r > 2 && first.lobes[0].y - first.lobes[0].r >= ImeBubbleModel::kH - 0.5f,
                "newcomer starts off the screen");
        float prevY = first.lobes[0].y;
        for (int t = int(fuseAt) + 8; double(t) < touch; t += 8) {
            const auto f = bubble.Step(double(t));
            Require(r, f.lobes[0].y <= prevY + 0.001f, "the newcomer rises monotonically to the skin");
            prevY = f.lobes[0].y;
        }
        const auto later = bubble.Step(fuseAt + 300);
        Require(r,
                later.lobes[0].r > 0.5f &&
                    std::hypot(later.lobes[0].x - later.cx, later.lobes[0].y - later.cy) < std::hypot(first.lobes[0].x - first.cx, first.lobes[0].y - first.cy),
                "it slides into the skin");
        Require(r,
                std::abs(bubble.Step(touch + 50).wob) > 0.3f && bubble.Step(fuseAt + bubble.AbsorbMs() + 20).lobeCount == 0 &&
                    std::abs(bubble.Step(touch + ImeBubbleModel::kWobbleMs + 40).wob) < 0.01f,
                "touch rocks then settles");
        const double before = bubble.EndMs();
        Require(r,
                before - fuseAt >= ImeBubbleModel::kPopAt + ImeBubbleModel::kSwellMs + ImeBubbleModel::kBurstMs - 1 && bubble.Fuse(2000) &&
                    bubble.EndMs() > before,
                "life extends on each merge");
    });
}

void TestNewcomerApproach(Runner& r)
{
    Run(r, "a newcomer is born off the screen and then only rises", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        Require(r, bubble.Fuse(1200), "it merges");
        const auto born = bubble.Step(1201);
        Require(r,
                born.lobeCount == 1 && born.lobes[0].y - born.lobes[0].r >= ImeBubbleModel::kH - 0.5f && born.lobes[0].y + born.lobes[0].r > ImeBubbleModel::kH,
                "born completely below window");
        float prevY = born.lobes[0].y;
        int frames = 0;
        for (int t = 1208; double(t) < 1200 + bubble.AbsorbMs(); t += 8) {
            const auto f = bubble.Step(double(t));
            if (f.lobeCount == 0)
                break;
            Require(r, f.lobes[0].y <= prevY + 0.05f, "the newcomer only ever rises");
            prevY = f.lobes[0].y;
            ++frames;
        }
        Require(r, frames > 0 && prevY < ImeBubbleModel::kCyRest + ImeBubbleModel::kRadius, "climbed to the skin");
    });
    Run(r, "the slow rise gives way to a far quicker approach into the skin", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        Require(r, bubble.Fuse(1200), "it merges");
        auto avgSpeed = [&](double from, double to) {
            float prev = bubble.Step(1200 + from).lobes[0].y;
            double moved = 0;
            int n = 0;
            for (int ms = 8; from + ms <= to; ms += 8) {
                const float y = bubble.Step(1200 + from + ms).lobes[0].y;
                moved += std::abs(prev - y);
                prev = y;
                ++n;
            }
            return n ? float(moved / (n * 8.0)) : 0.f;
        };
        const float slow = avgSpeed(0, bubble.RiseEndMs()), fast = avgSpeed(bubble.RiseEndMs(), bubble.TouchMs());
        Require(r, slow > 0 && fast >= 2.f * slow, "fast approach is at least twice slow rise");
    });
    Run(r, "the touch pivots the whole hit: size, squash and wobble await it", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        Require(r, bubble.Fuse(1200), "it merges");
        const double touch = 1200 + bubble.TouchMs();
        Require(r,
                std::abs(bubble.TouchFrac() - 0.82) < 0.001 && std::abs(bubble.Step(touch - 1).grow - 1.f) < 1e-4f && bubble.Step(touch + 40).grow > 1.f &&
                    std::abs(bubble.Step(touch - 1).wob) < 1e-6f && std::abs(bubble.Step(touch + 50).wob) > 0.3f,
                "size and wobble pivot on touch");
        const float atTouch = bubble.Step(touch).scale / bubble.Step(touch).grow;
        const float before = bubble.Step(1200 + 0.3 * bubble.AbsorbMs()).scale / bubble.Step(1200 + 0.3 * bubble.AbsorbMs()).grow;
        const float after = bubble.Step(1200 + 0.95 * bubble.AbsorbMs()).scale / bubble.Step(1200 + 0.95 * bubble.AbsorbMs()).grow;
        Require(r, atTouch < before && atTouch < after, "the hit compresses the skin deepest at the touch");
    });
    Run(r, "a newcomer's whole journey: off the screen, into the frame, swallowed", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        Require(r, bubble.Fuse(1200), "it merges");
        const double absorb = bubble.AbsorbMs();
        const auto away = bubble.Step(1200 + 0.02 * absorb), within = bubble.Step(1200 + 0.78 * absorb);
        Require(r, away.lobeCount == 1 && away.lobes[0].y - away.lobes[0].r >= ImeBubbleModel::kH - 0.5f, "it starts off the screen");
        Require(r, within.lobes[0].y - within.lobes[0].r >= 0 && within.lobes[0].y + within.lobes[0].r <= ImeBubbleModel::kH, "wholly inside frame");
        Require(r, std::hypot(within.lobes[0].x - within.cx, within.lobes[0].y - within.cy) < std::hypot(away.lobes[0].x - away.cx, away.lobes[0].y - away.cy),
                "nearer the skin");
        Require(r, std::abs(bubble.Step(1200 + absorb - 0.5).lobes[0].r) < 1.f && bubble.Step(1200 + absorb + 1).lobeCount == 0, "swallowed cleanly");
    });
}

void TestRapidMergesAndMelt(Runner& r)
{
    Run(r, "rapid merges rise together and fuse side by side", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        const double f1 = 1200, f2 = 1300;
        Require(r, bubble.Fuse(f1) && bubble.Fuse(f2), "both rapid states merge");
        const auto first = bubble.Step(f1), both = bubble.Step(f2);
        Require(r, first.lobeCount == 1 && first.lobes[0].r > 2 && first.lobes[0].y - first.lobes[0].r >= ImeBubbleModel::kH - 0.5f, "first rises");
        Require(r,
                both.lobeCount == 2 && both.lobes[1].y - both.lobes[1].r >= ImeBubbleModel::kH - 0.5f && both.lobes[1].y > both.lobes[0].y &&
                    std::abs(both.lobes[0].x - both.lobes[1].x) > 0.01f,
                "both rise side by side");
        const double touch1 = f1 + bubble.TouchMs(), touch2 = f2 + bubble.TouchMs();
        Require(r,
                std::abs(bubble.Step(touch1 - 1).grow - 1.f) < 1e-4f && bubble.Step(touch1 + 40).grow > 1.f &&
                    bubble.Step(touch2 - 1).grow > bubble.ScaleAt(2) && bubble.Step(touch2 + 40).grow > bubble.Step(touch2 - 1).grow &&
                    std::abs(bubble.Step(f2).grow - bubble.Step(f2 - 1).grow) < 1e-6f,
                "each touch adds its own step");
        ImeBubbleModel solo = MakeMerged(1000, 1, f1, 0);
        for (int t = int(f1) + 1; double(t) < bubble.PopAt(); t += 13)
            Require(r, bubble.Step(double(t)).grow >= solo.Step(double(t)).grow - 1e-5f, "second rise never takes first step back");
        float peak = 0;
        for (int ms = 0; ms <= 340; ms += 5)
            peak = std::max(peak, bubble.Step(touch1 + ms).grow);
        Require(r, peak > bubble.ScaleAt(2) + 0.05f && std::abs(bubble.Step(bubble.PopAt() - 1).grow - bubble.ScaleAt(3)) < 0.01f,
                "overshoots then settles two steps larger");
    });
    Run(r, "two states in the same millisecond both rise", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        Require(r, bubble.Fuse(1200) && bubble.Fuse(1200) && bubble.Fusions() == 3, "both merge at same time");
        const auto both = bubble.Step(1200);
        Require(r, both.lobeCount == 2, "both newcomers rise");
        for (int i = 0; i < both.lobeCount; ++i)
            Require(r,
                    both.lobes[i].r > 1 && std::abs(both.lobes[i].u) < 1e-6f &&
                        std::abs(both.lobes[i].y - (ImeBubbleModel::kH + ImeBubbleModel::kLobeBirthDip)) < 0.01f,
                    "both launch below window");
        const double touch = 1200 + bubble.TouchMs();
        Require(r, std::abs(bubble.Step(touch - 1).grow - 1.f) < 1e-4f && bubble.Step(touch + 1).grow > 1.f, "both touches press at once");
        const auto hit = bubble.Step(touch);
        Require(r,
                std::abs(hit.scale / hit.grow - (1 - ImeBubbleModel::kCompression)) < 0.02f &&
                    std::abs(bubble.Step(bubble.PopAt() - 1).grow - bubble.ScaleAt(3)) < 0.02f,
                "two hits squash to the limit and settle at 3");
    });
    Run(r, "ten merges, ten newcomers, one newest state at a time", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        double now = 1200;
        for (int i = 1; i <= 10; ++i, now += 150) {
            Require(r, bubble.Fuse(now), "every switch before the burst merges");
            const auto own = bubble.Step(now + 1);
            Require(r, own.lobeCount >= 1 && own.lobeCount <= ImeBubbleModel::kMaxFrameLobes, "frame carries newcomers");
            const auto& newest = own.lobes[own.lobeCount - 1];
            Require(r, newest.r > 1 && std::abs(newest.y - (ImeBubbleModel::kH + ImeBubbleModel::kLobeBirthDip)) < 0.01f, "newest born below window at once");
        }
        const auto nearPop = bubble.Step(bubble.PopAt() - 1);
        Require(r,
                bubble.Fusions() == 11 && bubble.Step(now - 150 + 1).lobeCount == 6 &&
                    std::abs(nearPop.grow - bubble.ScaleAt(ImeBubbleModel::kMaxFusions)) < 0.02f && nearPop.lobeCount == 0,
                "ten merges reach cap and swallow before burst");
    });
    Run(r, "a frame carries the newest newcomers, the model counts them all", [&] {
        ImeBubbleModel bubble = MakeMerged(1000, 8, 1200, 5.0);
        Require(r, bubble.Fusions() == 9, "the model counts them all");
        const auto f = bubble.Step(1240);
        const float thirdU = float((1240 - (1200 + 2 * 5.0)) / bubble.AbsorbMs());
        const float newestU = float((1240 - (1200 + 7 * 5.0)) / bubble.AbsorbMs());
        Require(r,
                f.lobeCount == ImeBubbleModel::kMaxFrameLobes && std::abs(f.lobes[0].u - thirdU) < 1e-5f &&
                    std::abs(f.lobes[f.lobeCount - 1].u - newestU) < 1e-5f && f.lobes[0].u > f.lobes[f.lobeCount - 1].u &&
                    std::abs(bubble.Step(bubble.PopAt() - 1).grow - bubble.ScaleAt(9)) < 0.02f,
                "frame carries newest and counts all");
    });
    Run(r, "the melt stays inside the paint's six parts", [&] {
        static_assert(ImeBubbleModel::kMaxMeltParts == 6, "the paint's own parts budget");
        for (int n = 0; n <= ImeBubbleModel::kMaxFrameLobes; ++n) {
            for (bool split : {false, true}) {
                const auto b = ImeBubbleModel::MeltFor(n, split);
                Require(r,
                        b.skinParts >= 1 && b.skinParts <= 2 && b.lobes == ImeBubbleModel::DrawnLobes(n, split) && b.lobes >= 0 && b.lobes <= n &&
                            b.skinParts + b.lobes <= ImeBubbleModel::kMaxMeltParts &&
                            (n <= 4 ? b.lobes == n : b.lobes == ImeBubbleModel::kMaxMeltParts - b.skinParts) &&
                            (!split || b.skinParts != 1 || n >= ImeBubbleModel::kMaxMeltParts - 1),
                        "melt budget invariant");
            }
        }
        Require(r,
                ImeBubbleModel::DrawnLobes(0, true) == 0 && ImeBubbleModel::DrawnLobes(4, true) == 4 && ImeBubbleModel::DrawnLobes(5, true) == 5 &&
                    ImeBubbleModel::DrawnLobes(6, true) == 5 && ImeBubbleModel::DrawnLobes(6, false) == 5,
                "drawn lobe counts match budget");
    });
    Run(r, "ten rapid switches keep every frame inside the window", [&] {
        ImeBubbleModel bubble = MakeMerged(1000, 10, 1150, 120.0);
        Require(r, bubble.Fusions() == 11, "ten switches on top of the first bubble");
        for (int t = 1008; double(t) < bubble.EndMs(); t += 8)
            if (const auto f = bubble.Step(double(t)); f.active)
                CheckFrameBounds(r, f, true);
    });
}

void TestFoamSparkle(Runner& r)
{
    Run(r, "the foam thins away to nothing with a last sparkle", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        const double burstAt = 1000 + ImeBubbleModel::kPopAt + ImeBubbleModel::kSwellMs;
        const int n = bubble.Step(burstAt + 4).beadCount;
        Require(r, n >= ImeBubbleModel::kMinBeads, "the burst has its foam");
        float prev[ImeBubbleModel::kMaxBeads]{}, lastAlive[ImeBubbleModel::kMaxBeads]{}, peak[ImeBubbleModel::kMaxBeads]{};
        std::fill_n(prev, n, -1.f);
        for (int ms = 4; burstAt + ms < bubble.EndMs(); ms += 4) {
            const auto f = bubble.Step(burstAt + ms);
            if (!f.active)
                break;
            for (int i = 0; i < n; ++i) {
                const auto& bead = f.beads[i];
                Require(r, bead.r <= (prev[i] < 0 ? 1e9f : prev[i] + 0.001f), "a drop only ever thins");
                if (bead.r > 0)
                    lastAlive[i] = bead.r;
                peak[i] = std::max(peak[i], bead.glint);
                prev[i] = bead.r;
            }
        }
        for (int i = 0; i < n; ++i)
            Require(r, peak[i] > 0.3f && prev[i] == 0 && lastAlive[i] < 2.f, "every drop sparkles, thins, and disappears");
    });
}

void TestBurstAndFoam(Runner& r)
{
    Run(r, "a bigger bubble throws more foam, and a fuller swarm scatters wider", [&] {
        const ImeBubbleModel defaults{};
        static_assert(ImeBubbleModel::kMinBeads == 9 && ImeBubbleModel::kMaxBeads == 30, "nine to thirty drops");
        Require(r,
                defaults.BeadsFor(1, 0) == ImeBubbleModel::kMinBeads && defaults.BeadsFor(1, 4) == 13 && defaults.BeadsFor(2, 2) == 18 &&
                    defaults.BeadsFor(ImeBubbleModel::kMaxFusions, 0) == ImeBubbleModel::kMaxBeads &&
                    defaults.BeadsFor(ImeBubbleModel::kMaxFusions, 4) == ImeBubbleModel::kMaxBeads,
                "drop counts match formula");
        for (int fusions = 1; fusions <= ImeBubbleModel::kMaxFusions; ++fusions) {
            for (int roll = 0; roll <= 4; ++roll) {
                const int want = std::clamp(ImeBubbleModel::kMinBeads + std::min(roll, 4) + int((defaults.ScaleAt(fusions) - 1) * 40),
                                            ImeBubbleModel::kMinBeads, ImeBubbleModel::kMaxBeads);
                Require(r,
                        defaults.BeadsFor(fusions, roll) == want && (fusions <= 1 || defaults.BeadsFor(fusions, roll) >= defaults.BeadsFor(fusions - 1, roll)),
                        "count is monotone and capped");
            }
        }
        Require(r,
                ImeBubbleModel::SpreadFor(ImeBubbleModel::kMaxBeads, 1.f) > ImeBubbleModel::SpreadFor(ImeBubbleModel::kMinBeads, 1.f) &&
                    std::abs(ImeBubbleModel::SpreadFor(ImeBubbleModel::kMinBeads, 1.f) - 1.f) < 0.001f,
                "more drops scatter wider");
        ImeBubbleModel plain;
        plain.Begin(1000);
        const int plainCount = plain.Step(1000 + ImeBubbleModel::kPopAt + ImeBubbleModel::kSwellMs + 10).beadCount;
        ImeBubbleModel fused = MakeMerged(1000, 9, 1150, 120.0);
        const auto burst = fused.Step(fused.PopAt() + ImeBubbleModel::kSwellMs + 10);
        Require(r, burst.beadCount > plainCount && burst.beadCount >= ImeBubbleModel::kMinBeads + 5, "a merged bubble's skin holds more foam");
    });
    Run(r, "a ten-merge bubble's foam is plainly larger than a lone bubble's", [&] {
        ImeBubbleModel lone;
        lone.Begin(1000);
        const int loneCount = lone.Step(1000 + ImeBubbleModel::kPopAt + ImeBubbleModel::kSwellMs + 10).beadCount;
        ImeBubbleModel merged = MakeMerged(1000, 10, 1150, 120.0);
        const int mergedCount = merged.Step(merged.PopAt() + ImeBubbleModel::kSwellMs + 10).beadCount;
        Require(r,
                merged.Fusions() == 11 && mergedCount - loneCount >= 15 && mergedCount == ImeBubbleModel::kMaxBeads && loneCount >= ImeBubbleModel::kMinBeads &&
                    loneCount <= 13,
                "ten-merge foam reaches cap and exceeds lone");
    });
    Run(r, "a fuller swarm flies further than a lone bubble's", [&] {
        auto meanReach = [](ImeBubbleModel& bubble) {
            const auto burst = bubble.Step(bubble.PopAt() + ImeBubbleModel::kSwellMs + 40);
            const float bx = ImeBubbleModel::kCx, by = ImeBubbleModel::kCyRest - ImeBubbleModel::kFloatRise;
            double sum = 0;
            for (int i = 0; i < burst.beadCount; ++i)
                sum += std::hypot(double(burst.beads[i].x - bx), double(burst.beads[i].y - by));
            return burst.beadCount > 0 ? sum / burst.beadCount : 0.0;
        };
        ImeBubbleModel lone;
        lone.Begin(1000);
        ImeBubbleModel merged = MakeMerged(1000, 10, 1150, 120.0);
        Require(r, meanReach(merged) > meanReach(lone), "the merged swarm scatters further on the whole");
    });
    Run(r, "every drop the burst throws gets a melt shape to ride in", [&] {
        Require(r,
                ImeBubbleModel::kMaxMeltParts == 6 && ImeBubbleModel::ChunksFor(0) == 0 && ImeBubbleModel::ChunksFor(1) == 1 &&
                    ImeBubbleModel::ChunksFor(6) == 1 && ImeBubbleModel::ChunksFor(7) == 2 && ImeBubbleModel::ChunksFor(30) == 5,
                "one shape per six drops: 0/1/6/7/30 -> 0/1/1/2/5");
        for (int parts = 0; parts <= ImeBubbleModel::kMaxBeads; ++parts) {
            const int chunks = ImeBubbleModel::ChunksFor(parts);
            Require(r,
                    chunks * ImeBubbleModel::kMaxMeltParts >= parts && (chunks == 0 || (chunks - 1) * ImeBubbleModel::kMaxMeltParts < parts) &&
                        chunks <= ImeBubbleModel::ChunksFor(ImeBubbleModel::kMaxBeads),
                    "chunks fit all drops with no empty shape");
        }
    });
}

} // namespace

int main()
{
    Runner r;
    TestLifecycle(r);
    TestDeterminismAndConstants(r);
    TestFeelAndTimeline(r);
    TestMergesAndFeelAnchor(r);
    TestNewcomerMotion(r);
    TestNewcomerApproach(r);
    TestRapidMergesAndMelt(r);
    TestFoamSparkle(r);
    TestBurstAndFoam(r);
    return r.failures ? 1 : 0;
}
