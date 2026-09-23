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
// hysteretic, and rate limited -- at most one move per 25 s, never straight back to the scale it just
// left, and usually far rarer than that (see Tuning). That is also how shipping dynamic resolution
// works, so it is the right shape rather than a concession.
//
// Why not vary the evaluated size inside one feature, the way DLSS SR does dynamic resolution with a
// max render size and a per-evaluate subrect? Because nothing says this feature can. It is created
// with one DLSSNR.Width/Height that is both its input and its output size -- there is no separate
// render/output pair like SR's -- and the subrects the forwarder fills at evaluate have only ever been
// set to the full created size. Feeding a smaller subrect to a feature built larger is undocumented,
// untested behaviour on the one path where a wrong guess removes the device mid-game. So the rebuild
// stays, and the controller is what keeps it rare (Resident Evil 2, 2026-09-18).
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

    // Everything below exists because every move costs the player a visible hitch, and v2.1.0 spent
    // them far too freely. Resident Evil 2 (Present route, 1440p, laptop GPU, 2026-09-18) moved the
    // scale every 5-30 s -- 55, 70, 55, 70, 85, 70 -- and each move rebuilt the model and held Present
    // for ~250 ms. The old rules (2 s over, 5 s under, 3 s dwell) were sized as if a move were free.
    // These are sized so a move is rare, and so the controller never undoes a move it has just made.
    // They are not in the ini: they are the controller's own stability, not a preference, and a
    // player who wants a different scale has WorkingScale and AutoScaleFloor for that.

    // Stepping down: at least downNeeded of the last downWindows windows must be over budget, and
    // over by overMargin rather than by a hair -- 4 of 5 s at 10% over is a scene that got heavy, not
    // a frame-pacing wobble around the line.
    int downWindows = 5;
    int downNeeded = 4;
    double overMargin = 1.10;

    // Stepping up: this many CONSECUTIVE windows under recoverAt of the budget (18 s), and the next
    // rung's cost -- predicted with the square law, which overstates it, so the guess errs towards
    // staying put -- must also fit under upHeadroom of the budget. Without the prediction, 55% at
    // 65 fps in RE2 read as "room to spare" against a 60 fps target and went up to 70%, which cost
    // about 2 ms and put it straight back over.
    int upWindows = 18;
    double recoverAt = 0.75;
    double upHeadroom = 0.90;

    // After any move, no further move for this long, whichever direction. One rebuild per 25 s,
    // worst case, instead of one per 3 s.
    double freezeMs = 25000.0;

    // Anti-flip: the scale just left is not returned to within this, unless the pass is missing its
    // budget by more than severeFactor -- which is not a wobble any more but a scene the current rung
    // plainly cannot hold. This is what stops 55 -> 70 -> 55 outright.
    double antiFlipMs = 60000.0;
    double severeFactor = 2.0;

    // Samples are thrown away for this long after a move, a rebuild or a hitch. The frames around a
    // rebuild are the rebuild's (a ~250 ms held Present, then the model settling), and letting them
    // into a window is how one move's own cost argued for the next one.
    double settleMs = 2000.0;
    // A frame this many times the smoothed frame time (and at least hitchMinMs) is a hitch: its window
    // is discarded rather than judged.
    double hitchFactor = 3.0;
    double hitchMinMs = 50.0;

    // Time constant of the smoothing applied to the frame figures the budget is derived from. The
    // budget itself was swinging 5.3-10.2 ms window to window in RE2, because TargetFps derived it from
    // the last one-second median of the frame; a scene that breathes for a second moved the goalposts.
    double budgetTauMs = 8000.0;
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
    // disturbed: the caller knows this frame is not representative -- the feature was just rebuilt.
    // The open window is discarded and sampling pauses for settleMs.
    // Returns a new scale only on the ticks where the rung actually moves.
    Decision Update(double passMs, double frameMs, double nowMs, const Tuning& tuning, bool disturbed = false);

    std::size_t Rung() const { return m_rung; }
    float Scale() const { return Rungs[m_rung]; }

  private:
    // The lowest rung index the floor allows.
    static std::size_t FloorRung(float floorScale);
    // The rung nearest a wanted scale, within the floor. Downward moves go straight there: the
    // sustain window above is what filters transients, so the square law is allowed to land in one.
    std::size_t StepToward(float wantedScale, std::size_t floorRung) const;
    // Drop the open window and pause sampling until nowMs + settleMs.
    void Discard(double nowMs, const Tuning& tuning);

    std::size_t m_rung = 0;
    std::vector<double> m_passSamples;
    std::vector<double> m_frameSamples;
    std::vector<double> m_restSamples; // frame minus pass: the part of the frame that is the game's
    double m_windowStartMs = 0.0;
    bool m_windowOpen = false;
    double m_settleUntilMs = 0.0;

    // Verdicts of the most recent closed windows, newest last, at most downWindows long: true = over
    // budget by the margin. Cleared on every move.
    std::vector<bool> m_overHistory;
    // Consecutive windows comfortably under budget. Any other window resets it.
    int m_underRun = 0;

    // Smoothed frame and rest-of-frame, in ms. Not reset by a move: the rest of the frame is the
    // game's and does not depend on the model's scale, which is exactly why the budget is built on it.
    double m_frameEma = 0.0;
    double m_restEma = 0.0;
    bool m_emaSeeded = false;

    bool m_changed = false; // has the controller moved at all yet (the freeze needs a first move)
    double m_lastChangeMs = 0.0;
    std::size_t m_leftRung = 0; // the rung the last move left
};

// Median of a sample window. A median rather than a mean because one loading-screen frame at 40 ms
// would drag a mean over budget on its own and cost a rebuild for nothing.
double Median(std::vector<double>& samples);

} // namespace DlssNrBudget
