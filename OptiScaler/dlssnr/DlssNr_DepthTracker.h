// Copyright (c) 2026 mrcgibb9876-hash. Original to this fork, not part of upstream OptiScaler.
// Distributed under the GNU General Public License version 3 -- see LICENSE.
#pragma once

#include <d3d12.h>

// Finds the game's main scene depth buffer by watching how the game uses its depth buffers, for games
// that never hand an upscaler their depth -- the Resident Evil titles running their own TAA, where there
// is no DLSS call to take guides from and Neural Rendering runs at Present (DlssNr_PresentRoute.h).
//
// Nothing names the scene depth, so it is inferred, the way ReShade's Generic Depth add-on does it:
//   CreateDepthStencilView   maps a DSV handle to its resource -- a command list only ever sees handles
//   OMSetRenderTargets       how often each depth is bound this frame, the main signal
//   ClearDepthStencilView    the scene depth is cleared about once a frame; the clear value also says
//                            whether depth is reversed (cleared to 0) or not (cleared to 1)
//   ResourceBarrier          the state each candidate was last moved to, so Present can copy it
// Draw calls are deliberately not hooked: thousands a frame, and binds already separate scene depth from
// shadow maps. Size is the strongest filter: scene depth matches the render size, shadow maps are square
// powers of two.
//
// Adapted from LCPD15's DXL (DLSS eXtended Loader, AGPL-3.0, https://github.com/LCPD15/DXL),
// src/core/DepthTracker.cpp, whose heuristics this follows -- including keeping the previous choice on a
// tie, because RE Engine hands out a freshly allocated depth buffer of identical size every frame.
// Hooks here are inline detours on the functions rather than vtable writes, like the Present route.

namespace DlssNr::DepthTracker
{
// Installs the hooks on this device's first use. Cheap: a throwaway command list shows where the
// functions are. Safe to call more than once.
bool Install(ID3D12Device* device);

bool Installed();

// Once per Present: closes the frame's counts and chooses the scene depth for a frame of this size.
void EndFrame(unsigned int renderWidth, unsigned int renderHeight);

struct Selection
{
    ID3D12Resource* resource = nullptr; // not owned
    unsigned int width = 0;
    unsigned int height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    bool reversed = true; // cleared to 0.0
};

// The chosen depth, when there is one whose current state is known. False otherwise -- a copy from a
// guessed state is a validation error at best.
bool Selected(Selection& out);

// The game rebuilt its swapchain or changed resolution: every candidate is stale.
void Invalidate();
} // namespace DlssNr::DepthTracker
