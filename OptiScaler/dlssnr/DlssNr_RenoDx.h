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

// Host API version 2 (mrcgibb9876-hash/renodx feat/dlssg-tags), for the DLSS-G tags the game sets while
// ReShade sits above Streamline (StreamlineHooks::hkslSetTag_renodx). All three are false with an add-on
// that only speaks version 1, or when the add-on has nothing to substitute; the tag is then left as the
// game set it. Native D3D12 resources and D3D12_RESOURCE_STATES.
bool TagApiAvailable();
// Whether the add-on can reset its settings (host API version 3), and doing it: every setting its own
// overlay's reset touches back to its default, saved.
bool CanReset();
// The add-on's host API when it speaks version 4 (presets, per-setting reset, buttons, the full setting
// description), else null. Each version-4 function pointer is still checked for null before a call.
const RenoDxHostApi* V4();

// True while a RenoDX add-on that speaks the host API is loaded. The DLSS 5 tone trim (Brightness,
// Contrast and their Auto modes) is grading, and in such a game RenoDX does the grading: the trim is
// hidden in the panel and sent to the shaders as identity (1, 1, Auto off), while the values saved in
// the ini are left alone so they come back if RenoDX is removed. Safe to call every frame from any thread.
bool ToneTrimSuppressed();
// One line per committed change from our panel or the pop-out, so "the slider does nothing" can be told
// apart from "the value never arrived": what was set, whether the add-on took it, what it reads back and
// which preset was current. Source is "page" or "pop-out"; action is "set", "reset", "press",
// "preset", "text" or "reset all". Capped per session.
void LogCommit(const char* source, const char* action, const char* key, double value, bool ok);
void ResetAll();
// 1 when the add-on clones the back buffers and writes the presented frame itself (mods::swapchain's proxy
// pass), 0 when it only replaces the game's shaders, -1 when the add-on cannot say (upstream builds, or
// ours before the question existed).
int UsesSwapchainProxy();
// The clone RenoDX redirects the resource's writes to, as RenoDX's own dlssfix does for every tag.
bool ResolveClone(void* nativeResource, void** outNativeResource);
// For a colour image DLSS-G compares with the presented frame (HUD-less colour, back buffer): a texture
// RenoDX fills at each present with its swap chain proxy pass over the image, so it is encoded exactly
// like the frame DLSS-G receives.
bool EncodeForSwapchain(void* nativeResource, uint32_t d3d12State, void** outNativeResource, uint32_t* outD3d12State);
// Copy-back: RenoDX's pass result copied back into the image itself at each present (alpha kept for the
// UI image), so the tag can stay as the game set it. True once RenoDX has taken the image on.
bool EncodeInPlaceForSwapchain(void* nativeResource, uint32_t d3d12State, bool isUi);
// The same for the UI colour-and-alpha image: its alpha and format kept, only its colour encoded.
bool EncodeUiForSwapchain(void* nativeResource, uint32_t d3d12State, void** outNativeResource, uint32_t* outD3d12State);
} // namespace DlssNrRenoDx
