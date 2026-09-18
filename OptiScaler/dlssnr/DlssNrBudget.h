#pragma once

#include <cstddef>
#include <optional>
#include <vector>

// Adaptive model resolution: hold the neural pass to a share of the frame.
//
// Everything this needs already existed and was never joined up. DlssNr_Dx12 reads the pass's own
// GPU time every frame through a timestamp query and filters implausible readings; State::frameTimes
// carries the frame time the overlay draws from; and DlssNrWorkingScale is a lever whose cost falls
// with the SQUARE of it. So the engine has both a sensor and an actuator, and until now the number
// between them was one a person had to guess once and live with for every scene in the game.
//
// Three ways to say what the budget is, because people think about this in three different ways:
//
//   Share    the pass may take at most N% of the frame. Normalises itself -- 15% is 2.5 ms at
//            60 fps and 1.25 ms at 120 -- so it holds as the scene and the frame rate move.
//   FixedMs  the pass may take at most N ms. Exactly what the engine measures, no arithmetic.
//   TargetFps  best effort at a frame rate. The honest one to get right: the engine can only give
//            back what the PASS costs, and the rest of the frame belongs to the game. So this aims
//            the pass at the shortfall and reports when even the floor is not enough -- at which
//            point the remaining gap is the game's, and saying so is more use than a silent floor.
//
// THE CONSTRAINT THAT SHAPES ALL OF THIS: changing WorkingScale sets resolutionChanged in
// DlssNr_Dx12, which REBUILDS the NGX feature. A controller that moved every frame would rebuild
// every frame and be far slower than doing nothing. So this one is quantised to a few rungs,
// hysteretic, and rate limited -- at most one rung every few seconds, which is rarer than the
// rebuilds an ordinary resolution change already causes. That is also how shipping dynamic
// resolution works, so it is the right shape rather than a concession.
namespace DlssNrBudget
{

// The rungs, coarsest first. Quantised because every move costs a rebuild, and repeatable so a
// scene that oscillates settles between two known values rather than wandering.
//
// 0.55 is the bottom, and it is the bottom RUNG rather than a floor set against a deeper ladder --
// a floor of 0.55 against rungs that stepped 0.60 then 0.50 would silently stop at 0.60 and the
// number in the setting would be a lie.
//
// Why there: community testing of this lever puts 0.75 at "most of the quality retained" and 0.50
// at the point where hair and other fine detail visibly break down. 0.55 sits just above that, so
// the controller can take most of the cost that is available to take without ever reaching the
// range where the trade stops being cost against quality and becomes cost against artefacts.
//
// Four rungs, each about a quarter of the cost off the one above it (100%, 72%, 49%, 30% of full
// cost). Enough resolution to aim with, few enough that a scene which oscillates settles between
// two known values -- and every extra rung is another rebuild the controller might spend.
inline constexpr float Rungs[] = { 1.00f, 0.85f, 0.70f, 0.55f };
inline constexpr std::size_t RungCount = sizeof(Rungs) / sizeof(Rungs[0]);

enum class Mode
{
    Share,     // a percentage of the frame
    FixedMs,   // a millisecond ceiling on the pass
    TargetFps, // best effort at a frame rate, limited to what the pass itself costs
};

struct Tuning
{
    // Frame rate by default. It is the one of the three a player already has a number in mind for,
    // and the only one they can judge the result of without reading a millisecond figure off a
    // panel. The other two are there for people who want the pass itself pinned.
    Mode mode = Mode::TargetFps;
    // Share of the frame the pass may take, as a percentage.
    int sharePercent = 15;
    // A flat ceiling on the pass, in milliseconds.
    double fixedMs = 2.0;
    // The frame rate to aim at.
    int targetFps = 60;
    // The lowest rung the controller may choose. Clamped to a real rung. Raise it to keep more
    // quality; it cannot go below the bottom rung, which is where the artefacts start.
    float floorScale = 0.55f;

    // A decision is made on a window of samples rather than a spike: a single expensive frame is a
    // loading screen or an alt-tab, not a scene that got heavy.
    double windowMs = 1000.0;
    // Sustained over budget for this long before stepping down. Short, because being over budget is
    // the thing the player asked not to happen.
    double overBudgetMs = 2000.0;
    // Comfortably under for this long before stepping back up. Longer and with headroom, so a scene
    // that is merely between heavy moments does not bounce.
    double underBudgetMs = 5000.0;
    // How far under budget counts as "comfortably": recover only below this share of the budget.
    double recoverAt = 0.70;
    // Never move twice inside this, whichever direction. One rebuild per this, worst case.
    double dwellMs = 3000.0;
};

// What the controller decided this tick. `scale` is set only when the rung changed.
struct Decision
{
    std::optional<float> scale;
    std::size_t rung = 0;
    double lastPassMs = 0.0;
    double lastBudgetMs = 0.0;
    // TargetFps only: the pass is as small as the floor allows and the frame rate is still short, so
    // the rest of the gap is the game's and nothing here can close it. The panel says so rather than
    // sitting at the floor looking broken.
    bool gameLimited = false;
};

class Controller
{
  public:
    // startScale seeds the rung from whatever WorkingScale the session began with, so turning the
    // feature on does not jump the picture.
    void Reset(float startScale);

    // passMs: the pass's own measured GPU time, already trust-filtered by the caller.
    // frameMs: the frame's time.
    // nowMs: a monotonic clock in milliseconds.
    // Returns a new scale only on the ticks where the rung actually moves.
    Decision Update(double passMs, double frameMs, double nowMs, const Tuning& tuning);

    std::size_t Rung() const { return m_rung; }
    float Scale() const { return Rungs[m_rung]; }

  private:
    // The lowest rung index the floor allows.
    static std::size_t FloorRung(float floorScale);
    // The rung nearest a wanted scale, within the floor. Downward moves go straight there: the
    // sustain window above is what filters transients, so the square law is allowed to land in one.
    std::size_t StepToward(float wantedScale, std::size_t floorRung) const;
    void Settle(double nowMs);

    std::size_t m_rung = 0;
    std::vector<double> m_passSamples;
    std::vector<double> m_frameSamples;
    double m_windowStartMs = 0.0;
    // When the current run of over- or under-budget windows began. 0 means "not in one".
    double m_overSinceMs = 0.0;
    double m_underSinceMs = 0.0;
    double m_lastChangeMs = 0.0;
    bool m_started = false;
};

// Median of a sample window. A median rather than a mean because one loading-screen frame at 40 ms
// would drag a mean over budget on its own and cost a rebuild for nothing.
double Median(std::vector<double>& samples);

} // namespace DlssNrBudget
