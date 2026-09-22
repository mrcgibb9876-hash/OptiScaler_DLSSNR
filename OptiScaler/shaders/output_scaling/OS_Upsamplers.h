#pragma once

// The upsamplers Output Scaling can use when it is ENLARGING.
//
// Why this exists: until now the enlarging direction had no choice at all. OS_Dx12.cpp and
// OS_Dx11.cpp ran FSR1 when the downscaler happened to be set to FSR1 and the LDS-tiled bicubic in
// OS_Common.h in every other case -- the Scaler the user picked was ignored going up. These filters
// only make sense in that direction, which is why they are a separate enum (Upsampler) rather than
// more entries in Scaler.
//
// They are runtime-compiled only. Every other shader here has a precompiled .cso/.spv blob checked
// in beside it, produced with fxc on Windows; there is no build rule that regenerates them, so a
// shader added from a non-Windows checkout cannot have one. Shader_Dx11/Dx12::CreateComputePipeline
// compiles from source when a shader has no blob, whatever UsePrecompiledShaders says. The Vulkan
// backend consumes SPIR-V only and has no runtime compiler, so OS_Vk falls back to bicubic and logs
// which upsampler it could not honour.
//
// Bindings and thread group match the downsamplers in OS_Common.h exactly -- the same root signature
// (1 CBV, 1 SRV, 1 UAV, 1 static sampler) and the same [numthreads(8, 8, 1)] -- so nothing outside
// the pipeline-creation switch has to know which of them is running.

// Forward declaration, as in OS_Dx12.h: a scoped enum with a fixed underlying type is a complete
// type when declared this way, so this header need not pull in Config.h.
enum class Upsampler : uint32_t;

// The HLSL for one upsampler, or nullptr for Upsampler::Bicubic -- that one is not here at all, it
// is the existing upsampleCode in OS_Common.h and stays byte-for-byte what it was.
const char* UpsamplerShaderSource(Upsampler which);

// The four controls that shape a resampling upsampler's result, for whichever pass is asking. All
// of them are a plain 0..1 with 0 the gentlest setting, so they can be a row of identical sliders
// rather than a switch here and a preset name there. The Neural Rendering up-leg has its own set of
// keys for the same reason it has its own filter: it is enlarging a different picture at a
// different size from an Output Scaling pass.
struct UpsamplerTuning
{
    float sharpness;
    float antiRinging;
    float sigmoid;
    float dither;
};

UpsamplerTuning UpsamplerTuningFor(bool neuralRendering);

// Short name for the pipeline label that ends up in the log.
const char* UpsamplerName(Upsampler which);
