#include "DlssNrBudget.h"

#include <algorithm>
#include <cmath>

namespace DlssNrBudget
{

double Median(std::vector<double>& samples)
{
    if (samples.empty())
        return 0.0;
    const std::size_t mid = samples.size() / 2;
    std::nth_element(samples.begin(), samples.begin() + mid, samples.end());
    const double upper = samples[mid];
    if (samples.size() % 2 != 0)
        return upper;
    // Even count: the true median is the mean of the two middle values, and nth_element has already
    // partitioned everything below mid, so the largest of those is the lower one.
    const double lower = *std::max_element(samples.begin(), samples.begin() + mid);
    return (lower + upper) / 2.0;
}

std::size_t Controller::FloorRung(float floorScale)
{
    // The deepest rung that is still at or above the floor. Walk from the top so a floor between two
    // rungs resolves to the shallower of them rather than overshooting past what was asked for.
    std::size_t deepest = 0;
    for (std::size_t i = 0; i < RungCount; ++i)
    {
        if (Rungs[i] >= floorScale - 1e-4f)
            deepest = i;
    }
    return deepest;
}

std::size_t Controller::StepToward(float wantedScale, std::size_t floorRung) const
{
    // The rung closest to what the arithmetic asked for...
    std::size_t nearest = 0;
    float bestGap = 1e9f;
    for (std::size_t i = 0; i <= floorRung; ++i)
    {
        const float gap = std::fabs(Rungs[i] - wantedScale);
        if (gap < bestGap)
        {
            bestGap = gap;
            nearest = i;
        }
    }

    // ...and it goes straight there rather than one rung at a time. Clamping the step to one rung
    // was the first shape of this and it was wrong: it defeats the square law, which is the whole
    // reason the pass cost can be aimed at instead of searched for, and it leaves the player over
    // budget for another freeze per rung while it crawls. What guards against diving on a transient
    // is the run of over-budget windows in Update, not a clamp here -- by the time this is reached,
    // the scene has genuinely been too expensive for most of the last five seconds.
    return nearest;
}

void Controller::Reset(float startScale)
{
    m_rung = 0;
    float bestGap = 1e9f;
    for (std::size_t i = 0; i < RungCount; ++i)
    {
        const float gap = std::fabs(Rungs[i] - startScale);
        if (gap < bestGap)
        {
            bestGap = gap;
            m_rung = i;
        }
    }
    m_passSamples.clear();
    m_frameSamples.clear();
    m_restSamples.clear();
    m_windowStartMs = 0.0;
    m_windowOpen = false;
    m_settleUntilMs = 0.0;
    m_overHistory.clear();
    m_underRun = 0;
    m_frameEma = 0.0;
    m_restEma = 0.0;
    m_emaSeeded = false;
    m_changed = false;
    m_lastChangeMs = 0.0;
    m_leftRung = m_rung;
}

void Controller::Discard(double nowMs, const Tuning& tuning)
{
    m_passSamples.clear();
    m_frameSamples.clear();
    m_restSamples.clear();
    m_windowOpen = false;
    m_settleUntilMs = std::max(m_settleUntilMs, nowMs + tuning.settleMs);
}

Decision Controller::Update(double passMs, double frameMs, double nowMs, const Tuning& tuning, bool disturbed)
{
    Decision out;
    out.rung = m_rung;

    // The caller saw the feature rebuilt. Whatever this window held is the rebuild's cost, not the
    // scene's, and judging it is how one move paid for the next in Resident Evil 2 (2026-09-18).
    if (disturbed)
    {
        Discard(nowMs, tuning);
        return out;
    }

    // A frame with no usable measurement teaches nothing. Dropping it is not the same as reading it
    // as zero, which would look like free headroom and walk the scale back up.
    if (!(passMs > 0.0) || !(frameMs > 0.0))
        return out;

    // A hitch -- a held Present, a loading stall, an alt-tab -- throws its whole window away rather
    // than riding in on a median that happens to be close. Only once there is a smoothed frame time
    // to compare with: a game that genuinely runs at 5 fps must still be steerable.
    if (m_emaSeeded && frameMs > std::max(tuning.hitchMinMs, m_frameEma * tuning.hitchFactor))
    {
        Discard(nowMs, tuning);
        return out;
    }

    if (nowMs < m_settleUntilMs)
        return out;

    if (!m_windowOpen)
    {
        m_windowOpen = true;
        m_windowStartMs = nowMs;
    }

    m_passSamples.push_back(passMs);
    m_frameSamples.push_back(frameMs);
    m_restSamples.push_back(std::max(frameMs - passMs, 0.0));

    const double spanMs = nowMs - m_windowStartMs;
    if (spanMs < tuning.windowMs)
        return out;

    const double medianPass = Median(m_passSamples);
    const double medianFrame = Median(m_frameSamples);
    const double medianRest = Median(m_restSamples);
    m_passSamples.clear();
    m_frameSamples.clear();
    m_restSamples.clear();
    m_windowStartMs = nowMs;

    // The budget is built on SMOOTHED frame figures. Built on one window's median it wandered
    // 5.3-10.2 ms in Resident Evil 2 (2026-09-18) with the scene barely changing, and a goalpost
    // that moves every second is one the pass is always about to miss or always about to clear.
    if (!m_emaSeeded)
    {
        m_frameEma = medianFrame;
        m_restEma = medianRest;
        m_emaSeeded = true;
    }
    else
    {
        const double alpha = 1.0 - std::exp(-spanMs / std::max(tuning.budgetTauMs, 1.0));
        m_frameEma += alpha * (medianFrame - m_frameEma);
        m_restEma += alpha * (medianRest - m_restEma);
    }

    double budgetMs = 0.0;
    switch (tuning.mode)
    {
    case Mode::FixedMs:
        budgetMs = tuning.fixedMs;
        break;
    case Mode::TargetFps:
    {
        // The pass can only give back what it costs, so its budget is the target frame less the part
        // of the frame that is the game's. That part does not depend on the model's scale, which is
        // what makes it the stable thing to smooth: v2.1.0 wrote this as "pass + (target - frame)",
        // the same number, but from one window's pass and frame, so every wobble in either moved it.
        const double targetFrameMs = 1000.0 / static_cast<double>(tuning.targetFps > 0 ? tuning.targetFps : 60);
        budgetMs = targetFrameMs - m_restEma;
        break;
    }
    case Mode::Share:
    default:
        budgetMs = m_frameEma * (static_cast<double>(tuning.sharePercent) / 100.0);
        break;
    }

    out.lastPassMs = medianPass;
    out.lastBudgetMs = budgetMs;

    const std::size_t floorRung = FloorRung(tuning.floorScale);

    // Asking for a frame rate the game itself cannot reach drives the budget to nothing or below.
    // That is over budget however small the pass is, and severely so.
    const bool hopeless = !(budgetMs > 0.0);
    const bool over = hopeless || medianPass > budgetMs * tuning.overMargin;
    const bool severe = hopeless || medianPass > budgetMs * tuning.severeFactor;
    const bool under = !hopeless && medianPass < budgetMs * tuning.recoverAt;

    m_overHistory.push_back(over);
    const std::size_t keep = static_cast<std::size_t>(std::max(tuning.downWindows, 1));
    while (m_overHistory.size() > keep)
        m_overHistory.erase(m_overHistory.begin());
    m_underRun = under ? m_underRun + 1 : 0;

    // TargetFps only: shrinking the pass to the floor is everything this can do, and once it is there
    // the honest report is that the remainder is not the pass's to give.
    if (tuning.mode == Mode::TargetFps && over && m_rung >= floorRung)
        out.gameLimited = true;

    // The freeze. Whatever the windows say, a move this recent is not undone or extended yet.
    if (m_changed && nowMs - m_lastChangeMs < tuning.freezeMs)
        return out;

    const int overCount = static_cast<int>(std::count(m_overHistory.begin(), m_overHistory.end(), true));
    std::size_t wantRung = m_rung;

    if (overCount >= tuning.downNeeded && m_rung < floorRung)
    {
        // Cost falls with the square of the scale, so the scale that would have fitted this window is
        // the current one times the square root of the ratio. That is the cost model the lever's own
        // help states, which is why this converges in a step instead of crawling down a rung at a time.
        const float wanted =
            hopeless ? 0.0f : static_cast<float>(Rungs[m_rung] * std::sqrt(budgetMs / medianPass));
        wantRung = StepToward(wanted, floorRung);
        if (wantRung <= m_rung)
            wantRung = m_rung + 1;
    }
    else if (m_underRun >= tuning.upWindows && m_rung > 0)
    {
        // Recovery is one rung at a time, and only if the rung above is predicted to fit with room to
        // spare. The square law overstates what the next rung costs (the model has a fixed part: in
        // RE2 55% -> 70% cost ~1.3x, not the 1.6x the areas say), so this errs towards staying put --
        // the cheap direction to be wrong in, since staying costs nothing and moving costs a hitch.
        const double ratio = static_cast<double>(Rungs[m_rung - 1]) / Rungs[m_rung];
        if (medianPass * ratio * ratio <= budgetMs * tuning.upHeadroom)
            wantRung = m_rung - 1;
    }

    if (wantRung == m_rung)
        return out;

    // Anti-flip: never straight back to the scale just left. Stepping down to it again is allowed
    // only when the miss is severe -- then the rung it is on is plainly wrong, and holding it for a
    // minute to save a hitch would be the worse trade. Stepping back UP to it never is: being under
    // budget is never urgent.
    if (m_changed && wantRung == m_leftRung && nowMs - m_lastChangeMs < tuning.antiFlipMs &&
        !(wantRung > m_rung && severe))
        return out;

    m_leftRung = m_rung;
    m_rung = wantRung;
    m_changed = true;
    m_lastChangeMs = nowMs;
    m_overHistory.clear();
    m_underRun = 0;
    Discard(nowMs, tuning);

    out.rung = m_rung;
    out.scale = Rungs[m_rung];
    return out;
}

} // namespace DlssNrBudget
