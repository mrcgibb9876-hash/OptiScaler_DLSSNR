#pragma once

#include <windows.h>

// The mouse for the DLSS 5 panel when it is seen as the DLSS5 Feeder's cast (the 32-bit route).
//
// The Feeder shows this helper's window inside the game (a DWM thumbnail) and forwards the mouse to it as
// posted messages, using the cursor position the game's ReShade tracks from the game window's own mouse
// messages. A game that reads the mouse some other way (raw input, DirectInput) gets none of those messages,
// so that position never moves and nothing reaches the panel -- the pointer sat still over it while the real
// cursor moved freely (Castlevania: Lords of Shadow 2 demo, 2026-09-19; the cursor itself was probed moving).
//
// So the helper reads the real cursor itself, but only while the cast is on screen, which the Feeder does not
// tell this process: it is read from the Feeder's own log beside the game ("cast: shown ..." / "cast: hidden
// ...", and "N x M ... shown at W x H (scale S)" for the size), with the corner from dlss5-feed.cfg
// (cast_anchor). The game window comes from the Feeder's command line (the game's pid is its first argument).
namespace DlssNr::CastMouse
{
// screen: the real cursor in physical screen pixels (per-monitor DPI aware). True, with the position in this
// window's client pixels, while the cast is shown and the cursor is over it (not over its close button, which
// is the Feeder's). False otherwise -- then the panel gets only what the Feeder posts, as before.
bool MapToPanel(const POINT& screen, POINT& client);

// Whether the cast is on screen as far as the Feeder's log says. Cheap; the log is read at most every 100 ms.
bool CastShown();
} // namespace DlssNr::CastMouse
