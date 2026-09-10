#pragma once
#include <string>

// Launch/close Lossless Scaling, and drive its Frame Generation on/off and multiplier -- offered
// from the in-game menu as the working alternative to OptiScaler's own Frame Generation for a
// DLSS5-Feeder game (see DlssNr_Menu.cpp, gated on DlssNr::IsFeederPresent()). This process does
// not read, write, or otherwise understand Lossless Scaling's own settings file -- OptiDLSS5-UI
// already does that safely (a real browser DOMParser/XMLSerializer round-trip) and writes the exe
// path and profile title this class needs into Config::Instance()->LosslessScalingExePath /
// LosslessScalingGameTitle. Comparing a hand-rolled XML editor written here against that would
// only add a second, less-safe way to do the same job.
//
// Reverse-engineered (ilspycmd against the real, installed LosslessScaling.dll -- a legitimately
// owned copy, purely for interoperability, 2026-09-10) rather than guessed at, after several live
// dead ends:
//   * Its "AutoScale" feature (matching a profile against whatever window is currently in the
//     foreground, UI.ForegroundWatcher/SetWinEventHook(EVENT_SYSTEM_FOREGROUND)) only fires on a
//     genuine foreground CHANGE event. Confirmed live it never fires no matter the launch order,
//     because the game was already the stable foreground window before Lossless Scaling started
//     watching -- there is no change event left for it to see.
//   * A synthetic SetForegroundWindow from an external, unrelated process (tried directly) does
//     not generate a real change event either -- Windows' own foreground-lock protection silently
//     blocks it. (Untested from in-process, where this actually runs -- may behave differently,
//     not relied on either way.)
//   * Its Ctrl+Alt+S hotkey (UI.GlobalKeyboardHook, a WH_KEYBOARD_LL hook) has NO anti-injection
//     check -- it reads Keyboard.IsKeyDown() for the modifiers at the moment it sees the base key,
//     so a synthetic press needs real delay between each key-down (confirmed the naive
//     no-delay keybd_event sequence failed live; never retried with proper spacing once UI
//     Automation was confirmed working end-to-end instead).
//   * What is proven, twice, live: UI Automation's InvokePattern on its ScaleButton
//     (AutomationId="ScaleButton", inside a ProfileList of AutomationId="ProfileList") genuinely
//     starts Frame Generation for whichever profile was first selected via
//     SelectionItemPattern -- but only once the window has actually been shown (even non-
//     activated) at least once since launch; interacting with a window minimized since startup
//     found and "selected" successfully at the automation level but never took real effect.
//   * The multiplier field (AutomationId="LSFG3Multiplier") is a live value, not something needing
//     a restart: its own ProfileChanged() calls Core.ApplySettings(...) (a native call into
//     Lossless.dll) whenever the profile being edited matches the one currently scaling, so
//     setting it via ValuePattern while active applies immediately.
class LosslessScaling
{
  public:
    // Any process named LosslessScaling.exe, not just ones this class started -- the user may
    // already have it open themselves.
    static bool IsRunning();

    // False if exePath is empty, doesn't exist, or CreateProcess itself fails. Does not wait for
    // the process to finish starting.
    static bool Launch(const std::wstring& exePath);

    // Posts WM_CLOSE to LosslessScaling.exe's own top-level window(s) first, same as the user
    // clicking its own close button, so it gets a chance to save state normally -- then guarantees
    // it's actually gone: confirmed live that closing its window alone leaves it running in the
    // background (Frame Generation keeps going), so this falls back to terminating it if it's
    // still alive after a short grace period. That wait runs on its own detached thread, not the
    // caller's, so a slow/stuck Lossless Scaling can't stall the game's own render thread. Returns
    // false only if the process wasn't running at all (nothing to do, not a failure to report).
    static bool Close();

    // Selects the profile named gameTitle in Lossless Scaling's own profile list and invokes its
    // Scale button via UI Automation -- the same toggle the Ctrl+Alt+S hotkey and a manual click
    // both drive, so calling this while already active turns it off, same as clicking Scale again.
    // Briefly shows the window (SW_SHOWNOACTIVATE, does not steal focus from the game) since
    // interacting with it never-shown-since-launch did not take real effect in testing, then
    // minimizes it again. Runs on its own detached thread (UI Automation COM calls take real time,
    // roughly half a second to a couple of seconds) -- does not block the caller.
    static void TriggerScaleAsync(const std::wstring& gameTitle);

    // Sets the LSFG3Multiplier field (2/3/4) for the given profile via UI Automation's
    // ValuePattern, not by touching Settings.xml. If that profile is the one currently active,
    // Lossless Scaling's own change-tracking applies it live, no restart needed. Same brief
    // show/minimize and background-thread behavior as TriggerScaleAsync.
    static void SetMultiplierAsync(const std::wstring& gameTitle, int multiplier);
};
