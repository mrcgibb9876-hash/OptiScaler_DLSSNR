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
    // budget for another few seconds per rung while it crawls. What guards against diving on a
    // transient is the two seconds of sustained over-budget windows above, not a clamp here -- by
    // the time this is reached, the scene has genuinely been too expensive for two seconds.
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
    m_windowStartMs = 0.0;
    m_overSinceMs = 0.0;
    m_underSinceMs = 0.0;
    m_lastChangeMs = 0.0;
    m_started = false;
}

void Controller::Settle(double nowMs)
{
    m_passSamples.clear();
    m_frameSamples.clear();
    m_windowStartMs = nowMs;
    m_overSinceMs = 0.0;
    m_underSinceMs = 0.0;
    m_lastChangeMs = nowMs;
}

Decision Controller::Update(double passMs, double frameMs, double nowMs, const Tuning& tuning)
{
    Decision out;
    out.rung = m_rung;

    // A frame with no usable measurement teaches nothing. Dropping it is not the same as reading it
    // as zero, which would look like free headroom and walk the scale back up.
    if (!(passMs > 0.0) || !(frameMs > 0.0))
        return out;

    if (!m_started)
    {
        m_started = true;
        m_windowStartMs = nowMs;
        m_lastChangeMs = nowMs;
    }

    m_passSamples.push_back(passMs);
    m_frameSamples.push_back(frameMs);

    if (nowMs - m_windowStartMs < tuning.windowMs)
        return out;

    const double medianPass = Median(m_passSamples);
    const double medianFrame = Median(m_frameSamples);
    m_passSamples.clear();
    m_frameSamples.clear();
    m_windowStartMs = nowMs;

    double budgetMs = 0.0;
    switch (tuning.mode)
    {
    case Mode::FixedMs:
        budgetMs = tuning.fixedMs;
        break;
    case Mode::TargetFps:
    {
        // The pass can only give back what it costs. If the frame is 4 ms longer than the target,
        // the pass has to lose 4 ms -- so its budget is what it spends now, less the shortfall. The
        // same arithmetic runs the other way when the frame is already inside the target, which is
        // what lets the scale climb back once there is room for it.
        const double targetFrameMs = 1000.0 / static_cast<double>(tuning.targetFps > 0 ? tuning.targetFps : 60);
        budgetMs = medianPass + (targetFrameMs - medianFrame);
        break;
    }
    case Mode::Share:
    default:
        budgetMs = medianFrame * (static_cast<double>(tuning.sharePercent) / 100.0);
        break;
    }

    out.lastPassMs = medianPass;
    out.lastBudgetMs = budgetMs;

    const std::size_t floorRung = FloorRung(tuning.floorScale);

    // Asking for a frame rate the game itself cannot reach drives the budget to nothing or below.
    // Shrinking the pass to the floor is everything this can do about that, and once it is there the
    // honest report is that the remainder is not the pass's to give.
    if (!(budgetMs > 0.0))
    {
        out.gameLimited = tuning.mode == Mode::TargetFps && m_rung >= floorRung;
        if (m_rung >= floorRung)
            return out;
        budgetMs = 1e-6; // below any real measurement, so the step below drives to the floor
    }
    const bool over = medianPass > budgetMs;
    const bool under = medianPass < budgetMs * tuning.recoverAt;

    // Each direction has to be sustained, and the two runs are exclusive: a window that is over
    // budget clears any recovery that was building, and the other way round.
    if (over)
    {
        m_underSinceMs = 0.0;
        if (m_overSinceMs == 0.0)
            m_overSinceMs = nowMs;
    }
    else if (under)
    {
        m_overSinceMs = 0.0;
        if (m_underSinceMs == 0.0)
            m_underSinceMs = nowMs;
    }
    else
    {
        // Between the budget and the recovery threshold is where it is meant to sit. Neither run
        // continues, so a scene hovering here never spends a rebuild.
        m_overSinceMs = 0.0;
        m_underSinceMs = 0.0;
        return out;
    }

    if (nowMs - m_lastChangeMs < tuning.dwellMs)
        return out;

    std::size_t wantRung = m_rung;

    if (over && m_overSinceMs != 0.0 && nowMs - m_overSinceMs >= tuning.overBudgetMs && m_rung < floorRung)
    {
        // Cost falls with the square of the scale, so the scale that would have fitted this window is
        // the current one times the square root of the ratio. That is the cost model the lever's own
        // help states, which is why this converges in a step instead of crawling down a rung at a time.
        const float wanted = static_cast<float>(Rungs[m_rung] * std::sqrt(budgetMs / medianPass));
        wantRung = StepToward(wanted, floorRung);
        if (wantRung <= m_rung)
            wantRung = m_rung + 1;
    }
    else if (under && m_underSinceMs != 0.0 && nowMs - m_underSinceMs >= tuning.underBudgetMs && m_rung > 0)
    {
        // Recovery is one rung at a time and never analytic. The measurement says what the pass costs
        // HERE; it cannot say what the next rung up would cost, and guessing that from the square law
        // would overshoot straight back over budget and undo itself.
        wantRung = m_rung - 1;
    }

    if (tuning.mode == Mode::TargetFps && over && m_rung >= floorRung)
        out.gameLimited = true;

    if (wantRung == m_rung)
        return out;

    m_rung = wantRung;
    Settle(nowMs);
    out.rung = m_rung;
    out.scale = Rungs[m_rung];
    return out;
}

} // namespace DlssNrBudget
