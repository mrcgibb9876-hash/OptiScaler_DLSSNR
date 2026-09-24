// Copyright (c) 2026 mrcgibb9876-hash. Original to this fork, not part of upstream OptiScaler.
// Distributed under the GNU General Public License version 3 -- see LICENSE.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>

// Whether the DLSS 5 pass timer's readings are fit to show.
//
// GPU timestamp queries are not honest on every card, driver and route. On Batman: Arkham Knight (the
// Feeder on a D3D11 game, 2026-09-15) the panel's green "Running - X ms per frame" was reported as
// garbage: numbers jumping about with nothing to do with the pass. A wrong number in the one green line
// is worse than none, so the readings are judged in windows, and a timer that keeps failing the
// judgement is not shown again this session -- the line then just says DLSS 5 is on.
//
// A window is bad when most of it is missing, or when a third of its readings sit more than 4x from
// the window's median. Three bad windows in a row, not one: a real scene change (a menu into gameplay)
// moves the median once and settles, while a broken timer stays scattered.
class NrTimingTrust
{
    static constexpr size_t kWindow = 120;
    static constexpr int kBadWindowsToGiveUp = 3;

    std::array<double, kWindow> _samples {};
    size_t _count = 0;
    int _badWindows = 0;
    bool _untrusted = false;

  public:
    // A pass this model runs cannot finish in under 0.05 ms, nor take a whole second.
    static bool Plausible(double ms) { return ms >= 0.05 && ms < 1000.0; }

    // One reading, or nullopt when the timer produced none that frame. Returns true only on the call
    // that gives up on the timer, so the caller can log it once.
    bool Add(std::optional<double> ms)
    {
        if (_untrusted)
            return false;

        _samples[_count++] = (ms.has_value() && Plausible(ms.value())) ? ms.value() : -1.0;

        if (_count < kWindow)
            return false;

        _count = 0;

        std::array<double, kWindow> valid {};
        size_t validCount = 0;

        for (double s : _samples)
            if (s > 0.0)
                valid[validCount++] = s;

        bool bad = validCount < kWindow / 2;

        if (!bad)
        {
            std::sort(valid.begin(), valid.begin() + validCount);
            const double median = valid[validCount / 2];
            size_t off = kWindow - validCount;

            for (size_t i = 0; i < validCount; ++i)
                if (valid[i] > median * 4.0 || valid[i] < median / 4.0)
                    ++off;

            bad = off * 3 > kWindow;
        }

        _badWindows = bad ? _badWindows + 1 : 0;

        if (_badWindows < kBadWindowsToGiveUp)
            return false;

        _untrusted = true;
        return true;
    }

    bool Untrusted() const { return _untrusted; }
};
