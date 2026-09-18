// Unit tests for the adaptive model-resolution controller.
//
// This is the one part of the neural pass that can be tested without a GPU: the controller takes
// two numbers and a clock and returns a rung, so its whole behaviour is reachable from a host
// compiler. Worth having, because a control loop is exactly the kind of code that looks right and
// oscillates, and because nothing else here can be run outside a game.
//
//   g++ -std=c++17 -I OptiScaler tests/dlssnr_budget_test.cpp OptiScaler/dlssnr/DlssNrBudget.cpp

#include "dlssnr/DlssNrBudget.h"

#include <cmath>
#include <cstdio>
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
    // full scale becomes 6 * 0.7^2 = 2.94 ms at the 0.70 rung, still over the 2.5 ms budget, so it
    // takes the next rung: 6 * 0.55^2 = 1.82 ms, inside budget and above the 1.75 ms recovery line.
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
        Run(c, t, 20.0, frame60, 900.0, clock, &changes); // under the 2s sustain
        Run(c, t, 1.0, frame60, 6000.0, clock, &changes);
        Check(changes == 0, "a sub-2s spike does not cost a rebuild");
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
        Run(c, high, 40.0, high.windowMs + 60000.0, clock = 0.0, clock);
        Run(c, high, 40.0, frame60, 60000.0, clock);
        Check(std::fabs(c.Scale() - 0.70f) < 1e-4f, "a raised floor is not passed");
    }

    // Recovery: cost falls away, and it climbs back one rung at a time, not in a leap.
    {
        Controller c;
        c.Reset(0.55f);
        double clock = 0.0;
        int changes = 0;
        Run(c, deep, 0.2, frame60, 9000.0, clock, &changes);
        Check(changes == 1 && std::fabs(c.Scale() - 0.70f) < 1e-4f,
              "recovery is one rung at a time, never analytic");
    }

    // ...and all the way back to full if it stays cheap.
    {
        Controller c;
        c.Reset(0.55f);
        double clock = 0.0;
        Run(c, deep, 0.2, frame60, 60000.0, clock);
        Check(std::fabs(c.Scale() - 1.00f) < 1e-4f, "a cheap scene recovers to full");
    }

    // The dead band. Between the budget and the recovery threshold is where it is meant to sit:
    // 2.0 ms is under the 2.5 ms budget but above 70% of it, so neither run builds.
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
    // over-budget cost can move at most 30s/dwell times.
    {
        Controller c;
        c.Reset(1.0f);
        double clock = 0.0;
        int changes = 0;
        Run(c, t, 60.0, frame60, 30000.0, clock, &changes);
        Check(changes <= 30000.0 / t.dwellMs + 1,
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
        // for 1.51 ms -- inside the ceiling and above the 1.4 ms recovery line.
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

    std::printf("\n%s\n", g_failures == 0 ? "all passed" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
