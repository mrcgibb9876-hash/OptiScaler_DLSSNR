// Copyright (c) 2026 mrcgibb9876-hash. Original to this fork, not part of upstream OptiScaler.
// Distributed under the GNU General Public License version 3 -- see LICENSE.
#pragma once
// Talking to ReLimiter, when it happens to be in the process.
//
// ReLimiter is a frame-pacing ReShade add-on (MIT, RankFTW/Lazorr/UltraMatt). Where the manager has
// deployed it, it is already loaded as relimiter.addon64 and ReShade drives it. This finds it and asks
// for its host API so the DLSS 5 panel can present its settings, rather than the player having two
// overlays with two keybinds and two places to look.
//
// EVERYTHING HERE IS OPTIONAL. No ReLimiter, an older build without the API, or a build whose API
// version we do not speak all end in the same place: Available() is false and the panel shows no
// pacing page. None of it is an error and none of it is worth a log line per frame.
#include "ReLimiter_Api.h"

namespace DlssNrReLimiter
{
// Cheap: a miss is re-asked at most every 2 s (the add-on can load after the panel's first frame), a
// hit or a final refusal is never asked again. Safe to call every frame from the panel.
bool Available();

// Null unless Available(). Valid for the life of the process once non-null: the module stays loaded.
const ReLimiterApi* Api();

// ReLimiter's product version, or nullptr.
const char* Version();

// Why Available() is false, as a stable code the Pacing page explains and OptiScaler.hosted.json
// carries to the pop-out: "not-loaded" (no ReLimiter in the process, or not yet), "no-api" (a build
// without the host API), "api-version" (one whose API this engine does not speak). nullptr when it is
// available.
const char* UnavailableReason();
} // namespace DlssNrReLimiter
