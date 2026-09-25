// Copyright (c) 2026 mrcgibb9876-hash. Original to this fork, not part of upstream OptiScaler.
// Distributed under the GNU General Public License version 3 -- see LICENSE.
#pragma once

#include <filesystem>

// The hosted add-ons (ReLimiter's pacing, RenoDX's HDR) for the pop-out DLSS 5 panel, which runs
// outside the game (OptiDLSS5-UI).
//
// WHY A SECOND CHANNEL AND NOT OptiScaler.ini. Neither add-on re-reads its own ini while the game runs,
// and ReLimiter rewrites relimiter.ini on exit -- so an app that edits those files mid-session is
// overwritten by the game it was trying to change. The only live door into either is its host API,
// and that is callable only from inside the process, on the thread that draws the panel. So the app
// asks through a file and this side, on that thread, does the calling:
//
//   OptiScaler.hosted.json      what the in-game Pacing and HDR pages would draw right now: every
//                               setting the pages show, with its label, range, choices and current
//                               value, plus `ack`, the last command applied. Rebuilt every 500 ms,
//                               written when it changes and at least every second so `at` stays fresh.
//                               Same gate as OptiScaler.live.json: nothing at all without a live request.
//   OptiScaler.hosted.set.json  written by the app: {"seq":N,"pid":P,"pacing":{key:value},"hdr":{...}}.
//                               Checked every 250 ms; applied once per new seq, only when `pid` is this
//                               process (a file left over from the last session must not replay into
//                               this one), then acknowledged by publishing ack = seq straight away.
namespace DlssNr::Hosted
{
// From DlssNr::Live::Tick, once per presented frame on the panel's own thread -- which is the thread
// both host APIs ask to be called on. `dir` is empty until Live has worked it out; `requested` is
// Live's own verdict on OptiScaler.live.request, so the two files start and stop together.
void Tick(const std::filesystem::path& dir, bool requested);
} // namespace DlssNr::Hosted
