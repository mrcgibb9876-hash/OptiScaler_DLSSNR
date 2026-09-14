#pragma once

// Motion vectors for DLSS 5 Neural Rendering in games that never hand an upscaler any: the Present route
// (DlssNr_PresentRoute.h). Estimated from the finished frames by AMD FidelityFX Optical Flow, then turned
// into a per-pixel field -- the method of LCPD15's DXL (src/core/OpticalFlow.cpp), whose FidelityFX subset
// and precompiled shaders this uses.
//
// A DLL of its own rather than part of OptiScaler.dll: OptiScaler links AMD's prebuilt FSR3 libraries,
// which carry the same FidelityFX backend symbols as this subset. Built apart, the two never meet.
//
// C interface, so the two modules share nothing but D3D12 pointers.

#include <d3d12.h>

#define OPTI_OF_ABI_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

// The version this DLL implements; the caller refuses a mismatch.
int OptiOF_AbiVersion(void);

// Creates a flow estimator for input frames of this size. quality 0/1/2 caps the estimation size at
// 640/960/1280 on the long side. Returns null when the GPU cannot run it (shader model 6.2 with wave
// operations is required); OptiOF_StaticError then says why.
void* OptiOF_Create(ID3D12Device* device, unsigned int width, unsigned int height, int quality);

// Records one frame's estimation onto list. input: a colour frame of the created size, in
// NON_PIXEL_SHADER_RESOURCE, left there. Returns the motion field -- R16G16_FLOAT, the input's size, in
// pixels (DLSS motion vector scale 1), in NON_PIXEL_SHADER_RESOURCE -- or null on failure. *sceneCut is
// set when the estimator saw a cut a few frames ago (its history is then worthless). reset starts the
// estimator's own history over. Overwrites the list's descriptor heaps, root signature and pipeline.
ID3D12Resource* OptiOF_Record(void* flow, ID3D12GraphicsCommandList* list, ID3D12Resource* input, int reset,
                              int* sceneCut);

// Destroys the estimator. Only once no command list recorded by it can still be executing.
void OptiOF_Destroy(void* flow);

// Why the last create or record failed, or null.
const char* OptiOF_LastError(void* flow);
const char* OptiOF_StaticError(void);

#ifdef __cplusplus
}
#endif
