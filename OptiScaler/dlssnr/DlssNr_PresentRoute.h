// Copyright (c) 2026 mrcgibb9876-hash. Original to this fork, not part of upstream OptiScaler.
// Distributed under the GNU General Public License version 3 -- see LICENSE.
#pragma once

#include <d3d12.h>

// Neural Rendering at Present, on a command list and queue of our own.
//
// Some games never show OptiScaler their swapchain -- Resident Evil 2 through REFramework and PureDark's
// Upscaler Base Plugin is the case that led here -- so the only place the pass could run was inside
// the upscaler's own command list. There it needed a root signature to restore that list never has,
// waited for a Present count that never moved, and ran at the moment the game's own GPU state was in
// flux. At Present none of that applies: the frame is finished, the list is ours, and there is nothing
// of the game's to put back.
//
// The approach follows LCPD15's DXL (DLSS eXtended Loader, AGPL-3.0,
// https://github.com/LCPD15/DXL), which runs its Neural Rendering pass at Present: a throwaway swapchain
// on a hidden window reveals the functions every swapchain of that class runs, and those functions are
// detoured, so the game's own swapchain is covered without ever being wrapped. As in DXL, the queue the
// game presents on is learned from ExecuteCommandLists, because a D3D12 swapchain will not hand it back.
// The implementation here is this project's own, written against OptiScaler's pass.
//
// [DlssNr] Placement: auto (default) | evaluate | present. auto takes Present for the games whose
// swapchain OptiScaler does not wrap (the OldOverlayMenu quirk: the RE Engine titles), and everywhere
// else stays where the pass has always run.

struct IDXGISwapChain;

namespace DlssNr::PresentRoute
{
// Whether this game should run the pass at Present. Decided once the device is known.
bool Wanted();

// Installs the Present hooks on the device's first use. Returns true when they are in place; false
// (once, logged) when they cannot be, in which case the pass stays at the upscaler call.
bool EnsureInstalled(ID3D12Device* device);

// True once hooks are in and the game has actually presented through them.
bool Active();

// How many times the game has presented through the hook.
unsigned long long PresentCount();

// The command list the game's upscale call was recorded on. The queue that list is executed on is the
// game's frame queue, which is the one the pass has to submit to at Present.
void NoteUpscaleList(ID3D12GraphicsCommandList* cmdList);

// Called when the game's D3D12 device is created. For a game on the Present route, installs the depth
// tracker and the Present hooks there and then, because a game with no upscale call has no later moment.
void PrepareForDevice(ID3D12Device* device);
} // namespace DlssNr::PresentRoute
