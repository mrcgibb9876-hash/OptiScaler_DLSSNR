#pragma once

// Live readings for the pop-out DLSS 5 panel, which runs outside the game (OptiDLSS5-UI).
//
// That window can only read files, and the only numbers it had were the timing lines in OptiScaler.log,
// written every 600 frames -- so its frame rate was never live. While the app asks for them, the engine
// now writes the same readings the in-game panel shows to a small file beside OptiScaler.ini:
//
//   OptiScaler.live.request   touched by the app every 2 s while its panel shows this game. Only a
//                             request written in the last 10 s counts, so a closed or crashed app
//                             stops the writes on its own.
//   OptiScaler.live.json      written about every 500 ms while a request is live, atomically (a .tmp
//                             beside it, then a replace). Nothing at all is written without a request.
namespace DlssNr::Live
{
// Once per presented frame, from MenuCommon::RenderMenu (DX11, DX12 and the Present route all draw
// through it every frame, menu up or not). Cheap: a clock check and an early return on almost every call.
void Tick();

// Frame generation's multiplier running right now: 3 for 3X, 0 when none is (the game's own DLSS-G, or
// OptiScaler's own frame generation).
int FgMultiplier();

// The two frame rates a player can mean. rendered: frames the game actually rendered (the pass's own rate
// while it runs). shown: what reaches the screen with frame generation -- the rate this panel is presented at
// when it sees the generated frames, otherwise rendered x the multiplier (estimated set true). With no frame
// generation both are the presented rate.
struct FrameRates
{
    double rendered = 0.0;
    double shown = 0.0;
    int multiplier = 0;
    bool estimated = false;
};
FrameRates Rates(double presentedFps);
} // namespace DlssNr::Live
