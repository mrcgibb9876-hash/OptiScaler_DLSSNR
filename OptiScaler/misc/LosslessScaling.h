#pragma once
#include <string>

// Launch/close Lossless Scaling and drive its Frame Generation on/off + multiplier from the
// in-game DLSS-NR panel, as the working alternative to OptiScaler's own Frame Generation (which
// crashes with the DLSS5 Feeder -- see FGHooks::CheckForFGStatus). Per-game setup (the Lossless
// Scaling profile itself) is done by OptiDLSS5-UI, which writes the exe path, profile title,
// multiplier and the LS global-hotkey chord into Config::Instance()->LosslessScaling* keys. This
// process never opens or shows Lossless Scaling's own window.
//
// WHY THE GLOBAL HOTKEY, NOT UI AUTOMATION (rewritten 2026-09-11 after live re-testing):
//   * Lossless Scaling runs ELEVATED by default (Settings.xml <StartAsAdmin>true); a normal game
//     process is medium integrity. UIPI blocks a medium-IL process from driving an elevated
//     window's UI Automation InvokePattern / posting it input at all -- so the old UIA "select
//     profile -> click Scale button" path only ever worked when the game itself happened to run
//     elevated, and silently did nothing otherwise. OptiDLSS5-UI now writes <StartAsAdmin>false so
//     Lossless Scaling matches the game's integrity, but the hotkey below does not even depend on
//     that.
//   * The UIA path REQUIRED briefly showing Lossless Scaling's window (a real, confirmed
//     requirement: interacting with a never-shown window did nothing) -- that visible flash over
//     the game is exactly what the user asked to be rid of. The hotkey shows no window at all.
//   * Lossless Scaling installs a low-level keyboard hook (WH_KEYBOARD_LL, GlobalKeyboardHook in
//     its own code) for its Ctrl+Alt+S toggle. A low-level hook sees synthetic input from
//     SendInput regardless of the sender's integrity level -- CONFIRMED LIVE 2026-09-11: a
//     non-elevated process toggled an elevated Lossless Scaling on and off with SendInput, no
//     window shown, only the fullscreen frame-gen overlay appearing/disappearing. Its own hotkey
//     handler activates whatever profile matches the current FOREGROUND window, which in-game is
//     the game itself -- so no profile has to be selected in any list. The chord just has to be
//     synthesised with a real gap between key events (its handler reads Keyboard.IsKeyDown for the
//     modifiers at the instant it sees the base key); a zero-gap burst is missed. When the chord
//     matches, that hook swallows the base key's press (returns 1), so the game never sees an S
//     key-down -- only the modifier hold and a stray key-up (read from its decompiled
//     GlobalKeyboardHook, 2026-09-12).
//
// WHAT LOSSLESS SCALING DOES WITH ITS SETTINGS FILE (from its decompiled MainWindow, 2026-09-12):
//   * Profiles are read from Settings.xml once, at startup. There is no live re-read.
//   * It writes its in-memory settings back over the file from its own UI (profile edits, window
//     size changes) and from a real close through its Closing handler -- but not when killed. With
//     <CloseToTray>true (which OptiDLSS5-UI sets) a WM_CLOSE only hides it to the tray.
//   So a change to the file only sticks if it is made while Lossless Scaling is not running, and
//   only takes effect once it starts: SetMultiplierAsync stops it, patches, then starts it again.
class LosslessScaling
{
  public:
    // Not MOD_CTRL/MOD_ALT/... -- those names are Windows RegisterHotKey macros and collide.
    enum Mod
    {
        HK_CTRL = 1,
        HK_ALT = 2,
        HK_SHIFT = 4,
        HK_WIN = 8
    };

    // Any process named LosslessScaling.exe, not just one this class started -- the user may
    // already have it open. Cached for half a second: the panel asks every frame, and a process
    // snapshot is not free. A Launch or Close through this class refreshes it at once.
    static bool IsRunning();

    // Launches Lossless Scaling minimized (-StartMinimized + its own tray settings keep it out of
    // sight). False if exePath is empty/missing or CreateProcess fails. Does not wait.
    static bool Launch(const std::wstring& exePath);

    // Posts WM_CLOSE, then terminates if still alive after a short grace (Lossless Scaling keeps
    // running in the tray on WM_CLOSE alone). Best-effort: terminating a higher-integrity process
    // from a medium-IL game is denied by Windows, which is another reason OptiDLSS5-UI sets
    // <StartAsAdmin>false. Returns false only if it was not running. Runs its wait on a detached
    // thread so a stuck Lossless Scaling never stalls the caller.
    static bool Close();

    // Turn Lossless Scaling's Frame Generation ON for this game. Detached thread: ensures the
    // process is running (launches + waits for it to be ready if not), then synthesises the global
    // hotkey once. No window is shown. mods is a Mod bitmask, vk the base virtual-key (e.g. 'S').
    static void ActivateAsync(const std::wstring& exePath, int mods, int vk);

    // Turn it OFF: synthesise the same toggle hotkey once (Lossless Scaling's toggle is symmetric).
    // Detached thread; assumes the process is already running. No window shown.
    static void DeactivateAsync(int mods, int vk);

    // Change the fixed multiplier for this game's profile. Lossless Scaling only reads its profiles
    // from Settings.xml at startup and exposes no live API a separate process can call, so this
    // stops it if it is running (a kill: a real close would write its stale in-memory copy back
    // over the file), edits the game's own <Profile> in Settings.xml (a minimal, backed-up,
    // value-only rewrite -- no structural change, no XML declaration touched), and starts it
    // again, re-activating afterwards when wasActive. All on a detached thread. No window shown.
    static void SetMultiplierAsync(const std::wstring& exePath, const std::wstring& gameTitle, int multiplier,
                                   int mods, int vk, bool wasActive);
};
