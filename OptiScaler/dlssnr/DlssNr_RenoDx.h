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

#include <string>

namespace DlssNrRenoDx
{
// Cheap: while nothing is found the loaded modules are walked again at most every 2 s (ReShade can
// load its add-ons after the panel's first frame); once found, or once refused, never again. Safe to
// call every frame from the panel.
bool Available();

// Null unless Available(). Valid for the life of the process once non-null: ReShade keeps the add-on
// loaded and this deliberately holds no reference of its own.
const RenoDxHostApi* Api();

// The add-on's own name for itself (RenoDX's `global_name`, e.g. "renodx"), or nullptr.
const char* AddonName();

// The module it was found in, e.g. "renodx-unrealengine.addon64", or nullptr. Shown in the panel
// because with a per-game add-on, WHICH one loaded is the thing a player needs to confirm.
const char* ModuleName();

// Why Available() is false, as a stable code the HDR page explains and OptiScaler.hosted.json carries
// to the pop-out: "not-loaded" (no RenoDX add-on in the process, or not yet), "no-api" (a module named
// renodx* is loaded but exports no host API -- every upstream build today), "api-version" (one whose
// API this engine does not speak). nullptr when it is available.
const char* UnavailableReason();

// The file name of a RenoDX add-on loaded in this process right now, upstream build or ours, or empty.
// Unlike Available() this does not need the host API and is not rate-limited: StreamlineHooks asks it
// once, when the game upgrades its DXGI factory, to decide whether ReShade has to sit above Streamline.
std::string AddonInProcess();

// NOT YET THERE, the proper fix for the DLSS-G HUD-less/UI tags (StreamlineHooks::hkslSetTag_renodx, which
// withholds them for now): the game's HUD-less colour is in RenoDX's intermediate encoding and clipped to
// the game's 10-bit format, while the frame DLSS-G receives has been through RenoDX's swap chain pass. To
// give DLSS-G a matching HUD-less image the host API would need one more entry, in RenoDX's
// src/utils/settings.hpp next to the settings ones: given a native D3D12 resource and a command list,
// return (a) the resource's clone when RenoDX redirects it (utils::resource::GetResourceInfo, clone_enabled
// and clone) and (b) otherwise run the same SwapchainProxyPass the add-on uses at present
// (src/utils/draw.hpp) from that resource into a RenoDX-owned texture of the swap chain format. The engine
// would then re-point the tag at what comes back instead of dropping it.
} // namespace DlssNrRenoDx
