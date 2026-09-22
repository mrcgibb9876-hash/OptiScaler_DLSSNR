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
enum class Scaler : uint32_t;
enum class Upsampler : uint32_t;

// What the enlarging direction uses when OutputScaling/Upscaler is left on auto: exactly what this
// pass did before that key existed, where the DOWNscaler decided the enlarging filter too and only
// its first two entries were reachable going up. It lives here rather than in each backend so that
// an ini which never mentions the key gets the same answer from all three of them.
Upsampler ConfiguredUpsampler(Scaler downscaler);

// The same question for the Neural Rendering pass's own up-leg, which has its own key. Separate
// function rather than a parameter because the two read different settings and the answer for an
// unset key has to be derived from the matching downscaler, not from Output Scaling's.
Upsampler ConfiguredNrUpsampler(Scaler downscaler);

// The HLSL for one upsampler, or nullptr for the two that are not here: Upsampler::Bicubic is the
// existing upsampleCode in OS_Common.h and Upsampler::FSR1 is the precompiled FSR1 EASU blob, and
// both stay byte-for-byte what they were.
const char* UpsamplerShaderSource(Upsampler which);

// Short name for the pipeline label that ends up in the log.
const char* UpsamplerName(Upsampler which);
