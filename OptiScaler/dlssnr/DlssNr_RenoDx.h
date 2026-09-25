// Copyright (c) 2026 mrcgibb9876-hash. Original to this fork, not part of upstream OptiScaler.
// Distributed under the GNU General Public License version 3 -- see LICENSE.
#pragma once
// Talking to RenoDX, when it happens to be in the process.
//
// RenoDX (MIT, Carlos Lopez Jr.) is a per-game HDR and tone-mapping ReShade add-on. Where the manager
// has deployed one it is already loaded and ReShade drives it; this finds it and asks for its host API
// so the DLSS 5 panel can present its settings, rather than the player having two overlays with two
// keybinds and two places to look. Same arrangement as DlssNr_ReLimiter, and for the same reason.
//
// ONE REAL DIFFERENCE FROM RELIMITER. ReLimiter is always relimiter.addon64, so a GetModuleHandleW on
// that name finds it. RenoDX ships one add-on PER GAME -- renodx-cp2077.addon64, renodx-eldenring.addon64,
// renodx-unrealengine.addon64 -- so there is no name to ask for. The loaded modules are enumerated
// instead and the one exporting RenoDxGetHostApi is taken, which also means a copy the user installed
// himself is found on equal terms with ours.
//
// EVERYTHING HERE IS OPTIONAL. No RenoDX, a build without the export (which today is every shipped
// build -- the API is offered upstream, not merged), or an API version we do not speak all end in the
// same place: Available() is false and the panel shows no RenoDX page. None of it is an error.
#include "RenoDx_Api.h"

namespace DlssNrRenoDx
{
// Cheap after the first call. Safe to call every frame from the panel.
bool Available();

// Null unless Available(). Valid for the life of the process once non-null: ReShade keeps the add-on
// loaded and this deliberately holds no reference of its own.
const RenoDxHostApi* Api();

// The add-on's own name for itself (RenoDX's `global_name`, e.g. "renodx"), or nullptr.
const char* AddonName();

// The module it was found in, e.g. "renodx-unrealengine.addon64", or nullptr. Shown in the panel
// because with a per-game add-on, WHICH one loaded is the thing a player needs to confirm.
const char* ModuleName();
} // namespace DlssNrRenoDx
