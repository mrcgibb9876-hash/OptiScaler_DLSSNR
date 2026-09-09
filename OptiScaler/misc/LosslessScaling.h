#pragma once
#include <string>

// Launch/close Lossless Scaling as a whole -- offered from the in-game menu as the working
// alternative to OptiScaler's own Frame Generation for a DLSS5-Feeder game (see DlssNr_Menu.cpp,
// gated on DlssNr::IsFeederPresent()). Deliberately does nothing beyond process lifetime: this
// process does not read, write, or otherwise understand Lossless Scaling's own settings file --
// OptiDLSS5-UI already does that safely (a real browser DOMParser/XMLSerializer round-trip) and
// writes the exe path this class needs into Config::Instance()->LosslessScalingExePath. Comparing
// a hand-rolled XML editor written here against that would only add a second, less-safe way to do
// the same job.
class LosslessScaling
{
  public:
    // Any process named LosslessScaling.exe, not just ones this class started -- the user may
    // already have it open themselves.
    static bool IsRunning();

    // False if exePath is empty, doesn't exist, or CreateProcess itself fails. Does not wait for
    // the process to finish starting.
    static bool Launch(const std::wstring& exePath);

    // Finds LosslessScaling.exe's own top-level window(s) and posts WM_CLOSE -- the same as the
    // user clicking its own close button, so it saves its state normally. Returns false if the
    // process isn't running or has no window to close (nothing to do, not a failure to report).
    static bool Close();
};
