// Unit tests for the adaptive model-resolution controller.
//
// This is the one part of the neural pass that can be tested without a GPU: the controller takes
// two numbers and a clock and returns a rung, so its whole behaviour is reachable from a host
// compiler. Worth having, because a control loop is exactly the kind of code that looks right and
// oscillates, and because nothing else here can be run outside a game.
//
//   g++ -std=c++17 -I OptiScaler tests/dlssnr_budget_test.cpp OptiScaler/dlssnr/DlssNrBudget.cpp
//   cl /std:c++17 /EHsc /I OptiScaler tests\dlssnr_budget_test.cpp OptiScaler\dlssnr\DlssNrBudget.cpp
//      (from a VS x64 developer prompt)

#include "dlssnr/DlssNrBudget.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace DlssNrBudget;

static int g_failures = 0;

static void Check(bool ok, const std::string& what)
{
    std::printf("%-72s %s\n", what.c_str(), ok ? "ok" : "FAILED");
    if (!ok)
        ++g_failures;
}

// Drive the controller for a stretch of wall clock at a fixed cost, 60 frames a second.
// Returns the last scale it asked for, if any.
static std::optional<float> Run(Controller& c, const Tuning& t, double passMs, double frameMs,
                                double forMs, double& clock, int* changes = nullptr)
{
    std::optional<float> last;
    const double step = 1000.0 / 60.0;
    for (double spent = 0.0; spent < forMs; spent += step)
    {
        clock += step;
        auto d = c.Update(passMs, frameMs, clock, t);
        if (d.scale.has_value())
        {
            last = d.scale;
            if (changes != nullptr)
                ++*changes;
        }
    }
    return last;
}

// Drive the controller against a cost that RESPONDS to the scale, the way a real pass does: cost
// falls with the square of it. A fixed cost is not a useful simulation of a closed loop -- it never
// converges, because nothing the controller does changes what it measures.
static void RunResponsive(Controller& c, const Tuning& t, double costAtFullScale, double frameMs,
                          double forMs, double& clock, int* changes = nullptr)
{
    const double step = 1000.0 / 60.0;
    for (double spent = 0.0; spent < forMs; spent += step)
    {
        clock += step;
        const double scale = c.Scale();
        const double passMs = costAtFullScale * scale * scale;
        if (c.Update(passMs, frameMs, clock, t).scale.has_value() && changes != nullptr)
            ++*changes;
    }
}

// The general form: pass and frame cost are functions of the current scale and the clock, and every
// move is recorded with the time it happened, so the tests can check WHEN as well as whether.
struct Move
{
    double atMs;
    float scale;
};

static std::vector<Move> Drive(Controller& c, const Tuning& t, const std::function<double(double, double)>& pass,
                               const std::function<double(double, double)>& frame, double forMs, double& clock,
                               std::vector<double>* budgets = nullptr)
{
    std::vector<Move> moves;
    const double step = 1000.0 / 60.0;
    for (double spent = 0.0; spent < forMs; spent += step)
    {
        clock += step;
        const double scale = c.Scale();
        auto d = c.Update(pass(scale, clock), frame(scale, clock), clock, t);
        if (budgets != nullptr && d.lastPassMs > 0.0)
            budgets->push_back(d.lastBudgetMs);
        if (d.scale.has_value())
            moves.push_back({ clock, *d.scale });
    }
    return moves;
}

int main()
{
    const double frame60 = 1000.0 / 60.0; // 16.67 ms
    // The shipped default is a frame-rate target; these tests name the mode they are about.
    Tuning t;
    t.mode = Mode::Share;                 // 15% of the frame = 2.5 ms at 60 fps
    Tuning deep = t;                      // for tests about the mechanism rather than the floor
    deep.floorScale = 0.55f;

    // Comfortably inside budget: never moves, never spends a rebuild.
    {
        Controller c;
        c.Reset(1.0f);
        double clock = 0.0;
        int changes = 0;
        Run(c, t, 1.0, frame60, 30000.0, clock, &changes);
        Check(changes == 0 && c.Scale() == 1.0f, "inside budget for 30s: never moves");
    }

    // Over budget: the square law lands the first move rather than crawling a rung at a time.
    // 6 ms against a 2.5 ms budget wants 1.0 * sqrt(2.5/6) = 0.645, and 0.70 is the nearest rung.
    {
        Tuning t = deep;
        Controller c;
        c.Reset(1.0f);
        double clock = 0.0;
        std::optional<float> first;
        const double step = 1000.0 / 60.0;
        for (double spent = 0.0; spent < 8000.0 && !first.has_value(); spent += step)
        {
            clock += step;
            first = c.Update(6.0, frame60, clock, t).scale;
        }
        Check(first.has_value() && std::fabs(*first - 0.70f) < 1e-4f,
              "6ms against a 2.5ms budget: the first move is straight to the 0.70 rung");
    }

    // ...and against a cost that responds to the scale, it converges and then stays put. 6 ms at
    // full scale becomes 6 * 0.7^2 = 2.94 ms at the 0.70 rung, still over the 2.5 ms budget (and its
    // 2.75 ms margin), so after the freeze it takes the next rung: 6 * 0.55^2 = 1.82 ms. That is under
    // the 1.875 ms recovery line, but 0.70 is predicted at 2.94 ms, over 90% of budget, so it holds.
    {
        Tuning t = deep;
        Controller c;
        c.Reset(1.0f);
        double clock = 0.0;
        int changes = 0;
        RunResponsive(c, t, 6.0, frame60, 60000.0, clock, &changes);
        Check(changes == 2 && std::fabs(c.Scale() - 0.55f) < 1e-4f,
              "a responsive cost converges and then holds");
    }

    // A cost that is only just over budget settles one rung down and does not oscillate: 2.9 ms at
    // full scale is over 2.5, and 2.9 * 0.85^2 = 2.10 ms lands in the dead band at the 0.85 rung.
    {
        Controller c;
        c.Reset(1.0f);
        double clock = 0.0;
        int changes = 0;
        RunResponsive(c, t, 2.9, frame60, 90000.0, clock, &changes);
        Check(changes == 1 && std::fabs(c.Scale() - 0.85f) < 1e-4f,
              "a marginal overshoot settles one rung down without oscillating");
    }

    // A spike is not a scene. One expensive second inside an otherwise fine stretch must not move it.
    {
        Controller c;
        c.Reset(1.0f);
        double clock = 0.0;
        int changes = 0;
        Run(c, t, 1.0, frame60, 6000.0, clock, &changes);
        Run(c, t, 20.0, frame60, 2900.0, clock, &changes); // under 4 of 5 windows
        Run(c, t, 1.0, frame60, 6000.0, clock, &changes);
        Check(changes == 0, "a 3s spike does not cost a rebuild");
    }

    // The artefact guard. However hopeless the budget, the controller stops at 0.55 -- just above
    // where community testing puts visible breakdown in hair and fine detail -- because that is the
    // bottom rung and there is nowhere further to go.
    {
        Controller c;
        c.Reset(1.0f);
        double clock = 0.0;
        Run(c, t, 40.0, frame60, 60000.0, clock);
        Check(std::fabs(c.Scale() - 0.55f) < 1e-4f,
              "an impossible budget stops at 0.55, short of the artefact range");
    }

    // A floor cannot be set below the bottom rung, so the guard cannot be configured away.
    {
        Tuning below = t;
        below.floorScale = 0.20f;
        Controller c;
        c.Reset(1.0f);
        double clock = 0.0;
        Run(c, below, 40.0, frame60, 60000.0, clock);
        Check(std::fabs(c.Scale() - 0.55f) < 1e-4f, "asking for a floor below the ladder still stops at 0.55");
    }

    // A raised floor is honoured too.
    {
        Tuning high = t;
        high.floorScale = 0.70f;
        Controller c;
        c.Reset(1.0f);
        double clock = 0.0;
        Run(c, high, 40.0, frame60, 60000.0, clock);
        Check(std::fabs(c.Scale() - 0.70f) < 1e-4f, "a raised floor is not passed");
    }

    // Recovery: cost falls away, and it climbs back one rung at a time, not in a leap.
    {
        Controller c;
        c.Reset(0.55f);
        double clock = 0.0;
        int changes = 0;
        Run(c, deep, 0.2, frame60, 25000.0, clock, &changes);
        Check(changes == 1 && std::fabs(c.Scale() - 0.70f) < 1e-4f,
              "recovery is one rung at a time, never analytic");
    }

    // ...and all the way back to full if it stays cheap: 18 s, then a 25 s freeze per rung.
    {
        Controller c;
        c.Reset(0.55f);
        double clock = 0.0;
        Run(c, deep, 0.2, frame60, 120000.0, clock);
        Check(std::fabs(c.Scale() - 1.00f) < 1e-4f, "a cheap scene recovers to full");
    }

    // The dead band. Between the budget and the recovery threshold is where it is meant to sit:
    // 2.0 ms is under the 2.5 ms budget but above 75% of it, so neither run builds.
    {
        Controller c;
        c.Reset(0.70f);
        double clock = 0.0;
        int changes = 0;
        Run(c, t, 2.0, frame60, 60000.0, clock, &changes);
        Check(changes == 0 && std::fabs(c.Scale() - 0.70f) < 1e-4f,
              "sitting in the dead band never spends a rebuild");
    }

    // Rebuilds are rate limited. A pathological scene may not thrash the feature: 30s of wildly
    // over-budget cost can move at most once per freeze.
    {
        Controller c;
        c.Reset(1.0f);
        double clock = 0.0;
        int changes = 0;
        Run(c, t, 60.0, frame60, 30000.0, clock, &changes);
        Check(changes <= 30000.0 / t.freezeMs + 1,
              "rebuilds stay rate limited under a pathological scene");
    }

    // The share normalises across frame rates: the same pass cost that is over budget at 120 fps is
    // inside it at 30, because the budget is a share of the frame and not a fixed millisecond figure.
    {
        Controller c120;
        c120.Reset(1.0f);
        double clock = 0.0;
        Run(c120, t, 1.5, 1000.0 / 120.0, 12000.0, clock); // budget 1.25 ms -> over
        Controller c30;
        c30.Reset(1.0f);
        clock = 0.0;
        int changes30 = 0;
        Run(c30, t, 1.5, 1000.0 / 30.0, 12000.0, clock, &changes30); // budget 5.0 ms -> fine
        Check(c120.Scale() < 1.0f && changes30 == 0,
              "the same cost is over budget at 120fps and inside it at 30");
    }

    // Unusable measurements are dropped, not read as zero -- zero would look like free headroom and
    // walk the scale back up on a machine whose timer never worked.
    {
        Controller c;
        c.Reset(0.70f);
        double clock = 0.0;
        int changes = 0;
        Run(c, t, 0.0, frame60, 60000.0, clock, &changes);
        Check(changes == 0 && std::fabs(c.Scale() - 0.70f) < 1e-4f,
              "no usable measurement means no decision, not free headroom");
    }

    // Median, not mean: one 40 ms loading frame in a second of 1 ms frames must not read as over.
    {
        std::vector<double> s;
        for (int i = 0; i < 59; ++i)
            s.push_back(1.0);
        s.push_back(40.0);
        Check(std::fabs(Median(s) - 1.0) < 1e-9, "one 40ms frame does not drag the window over");
    }

    // Seeding from the session's own scale: turning the feature on must not jump the picture.
    {
        Controller c;
        c.Reset(0.70f);
        Check(std::fabs(c.Scale() - 0.70f) < 1e-4f, "enabling mid-session starts where the game already was");
    }

    // --- FixedMs: a flat ceiling on the pass, which is exactly what the engine measures. ---
    {
        Tuning ms = deep;
        ms.mode = Mode::FixedMs;
        ms.fixedMs = 2.0;
        Controller c;
        c.Reset(1.0f);
        double clock = 0.0;
        int changes = 0;
        // 5 ms at full scale: 0.70 gives 2.45 ms, still over the 2.0 ms ceiling, so it takes 0.55
        // for 1.51 ms -- inside the ceiling and above the 1.5 ms recovery line.
        RunResponsive(c, ms, 5.0, frame60, 60000.0, clock, &changes);
        Check(std::fabs(c.Scale() - 0.55f) < 1e-4f, "a millisecond ceiling converges and holds");
    }

    // A millisecond ceiling ignores the frame rate, unlike a share: the same pass cost is judged the
    // same whether the game is running at 30 or 120.
    {
        Tuning ms = deep;
        ms.mode = Mode::FixedMs;
        ms.fixedMs = 2.0;
        Controller a, b;
        a.Reset(1.0f);
        b.Reset(1.0f);
        double c1 = 0.0, c2 = 0.0;
        RunResponsive(a, ms, 5.0, 1000.0 / 30.0, 60000.0, c1);
        RunResponsive(b, ms, 5.0, 1000.0 / 120.0, 60000.0, c2);
        Check(std::fabs(a.Scale() - b.Scale()) < 1e-4f,
              "a millisecond ceiling reads the same at 30fps and 120fps");
    }

    // --- TargetFps: the pass gives back what it costs, and no more. ---
    {
        Tuning fps = deep;
        fps.mode = Mode::TargetFps;
        fps.targetFps = 60; // 16.67 ms
        Controller c;
        c.Reset(1.0f);
        double clock = 0.0;
        // A 20 ms frame of which the pass is 5 ms: dropping the pass by 3.33 ms reaches the target,
        // so the pass is aimed at 1.67 ms, which is 0.58 of full scale -- nearest rung 0.55.
        const double step = 1000.0 / 60.0;
        std::optional<float> first;
        for (double spent = 0.0; spent < 8000.0 && !first.has_value(); spent += step)
        {
            clock += step;
            first = c.Update(5.0, 20.0, clock, fps).scale;
        }
        Check(first.has_value() && std::fabs(*first - 0.55f) < 1e-4f,
              "a frame rate target aims the pass at the shortfall");
    }

    // The honest failure. Asking 120 fps of a game whose frame is 33 ms cannot be met by shrinking a
    // 1 ms pass: the controller goes to the floor and then SAYS the rest is the game's, rather than
    // sitting at the floor looking broken.
    {
        Tuning fps = deep;
        fps.mode = Mode::TargetFps;
        fps.targetFps = 120;
        Controller c;
        c.Reset(1.0f);
        double clock = 0.0;
        bool sawGameLimited = false;
        const double step = 1000.0 / 60.0;
        for (double spent = 0.0; spent < 60000.0; spent += step)
        {
            clock += step;
            auto d = c.Update(1.0, 33.0, clock, fps);
            if (d.gameLimited)
                sawGameLimited = true;
        }
        Check(std::fabs(c.Scale() - 0.55f) < 1e-4f && sawGameLimited,
              "an unreachable frame rate hits the floor and reports the game as the limit");
    }

    // ...and it is not reported when the target IS being met, so the panel does not cry wolf.
    {
        Tuning fps = deep;
        fps.mode = Mode::TargetFps;
        fps.targetFps = 60;
        Controller c;
        c.Reset(1.0f);
        double clock = 0.0;
        bool sawGameLimited = false;
        const double step = 1000.0 / 60.0;
        for (double spent = 0.0; spent < 30000.0; spent += step)
        {
            clock += step;
            if (c.Update(1.0, 14.0, clock, fps).gameLimited)
                sawGameLimited = true;
        }
        Check(!sawGameLimited, "a met frame rate target never blames the game");
    }

    // --- Hysteresis, freeze and anti-flip (Resident Evil 2, 2026-09-18). v2.1.0 moved every 5-30 s
    // there, 55 -> 70 -> 55 -> 70 -> 85 -> 70, and every move held Present for ~250 ms. ---

    const auto fixed = [](double v) { return [v](double, double) { return v; }; };

    // Over budget, but only by a hair: 2.6 ms against 2.5 is inside the 10% margin and never moves.
    {
        Controller c;
        c.Reset(1.0f);
        double clock = 0.0;
        auto moves = Drive(c, t, fixed(2.6), fixed(frame60), 60000.0, clock);
        Check(moves.empty(), "a 4% overshoot is inside the margin and never moves");
    }

    // Stepping down takes about five seconds of windows, not two.
    {
        Controller c;
        c.Reset(1.0f);
        double clock = 0.0;
        auto moves = Drive(c, deep, fixed(6.0), fixed(frame60), 10000.0, clock);
        Check(!moves.empty() && moves[0].atMs >= 4000.0 && moves[0].atMs <= 7000.0,
              "stepping down waits for ~5s of over-budget windows");
    }

    // Stepping up takes about eighteen seconds comfortably under, not five.
    {
        Controller c;
        c.Reset(0.55f);
        double clock = 0.0;
        auto moves = Drive(c, deep, fixed(0.2), fixed(frame60), 25000.0, clock);
        Check(moves.size() == 1 && moves[0].atMs >= 18000.0 && moves[0].atMs <= 21000.0,
              "stepping up waits for ~18s comfortably under budget");
    }

    // Recovery is refused when the next rung is predicted not to fit: 1.8 ms at 0.55 is under 75%
    // of 2.5, but 0.70 is predicted at 1.8 * (0.70/0.55)^2 = 2.92 ms, over budget. This is the RE2
    // flip -- 55% looked like room to spare, 70% was straight back over.
    {
        Controller c;
        c.Reset(0.55f);
        double clock = 0.0;
        auto moves = Drive(c, deep, fixed(1.8), fixed(frame60), 120000.0, clock);
        Check(moves.empty(), "no step up into a rung predicted to miss the budget");
    }

    // The freeze: consecutive moves are at least 25 s apart, even while still over budget.
    {
        Controller c;
        c.Reset(1.0f);
        double clock = 0.0;
        auto moves = Drive(c, deep, [](double s, double) { return 6.0 * s * s; }, fixed(frame60), 90000.0, clock);
        bool spaced = moves.size() == 2;
        for (std::size_t i = 1; i < moves.size(); ++i)
            spaced = spaced && moves[i].atMs - moves[i - 1].atMs >= deep.freezeMs;
        Check(spaced, "moves are at least a freeze apart, even when still over budget");
    }

    // Anti-flip, upward: having stepped 0.70 -> 0.55, it does not climb back to 0.70 inside 60 s
    // however cheap the scene gets -- and does once the minute is up.
    {
        Controller c;
        c.Reset(0.70f);
        double clock = 0.0;
        auto down = Drive(c, deep, fixed(3.0), fixed(frame60), 8000.0, clock);
        const bool wentDown = down.size() == 1 && std::fabs(down[0].scale - 0.55f) < 1e-4f;
        const double t1 = wentDown ? down[0].atMs : 0.0;
        auto up = Drive(c, deep, fixed(0.5), fixed(frame60), 80000.0, clock);
        Check(wentDown && !up.empty() && up[0].atMs >= t1 + deep.antiFlipMs && std::fabs(up[0].scale - 0.70f) < 1e-4f,
              "anti-flip: no return up to the scale just left within 60s");
    }

    // Anti-flip, downward: having stepped 0.55 -> 0.70, a moderate miss (1.2x budget) does not send
    // it back to 0.55 inside 60 s...
    {
        Controller c;
        c.Reset(0.55f);
        double clock = 0.0;
        auto up = Drive(c, deep, fixed(0.5), fixed(frame60), 21000.0, clock);
        const bool wentUp = up.size() == 1 && std::fabs(up[0].scale - 0.70f) < 1e-4f;
        const double t1 = wentUp ? up[0].atMs : 0.0;
        auto down = Drive(c, deep, fixed(3.0), fixed(frame60), 70000.0, clock);
        Check(wentUp && !down.empty() && down[0].atMs >= t1 + deep.antiFlipMs,
              "anti-flip: a moderate miss does not flip back down within 60s");
    }

    // ...but a severe one (2.4x budget) does, as soon as the freeze allows.
    {
        Controller c;
        c.Reset(0.55f);
        double clock = 0.0;
        auto up = Drive(c, deep, fixed(0.5), fixed(frame60), 21000.0, clock);
        const bool wentUp = up.size() == 1;
        const double t1 = wentUp ? up[0].atMs : 0.0;
        auto down = Drive(c, deep, fixed(6.0), fixed(frame60), 40000.0, clock);
        Check(wentUp && !down.empty() && down[0].atMs >= t1 + deep.freezeMs && down[0].atMs < t1 + deep.antiFlipMs,
              "anti-flip: a severe miss overrides it once the freeze is over");
    }

    // A stable budget. The frame alternates 12 ms / 20 ms a second at a time, which put the old
    // one-window budget anywhere from 1.7 to 9.7 ms; smoothed, it barely moves and nothing is rebuilt.
    {
        Tuning fps = deep;
        fps.mode = Mode::TargetFps;
        fps.targetFps = 60;
        Controller c;
        c.Reset(1.0f);
        double clock = 0.0;
        std::vector<double> budgets;
        auto frame = [](double, double now) { return (static_cast<long long>(now / 1000.0) % 2) == 0 ? 12.0 : 20.0; };
        Drive(c, fps, fixed(5.0), frame, 30000.0, clock);
        auto moves = Drive(c, fps, fixed(5.0), frame, 60000.0, clock, &budgets);
        const auto [lo, hi] = std::minmax_element(budgets.begin(), budgets.end());
        Check(!budgets.empty() && *hi - *lo < 1.5 && moves.empty(),
              "the budget is smoothed: a breathing frame rate does not move it");
    }

    // A rebuild's frames are thrown away. Wildly over budget, but a rebuild every 1.5 s means no
    // window is ever judged -- and once the rebuilds stop, the same cost moves it within seconds.
    {
        Controller c;
        c.Reset(1.0f);
        double clock = 0.0;
        int changes = 0;
        Run(c, t, 1.0, frame60, 3000.0, clock, &changes);
        const double step = 1000.0 / 60.0;
        double nextRebuild = clock;
        for (double spent = 0.0; spent < 20000.0; spent += step)
        {
            clock += step;
            const bool rebuilt = clock >= nextRebuild;
            if (rebuilt)
                nextRebuild = clock + 1500.0;
            if (c.Update(6.0, frame60, clock, t, rebuilt).scale.has_value())
                ++changes;
        }
        const int duringRebuilds = changes;
        Run(c, t, 6.0, frame60, 10000.0, clock, &changes);
        Check(duringRebuilds == 0 && changes == 1, "windows containing a rebuild are discarded, not judged");
    }

    // ...and so is a hitch the caller did not announce: a 300 ms frame (a held Present) every 1.5 s.
    {
        Controller c;
        c.Reset(1.0f);
        double clock = 0.0;
        int changes = 0;
        Run(c, t, 1.0, frame60, 3000.0, clock, &changes);
        const double step = 1000.0 / 60.0;
        double nextHitch = clock;
        for (double spent = 0.0; spent < 20000.0; spent += step)
        {
            clock += step;
            double frameMs = frame60;
            if (clock >= nextHitch)
            {
                frameMs = 300.0;
                nextHitch = clock + 1500.0;
            }
            if (c.Update(6.0, frameMs, clock, t).scale.has_value())
                ++changes;
        }
        Check(changes == 0, "a window containing a held Present is discarded, not judged");
    }

    // A replay of the RE2 shape: a 60 fps target, a pass with a fixed part (3.5 ms + 6 ms * area), and
    // a game that breathes between 9.5 and 11.5 ms over 40 s with frame-to-frame noise. Ten minutes
    // of it must settle in a handful of moves, a freeze apart, and never flip back inside a minute.
    {
        Tuning fps = deep;
        fps.mode = Mode::TargetFps;
        fps.targetFps = 60;
        Controller c;
        c.Reset(1.0f);
        double clock = 0.0;
        unsigned int seed = 12345u;
        auto noise = [&seed]() {
            seed = seed * 1664525u + 1013904223u;
            return static_cast<double>(seed >> 8) / static_cast<double>(1u << 24) - 0.5; // -0.5..0.5
        };
        auto pass = [&noise](double s, double) { return (3.5 + 6.0 * s * s) * (1.0 + 0.1 * noise()); };
        auto frame = [&noise](double s, double now) {
            const double rest = 10.5 + std::sin(now / 40000.0 * 6.283185) + 2.0 * noise();
            return rest + 3.5 + 6.0 * s * s;
        };
        auto moves = Drive(c, fps, pass, frame, 600000.0, clock);
        bool ok = moves.size() <= 4;
        for (std::size_t i = 1; i < moves.size(); ++i)
            ok = ok && moves[i].atMs - moves[i - 1].atMs >= fps.freezeMs;
        for (std::size_t i = 2; i < moves.size(); ++i)
            ok = ok && !(std::fabs(moves[i].scale - moves[i - 2].scale) < 1e-4f &&
                         moves[i].atMs - moves[i - 1].atMs < fps.antiFlipMs);
        std::printf("  (RE2 replay: %zu moves", moves.size());
        for (const auto& m : moves)
            std::printf(", %.0f%% at %.0fs", m.scale * 100.0f, m.atMs / 1000.0);
        std::printf(")\n");
        Check(ok, "an RE2-shaped scene settles in a few moves, a freeze apart, no flips");
    }

    std::printf("\n%s\n", g_failures == 0 ? "all passed" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
