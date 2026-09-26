// Copyright (c) 2026 mrcgibb9876-hash. Original to this fork, not part of upstream OptiScaler.
// Distributed under the GNU General Public License version 3 -- see LICENSE.
#include "pch.h"

#include <set>

#include <dlssnr/DlssNr.h>

#include <dlssnr/DlssNr_Capture.h>
#include <dlssnr/DlssNr_CleanCapture.h>
#include <dlssnr/DlssNr_Proxy.h>
#include <dlssnr/DlssNr_ExposureScan.h>
#include <dlssnr/DlssNr_PresentRoute.h>
#include <dlssnr/DlssNr_DepthTracker.h>
#include <dlssnr/DlssNr_RenoDx.h>
#include <menu/menu_dx12.h>
#include <menu/menu_common.h>
#include <dlssnr/DlssNrFeature_Dx12.h>
#include <NVNGX_Parameter.h>
#include <dxgi1_4.h>

#include "DlssNr_Dx12.h"
#include "DlssNr_ActiveColor.h"

#include <Config.h>
#include <State.h>
#include <Util.h>

#include <proxies/NVNGX_Proxy.h>
#include <hooks/D3D12_Hooks.h>
#include <gpu_time/GpuTime_Dx12.h>
#include <dlssnr/DlssNr_TimingTrust.h>

#include <mutex>
#include <map>
#include <chrono>
#include <cmath>
#include <vector>
#include <format>
#include <algorithm>
#include <cstring>
#include "precompile/DlssNr_Shader.h"
#include "precompile/DlssNr_ShaderPrep.h"
#include "../output_scaling/OS_Dx12.h"

namespace
{
// NGX result codes, by name.
//
// A user's log recently read "init 0x-452FFFFF", which is an int formatted as hex and is
// undiagnosable by anyone. It was 0xBAD00001, FeatureNotSupported -- a complete answer, printed as
// noise. Names cost nothing and turn a bug report into a diagnosis.
const char* NgxResultName(unsigned int r)
{
    switch (r)
    {
    case 0x1:
        return "Success";
    case 0xBAD00001:
        return "FAIL_FeatureNotSupported";
    case 0xBAD00002:
        return "FAIL_PlatformError";
    case 0xBAD00003:
        return "FAIL_FeatureAlreadyExists";
    case 0xBAD00004:
        return "FAIL_FeatureNotFound";
    case 0xBAD00005:
        return "FAIL_InvalidParameter";
    case 0xBAD00006:
        return "FAIL_ScratchBufferTooSmall";
    case 0xBAD00007:
        return "FAIL_NotInitialized";
    case 0xBAD00008:
        return "FAIL_UnsupportedInputFormat";
    case 0xBAD00009:
        return "FAIL_RWFlagMissing";
    case 0xBAD0000A:
        return "FAIL_MissingInput";
    case 0xBAD0000B:
        return "FAIL_UnableToInitializeFeature";
    case 0xBAD0000C:
        return "FAIL_OutOfDate";
    case 0xBAD0000D:
        return "FAIL_OutOfGPUMemory";
    case 0xBAD0000E:
        return "FAIL_UnsupportedFormat";
    case 0xBAD0000F:
        return "FAIL_UnableToWriteToAppDataPath";
    case 0xBAD00010:
        return "FAIL_UnsupportedParameter";
    case 0xBAD00011:
        return "FAIL_Denied";
    case 0xBAD00012:
        return "FAIL_NotImplemented";
    default:
        return "unknown";
    }
}

// Does the driver's own nvngx.dll dispatch Neural Rendering?
//
// The trick is that correct parameters are not needed to find out, because the KIND of failure is
// the answer. A dispatcher that has never heard of feature 18 rejects it before looking at anything:
//
//   FeatureNotFound / FeatureNotSupported / NotImplemented -- the driver does not route it, and the
//       forwarder is necessary rather than merely tolerated.
//   MissingInput / InvalidParameter / UnsupportedParameter -- the driver DOES route it. It reached
//       the feature, which then complained about the arguments. That is the win: it means the whole
//       forwarder, and the per-game copy of the model, can go.
//   Success -- better still, though not expected from an empty parameter block.
//
// Once per session, and only when asked for.
void ProbeProxyDispatch(ID3D12GraphicsCommandList* cmdList)
{
    static bool done = false;

    if (done)
        return;

    done = true;

    if (!NVNGXProxy::IsDx12Inited())
    {
        LOG_INFO("DLSS-NR proxy probe: the driver's nvngx is not initialised here, nothing to ask");
        return;
    }

    const auto allocate = NVNGXProxy::D3D12_AllocateParameters();
    const auto destroy = NVNGXProxy::D3D12_DestroyParameters();
    const auto create = NVNGXProxy::D3D12_CreateFeature();
    const auto release = NVNGXProxy::D3D12_ReleaseFeature();

    if (allocate == nullptr || create == nullptr)
    {
        LOG_INFO("DLSS-NR proxy probe: the driver's nvngx does not export what the probe needs");
        return;
    }

    NVSDK_NGX_Parameter* params = nullptr;

    if (allocate(&params) != NVSDK_NGX_Result_Success || params == nullptr)
    {
        LOG_INFO("DLSS-NR proxy probe: could not allocate a parameter block");
        return;
    }

    // Feature 18, and a feature that certainly does not exist, asked the same way.
    //
    // A single result cannot answer this. "UnableToInitializeFeature" for 18 looks like the
    // dispatcher having found the feature and failed to start it on an empty parameter block -- but
    // it might equally be what this dispatcher says about anything it cannot set up. The control
    // settles it: if a nonsense id comes back differently, the difference is knowledge of feature
    // 18. If both come back the same, the first result meant nothing.
    NVSDK_NGX_Handle* handle = nullptr;
    const auto result = (unsigned int) create(cmdList, (NVSDK_NGX_Feature) 18, params, &handle);

    if (handle != nullptr && release != nullptr)
        release(handle);

    NVSDK_NGX_Handle* controlHandle = nullptr;
    const auto control = (unsigned int) create(cmdList, (NVSDK_NGX_Feature) 200, params, &controlHandle);

    if (controlHandle != nullptr && release != nullptr)
        release(controlHandle);

    LOG_INFO("DLSS-NR proxy probe: feature 18 -> 0x{:X} ({}), control feature 200 -> 0x{:X} ({})", result,
             NgxResultName(result), control, NgxResultName(control));

    const bool rejectedOutright = result == 0xBAD00004 || result == 0xBAD00001 || result == 0xBAD00012;

    if (result == control)
        LOG_INFO("DLSS-NR proxy probe: both answers identical, so this says nothing about feature 18 "
                 "-- the driver treats it exactly as it treats a feature that does not exist");
    else if (rejectedOutright)
        LOG_INFO("DLSS-NR proxy probe: feature 18 is rejected outright -- the driver does not route "
                 "it and the forwarder is required");
    else
        LOG_INFO("DLSS-NR proxy probe: feature 18 answers differently from a nonexistent one, so the "
                 "driver knows it -- the forwarder and the per-game model copy could both go");

    if (destroy != nullptr)
        destroy(params);
}

// Everything the model is reached through. The snippet refuses callers whose module path does not
// contain "nvngx.dll", so the calls are made from a small library named for exactly that reason and
// shipped beside OptiScaler; see nvngx.dll_dlssnr.dll.
using PFN_NrCreate = void*(__cdecl*) (const wchar_t*, const wchar_t*, ID3D12Device*, ID3D12GraphicsCommandList*, void*,
                                      unsigned int, unsigned int, int, float, int, float, float, float, int, int);
using PFN_NrEvaluate = int(__cdecl*)(ID3D12GraphicsCommandList*, void*, void*, ID3D12Resource*, ID3D12Resource*,
                                     ID3D12Resource*, ID3D12Resource*, unsigned int, unsigned int, unsigned int,
                                     unsigned int, int, int, float, int, float, float, float, int, float, float);
using PFN_NrRelease = void(__cdecl*)(void*);
using PFN_NrSetExtras = void(__cdecl*)(void*, float, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*, unsigned int,
                                       unsigned int, unsigned int, unsigned int);
using PFN_NrSetFloatSlot = void(__cdecl*)(int);
using PFN_NrProbeFloat = void(__cdecl*)(void*, const char*, float, int);

// One per back buffer, so an allocator is never reset while its frame is still in flight.

struct NrState
{
    HMODULE forwarder = nullptr;
    PFN_NrCreate create = nullptr;
    PFN_NrEvaluate evaluate = nullptr;
    PFN_NrRelease release = nullptr;
    PFN_NrSetExtras setExtras = nullptr;
    PFN_NrSetFloatSlot setFloatSlot = nullptr;
    PFN_NrProbeFloat probeFloat = nullptr;
    bool floatSlotKnown = false;

    // The scaling-ratio probe, resolved alongside the other forwarder entry points.
    int (*queryRatio)(const wchar_t*, void*, unsigned int, float*) = nullptr;
    const int* lastRatioStage = nullptr;
    int* lastInit = nullptr;
    int* lastCreate = nullptr;

    NVSDK_NGX_Parameter* capabilityParams = nullptr;
    void* feature = nullptr;
    bool featurePendingSubmission = false;
    unsigned long long featureCreateEpoch = 0;

    // A feature per extra pass, each with its own temporal history.
    //
    // One feature run three times in a frame is told three frames passed with nothing moving between
    // them, so its history fights every pass after the first -- which is what "loses detail on later
    // passes" was. Separate features each see one frame per frame, which is the contract they were
    // built for.
    //
    // It is also the only reading that fits the one clue we have about how this is done elsewhere:
    // that implementation's memory grows with the pass count, and reusing a single feature cannot do
    // that. A feature apiece can, because each carries its own history.
    //
    // Indexed by pass, so [0] is unused and the first extra pass is [1]. Wasting one pointer keeps
    // every index here equal to the pass number it belongs to. Extra features are created on a
    // build-only invocation and first evaluated on a later command list.
    void* passFeature[DlssNr::MaxPassCount] = {};
    bool passNeedsReset[DlssNr::MaxPassCount] = {};
    bool passCreateFailed[DlssNr::MaxPassCount] = {};
    bool passPendingSubmission[DlssNr::MaxPassCount] = {};
    unsigned long long passCreateEpoch[DlssNr::MaxPassCount] = {};

    // The model cannot read and write one resource, so the frame is staged through these.
    ID3D12Resource* colorCopy = nullptr;
    ID3D12Resource* output = nullptr;

    // The second half of the model-output ping-pong. The base proxy stays immutable: pass 0 writes
    // output (A), pass 1 writes this (B), and pass 2 writes A again. Only the final answer is composed.
    ID3D12Resource* passScratch = nullptr;
    bool passScratchFailed = false;

    // The frame as the upscaler wrote it. The resolve adds the model's edit to this rather than
    // reconstructing it by inverting the tone curve, which is what turned every light in the frame into
    // a string of coloured cells.
    ID3D12Resource* hdrCopy = nullptr;

    // Compact origin-zero pre-SR image, only needed when Color has allocation padding. All codec,
    // hold and capture paths then see the real raster. UAV at rest, retired with the scratch set.
    ID3D12Resource* activeColor = nullptr;

    // The frame shrunk for the model, when it is working below full resolution.
    ID3D12Resource* colorSmall = nullptr;

    // Supersampling (working scale > 1): the Output Scaling upsampler used to enlarge the proxy to the
    // model's larger-than-native working size with a real filter instead of the box minifier. Created
    // lazily on the first super-native frame, released in Shutdown; sizes from the resources each call,
    // so a resolution change needs no rebuild.
    OS_Dx12* superUp = nullptr;

    // Supersampling down-leg: the native-sized buffer the Nx model answer is averaged into, and the
    // downscaler that does it. With superUp this lands the super-native answer at native for a 1:1
    // composite (no aliased minify). nrScaler is the filter both were built with, so a changed
    // DlssNrScalingDownscaler rebuilds them.
    ID3D12Resource* outputNative = nullptr;
    OS_Dx12* superDown = nullptr;
    Scaler nrScaler = Scaler::Count;

    // The same for the up-leg's filter. Only superUp reads it -- superDown is shrinking and takes
    // nrScaler -- but both are torn down together, so it sits beside nrScaler and is compared with it.
    Upsampler nrUpsampler = Upsampler::Count;

    // Below full resolution (working scale < 1): the model's answer and the small proxy it is measured
    // against, both enlarged to frame size with the chosen Upscaler before the resolve reads them.
    //
    // The resolve used to do this itself, by sampling two small textures bilinearly at full-size UVs --
    // so the filter the panel offered did nothing at all where a model actually runs small, which is
    // every reduced-resolution setting (found 2026-09-22, "50% and every filter measures the same").
    // Both legs take the SAME filter: the edit handed to the composition is model minus proxy, and two
    // different filters would leave the difference between them inside that edit.
    ID3D12Resource* editNative = nullptr;
    ID3D12Resource* proxyNative = nullptr;
    OS_Dx12* editUp = nullptr;
    OS_Dx12* proxyUp = nullptr;

    // Frame hold (design/frame-hold.md): a persistent copy of the output taken on hold-on and restored
    // over the live output before the encode reads it while held, so a setting change re-renders the
    // same frame. heldWhitePoint is the snapshot used while held -- measurement is suspended.
    ID3D12Resource* heldColor = nullptr;
    bool heldActive = false;
    unsigned int heldWidth = 0;
    unsigned int heldHeight = 0;
    DXGI_FORMAT heldFormat = DXGI_FORMAT_UNKNOWN;
    float heldWhitePoint = 1.0f;

    unsigned int workWidth = 0;
    unsigned int workHeight = 0;

    // The white point meter.
    //
    // A 64x64 grid of tile luminances, copied to a readback buffer and looked at a few frames later.
    // Four buffers deep rather than one: the copy is recorded into the game's own command list and
    // there is no fence here to wait on, so the only thing making a read safe is that the frame it
    // came from is long retired. Three frames of distance is what the meter this replaces used.
    //
    // A stale read costs a slightly wrong float that the average below absorbs. A read of a buffer
    // still being written would cost the same, which is why the value is smoothed rather than used
    // raw.
    ID3D12Resource* meter = nullptr;
    ID3D12Resource* meterReadback[4] = {};

    // The calibration grid: what scale the game's buffer is on, measured from the untouched copy.
    // Its own surface and ring rather than sharing the meter's, because the two run at different
    // sizes -- the meter fetches one texel and this reads the whole frame.
    ID3D12Resource* calib = nullptr;
    ID3D12Resource* calibReadback[4] = {};
    unsigned long long calibFrames = 0;

    // The last few answers, so the menu can say how settled the number is. A suggestion taken during
    // a fade or a loading screen is worth less than one taken while standing still, and the spread
    // across recent frames is what tells them apart.
    static constexpr unsigned int kCalibHistory = 32;
    float calibHistory[kCalibHistory] = {};
    unsigned int calibCount = 0;
    float calibSuggestion = 0.0f;
    float calibSteadiness = 0.0f;
    bool calibUsable = false;
    const char* calibWhy = "measuring...";
    bool calibPassthrough = false;

    // Auto brightness / Auto contrast (2026-09-23). A 64x64 grid of tile MEANS of the frame arriving at
    // the pass, from the meter's own tile-mean branch dispatched over the whole grid, read back four
    // frames later on its own ring, and turned into the Brightness and Contrast the resolve uses.
    //
    // Measured on the way in, never on the way out: the trim is applied at the end of the resolve, so
    // measuring the finished picture would chase its own correction. What remains is the game's own
    // temporal history carrying some of last frame's lift into this one -- negative feedback (brighter
    // in, less lift), which the slow smoothing below lets settle instead of swinging.
    ID3D12Resource* tone = nullptr;
    ID3D12Resource* toneReadback[4] = {};
    unsigned long long toneFrames = 0;
    float autoBrightness = 1.0f;
    float autoContrast = 1.0f;
    bool toneSettled = false; // the first real reading is taken whole rather than eased into
    std::chrono::steady_clock::time_point toneLastUpdate {};
    // What the last reading saw, for the panel: the frame's average and its dark-to-light spread.
    float toneMean = 0.0f;
    float toneSpreadStops = 0.0f;

    // Whether the frame that filled each readback slot actually had an exposure texture bound.
    //
    // The meter writes tile 0 from whatever sits in the exposure slot, and DispatchPass substitutes
    // the source picture when nothing is bound -- so without this the "exposure" read back is the red
    // channel of the frame's top-left pixel. In Cyberpunk, which supplies no exposure texture, that
    // pixel is scene content: it moved by up to 272x between consecutive frames and drove the white
    // point from 0.18 to 74. That is the whole frame flashing in luminance.
    //
    // The grid is read three frames after it is written, so the flag has to travel with the slot
    // rather than being asked of the current frame.
    bool meterExposureValid[4] = {};
    unsigned int meterSlot = 0;
    unsigned long long meterFrames = 0;

    // Whether the setting was on last frame, so the off->on edge can be caught.
    //
    // Deliberately the SETTING and not `wantExposure`: the texture itself comes and goes between
    // frames and holding the last good value across those gaps is the whole point of the field below.
    // Only the user turning the option back on means "anything held is from an unknown time ago".
    bool exposureSettingWasOn = false;

    // The game's exposure, as last read back, and the pre-exposure that goes with it. Held rather
    // than defaulted: the texture comes and goes between frames and a fallback to 1.0 on the gaps
    // would be a flicker source.
    float gameExposure = 0.0f;
    float gamePreExposure = 1.0f;

    // What the game OFFERS, as opposed to what has been read. Recorded from the parameter block every
    // frame whether or not the setting is on, and deliberately so: the menu has to be able to answer
    // "would this do anything here?" before the user turns it on, and reading a pointer for null costs
    // nothing. Whether it was ever offered is kept separately from whether it was offered this frame,
    // because games drop it on transitions -- GTA V dropped it three times in one session -- and one
    // absent frame is not the same answer as never.
    bool exposureOfferedNow = false;
    bool exposureEverOffered = false;
    unsigned long long exposureFrames = 0;

    // Cloned unconditionally when running at present, and only for typeless formats otherwise.
    ID3D12Resource* depthClone = nullptr;
    ID3D12Resource* motionClone = nullptr;

    // The constant-depth probe's surface. Separate from depthClone on purpose: it is defined by
    // never having been written, and sharing a surface with a mode that writes would destroy that.
    ID3D12Resource* depthConstant = nullptr;

    unsigned int width = 0;
    unsigned int height = 0;
    bool beforeUpscale = false;
    bool reset = true;

    // Dimensions of the guides as the upscaler handed them over, kept for the present path, which runs
    // long after that call has returned.
    unsigned int guideWidth = 0;
    unsigned int guideHeight = 0;

    // How the game encodes its guides, as the game itself reports it. Captured with the guides, since
    // the finished-frame path runs long after the upscaler's call has returned.
    bool guideDepthInverted = false;
    float guideMvScaleX = 1.0f;
    float guideMvScaleY = 1.0f;

    // The values each live feature was created with. Preset and style may differ per layer; the
    // remaining strengths are intentionally shared by the stack.
    unsigned int builtPreset[DlssNr::MaxPassCount] = {};
    float builtIntensity = 0.0f;
    unsigned int builtStyle[DlssNr::MaxPassCount] = {};
    float builtLocalStructure = 0.0f;
    float builtLocalTone = 0.0f;
    float builtSkinStructure = 0.0f;
    bool builtAutoMask = false;
    bool builtUICorrection = true;
    unsigned long long settledAt = 0;

    // Image Clean Up (see the block in dlssnr.hlsl). Three 64x64 halo grids, reduced from what the resolve
    // itself measured per pixel against one band and one mask -- the composed picture before the clean up
    // (0), after it (1), and the model's own change laid on the frame (2) -- each read back four frames
    // later on its own ring, like the tone grid. Auto drives its strength from grid 0.
    ID3D12Resource* haloGrid[3] = {};
    ID3D12Resource* haloReadback[3][4] = {};
    unsigned long long haloFrames = 0;
    float haloBefore = -1.0f; // stops, -1 until a reading lands
    float haloAfter = -1.0f;
    float haloModel = -1.0f;
    float haloSmoothed = -1.0f;
    int haloJumpHeld = 0;
    float cleanAutoStrength = 0.0f;
    float cleanApplied = 0.0f; // the strength the last resolve ran with, 0 when off
    bool cleanAutoSettled = false;
    std::chrono::steady_clock::time_point haloLastUpdate {};
    unsigned long long cleanHoldUntil = 0; // Auto holds still until this frame after a cut

    // The clean up's mask history: two full-size R16G16B16A16 surfaces (mask, log depth, and the mask-
    // weighted excess before and after the clean up), one read and one written each frame, swapping; and
    // one R16 surface for the model reading.
    ID3D12Resource* cleanHistory[2] = {};
    ID3D12Resource* cleanModel = nullptr;
    // The prep surface (DlssNrMode_CleanupPrep): the frame's size plus a row of 8x8 blocks under it, RGBA16F.
    ID3D12Resource* cleanPrep = nullptr;
    D3D12_RESOURCE_STATES cleanPrepState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    unsigned int cleanPrepW = 0;
    unsigned int cleanPrepH = 0;
    D3D12_RESOURCE_STATES cleanModelState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES cleanHistoryState[2] = { D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
    unsigned int cleanHistoryW = 0;
    unsigned int cleanHistoryH = 0;
    unsigned int cleanHistoryIndex = 0;
    bool cleanHistoryValid = false;

    // Once something fails there is no recovering it mid-session, and retrying every frame turns a
    // failure into a crash. It stays off and says why.
    bool failed = false;
    const char* reason = "";
};

NrState g_nr;
std::unique_ptr<DlssNr_Dx12> g_compose;

// What the pass costs on the GPU, for the breakdown in the overlay.
std::unique_ptr<GpuTime_Dx12> g_gpuTime;

// A second timer, around the model's evaluate and nothing else.
//
// The first one brackets the whole pass, which is the number the menu shows and the right one for
// "what does this feature cost". It is the wrong number for deciding what to optimise: the 4.10 ms at
// full model resolution and 2.24 ms at half were both whole-pass, and both included this pass's own
// encode and resolve at DISPLAY resolution plus the guide copies, none of which move when the model's
// resolution does. Fitting a fixed term to those two points therefore attributes our own unchanging
// work to NGX overhead.
//
// Splitting them says how much of the pass is the model and how much is ours -- and ours is the half
// we can actually do something about.
std::unique_ptr<GpuTime_Dx12> g_ngxTime;
std::optional<double> g_lastNgxTime;

// A third, around the resolve (the composition) alone, so what Image Clean Up adds to it can be read off
// the cost line with it on and off: it runs inside the resolve rather than as a pass of its own.
std::unique_ptr<GpuTime_Dx12> g_composeTime;
std::optional<double> g_lastComposeTime;
std::optional<double> g_lastGpuTime;

// Whether g_lastGpuTime is fit for the panel at all; see DlssNr_TimingTrust.h.
NrTimingTrust g_timingTrust;

// Writes matched before/after frames on request, so comparisons stop depending on video.
capture::FrameCapture g_capture;

// One capture happens on its own each session, so there is always a fresh sample without anyone having
// to remember to ask. Started after the scene has had a moment to settle: the first frames after a
// feature is built carry its reset, and are not representative of anything.
constexpr unsigned long long kAutoCaptureAfterFrames = 180;
bool g_autoCaptureDone = false;

// Cleared once per run, so a session's captures are its own and nothing accumulates across launches.
void ClearCaptureDirectory()
{
    static bool cleared = false;

    if (cleared)
        return;

    cleared = true;

    std::error_code ec;
    const auto dir = Util::DllPath().remove_filename() / "dlssnr-capture";

    if (std::filesystem::exists(dir, ec))
    {
        std::filesystem::remove_all(dir, ec);

        if (ec)
            LOG_WARN("DLSS-NR could not clear {}: {}", dir.string(), ec.message());
    }
}

unsigned long long g_frames = 0;

// A capture requested from outside the game: when the render path has no fence of its own, the write
// waits until this frame count, by which point the GPU is certainly past the copies.
unsigned long long g_captureWriteAtFrame = 0;

// Dropping a file named dlssnr-capture.trigger beside OptiScaler requests a capture, so a session can
// be asked for one from outside the game -- no alt-tab, no menu. Checked once a second, effectively.
cleancapture::CleanCapture g_cleanCapture;
ID3D12Resource* g_cleanCaptureBefore = nullptr;

// Image Clean Up's one-frame capture, from any of four places: Ctrl+Shift+F12, the panel's button,
// [DlssNr] CleanUpCapture=true (set back to false and saved at once, so a live reload fires it once), or a
// file named dlssnr-cleanup-capture.trigger beside OptiScaler.
void CheckCleanCaptureTrigger()
{
    static bool keyWasDown = false;
    const bool keyDown = (GetAsyncKeyState(VK_F12) & 0x8000) != 0 && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 &&
                         (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (keyDown && !keyWasDown)
    {
        g_cleanCapture.request();
        LOG_INFO("DLSS-NR image clean up capture requested (Ctrl+Shift+F12)");
    }
    keyWasDown = keyDown;

    auto* cfg = Config::Instance();
    if (cfg->DlssNrCleanUpCapture.value_or_default())
    {
        cfg->DlssNrCleanUpCapture = false;
        cfg->SaveIni();
        g_cleanCapture.request();
        LOG_INFO("DLSS-NR image clean up capture requested ([DlssNr] CleanUpCapture)");
    }

    if ((g_frames % 60) == 0)
    {
        std::error_code ec;
        const auto trigger = Util::DllPath().remove_filename() / "dlssnr-cleanup-capture.trigger";
        if (std::filesystem::exists(trigger, ec))
        {
            std::filesystem::remove(trigger, ec);
            g_cleanCapture.request();
            LOG_INFO("DLSS-NR image clean up capture requested by trigger file");
        }
    }
}

void CheckCaptureTrigger()
{
    if ((g_frames % 60) != 0)
        return;

    std::error_code ec;
    const auto trigger = Util::DllPath().remove_filename() / "dlssnr-capture.trigger";

    if (std::filesystem::exists(trigger, ec))
    {
        std::filesystem::remove(trigger, ec);
        DlssNr::RequestCapture(capture::kMaxFrames);
        LOG_INFO("DLSS-NR capture requested by trigger file");
    }
}

// The encoded mean is aimed here. Mid-grey rather than anything brighter: the model has to see both the
// shadow detail it might lift and the highlights it must not blow out.
constexpr float kTargetEncodedMean = 0.45f;

// How fast the derived value follows the scene. Readings arrive a few times a second, and an exposure
// that lunges at every cut is worse than one that arrives a moment late.
constexpr float kWhitePointBlend = 0.25f;

// Recomputes the white point from a measured mean. Inverting the encode for the white point that puts
// that mean at the target gives wp = mean * (1 - t^g) / t^g.
float WhitePointForMean(float meanLuma)
{
    const float encoded = powf(kTargetEncodedMean, 2.2f);
    const float ratio = encoded / (1.0f - encoded);
    const float wp = meanLuma / ratio;
    // A black frame between scenes would otherwise drive this to zero and divide the next frame by it.
    return wp < 0.01f ? 0.01f : (wp > 10000.0f ? 10000.0f : wp);
}

std::filesystem::path g_dllDir;

// Another add-on already running this same model in this process, or nullptr.
//
// Checked by module name because that is what ReShade gives us: it LoadLibrary's its add-ons, so
// they are ordinary loaded modules. Both of these apply DLSS 5 Neural Rendering at the ReShade
// stage, downstream of this pass.
//
// Only worth checking at create time. ReShade loads its add-ons during device creation, long before
// a game has rendered enough for this pass to be built, so one appearing later is not a case worth
// carrying complexity for.
const char* ConflictingNrAddon()
{
    struct Addon
    {
        const wchar_t* module;
        const char* name;
    };

    // Only NR *consumers* belong here -- add-ons that apply Neural Rendering themselves. Two
    // consumers denoise the same frame twice. The DLSS5 Feeder (dlss5-feed.addon64) is NOT one:
    // it is a *producer* -- on a game with no native DLSS it synthesises a DLSS DLAA evaluate from
    // ReShade depth + motion vectors for a consumer to hook, and this pass is that consumer. So the
    // feeder is deliberately absent from this list; refusing it would refuse the one thing that
    // feeds us on no-native-DLSS games. Do not add it back.
    //
    // The 32-bit variant is listed for completeness: OptiScaler is x64, so it will never share a
    // process with it, but naming it costs nothing.
    static constexpr Addon kAddons[] = {
        { L"renodx-dlss5.addon64", "renodx-dlss5.addon64" },
        { L"renodx-dlss5.addon32", "renodx-dlss5.addon32" },
    };

    for (const auto& addon : kAddons)
    {
        if (GetModuleHandleW(addon.module) != nullptr)
            return addon.name;
    }

    return nullptr;
}

// Loads the forwarder that owns the calls into the snippet.
bool EnsureForwarder()
{
    if (g_nr.forwarder != nullptr)
        return g_nr.create != nullptr;

    if (g_dllDir.empty())
        g_dllDir = Util::DllPath().remove_filename();

    // Beside OptiScaler first, then beside the executable: someone dropping this into a game folder may
    // reasonably put it in either place.
    auto found = Util::FindFilePath(g_dllDir, "nvngx.dll_dlssnr.dll");

    if (!found.has_value())
        found = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx.dll_dlssnr.dll");

    if (!found.has_value())
    {
        LOG_ERROR("nvngx.dll_dlssnr.dll not found beside OptiScaler ({}) or the game executable", g_dllDir.string());
        g_nr.reason = "nvngx.dll_dlssnr.dll is missing";
        return false;
    }

    // FindFilePath hands back the file itself, not the directory holding it.
    const auto path = found.value();
    g_nr.forwarder = LoadLibraryW(path.wstring().c_str());

    if (g_nr.forwarder == nullptr)
    {
        LOG_ERROR("nvngx.dll_dlssnr.dll found at {} but would not load, error {}", path.string(), GetLastError());
        g_nr.reason = "nvngx.dll_dlssnr.dll would not load";
        return false;
    }

    g_nr.queryRatio = (int (*)(const wchar_t*, void*, unsigned int, float*)) GetProcAddress(
        g_nr.forwarder, "dlssnr_query_scaling_ratio");
    g_nr.lastRatioStage = (const int*) GetProcAddress(g_nr.forwarder, "dlssnr_last_ratio_stage");

    g_nr.create = (PFN_NrCreate) GetProcAddress(g_nr.forwarder, "dlssnr_call_create");
    g_nr.evaluate = (PFN_NrEvaluate) GetProcAddress(g_nr.forwarder, "dlssnr_call_evaluate");
    g_nr.release = (PFN_NrRelease) GetProcAddress(g_nr.forwarder, "dlssnr_call_release");
    // Optional: an older forwarder simply lacks it, and the model runs as before.
    g_nr.setExtras = (PFN_NrSetExtras) GetProcAddress(g_nr.forwarder, "dlssnr_call_set_extras");
    g_nr.setFloatSlot = (PFN_NrSetFloatSlot) GetProcAddress(g_nr.forwarder, "dlssnr_call_set_float_slot");
    g_nr.probeFloat = (PFN_NrProbeFloat) GetProcAddress(g_nr.forwarder, "dlssnr_call_probe_float");
    g_nr.lastInit = (int*) GetProcAddress(g_nr.forwarder, "dlssnr_call_last_init");
    g_nr.lastCreate = (int*) GetProcAddress(g_nr.forwarder, "dlssnr_call_last_create");

    if (g_nr.create == nullptr || g_nr.evaluate == nullptr)
    {
        g_nr.reason = "the forwarder is missing its exports";
        return false;
    }

    LOG_INFO("DLSS-NR forwarder loaded from {}", path.string());
    return true;
}

// The model needs the driver core's own capability block: it carries the snippet and preset callbacks a
// feature expects at create time, which a freshly allocated block does not have.
void DiscoverFloatSlot(NVSDK_NGX_Parameter* params);
void ReportScalingRatios();

bool EnsureCapabilityParams(ID3D12Device* device)
{
    if (g_nr.capabilityParams != nullptr)
        return true;

    if (!NVNGXProxy::IsDx12Inited() && !NVNGXProxy::InitDx12(device))
    {
        g_nr.reason = "the NGX core would not initialise";
        return false;
    }

    if (NVNGXProxy::D3D12_GetCapabilityParameters() == nullptr)
    {
        g_nr.reason = "the NGX core has no capability parameters";
        return false;
    }

    if (NVNGXProxy::D3D12_GetCapabilityParameters()(&g_nr.capabilityParams) != NVSDK_NGX_Result_Success ||
        g_nr.capabilityParams == nullptr)
    {
        g_nr.capabilityParams = nullptr;
        g_nr.reason = "the NGX core refused its capability parameters";
        return false;
    }

    // Before anything is written to it, work out where this block keeps floats.
    DiscoverFloatSlot(g_nr.capabilityParams);

    // Ask the model what scaling ratio it wants, once, for every quality level it might accept.
    //
    // Read-only and answered before any feature exists. The point is to find out whether NVIDIA's own
    // performance mode for this model is reachable: the snippet has ComputeScalingRatioCommon and the
    // kernel table has _ds, _upsample and _upsample_tilesync variants of every fused Swin block, which
    // together suggest the model can run its interior below display resolution natively -- rather than
    // being handed a picture we shrank ourselves, which costs an extra resample of the edit on the way
    // back and quantises the Swin grid to a lattice we chose rather than the one it was trained on.
    ReportScalingRatios();
    return true;
}

// What the model says it wants to run at, per quality level. Logged once, used for nothing yet.
//
// Answered by the snippet's own callback rather than chosen by us. If it answers, NVIDIA ships a
// performance mode for Neural Rendering and the resolution slider is a worse hand-rolled version of
// it. If it does not, the slider is all there is and that is worth knowing too.
void ReportScalingRatios()
{
    if (g_nr.queryRatio == nullptr || g_nr.capabilityParams == nullptr)
        return;

    auto snippet = Util::FindFilePath(g_dllDir, "nvngx_dlssnr.dll");

    if (!snippet.has_value())
        snippet = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssnr.dll");

    if (!snippet.has_value())
        return;

    static const char* kNames[] = { "MaxPerf", "Balanced", "MaxQuality", "UltraPerformance", "UltraQuality", "DLAA" };

    char line[512] = {};
    size_t used = 0;
    bool any = false;

    for (unsigned int q = 0; q < 6; ++q)
    {
        float ratio = -1.0f;
        const int rc = g_nr.queryRatio(snippet->wstring().c_str(), g_nr.capabilityParams, q, &ratio);
        int written = 0;

        if (rc == 1)
        {
            any = true;
            written = snprintf(line + used, sizeof(line) - used, "%s=%.4f ", kNames[q], ratio);
        }
        else if (rc == -1)
        {
            written = snprintf(line + used, sizeof(line) - used, "%s=refused ", kNames[q]);
        }

        if (written > 0)
            used += (size_t) written;
    }

    if (any)
        LOG_INFO("DLSS-NR the model's own scaling ratios: {}", line);
    else
        LOG_INFO("DLSS-NR scaling ratio callback not published by this snippet (stage {})",
                 g_nr.lastRatioStage != nullptr ? *g_nr.lastRatioStage : -1);
}

// Works out which vtable slot this parameter block keeps floats in, by writing a known value through
// each candidate and asking for it back through the header's typed getter. Only a slot that returns the
// value it was given is accepted.
//
// Slot 1 is where the public header declares the float overload, so it is tried first and wins wherever
// that assumption holds. It does not hold for the driver's own block: every float written there reads
// back as FAIL_UnsupportedParameter while every uint lands, which is why intensity, local structure,
// local tone and skin structure never did anything.
void DiscoverFloatSlot(NVSDK_NGX_Parameter* params)
{
    if (g_nr.floatSlotKnown || params == nullptr || g_nr.probeFloat == nullptr || g_nr.setFloatSlot == nullptr)
        return;

    g_nr.floatSlotKnown = true;

    static const char* kProbeKey = "DLSSNR.OptiScalerFloatProbe";
    static const int kCandidates[] = { 1, 2, 5, 6, 7, 4, 3, 0 };
    const float expected = 0.375f; // exact in binary, so the round trip is exact or it is wrong

    for (int slot : kCandidates)
    {
        float readBack = 0.0f;
        g_nr.probeFloat(params, kProbeKey, expected, slot);

        if (params->Get(kProbeKey, &readBack) == NVSDK_NGX_Result_Success && readBack == expected)
        {
            g_nr.setFloatSlot(slot);
            LOG_INFO("DLSS-NR float parameters go through vtable slot {}", slot);
            return;
        }
    }

    LOG_ERROR("DLSS-NR could not find the float setter: intensity, local structure, local tone and skin "
              "structure will have no effect. The uint parameters still apply.");
}

// Switching inject points changes the surface format underneath the scratch set: the finished frame
// works in the swapchain's format, the pre-frame-generation path in the upscaler's. A stale set either
// clamps linear HDR into an 8-bit texture -- wrong brightness until something forces a rebuild -- or
// hands CopyResource mismatched formats, which fails silently and makes the whole pass appear to do
// nothing. So the set is torn down whenever the format it was built for is not the format needed now.
// Retired model features and surfaces are parked and freed a comfortable number of evaluates later.
// Releasing them immediately was the device hang: with frame generation the GPU runs several frames
// behind, this work rides the game's own queue that no module fence covers, and an NGX feature or
// scratch texture freed under in-flight work kills the device.
struct NrRetired
{
    void* feature = nullptr;
    ID3D12Resource* resource = nullptr;
    // A scaling filter the GPU may still be reading from. Changing the Upscaler used to delete its
    // pipeline and descriptors on the spot, while frames using them were still in flight: the device
    // hung a few seconds later, every time, the moment the filter was changed (Shadow of the Tomb
    // Raider, 2026-09-22). It waits its turn here like everything else now.
    OS_Dx12* scaler = nullptr;
    int framesLeft = 32;
};

std::vector<NrRetired> g_nrRetired;

// Video memory. Cyberpunk 2077 on a 12 GB laptop GPU (path tracing, Ray Reconstruction, DLSS Frame
// Generation) sat at 9.8 GB; switching DLSS 5 from before Ray Reconstruction to after it rebuilt every
// feature at 2560x1600 while the old ones were still parked, use reached 11.15 GB, and the device was
// lost one second later (2026-09-16). Aftermath called the other crashes "Hung" and one a DMA page fault
// -- what a GPU does when it is made to evict. So features are only built when they fit.
std::atomic<uint64_t> g_vramUsage { 0 };
std::atomic<uint64_t> g_vramBudget { 0 };
IDXGIAdapter3* g_vramAdapter = nullptr;
LUID g_vramLuid {};

// Reads the process's local video memory use and budget on the device's adapter. Cheap: a kernel query,
// no allocation after the adapter is found once.
bool ReadVideoMemory(ID3D12Device* device, uint64_t& usage, uint64_t& budget)
{
    if (device == nullptr)
        return false;

    const LUID luid = device->GetAdapterLuid();
    if (g_vramAdapter == nullptr || luid.LowPart != g_vramLuid.LowPart || luid.HighPart != g_vramLuid.HighPart)
    {
        if (g_vramAdapter != nullptr)
        {
            g_vramAdapter->Release();
            g_vramAdapter = nullptr;
        }

        IDXGIFactory4* factory = nullptr;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || factory == nullptr)
            return false;

        factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&g_vramAdapter));
        factory->Release();

        if (g_vramAdapter == nullptr)
            return false;

        g_vramLuid = luid;
    }

    DXGI_QUERY_VIDEO_MEMORY_INFO info {};
    if (FAILED(g_vramAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)) || info.Budget == 0)
        return false;

    usage = info.CurrentUsage;
    budget = info.Budget;
    g_vramUsage = usage;
    g_vramBudget = budget;
    return true;
}

// What one NR feature is taken to cost, per model pixel. Measured on that Cyberpunk run: two features and
// their surfaces at 2560x1600 added about 1.3 GB, so a little under 150 bytes a pixel each; rounded up.
constexpr uint64_t kFeatureBytesPerPixel = 192;

// Whether something needing `need` more bytes fits under the budget with room to spare: 5% of the budget,
// at least 384 MB. Unknown (the query failed) counts as fitting, so a driver without the query is not
// stopped from running at all.
uint64_t StandardReserve(uint64_t budget) { return std::max<uint64_t>(budget / 20, 384ull << 20); }

bool FitsInVideoMemory(ID3D12Device* device, uint64_t need, uint64_t* usageOut = nullptr, uint64_t* budgetOut = nullptr)
{
    uint64_t usage = 0, budget = 0;
    if (!ReadVideoMemory(device, usage, budget))
        return true;

    if (usageOut != nullptr)
        *usageOut = usage;
    if (budgetOut != nullptr)
        *budgetOut = budget;

    return usage + need + StandardReserve(budget) <= budget;
}

// How long the releases of retired features and surfaces took since the last build line -- the teardown
// half of a rebuild. The releases happen in TickNrRetired, 32 evaluates after the park, so they are
// summed here and reported with the build that follows (Resident Evil 2, 2026-09-18: which part of the
// ~250 ms Present hold is teardown, which CreateFeature, which the first evaluate).
double g_teardownMs = 0.0;
unsigned int g_teardownFeatures = 0;

bool AnyFeatureParked()
{
    for (const NrRetired& r : g_nrRetired)
        if (r.feature != nullptr)
            return true;
    return false;
}

void ParkNrFeature(void*& feature)
{
    if (feature == nullptr)
        return;

    NrRetired r;
    r.feature = feature;
    feature = nullptr;
    g_nrRetired.push_back(r);
}

void ParkNrScaler(OS_Dx12*& scaler)
{
    if (scaler == nullptr)
        return;

    NrRetired r;
    r.scaler = scaler;
    scaler = nullptr;
    g_nrRetired.push_back(r);
}

void ParkNrResource(ID3D12Resource*& res)
{
    if (res == nullptr)
        return;

    NrRetired r;
    r.resource = res;
    res = nullptr;
    g_nrRetired.push_back(r);
}

void TickNrRetired()
{
    for (size_t i = 0; i < g_nrRetired.size();)
    {
        if (--g_nrRetired[i].framesLeft > 0)
        {
            ++i;
            continue;
        }

        const auto started = std::chrono::steady_clock::now();

        if (g_nrRetired[i].feature != nullptr && g_nr.release != nullptr)
        {
            g_nr.release(g_nrRetired[i].feature);
            ++g_teardownFeatures;
        }

        if (g_nrRetired[i].resource != nullptr)
            g_nrRetired[i].resource->Release();

        if (g_nrRetired[i].scaler != nullptr)
            delete g_nrRetired[i].scaler;

        // Last, so it measures the whole teardown including the scaler above -- the two sides of
        // this merge each added one of these lines and both belong, in this order.
        g_teardownMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

        g_nrRetired.erase(g_nrRetired.begin() + i);
    }
}

// Size of a resource in video memory, for taking the full-frame surfaces out of a build's measured cost.
uint64_t ResourceBytes(ID3D12Device* device, ID3D12Resource* res)
{
    if (device == nullptr || res == nullptr)
        return 0;

    const D3D12_RESOURCE_DESC d = res->GetDesc();
    return device->GetResourceAllocationInfo(0, 1, &d).SizeInBytes;
}

// One primary build, measured phase by phase and logged once its first evaluate has run.
struct NrBuildMeasure
{
    bool awaitingFirstEval = false;
    const void* feature = nullptr;
    const char* why = "first build";
    unsigned int workWidth = 0;
    unsigned int workHeight = 0;
    unsigned int width = 0;
    unsigned int height = 0;
    double teardownMs = 0.0;
    unsigned int teardownFeatures = 0;
    double surfacesMs = 0.0;
    double createMs = 0.0;
    bool vramKnown = false;
    uint64_t vramBefore = 0;
    uint64_t vramAfterCreate = 0;
    uint64_t fullFrameBytes = 0; // full-frame surfaces allocated by this build, not part of the size's cost
};

NrBuildMeasure g_buildMeasure;

// Why the next primary build happens, set wherever the live feature is let go.
const char* g_nextBuildWhy = "first build";

// The inject point decides which buffer is being measured -- the upscaler's linear output or the
// finished frame in swapchain format -- so a reading taken before a change describes a different
// picture to one taken after. Everything else that depends on the format is invalidated here.
void ForgetCalibration()
{
    g_nr.calibCount = 0;
    g_nr.calibSuggestion = 0.0f;
    g_nr.calibSteadiness = 0.0f;
    g_nr.calibUsable = false;
    g_nr.calibWhy = "measuring...";
}

void ReleaseSurfaces()
{
    ForgetCalibration();

    // Everything is being rebuilt from scratch (a format change, or a Present-route list that never ran --
    // a feature created on it must never be evaluated, Devil May Cry 5).
    g_nextBuildWhy = "surfaces rebuilt";

    ParkNrFeature(g_nr.feature);
    g_nr.featurePendingSubmission = false;

    // The extras go with it: they were built for this raster and this tuning too.
    for (unsigned int i = 1; i < DlssNr::MaxPassCount; ++i)
    {
        ParkNrFeature(g_nr.passFeature[i]);
        g_nr.passNeedsReset[i] = false;
        g_nr.passCreateFailed[i] = false;
        g_nr.passPendingSubmission[i] = false;
    }

    for (ID3D12Resource** r : { &g_nr.output, &g_nr.passScratch, &g_nr.colorCopy, &g_nr.hdrCopy, &g_nr.colorSmall,
                                &g_nr.outputNative, &g_nr.editNative, &g_nr.proxyNative, &g_nr.activeColor })
        ParkNrResource(*r);

    g_nr.passScratchFailed = false;

    g_nr.reset = true;
}

void ReleaseSurfacesIfFormatChanged(DXGI_FORMAT needed)
{
    if (g_nr.output == nullptr || g_nr.output->GetDesc().Format == needed)
        return;

    LOG_INFO("DLSS-NR rebuilding surfaces: format {} -> {} (inject point changed)", (int) g_nr.output->GetDesc().Format,
             (int) needed);

    ReleaseSurfaces();
}

void Barrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* res, D3D12_RESOURCE_STATES from,
             D3D12_RESOURCE_STATES to);

// The meter's grid is R32_FLOAT, which makes a row exactly 64 * 4 = 256 bytes -- the alignment a
// texture-to-buffer copy demands, met without padding, so the readback is a flat array of floats.
constexpr unsigned int kMeterRowBytes = kDlssNrMeterGrid * sizeof(float);
constexpr unsigned int kMeterBytes = kMeterRowBytes * kDlssNrMeterGrid;

// Records the copy of this frame's grid into whichever readback buffer is furthest from being read.
// Same shape as the meter's copy, against the calibration surface and its own ring.
void CopyCalibrationToReadback(ID3D12GraphicsCommandList* cmdList)
{
    const unsigned int slot = (unsigned int) (g_nr.calibFrames % 4);

    if (g_nr.calibReadback[slot] == nullptr || g_nr.calib == nullptr)
        return;

    D3D12_TEXTURE_COPY_LOCATION srcLoc {};
    srcLoc.pResource = g_nr.calib;
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = g_nr.calibReadback[slot];
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
    dst.PlacedFootprint.Footprint.Width = kDlssNrMeterGrid;
    dst.PlacedFootprint.Footprint.Height = kDlssNrMeterGrid;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = kMeterRowBytes;

    Barrier(cmdList, g_nr.calib, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);
    Barrier(cmdList, g_nr.calib, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    g_nr.calibFrames++;
}

void CopyMeterToReadback(ID3D12GraphicsCommandList* cmdList, ID3D12Device* device, bool exposureBound)
{
    const unsigned int slot = (unsigned int) (g_nr.meterFrames % 4);

    if (g_nr.meterReadback[slot] == nullptr)
        return;

    // Travels with the grid: read back three frames from now, alongside the tiles it describes.
    g_nr.meterExposureValid[slot] = exposureBound;

    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = g_nr.meter;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = g_nr.meterReadback[slot];
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
    dst.PlacedFootprint.Footprint.Width = kDlssNrMeterGrid;
    dst.PlacedFootprint.Footprint.Height = kDlssNrMeterGrid;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = kMeterRowBytes;

    Barrier(cmdList, g_nr.meter, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(cmdList, g_nr.meter, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    g_nr.meterFrames++;
}

// Takes the game's exposure out of tile 0 of the grid recorded three frames ago.
//
// Only tile 0 is written now. The frame-statistics meter this served was removed: a divisor measured
// off a frame this pass writes is a feedback loop rather than a measurement. What is left is a
// courier -- the game's exposure is a 1x1 texture in a resource state this pass did not set and must
// not transition, so the shader reads it as an SRV and it rides home on a readback that exists.
// Reads the calibration grid written four frames ago and turns it into one number.
//
// A high percentile of tile peaks, not the maximum: the maximum is a sun or a specular hit and would
// normalise the whole picture into the dark. The 90th percentile is high enough to sit at the top of
// the real range and common enough that no single highlight decides it.
void ConsumeCalibrationReadback()
{
    if (g_nr.calibFrames < 4)
        return;

    const unsigned int slot = (unsigned int) (g_nr.calibFrames % 4);
    ID3D12Resource* buffer = g_nr.calibReadback[slot];

    if (buffer == nullptr)
        return;

    void* mapped = nullptr;
    D3D12_RANGE range { 0, kMeterBytes };

    if (FAILED(buffer->Map(0, &range, &mapped)) || mapped == nullptr)
        return;

    const float* src = (const float*) mapped;

    std::vector<float> tiles;
    tiles.reserve(kDlssNrMeterGrid * kDlssNrMeterGrid);

    for (unsigned int i = 0; i < kDlssNrMeterGrid * kDlssNrMeterGrid; ++i)
    {
        if (std::isfinite(src[i]) && src[i] > 1e-6f)
            tiles.push_back(src[i]);
    }

    D3D12_RANGE nothingWritten { 0, 0 };
    buffer->Unmap(0, &nothingWritten);

    if (tiles.size() < 16)
        return;

    const size_t nth = (size_t) ((float) (tiles.size() - 1) * 0.90f);
    std::nth_element(tiles.begin(), tiles.begin() + nth, tiles.end());

    // How much of the frame carries light, measured against its own brightest tile rather than an
    // absolute threshold -- the units here are the game's and there is no absolute scale.
    //
    // This is what separates "the buffer is scaled by 240" from "I am standing in a dark cave". A
    // percentile of tile peaks is a statement about scene content; it only describes the buffer when
    // enough of the picture is lit for the top of the range to actually appear in it.
    float brightest = 0.0f;

    for (float v : tiles)
        brightest = std::max(brightest, v);

    unsigned int lit = 0;

    for (float v : tiles)
    {
        if (v > brightest * 0.10f)
            ++lit;
    }

    const float litFraction = tiles.empty() ? 0.0f : (float) lit / (float) tiles.size();

    // A torn readback survives isfinite and would clamp to exactly the ceiling, which since the
    // ceiling became 2000 is a value the slider can hold -- so a garbage frame could be offered as a
    // real answer. Reject rather than clamp.
    if (!(tiles[nth] > 0.0f) || tiles[nth] >= 1999.0f)
        return;

    const float suggestion = std::clamp(tiles[nth], 0.25f, 1990.0f);

    g_nr.calibUsable = !g_nr.calibPassthrough && litFraction > 0.20f;
    g_nr.calibWhy = g_nr.calibPassthrough
                        ? "this game hands over a frame it already tone mapped, so there is nothing to normalise"
                    : litFraction <= 0.20f ? "too little of this scene is lit to say where the top of the range is"
                                           : "";

    g_nr.calibHistory[g_nr.calibCount % NrState::kCalibHistory] = suggestion;
    g_nr.calibCount++;
    g_nr.calibSuggestion = suggestion;

    // Confidence is the spread of recent answers, not their absolute size. A number that has held
    // still for a second is one worth taking; one that is swinging means the scene is changing under
    // the measurement, and no single value would serve anyway.
    const unsigned int have = std::min<unsigned int>(g_nr.calibCount, NrState::kCalibHistory);

    if (have >= 8)
    {
        float lo = g_nr.calibHistory[0];
        float hi = g_nr.calibHistory[0];

        for (unsigned int i = 0; i < have; ++i)
        {
            lo = std::min(lo, g_nr.calibHistory[i]);
            hi = std::max(hi, g_nr.calibHistory[i]);
        }

        // A spread of 1.0x is perfect agreement and 2x or worse is none.
        const float spread = hi / lo;
        g_nr.calibSteadiness = std::clamp(1.0f - (spread - 1.0f), 0.0f, 1.0f);
    }
}

void ConsumeMeterReadback()
{
    if (g_nr.meterFrames < 4)
        return;

    const unsigned int slot = (unsigned int) (g_nr.meterFrames % 4);
    ID3D12Resource* buffer = g_nr.meterReadback[slot];

    if (buffer == nullptr)
        return;

    void* mapped = nullptr;
    D3D12_RANGE range { 0, sizeof(float) };

    if (FAILED(buffer->Map(0, &range, &mapped)) || mapped == nullptr)
        return;

    const float* src = (const float*) mapped;

    // Only believed when the frame that wrote this grid actually had an exposure texture bound. With
    // nothing bound DispatchPass substitutes the source picture, and tile 0 is then a scene pixel
    // rather than an exposure -- believing it made the white point follow the top-left corner of the
    // screen, which in Cyberpunk moved by up to 272x between frames and flashed the whole picture.
    //
    // When it is not believed gameExposure keeps its last good value, or stays 0 and lets
    // ResolveWhitePoint fall back to the slider, which is what a game supplying none should get.
    if (g_nr.meterExposureValid[slot] && std::isfinite(src[0]) && src[0] > 0.0f)
        g_nr.gameExposure = src[0];

    D3D12_RANGE nothingWritten { 0, 0 };
    buffer->Unmap(0, &nothingWritten);
}

// Forget everything the meter knows, so nothing read before this moment can be believed after it.
//
// The exposure is written only inside the block that dispatches the meter, and that block does not
// run while the option is off. Nothing used to clear any of this when it stopped, so the reading
// simply froze: switching the option back on returned the value from whenever it was switched off,
// and ResolveWhitePoint took it as current because a held value is exactly what it expects to see.
// GTA V's exposure spans 0.127 to 0.511 in one session, so re-enabling in different light handed the
// encode a white point up to 4x wrong -- which trips the soft knee, scales the model's answer away
// and leaves its hue behind. That is the colour cast, and it looked random because it depends on the
// light at the moment of the PREVIOUS switch-off, which nothing on screen shows.
//
// The readback ring made it worse. `meterFrames` also only advances inside that block, so the four
// slots kept their contents and their valid flags across the gap, and the first frames after
// re-enabling consumed buffers written before it as though they had just arrived.
//
// Zero is not a fallback value here, it is the absence of one: ResolveWhitePoint's `> 1e-6f` guard
// fails and the manual slider is used, which is what a game supplying no exposure already gets.
void InvalidateExposureMeter()
{
    g_nr.gameExposure = 0.0f;

    for (bool& valid : g_nr.meterExposureValid)
        valid = false;

    // Re-arms the `< 4` guard in ConsumeMeterReadback, so nothing is read back until four frames
    // have genuinely been queued since this point.
    g_nr.meterFrames = 0;
}

// Turns what the meter saw into the divisor the encode uses, or falls back to the slider.
//
// `cut` says the exposure may jump rather than drift, and it is the difference between this working
// and not. GTA V's character switch pulls the camera up through the sky: a linear HDR buffer's sky is
// tens of times brighter than the ground, the proxy clips to flat white, and the frame blows out until
// the camera comes back down. Easing across that at two percent a frame takes three and a half
// seconds, which is longer than the transition -- so a meter that only eases would lag through the
// whole thing and fix nothing.
//
// So a cut snaps and a drift eases. Walking out of a cave is a drift; a camera cut is not, and
// pretending otherwise to avoid pumping just moves the failure somewhere more visible.
float ResolveWhitePoint(const Config& cfg, bool isHdrBuffer)
{
    const float slider = cfg.DlssNrWhitePointScale.value_or_default();

    // A frame the game already tone mapped is display-referred: white is at 1 by definition and there
    // is nothing to measure. The slider stays available as a manual exposure on that path.
    if (!isHdrBuffer)
        return slider;

    // The game's own exposure, where it supplies one.
    //
    // Exposure is the step that makes a cave and a field comparable: the renderer works in arbitrary
    // scene-referred units and multiplies by this before tone mapping, which is precisely why one
    // fixed paper white cannot serve both. FSR spells the relationship out -- frame / preExposure *
    // exposure -- so undoing it gives the divisor this pass wants, and paper white becomes a constant
    // on top rather than a value chasing the scene.
    //
    // Unlike anything measured off the frame this cannot be moved by what the pass writes, which is
    // what killed the statistical meter. It is the game's number, decided upstream.
    //
    // Held across the frames where the texture is absent -- GTA V dropped it three times in one
    // session -- because falling back to a default on those frames is a flicker, not a fallback.
    // The scan's anchor, where the game supplies no exposure of its own.
    //
    // Only ratios are used, so the units of the buffer never have to be known -- which is the whole
    // reason this is anchored rather than absolute. The anchor is the user's own white point at the
    // moment they pressed the button; everything after that is the scan moving it.
    //
    // Deliberately below the exposure texture in priority and mutually exclusive with it in the
    // menu. A game that hands over a real exposure has no business being driven by a buffer found by
    // its shape, and two sources fighting over one number is the class of bug worth making
    // unreachable rather than merely unlikely.
    if (cfg.DlssNrWhitePointSource.value_or_default() == 2)
    {
        // Multi-point: one or more calibration points the user placed, interpolated in log space by
        // the current scan value. One point is the original ratio law; more fit the buffer's actual
        // relationship so the white point holds across the whole range, not only near one anchor.
        const float w = DlssNr::ExposureScan::AnchoredWhitePoint(DlssNr::ExposureScan::BestValue(),
                                                                 cfg.DlssNrScanInverted.value_or_default(),
                                                                 cfg.DlssNrScanTrim.value_or_default());

        if (w > 0.0f)
            return w;
    }

    if (cfg.DlssNrWhitePointSource.value_or_default() == 1 && g_nr.gameExposure > 1e-6f)
    {
        // Its own setting, not the manual divisor. See Config: they are different quantities with
        // different units and different sensible ranges, and sharing one value meant adjusting the
        // trim destroyed the divisor somebody had found by hand.
        //
        // Still bounded at the point of use rather than only in the menu that draws it.
        //
        // Bounding it at the slider would have been cosmetic: someone who found 64 by hand on the
        // manual path and then switched the exposure source on keeps that 64 in their ini, and the
        // composition would go on reading it until they happened to touch the control. The picture
        // would be wrong for a reason the menu was no longer showing.
        //
        // Their value is left in the config untouched, so switching back to manual restores the
        // number they arrived at. It is only what this path consumes that is limited.
        const float trim = std::clamp(cfg.DlssNrWhitePointTrim.value_or_default(), 0.25f, 4.0f);

        return std::clamp(g_nr.gamePreExposure / g_nr.gameExposure * trim, 0.01f, 4096.0f);
    }

    // Otherwise the slider, and only the slider.
    //
    // Measuring white from the frame was tried and removed. It could not be made to work because the
    // pass writes the frame it measures: in Enshrouded one session walked the divisor from 0.010 to
    // 97.910, and toggling NR at a fixed spot read 41.31 off and 0.46 on. Two attempts to damp it --
    // a relative lit threshold, then a rate limit with a cut snap -- both treated a coupled system as
    // a noisy one and neither held. A constant cannot do that, which is the whole argument for it,
    // and is what RenoDX has always done.
    return slider;
}

ID3D12Resource* CreateScratch(ID3D12Device* device, DXGI_FORMAT format, unsigned int width, unsigned int height)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // The model writes its result, so the destination has to be a UAV.
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ID3D12Resource* res = nullptr;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                    IID_PPV_ARGS(&res));
    return res;
}

void Barrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* res, D3D12_RESOURCE_STATES from,
             D3D12_RESOURCE_STATES to)
{
    if (from == to)
        return;

    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    cmdList->ResourceBarrier(1, &b);
}

// ---- Auto brightness / Auto contrast --------------------------------------------------------------
//
// Tunables, not measurements. The frame's average is a log-average of tile means in the normalised space
// the resolve works in (1 = paper white). Below kAutoKey the picture reads as dark and Brightness is raised
// toward it -- by kAutoStrength of the way, so a scene meant to be dark stays darker than a lit one -- and
// never above kAutoMaxBrightness, and never below 1: Auto only ever lifts. Contrast follows the spread
// between the darkest and brightest tenth of the frame, pushed toward kAutoSpreadStops and held inside a
// narrow band, because a large automatic contrast change reads as the picture pumping.
constexpr float kAutoKey = 0.12f;
constexpr float kAutoStrength = 0.75f;
constexpr float kAutoMaxBrightness = 1.6f;
constexpr float kAutoSpreadStops = 6.0f;
constexpr float kAutoContrastPerStop = 0.05f;
constexpr float kAutoMinContrast = 0.85f;
constexpr float kAutoMaxContrast = 1.25f;
// Seconds for the applied value to cover about two thirds of a change. Slow on purpose: walking out of a
// cave should brighten over a moment, not snap.
constexpr float kAutoSeconds = 1.5f;
// A frame this dark is a fade, a loading screen or a menu going black, not a scene to be lifted.
constexpr float kAutoBlackFrame = 0.004f;

void ResetAutoTone()
{
    g_nr.autoBrightness = 1.0f;
    g_nr.autoContrast = 1.0f;
    g_nr.toneSettled = false;
    g_nr.toneFrames = 0;
    g_nr.toneMean = 0.0f;
    g_nr.toneSpreadStops = 0.0f;
}

bool EnsureToneGrid(ID3D12Device* device)
{
    if (g_nr.tone != nullptr)
        return true;

    g_nr.tone = CreateScratch(device, DXGI_FORMAT_R32_FLOAT, kDlssNrMeterGrid, kDlssNrMeterGrid);
    if (g_nr.tone == nullptr)
        return false;

    D3D12_HEAP_PROPERTIES readback {};
    readback.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC bufferDesc {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = kMeterBytes;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for (auto& rb : g_nr.toneReadback)
    {
        if (FAILED(device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb))))
            rb = nullptr;
    }

    ResetAutoTone();
    LOG_INFO("DLSS-NR: auto brightness/contrast meter up, {}x{} tiles", kDlssNrMeterGrid, kDlssNrMeterGrid);
    return true;
}

void CopyToneToReadback(ID3D12GraphicsCommandList* cmdList)
{
    const unsigned int slot = (unsigned int) (g_nr.toneFrames % 4);

    if (g_nr.toneReadback[slot] == nullptr || g_nr.tone == nullptr)
        return;

    D3D12_TEXTURE_COPY_LOCATION srcLoc {};
    srcLoc.pResource = g_nr.tone;
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = g_nr.toneReadback[slot];
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
    dst.PlacedFootprint.Footprint.Width = kDlssNrMeterGrid;
    dst.PlacedFootprint.Footprint.Height = kDlssNrMeterGrid;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = kMeterRowBytes;

    Barrier(cmdList, g_nr.tone, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);
    Barrier(cmdList, g_nr.tone, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    g_nr.toneFrames++;
}

// Reads the grid written four frames ago -- the same distance the meter trusts, for the same reason: no
// fence, so only a long-retired frame is safe to map -- and eases the applied values toward what it says.
// normScale is the resolve's: the white point for a linear HDR buffer, 1 for a frame already tone mapped.
void ConsumeToneReadback(float normScale, bool wantBrightness, bool wantContrast)
{
    if (g_nr.toneFrames < 4)
        return;

    const unsigned int slot = (unsigned int) (g_nr.toneFrames % 4);
    ID3D12Resource* buffer = g_nr.toneReadback[slot];
    if (buffer == nullptr)
        return;

    void* mapped = nullptr;
    D3D12_RANGE range { 0, kMeterBytes };
    if (FAILED(buffer->Map(0, &range, &mapped)) || mapped == nullptr)
        return;

    const float* src = (const float*) mapped;
    const float scale = normScale > 1e-6f ? 1.0f / normScale : 1.0f;

    std::vector<float> logs;
    logs.reserve(kDlssNrMeterGrid * kDlssNrMeterGrid);
    double logSum = 0.0;

    // Tile 0 is the meter's exposure courier, not a tile mean -- skipped.
    for (unsigned int i = 1; i < kDlssNrMeterGrid * kDlssNrMeterGrid; ++i)
    {
        if (!std::isfinite(src[i]) || src[i] < 0.0f)
            continue;
        const float l = std::log(std::max(src[i] * scale, 1e-5f));
        logs.push_back(l);
        logSum += l;
    }

    D3D12_RANGE nothingWritten { 0, 0 };
    buffer->Unmap(0, &nothingWritten);

    if (logs.size() < 256)
        return;

    const float mean = std::exp((float) (logSum / (double) logs.size()));
    const size_t lo = logs.size() / 10;
    const size_t hi = logs.size() - 1 - logs.size() / 10;
    std::nth_element(logs.begin(), logs.begin() + lo, logs.end());
    const float p10 = logs[lo];
    std::nth_element(logs.begin(), logs.begin() + hi, logs.end());
    const float p90 = logs[hi];
    const float spreadStops = (p90 - p10) / std::log(2.0f);

    g_nr.toneMean = mean;
    g_nr.toneSpreadStops = spreadStops;

    // A black frame says nothing about the scene; hold what was there.
    if (!(mean > kAutoBlackFrame))
        return;

    float wantB = 1.0f;
    if (wantBrightness && mean < kAutoKey)
    {
        // The Brightness that would carry this average to the key, from the trim's own curve (a power of
        // 1/b in the gamma-2.2 scale -- see ApplyToneTrim in dlssnr.hlsl), then only part of the way.
        const float full = std::log(mean) / std::log(kAutoKey);
        wantB = std::clamp(1.0f + (full - 1.0f) * kAutoStrength, 1.0f, kAutoMaxBrightness);
    }

    float wantC = 1.0f;
    if (wantContrast && std::isfinite(spreadStops))
        wantC = std::clamp(1.0f + (kAutoSpreadStops - spreadStops) * kAutoContrastPerStop, kAutoMinContrast,
                           kAutoMaxContrast);

    const auto now = std::chrono::steady_clock::now();
    if (!g_nr.toneSettled)
    {
        g_nr.autoBrightness = wantB;
        g_nr.autoContrast = wantC;
        g_nr.toneSettled = true;
    }
    else
    {
        const float dt = std::clamp(std::chrono::duration<float>(now - g_nr.toneLastUpdate).count(), 0.0f, 0.25f);
        const float k = 1.0f - std::exp(-dt / kAutoSeconds);
        g_nr.autoBrightness += (wantB - g_nr.autoBrightness) * k;
        g_nr.autoContrast += (wantC - g_nr.autoContrast) * k;
    }
    g_nr.toneLastUpdate = now;
}

// ---- Image Clean Up ------------------------------------------------------------------------------
//
// Auto's controller. The halo is the 75th percentile, across the tiles that have an edge in them, of how
// far the model's answer strays past the band the clean up would allow it (grid 0, in stops). Strength
// follows it in proportion -- none at none, the cap at kCleanHaloFull and above -- eased over
// kCleanSeconds and never faster than kCleanRate per second, so it does not pump. It holds still after a
// cut (the model's history reset), when too few tiles have an edge to say anything, and for a couple of
// readings when the halo jumps several-fold at once, which is a new scene rather than a new halo.
constexpr float kCleanHaloFull = 0.05f;
constexpr float kCleanSeconds = 1.0f;
constexpr float kCleanRate = 0.5f;
constexpr unsigned int kCleanMinTiles = 24;
// What Auto uses for the settings the Manual rows set.
constexpr float kCleanAutoEdge = 1.5f;
constexpr float kCleanAutoBalance = 1.0f;
constexpr float kCleanAutoMotion = 0.5f;

void ResetCleanUp()
{
    g_nr.haloBefore = -1.0f;
    g_nr.haloAfter = -1.0f;
    g_nr.haloModel = -1.0f;
    g_nr.haloSmoothed = -1.0f;
    g_nr.haloJumpHeld = 0;
    g_nr.haloFrames = 0;
    g_nr.cleanAutoStrength = 0.0f;
    g_nr.cleanAutoSettled = false;
}

bool EnsureHaloGrids(ID3D12Device* device)
{
    if (g_nr.haloGrid[0] != nullptr && g_nr.haloGrid[1] != nullptr && g_nr.haloGrid[2] != nullptr)
        return true;

    D3D12_HEAP_PROPERTIES readback {};
    readback.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC bufferDesc {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = kMeterBytes;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for (int g = 0; g < 3; ++g)
    {
        if (g_nr.haloGrid[g] == nullptr)
            g_nr.haloGrid[g] = CreateScratch(device, DXGI_FORMAT_R32_FLOAT, kDlssNrMeterGrid, kDlssNrMeterGrid);
        if (g_nr.haloGrid[g] == nullptr)
            return false;

        for (auto& rb : g_nr.haloReadback[g])
        {
            if (rb == nullptr &&
                FAILED(device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb))))
                rb = nullptr;
        }
    }

    ResetCleanUp();
    LOG_INFO("DLSS-NR: image clean up halo meter up, {}x{} tiles", kDlssNrMeterGrid, kDlssNrMeterGrid);
    return true;
}

void CopyHaloToReadback(ID3D12GraphicsCommandList* cmdList, int grid)
{
    const unsigned int slot = (unsigned int) (g_nr.haloFrames % 4);

    if (g_nr.haloReadback[grid][slot] == nullptr || g_nr.haloGrid[grid] == nullptr)
        return;

    D3D12_TEXTURE_COPY_LOCATION srcLoc {};
    srcLoc.pResource = g_nr.haloGrid[grid];
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = g_nr.haloReadback[grid][slot];
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
    dst.PlacedFootprint.Footprint.Width = kDlssNrMeterGrid;
    dst.PlacedFootprint.Footprint.Height = kDlssNrMeterGrid;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = kMeterRowBytes;

    Barrier(cmdList, g_nr.haloGrid[grid], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);
    Barrier(cmdList, g_nr.haloGrid[grid], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

// The grid this slot held four frames ago (the one about to be overwritten -- long retired, as the tone
// grid's reasoning goes), reduced to the halo in stops; -1 when too few tiles have an edge in them.
float ReadHalo(int grid, unsigned int* edgeTiles)
{
    *edgeTiles = 0;
    if (g_nr.haloFrames < 4)
        return -1.0f;

    ID3D12Resource* buffer = g_nr.haloReadback[grid][g_nr.haloFrames % 4];
    if (buffer == nullptr)
        return -1.0f;

    void* mapped = nullptr;
    D3D12_RANGE range { 0, kMeterBytes };
    if (FAILED(buffer->Map(0, &range, &mapped)) || mapped == nullptr)
        return -1.0f;

    const float* src = (const float*) mapped;
    std::vector<float> tiles;
    tiles.reserve(kDlssNrMeterGrid * kDlssNrMeterGrid);

    for (unsigned int i = 0; i < kDlssNrMeterGrid * kDlssNrMeterGrid; ++i)
    {
        if (std::isfinite(src[i]) && src[i] >= 0.0f)
            tiles.push_back(std::min(src[i], 8.0f));
    }

    D3D12_RANGE nothingWritten { 0, 0 };
    buffer->Unmap(0, &nothingWritten);

    *edgeTiles = (unsigned int) tiles.size();
    if (tiles.size() < kCleanMinTiles)
        return -1.0f;

    const size_t at = (tiles.size() * 3) / 4;
    std::nth_element(tiles.begin(), tiles.begin() + at, tiles.end());
    return tiles[at];
}

void UpdateCleanAuto(float halo, float maxStrength)
{
    const auto now = std::chrono::steady_clock::now();
    const float dt = g_nr.cleanAutoSettled
                         ? std::clamp(std::chrono::duration<float>(now - g_nr.haloLastUpdate).count(), 0.0f, 0.25f)
                         : 0.0f;
    g_nr.haloLastUpdate = now;
    g_nr.cleanAutoSettled = true;

    // A cap lowered below where Auto sits takes effect at once; raising it lets Auto climb as usual.
    g_nr.cleanAutoStrength = std::min(g_nr.cleanAutoStrength, maxStrength);

    if (halo < 0.0f || g_frames < g_nr.cleanHoldUntil)
        return;

    if (g_nr.haloSmoothed > 0.01f && halo > 0.01f && std::fabs(std::log(halo / g_nr.haloSmoothed)) > std::log(4.0f) &&
        g_nr.haloJumpHeld < 2)
    {
        ++g_nr.haloJumpHeld;
        return;
    }
    g_nr.haloJumpHeld = 0;

    const float k = 1.0f - std::exp(-dt / kCleanSeconds);
    g_nr.haloSmoothed = g_nr.haloSmoothed < 0.0f ? halo : g_nr.haloSmoothed + (halo - g_nr.haloSmoothed) * k;

    const float want = std::clamp(g_nr.haloSmoothed / kCleanHaloFull, 0.0f, 1.0f) * maxStrength;
    const float step = std::clamp((want - g_nr.cleanAutoStrength) * k, -kCleanRate * dt, kCleanRate * dt);
    g_nr.cleanAutoStrength = std::clamp(g_nr.cleanAutoStrength + step, 0.0f, maxStrength);
}

// The mask history at this size, created or rebuilt as needed. False leaves the clean up without it.
bool EnsureCleanHistory(ID3D12Device* device, unsigned int width, unsigned int height)
{
    if (g_nr.cleanHistory[0] != nullptr && g_nr.cleanHistoryW == width && g_nr.cleanHistoryH == height)
        return true;

    for (int i = 0; i < 2; ++i)
    {
        // Parked, not released: last frame's list may still read it.
        if (g_nr.cleanHistory[i] != nullptr)
            ParkNrResource(g_nr.cleanHistory[i]);
        g_nr.cleanHistory[i] = CreateScratch(device, DXGI_FORMAT_R16G16B16A16_FLOAT, width, height);
        g_nr.cleanHistoryState[i] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    if (g_nr.cleanModel != nullptr)
        ParkNrResource(g_nr.cleanModel);
    g_nr.cleanModel = CreateScratch(device, DXGI_FORMAT_R16_FLOAT, width, height);
    g_nr.cleanModelState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    g_nr.cleanHistoryValid = false;
    g_nr.cleanHistoryIndex = 0;
    g_nr.cleanHistoryW = width;
    g_nr.cleanHistoryH = height;

    if (g_nr.cleanHistory[0] == nullptr || g_nr.cleanHistory[1] == nullptr || g_nr.cleanModel == nullptr)
    {
        for (auto& h : g_nr.cleanHistory)
        {
            if (h != nullptr)
                ParkNrResource(h);
        }
        if (g_nr.cleanModel != nullptr)
            ParkNrResource(g_nr.cleanModel);
        g_nr.cleanHistoryW = 0;
        g_nr.cleanHistoryH = 0;
        return false;
    }

    return true;
}

void MoveCleanHistory(ID3D12GraphicsCommandList* cmdList, int index, D3D12_RESOURCE_STATES to)
{
    Barrier(cmdList, g_nr.cleanHistory[index], g_nr.cleanHistoryState[index], to);
    g_nr.cleanHistoryState[index] = to;
}

void MoveCleanModel(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES to)
{
    Barrier(cmdList, g_nr.cleanModel, g_nr.cleanModelState, to);
    g_nr.cleanModelState = to;
}

// The clean up's prep surface for a frame of this size: the frame's own pixels, then one row per eight of
// them holding the 8x8 blocks' ranges. False leaves the resolve working its tile out itself, as before.
bool EnsureCleanPrep(ID3D12Device* device, unsigned int width, unsigned int height)
{
    if (g_nr.cleanPrep != nullptr && g_nr.cleanPrepW == width && g_nr.cleanPrepH == height)
        return true;

    // Parked, not released: last frame's list may still read it.
    if (g_nr.cleanPrep != nullptr)
        ParkNrResource(g_nr.cleanPrep);
    g_nr.cleanPrep = CreateScratch(device, DXGI_FORMAT_R16G16B16A16_FLOAT, width, height + (height + 7) / 8);
    g_nr.cleanPrepState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    g_nr.cleanPrepW = g_nr.cleanPrep != nullptr ? width : 0;
    g_nr.cleanPrepH = g_nr.cleanPrep != nullptr ? height : 0;
    return g_nr.cleanPrep != nullptr;
}

void MoveCleanPrep(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES to)
{
    Barrier(cmdList, g_nr.cleanPrep, g_nr.cleanPrepState, to);
    g_nr.cleanPrepState = to;
}

// Whether the DLSS call this pass attached to came from the DLSS5 Feeder rather than the game's
// own DLSS. Asked once: a ReShade add-on cannot appear or leave mid-process, and this is consulted
// every frame.
bool OnFeederRoute()
{
    static const bool feeder = DlssNr::IsFeederPresent();
    return feeder;
}

// A typeless resource cannot be viewed, and NGX builds its own views with nothing to tell it which
// format to use. Depth is very often declared typeless, so the typed member of the same family is
// substituted; CopyResource accepts that as a destination for the typeless original.
DXGI_FORMAT TypedGuideFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R24G8_TYPELESS:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R32G32_TYPELESS:
        return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return f;
    }
}

bool IsTypeless(DXGI_FORMAT f) { return TypedGuideFormat(f) != f; }

// Whether the depth guide can be bound as a shader input for the clean up's mask. A typeless guide has
// already been cloned to its typed, readable member (ReadableGuide); one that arrives fully typed in a
// depth format has no shader view at all, and one that denies shader access cannot be read -- either
// way the clean up works from brightness alone rather than risk an invalid view.
bool DepthReadable(ID3D12Resource* depth)
{
    if (depth == nullptr)
        return false;

    const D3D12_RESOURCE_DESC desc = depth->GetDesc();
    if ((desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0 || IsTypeless(desc.Format))
        return false;

    switch (desc.Format)
    {
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return false;
    default:
        return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    }
}

// Creates a typed twin of a guide buffer, matching everything but the format.
ID3D12Resource* CreateGuideClone(ID3D12Device* device, ID3D12Resource* source)
{
    D3D12_RESOURCE_DESC desc = source->GetDesc();
    desc.Format = TypedGuideFormat(desc.Format);
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    ID3D12Resource* res = nullptr;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                    IID_PPV_ARGS(&res));
    return res;
}

// Hands back something the model can actually read: the guide itself when it is typed, or a typed copy
// of it when it is not. NGX requires its inputs in NON_PIXEL_SHADER_RESOURCE at evaluate time, which is
// a documented contract rather than a guess about any one game's frame graph, so that is the state
// transitioned away from and back to here.
// Freezing is a diagnostic, and it reuses this function because the clone it already keeps is
// exactly the thing a frozen guide is: a private copy the model reads instead of the live resource.
// Freezing is then not a new mechanism but the absence of one -- stop refreshing the copy.
//
// A frozen guide is valid data that is wrong for this frame, which is a far better probe than a
// constant would be. A constant is degenerate and a model may special-case it; stale depth is
// ordinary depth that simply disagrees with the picture, and anything reading it has to notice.
// Hands back something the model can actually read: the guide itself when it is typed, or a typed
// copy of it when it is not. NGX requires its inputs in NON_PIXEL_SHADER_RESOURCE at evaluate time,
// which is a documented contract rather than a guess about any one game's frame graph, so that is
// the state transitioned away from and back to here.
ID3D12Resource* ReadableGuide(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* source,
                              ID3D12Resource** clone)
{
    if (source == nullptr || !IsTypeless(source->GetDesc().Format))
        return source;

    // A dynamic-resolution game reallocates its depth and motion vectors as the render size moves, so
    // the clone made for the old size no longer matches -- and CopyResource demands identical
    // dimensions. Copying a 1970x1108 source into a 984x554 clone is undefined and removes the device,
    // which is the DRS crash. Rebuild the clone whenever the source's shape has changed under it.
    if (*clone != nullptr)
    {
        const D3D12_RESOURCE_DESC have = (*clone)->GetDesc();
        const D3D12_RESOURCE_DESC want = source->GetDesc();

        if (have.Width != want.Width || have.Height != want.Height || have.Format != TypedGuideFormat(want.Format))
        {
            // Retired, not released: the previous copy may still be in flight on the game's queue.
            ParkNrResource(*clone);
        }
    }

    if (*clone == nullptr)
    {
        *clone = CreateGuideClone(device, source);

        if (*clone == nullptr)
            return nullptr;

        LOG_DEBUG("DLSS-NR cloned a typeless guide as format {}", (int) TypedGuideFormat(source->GetDesc().Format));
    }

    Barrier(cmdList, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyResource(*clone, source);
    Barrier(cmdList, source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmdList, *clone, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return *clone;
}

// The upscaler's own names differ between super resolution and ray reconstruction, and only one set is
// present on any given block.
// Whether a surface can physically hold linear HDR.
//
// Only a float format can: linear light is open-ended and runs far past 1.0, which a normalised
// integer surface cannot represent. An 8-bit UNORM frame is finished, display-referred output, and
// so is a 10-bit one -- HDR10 is PQ-encoded, which is display-referred too.
//
// The game's IsHDR flag is a statement of intent that is not always true, and believing it over a
// format that cannot hold linear light means encoding an already-encoded frame a second time.
bool FormatCanHoldLinearHdr(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return true;
    default:
        return false;
    }
}

ID3D12Resource* GetResource(NVSDK_NGX_Parameter* params, const char* a, const char* b)
{
    ID3D12Resource* res = nullptr;

    if (params->Get(a, &res) == NVSDK_NGX_Result_Success && res != nullptr)
        return res;

    res = nullptr;

    if (params->Get(b, &res) == NVSDK_NGX_Result_Success && res != nullptr)
        return res;

    // The same key again, as a plain pointer.
    //
    // NVSDK_NGX_Parameter has a typed setter per resource kind and an untyped one, and on a real NGX
    // parameter block those are separate slots: what goes in through Set(name, void*) does not come
    // back out of Get(name, ID3D12Resource**). A game running its own D3D12 upscaler sets these
    // typed, so the typed read above is enough and always was.
    //
    // Both of OptiScaler's bridges write them untyped. IFeature_Dx11wDx12 and IFeature_VkwDx12 turn
    // the game's D3D11 textures or Vulkan images into D3D12 resources and hand them over with
    // Set(name, (void*) resource) -- so the typed read came back null a few lines after the resource
    // had been written, and the pass quietly did nothing. That is the whole reason this never ran in
    // a DirectX 11 or Vulkan game.
    void* untyped = nullptr;

    if (params->Get(a, &untyped) == NVSDK_NGX_Result_Success && untyped != nullptr)
        return static_cast<ID3D12Resource*>(untyped);

    untyped = nullptr;

    if (params->Get(b, &untyped) == NVSDK_NGX_Result_Success && untyped != nullptr)
        return static_cast<ID3D12Resource*>(untyped);

    return nullptr;
}

// A change has to hold still before it is acted on: a slider being dragged reports a new value every
// frame, and each one would otherwise mean a new model.
constexpr unsigned long long kSettleFrames = 30;

// The extras the official integration sets: global tone (read at create) and the interface inputs.
// Written before every create and evaluate, nulls included, so nothing stale ever sits in the block.
void SetExtras(const Config& cfg, ID3D12Resource* ui, ID3D12Resource* backbuffer, unsigned int uiWidth,
               unsigned int uiHeight, unsigned int bbWidth, unsigned int bbHeight)
{
    if (g_nr.setExtras == nullptr || g_nr.capabilityParams == nullptr)
        return;

    // Global tone is written at the model's own default: the control that exposed it changed nothing
    // that could be seen, and the block persists, so a value still has to be put there.
    g_nr.setExtras(g_nr.capabilityParams, 1.0f, ui, ui, backbuffer, uiWidth, uiHeight, bbWidth, bbHeight);
}

unsigned int PassPreset(const Config& cfg, unsigned int pass)
{
    const unsigned int base = std::min(cfg.DlssNrPreset.value_or_default(), 3u);

    if (pass == 1 && cfg.DlssNrPass2Preset.has_value())
        return std::min(cfg.DlssNrPass2Preset.value(), 3u);

    if (pass == 2 && cfg.DlssNrPass3Preset.has_value())
        return std::min(cfg.DlssNrPass3Preset.value(), 3u);

    return base;
}

unsigned int PassStyle(const Config& cfg, unsigned int pass)
{
    const unsigned int base = std::min(cfg.DlssNrStyle.value_or_default(), 2u);

    if (pass == 1 && cfg.DlssNrPass2Style.has_value())
        return std::min(cfg.DlssNrPass2Style.value(), 2u);

    if (pass == 2 && cfg.DlssNrPass3Style.has_value())
        return std::min(cfg.DlssNrPass3Style.value(), 2u);

    return base;
}

bool TuningMatchesFeature(const Config& cfg, unsigned int requestedPasses)
{
    if (g_nr.builtIntensity != cfg.DlssNrIntensity.value_or_default() ||
        g_nr.builtLocalStructure != cfg.DlssNrLocalStructure.value_or_default() ||
        g_nr.builtLocalTone != cfg.DlssNrLocalTone.value_or_default() ||
        g_nr.builtSkinStructure != cfg.DlssNrSkinStructure.value_or_default() ||
        g_nr.builtAutoMask != cfg.DlssNrAutoMask.value_or_default() ||
        g_nr.builtUICorrection != cfg.DlssNrUICorrectionEffective())
        return false;

    for (unsigned int pass = 0; pass < requestedPasses; ++pass)
    {
        // A profile cannot be stale until its feature exists. This lets a user prepare pass 2 or 3
        // while running fewer layers without needlessly rebuilding pass 1.
        if (pass > 0 && g_nr.passFeature[pass] == nullptr)
            continue;

        if (g_nr.builtPreset[pass] != PassPreset(cfg, pass) || g_nr.builtStyle[pass] != PassStyle(cfg, pass))
            return false;
    }

    return true;
}

void RecordBuiltPrimaryTuning(const Config& cfg)
{
    g_nr.builtPreset[0] = PassPreset(cfg, 0);
    g_nr.builtIntensity = cfg.DlssNrIntensity.value_or_default();
    g_nr.builtStyle[0] = PassStyle(cfg, 0);
    g_nr.builtLocalStructure = cfg.DlssNrLocalStructure.value_or_default();
    g_nr.builtLocalTone = cfg.DlssNrLocalTone.value_or_default();
    g_nr.builtSkinStructure = cfg.DlssNrSkinStructure.value_or_default();
    g_nr.builtAutoMask = cfg.DlssNrAutoMask.value_or_default();
    g_nr.builtUICorrection = cfg.DlssNrUICorrectionEffective();
}

// Guards the module's state. Every caller is now on the game's render thread, so this is no longer
// holding two threads apart -- but the D3D11-on-D3D12 bridge enters from its own call site, and the
// cost is a CPU-side lock on a path that already records command lists.
std::mutex g_nrMutex;

// Runs the pass inside the same state envelope every other OptiScaler compute pass runs in.
//
// The upscaler's own evaluate is wrapped like this by TryEvaluateOptiFeature: root-signature tracking
// off so the hooks do not record the pass's binds as the game's, heap capture skipped, and RestoreRoot
// afterwards to put the game's compute state back. Neural Rendering ran outside that envelope -- after
// the upscaler had already restored and re-armed -- so it left its own root signature and descriptor
// heaps bound and captured. On an ordinary engine the game rebinds and never notices. On a bindless
// engine (007 First Light, Monster Hunter Wilds, and the rest of the RestoreComputeSig* quirks) the
// game resumes off the pass's bindings and the device is removed.
//
// As RAII so every early return from the pass is covered. RestoreRoot is gated internally on the
// RestoreComputeSignature / RestoreGraphicSignature config, so this is a no-op on games that do not
// ask for it and only acts where it is needed.
struct ScopedNrStateEnvelope
{
    ID3D12GraphicsCommandList* cmd;
    ScopedSkipHeapCapture skipHeap;

    explicit ScopedNrStateEnvelope(ID3D12GraphicsCommandList* c) : cmd(c)
    {
        D3D12Hooks::SetRootSignatureTracking(false);
    }

    ~ScopedNrStateEnvelope()
    {
        D3D12Hooks::RestoreRoot(cmd);
        D3D12Hooks::SetRootSignatureTracking(true);
    }
};

// Every way out of the pass before it does anything is silent on purpose -- an evaluate that carries
// no depth is normal and would otherwise print every frame forever. That silence is fine until the
// pass does nothing at all and the log has no opinion about why.
//
// So each distinct reason is reported once. Once, not once per frame.
void ReportSkipOnce(const char* reason)
{
    static std::set<std::string> seen;

    if (seen.insert(reason).second)
        LOG_INFO("DLSS-NR did not run: {}", reason);
}

} // namespace

// ---------------------------------------------------------------------------------------------
// The pass itself. Everything above is what it is made of; everything below is the shape the rest
// of OptiScaler sees.
// ---------------------------------------------------------------------------------------------

DlssNr_Dx12::DlssNr_Dx12(std::string InName, ID3D12Device* InDevice) : Shader_Dx12(InName, InDevice)
{
    if (InDevice == nullptr)
    {
        LOG_ERROR("InDevice is nullptr!");
        return;
    }

    LOG_DEBUG("{0} start!", _name);

    // Five inputs, two outputs, one constant buffer, and a clamped linear sampler.
    //
    // The sampler exists because the model may be run below full resolution, in which case its answer
    // has to be read back at a different size from the frame it is being transferred onto.
    D3D12_STATIC_SAMPLER_DESC sampler {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    if (!SetupRootSignature(InDevice, kSrvCount, kUavCount, 1, 0, 0, 1, &sampler))
    {
        LOG_ERROR("[{0}] Failed to setup root signature", _name);
        return;
    }

    D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(DlssNrConstants));
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    for (uint32_t i = 0; i < DLSSNR_NUM_OF_HEAPS; ++i)
    {
        auto result = InDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                        IID_PPV_ARGS(&_constantBuffers[i]));

        if (result != S_OK)
        {
            LOG_ERROR("[{0}] CreateCommittedResource error {1:x}", _name, (unsigned int) result);
            return;
        }
    }

    // Precompiled, with no source fallback. The shader used to be compiled at runtime from a string,
    // which would have meant no shader at all for anyone leaving UsePrecompiledShaders at its
    // default.
    if (!CreateComputePipeline(InDevice, &_pipelineState, DlssNr_cso, sizeof(DlssNr_cso), nullptr))
    {
        LOG_ERROR("[{0}] Failed to create the compute pipeline", _name);
        return;
    }

    // Image Clean Up's prep, same root signature. Optional: without it the resolve does the work itself.
    if (!CreateComputePipeline(InDevice, &_prepPipelineState, DlssNr_prep_cso, sizeof(DlssNr_prep_cso), nullptr))
    {
        LOG_WARN("[{0}] Image Clean Up's prep pipeline could not be created -- the resolve works it out itself", _name);
        _prepPipelineState = nullptr;
    }

    _init = InitHeaps(InDevice, _frameHeaps, DLSSNR_NUM_OF_HEAPS);
}

bool DlssNr_Dx12::DispatchPass(ID3D12GraphicsCommandList* InCmdList, const DlssNrConstants& InConstants,
                               ID3D12Resource* InSource, ID3D12Resource* InModel, ID3D12Resource* InOriginal,
                               ID3D12Resource* InMotion, ID3D12Resource* InPrevEdit, ID3D12Resource* OutTarget,
                               ID3D12Resource* OutKeep, ID3D12Resource* InDepth, ID3D12Resource* InHistory,
                               ID3D12Resource* OutAux, ID3D12Resource* InCleanModel)
{
    if (!_init || InCmdList == nullptr || _device == nullptr || InSource == nullptr || OutTarget == nullptr)
        return false;

    if (InConstants.Mode == DlssNrMode_CleanupPrep && _prepPipelineState == nullptr)
        return false;

    const uint32_t slot = _heapIndex;
    _heapIndex = (_heapIndex + 1) % DLSSNR_NUM_OF_HEAPS;

    FrameDescriptorHeap& currentHeap = _frameHeaps[slot];

    // Every slot in the table gets a view, whether the mode reads it or not. An unbound descriptor is
    // not an empty read; it is a read from nothing, and the source stands in wherever a mode has
    // nothing of its own to put there.
    ID3D12Resource* const srvs[kSrvCount] = {
        InSource,
        InModel != nullptr ? InModel : InSource,
        InOriginal != nullptr ? InOriginal : InSource,
        InMotion != nullptr ? InMotion : InSource,
        InPrevEdit != nullptr ? InPrevEdit : InSource,
        InDepth != nullptr ? InDepth : InSource,
        InHistory != nullptr ? InHistory : InSource,
        InCleanModel != nullptr ? InCleanModel : InSource,
    };

    // The depth guide keeps its own format: its clone is already the readable member of its family
    // (R32_FLOAT_X8X24_TYPELESS, R24_UNORM_X8_TYPELESS...), which the shared typeless translation would
    // turn back into a depth format no shader view accepts.
    for (uint32_t i = 0; i < kSrvCount; ++i)
        CreateShaderResourceView(_device, srvs[i], currentHeap.GetSrvCPU(i), DXGI_FORMAT_UNKNOWN,
                                 !(i == 5 && InDepth != nullptr));

    ID3D12Resource* const uavs[kUavCount] = {
        OutTarget,
        OutKeep != nullptr ? OutKeep : OutTarget,
        OutAux != nullptr ? OutAux : OutTarget,
    };

    for (uint32_t i = 0; i < kUavCount; ++i)
        CreateUnorderedAccessView(_device, uavs[i], currentHeap.GetUavCPU(i), 0);

    if (!CreateConstantsBuffer(_device, _constantBuffers[slot], InConstants, currentHeap.GetCbvCPU(0)))
    {
        LOG_ERROR("[{0}] Failed to create a constants buffer", _name);
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    InCmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    InCmdList->SetComputeRootSignature(_rootSignature);
    InCmdList->SetPipelineState(InConstants.Mode == DlssNrMode_CleanupPrep ? _prepPipelineState : _pipelineState);
    InCmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());

    // Sized from the constants rather than from a resource, because the pass that shrinks the proxy
    // writes fewer pixels than its source has.
    const UINT dispatchWidth = (InConstants.Width + _numThreadsX - 1) / _numThreadsX;
    const UINT dispatchHeight = (InConstants.Height + _numThreadsY - 1) / _numThreadsY;
    InCmdList->Dispatch(dispatchWidth, dispatchHeight, 1);

    return true;
}

DlssNr_Dx12::~DlssNr_Dx12()
{
    if (_prepPipelineState != nullptr)
    {
        _prepPipelineState->Release();
        _prepPipelineState = nullptr;
    }

    for (auto& buffer : _constantBuffers)
    {
        if (buffer != nullptr)
        {
            buffer->Release();
            buffer = nullptr;
        }
    }
}

// Where the pass leaves early, counted by source line. "Running after SR" is logged once per build and
// the heartbeat only on frames that finish, so a pass that started and then left early on every frame
// looked the same in the log as one that ran all session (Resident Evil 2 under XeSS, 2026-09-14: started,
// then no heartbeat in 38 seconds). Dispatch runs under g_nrMutex, so plain counters are enough.
static std::map<int, unsigned long long> g_dispatchBails;
// Set only while DlssNr::RunAtPresent calls into the pass: the target is then the swapchain's back buffer,
// in PRESENT state, and the command list is this app's own -- no game state on it to restore.
static bool g_presentRouteDispatch = false;
// Set by the Present route beside g_presentRouteDispatch: its vectors this frame are the zero field (no
// optical flow). Image Clean Up then neither rejects on motion nor reprojects its mask by it.
static bool g_presentMotionBlind = false;

// Keys on the Present route's private parameter block: the letterboxed picture's rectangle in the frame.
static constexpr const char* kPresentActiveX = "OptiDlssNr.Present.Active.X";
static constexpr const char* kPresentActiveY = "OptiDlssNr.Present.Active.Y";
static constexpr const char* kPresentActiveWidth = "OptiDlssNr.Present.Active.Width";
static constexpr const char* kPresentActiveHeight = "OptiDlssNr.Present.Active.Height";
static unsigned long long g_dispatchCalls = 0;
static unsigned long long g_dispatchDone = 0;
#define DLSSNR_BAIL()                                                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        ++g_dispatchBails[__LINE__];                                                                                   \
        return;                                                                                                        \
    } while (0)

void DlssNr_Dx12::Dispatch(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* colour, ID3D12Resource* depth,
                           ID3D12Resource* motion, ID3D12Resource* output, const DlssNrFrameInfo& frame,
                           ID3D12CommandQueue* timingQueue)
{
    std::lock_guard<std::mutex> nrLock(g_nrMutex);

    ++g_dispatchCalls;

    if (g_dispatchCalls % 600 == 0)
    {
        std::string bails;
        for (const auto& [line, count] : g_dispatchBails)
            bails += std::format("{}line {} x{}", bails.empty() ? "" : ", ", line, count);

        LOG_INFO("DLSS-NR calls: {} into the pass, {} ran to the end, left early: {}", g_dispatchCalls, g_dispatchDone,
                 bails.empty() ? std::string("never") : bails);
    }

    // Settings changed from outside this process reach the running game here. That is what makes
    // the 32-bit route tunable: OptiScaler runs inside the Feeder's 64-bit helper, which has no
    // window, so its own panel cannot be shown over the game -- the manager writes
    // host64\OptiScaler.ini instead and this is where the game notices. Rate-limited inside, and
    // taken before the config reference below so a frame runs wholly on one set of values or the
    // other. Create-time settings are compared against what each feature was built with further
    // down and rebuild exactly as the in-game panel's own sliders do.
    // Still polled here as well as at the entry points: harmless (rate-limited to one stat per 250 ms
    // inside), and it keeps a caller that reaches the pass some other way current too. Qualified: this
    // is DlssNr_Dx12::Dispatch, a class method, not code inside namespace DlssNr.
    DlssNr::PollSettingsFromDisk();

    const Config& cfg = *Config::Instance();

    if (g_nr.failed || cmdList == nullptr || colour == nullptr || depth == nullptr || motion == nullptr ||
        output == nullptr)
    {
        ReportSkipOnce(g_nr.failed ? "it already failed this session" : "a resource was missing");
        DLSSNR_BAIL();
    }

    ID3D12Resource* target = output;

    // A completed upscaler output normally arrives as a UAV. The pre-SR colour input instead arrives
    // readable. Track every transition so both paths return the resource exactly as their caller gave
    // it to us; a pre-SR resource without UAV support is written through a scratch-and-copy fallback.
    //
    // Getting the arrival state wrong is not cosmetic. It becomes the StateBefore of the first
    // transition below, D3D12 rejects a transition that disagrees with the resource's real state,
    // and it is then the CALLER'S command list that fails to close -- killing a frame this pass
    // does not own. That is DLSS5-Feeder#104: Close() returned E_INVALIDARG on the first frame this
    // pass recorded into the feeder's list.
    //
    // Post-upscale the target is the output DLSS was just told to write, and NGX writes its output as a
    // UAV, so UNORDERED_ACCESS is the arrival state on every route -- the DLSS5 Feeder's included.
    //
    // For a day (v1.0.21-v1.0.24) the Feeder route assumed RENDER_TARGET instead, on the reasoning that
    // its texture comes from ReShade, to explain DLSS5-Feeder#104 (Armored Core VI's list failing to
    // close). That was a guess and it was wrong where it could be checked: Batman: Arkham Knight, the
    // Feeder game that worked, then failed Close() with E_INVALIDARG on the first frame after SR and the
    // Feeder stopped; with OutputResourceBarrier=8 (UAV) the same build ran over 1,200 frames clean
    // (2026-09-14). AC6 remains blocked by the Feeder's own command-list bug either way. [Hotfix]
    // OutputResourceBarrier still overrides this, and the log line below says which was assumed.
    const bool feederRoute = OnFeederRoute();
    const D3D12_RESOURCE_STATES outputArrival =
        g_presentRouteDispatch ? D3D12_RESOURCE_STATE_PRESENT
        : frame.BeforeUpscale  ? (Config::Instance()->ColorResourceBarrier.has_value()
                                      ? (D3D12_RESOURCE_STATES) Config::Instance()->ColorResourceBarrier.value()
                                      : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
        : Config::Instance()->OutputResourceBarrier.has_value()
            ? (D3D12_RESOURCE_STATES) Config::Instance()->OutputResourceBarrier.value()
            : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    // Said once per distinct value, because it is the first thing to check when a frame dies at
    // Close and the only way to tell an assumption apart from a setting after the fact.
    {
        static unsigned int said = ~0u;

        if (said != (unsigned int) outputArrival)
        {
            said = (unsigned int) outputArrival;
            LOG_INFO("DLSS-NR: target assumed to arrive in D3D12 state 0x{:X} ({}). If a frame fails "
                     "to close, override it with [Hotfix] OutputResourceBarrier.",
                     (unsigned int) outputArrival,
                     g_presentRouteDispatch                                  ? "Present route back buffer"
                     : Config::Instance()->OutputResourceBarrier.has_value() ? "set in the ini"
                     : feederRoute                                           ? "Feeder route default"
                                                                             : "native route default");
        }
    }
    D3D12_RESOURCE_STATES targetState = outputArrival;
    const auto TransitionTarget = [&](D3D12_RESOURCE_STATES to)
    {
        Barrier(cmdList, target, targetState, to);
        targetState = to;
    };

    ID3D12Device* device = nullptr;

    if (FAILED(target->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
    {
        ReportSkipOnce("the output texture belongs to no D3D12 device");
        DLSSNR_BAIL();
    }

    const D3D12_RESOURCE_DESC desc = target->GetDesc();

    // A letterboxed frame on the Present route: run on the picture between the bars, where the guides are.
    // The staging texture is a UAV in the frame's own format, which an sRGB format cannot be -- such a frame
    // keeps the whole-frame placement rather than failing the pass for the session.
    const bool srgbFrame = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                           desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                           desc.Format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    const bool activeRect = !frame.BeforeUpscale && g_presentRouteDispatch && !srgbFrame && frame.ActiveWidth != 0 &&
                            frame.ActiveHeight != 0 && frame.ActiveBaseX + frame.ActiveWidth <= desc.Width &&
                            frame.ActiveBaseY + frame.ActiveHeight <= desc.Height;

    const auto active =
        frame.BeforeUpscale ? DlssNr::PreSrColorExtent(desc, frame.RenderSubrectWidth, frame.RenderSubrectHeight)
        : activeRect
            ? std::optional<DlssNr::ColorExtent> { DlssNr::ColorExtent { frame.ActiveWidth, frame.ActiveHeight,
                                                                         frame.ActiveBaseX, frame.ActiveBaseY } }
            : std::optional<DlssNr::ColorExtent> { DlssNr::ColorExtent { (unsigned int) desc.Width, desc.Height } };
    if (!active)
    {
        ReportSkipOnce("the pre-SR active colour size is invalid");
        device->Release();
        DLSSNR_BAIL();
    }
    const auto width = active->width;
    const auto height = active->height;
    const bool cropColor = (frame.BeforeUpscale || activeRect) && (width != desc.Width || height != desc.Height);

    if (activeRect && cropColor)
    {
        static unsigned int saidBaseY = ~0u;
        if (saidBaseY != active->baseY)
        {
            saidBaseY = active->baseY;
            LOG_INFO("DLSS-NR: letterboxed frame -- running on the {}x{} picture at {},{} inside the {}x{} frame",
                     width, height, active->baseX, active->baseY, (UINT) desc.Width, (UINT) desc.Height);
        }
    }
    const bool targetSupportsUav = cropColor || (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;

    // Depth and motion vectors are the upscaler's inputs and so are at render resolution, while colour
    // and output are at display resolution. The model takes that as a subrect per resource rather than
    // needing them resampled, which is why nothing here rescales anything.
    // The guides are the upscaler's inputs and so are at render resolution, while colour and output
    // are at display resolution. Their sizes come from the resources rather than from the caller:
    // one less thing a call site can get wrong, and the model takes the difference as a subrect per
    // resource rather than needing anything resampled.
    const D3D12_RESOURCE_DESC guideDesc = depth->GetDesc();
    unsigned int guideWidth = (unsigned int) guideDesc.Width;
    unsigned int guideHeight = guideDesc.Height;

    if (guideWidth == 0 || guideHeight == 0)
    {
        guideWidth = width;
        guideHeight = height;
    }

    // What the game rendered wins over how big the texture is.
    //
    // The comment above says the sizes come from the resources so there is one less thing a call site
    // can get wrong, and that was right about call sites and wrong about the game. A dynamic
    // resolution title allocates its depth once at the maximum it will ever need and renders into
    // the corner; the resource then describes the allocation, not the picture, and the model gets
    // handed the stale margin as though it were scene.
    //
    // Bounded by the resource because a subrect larger than the texture is a game bug that would
    // otherwise become a read off the end of it.
    if (frame.RenderSubrectWidth != 0 && frame.RenderSubrectHeight != 0)
    {
        const unsigned int subW = std::min(frame.RenderSubrectWidth, guideWidth);
        const unsigned int subH = std::min(frame.RenderSubrectHeight, guideHeight);

        if (subW != guideWidth || subH != guideHeight)
        {
            static unsigned int saidW = 0, saidH = 0;

            if (saidW != subW || saidH != subH)
            {
                saidW = subW;
                saidH = subH;
                LOG_INFO("DLSS-NR guides: the game renders {}x{} into a {}x{} texture, so the model is "
                         "told the smaller number",
                         subW, subH, guideWidth, guideHeight);
            }
        }

        guideWidth = subW;
        guideHeight = subH;
    }

    g_nr.guideWidth = guideWidth;
    g_nr.guideHeight = guideHeight;
    // The game states its depth convention in the flags it created its own feature with, and
    // following it is right almost always -- but a game that states it wrongly used to leave nothing
    // to do about it. 0 keeps following the game, 1 and 2 override it.
    switch (Config::Instance()->DlssNrDepthConvention.value_or_default())
    {
    case 1:
        g_nr.guideDepthInverted = false;
        break;

    case 2:
        g_nr.guideDepthInverted = true;
        break;

    default:
        g_nr.guideDepthInverted = frame.DepthInverted;
        break;
    }

    // The game's own encoding, passed through. Every resource already carries a subrect saying how
    // big it is, so scaling by the resolution ratio on top of that counts it twice -- vectors come
    // out too long and the model warps its history past where the surface went.
    g_nr.guideMvScaleX = frame.MvScaleX;
    g_nr.guideMvScaleY = frame.MvScaleY;

    if (frame.Reset)
    {
        g_nr.reset = true;

        static unsigned long long resets = 0;
        ++resets;

        if (resets <= 3 || resets % 100 == 0)
            LOG_INFO("DLSS-NR: the game asked for a history reset ({} so far)", resets);
    }

    // Logged whenever it changes, not once per session.
    //
    // A guide size change does not rebuild the feature -- the guides are handed over as subrects and
    // the output size is what the model is built for -- so a once-only line goes stale the moment the
    // player moves the quality slider, and every later line in the log is then read against numbers
    // that stopped being true. In Nioh 3 the session opened at DLAA, moved to 66% and ended at 33%,
    // and the log claimed 1920x1080 guides throughout.
    struct GuideReport
    {
        bool valid;
        bool depthInverted;
        float mvScaleX;
        float mvScaleY;
        unsigned int guideW;
        unsigned int guideH;
        unsigned int frameW;
        unsigned int frameH;
    };

    static GuideReport loggedGuides {};

    const GuideReport guidesNow {
        true,  g_nr.guideDepthInverted, g_nr.guideMvScaleX, g_nr.guideMvScaleY, guideWidth, guideHeight,
        width, (unsigned int) height
    };

    if (!loggedGuides.valid || loggedGuides.depthInverted != guidesNow.depthInverted ||
        loggedGuides.mvScaleX != guidesNow.mvScaleX || loggedGuides.mvScaleY != guidesNow.mvScaleY ||
        loggedGuides.guideW != guidesNow.guideW || loggedGuides.guideH != guidesNow.guideH ||
        loggedGuides.frameW != guidesNow.frameW || loggedGuides.frameH != guidesNow.frameH)
    {
        loggedGuides = guidesNow;
        LOG_INFO("DLSS-NR guides: depth {}, motion vector scale {} x {}, guides {}x{} for a {}x{} frame",
                 g_nr.guideDepthInverted ? "inverted" : "not inverted", g_nr.guideMvScaleX, g_nr.guideMvScaleY,
                 guideWidth, guideHeight, width, height);
    }

    if (cfg.DlssNrProxyProbe.value_or_default())
        ProbeProxyDispatch(cmdList);

    // Refuse to be the second thing applying this model to the same frame.
    //
    // ReShade loads its add-ons with LoadLibrary, so they are ordinary modules and can be found by
    // name. A DLSS 5 Neural Rendering *consumer* add-on (RenoDX's) runs NR at the ReShade stage,
    // AFTER this pass -- so with both active the frame is denoised twice, the second working on a
    // picture the first already rewrote. That does not read as more detail; it reads as smeared and
    // over-sharpened, and neither tool would say a word about why. (The DLSS5 Feeder is not a
    // consumer and is intentionally not refused -- see ConflictingNrAddon.)
    //
    // Refused rather than warned. This pass exists to decide what the frame looks like, and two
    // things cannot both be that. The message names the file so it is obvious which to remove.
    if (const char* other = ConflictingNrAddon())
    {
        // Static, because g_nr.reason is a borrowed pointer that has to outlive this frame. Kept
        // short because the panel prints it on one unwrapped line; the log carries the rest.
        static std::string conflict;
        conflict = std::string(other) + " is already doing this -- remove it, or turn this off";

        LOG_ERROR("DLSS-NR unavailable: {} is loaded and applies Neural Rendering at the ReShade stage, "
                  "downstream of this pass. Running both denoises every frame twice, the second working "
                  "on a picture the first already rewrote.",
                  other);

        g_nr.failed = true;
        g_nr.reason = conflict.c_str();
        device->Release();
        DLSSNR_BAIL();
    }

    if (!EnsureForwarder() || !EnsureCapabilityParams(device))
    {
        g_nr.failed = true;
        LOG_ERROR("DLSS-NR unavailable: {}", g_nr.reason);
        device->Release();
        DLSSNR_BAIL();
    }

    // What the model works at. The frame and its edit stay full resolution; only the model's input and
    // answer change size, and the resolve enlarges (or minifies) the answer while compositing. Below 1
    // the model runs reduced and cheaper; above 1 it SUPERSAMPLES -- the proxy is upscaled to a larger
    // working size so the model denoises a super-native input, which the resolve then samples back down.
    // Capped at 2x: cost grows with the area and NGX acceptance above native is what this probe tests.
    float workScale = cfg.DlssNrWorkingScale.value_or_default();
    workScale = workScale < 0.25f ? 0.25f : (workScale > 2.0f ? 2.0f : workScale);
    const auto workWidth = (unsigned int) (width * workScale + 0.5f);
    const auto workHeight = (unsigned int) (height * workScale + 0.5f);
    const bool reduced = workWidth != width || workHeight != height;
    const unsigned int configuredPasses = std::clamp(cfg.DlssNrPasses.value_or_default(), 1u, DlssNr::MaxPassCount);
    const bool proxyBackend = cfg.DlssNrUseProxy.value_or_default();
    const unsigned int requestedPasses = proxyBackend ? 1u : configuredPasses;

    if (proxyBackend && configuredPasses > 1)
    {
        static bool warnedProxyPasses = false;
        if (!warnedProxyPasses)
        {
            warnedProxyPasses = true;
            LOG_WARN("DLSS-NR: the driver-proxy backend supports one pass; Passes={} is using 1", configuredPasses);
        }
    }

    ReleaseSurfacesIfFormatChanged(desc.Format);

    const bool resolutionChanged =
        g_nr.width != width || g_nr.height != height || g_nr.workWidth != workWidth || g_nr.workHeight != workHeight;
    const bool placementChanged = g_nr.feature != nullptr && g_nr.beforeUpscale != frame.BeforeUpscale;

    // The model reads its tuning once, while the feature is built, so a changed setting only takes
    // effect when the feature is rebuilt. TuningMatchesFeature was written to notice that and then
    // never called, which is why every one of these controls appeared to do nothing until something
    // else -- a resolution change -- happened to force a rebuild by accident.
    const bool tuningChanged = !TuningMatchesFeature(cfg, requestedPasses);

    // Only the model's size moved (WorkingScale), not the frame's.
    const bool sizeOnlyChange =
        resolutionChanged && !tuningChanged && !placementChanged && g_nr.width == width && g_nr.height == height;

    if (g_nr.feature != nullptr && (resolutionChanged || tuningChanged || placementChanged))
    {
        g_nextBuildWhy = tuningChanged      ? "settings changed"
                         : placementChanged ? "placement changed"
                         : sizeOnlyChange   ? "model size changed"
                                            : "frame size changed";

        // Parked rather than released: with frame generation the GPU can still be several frames
        // deep in work that references all of it.
        ParkNrFeature(g_nr.feature);
        g_nr.featurePendingSubmission = false;

        for (unsigned int i = 1; i < DlssNr::MaxPassCount; ++i)
        {
            ParkNrFeature(g_nr.passFeature[i]);
            g_nr.passNeedsReset[i] = false;
            g_nr.passCreateFailed[i] = false;
            g_nr.passPendingSubmission[i] = false;
        }

        // Resolution and seam changes invalidate the scratch state. Tuning does not, and throwing
        // resources away for it would mean a reallocation every time a slider moves.
        if (resolutionChanged || placementChanged)
        {
            if (placementChanged)
                ForgetCalibration();

            ParkNrResource(g_nr.output);
            ParkNrResource(g_nr.passScratch);
            ParkNrResource(g_nr.colorCopy);
            ParkNrResource(g_nr.hdrCopy);
            ParkNrResource(g_nr.colorSmall);
            ParkNrResource(g_nr.outputNative);
            ParkNrResource(g_nr.editNative);
            ParkNrResource(g_nr.proxyNative);
            ParkNrResource(g_nr.activeColor);
            g_nr.passScratchFailed = false;
        }
    }

    // Before anything new is built: the old features have to be gone, and the new one has to fit.
    // Building the new set while the old one was still parked is what put the Cyberpunk run over the
    // edge. The parked features are released TickNrRetired's 32 evaluates later, so the model is off for
    // about that long after a rebuild -- ticked here, since this return skips the tick further down.
    if (g_nr.feature == nullptr)
    {
        if (AnyFeatureParked())
        {
            TickNrRetired();
            device->Release();
            DLSSNR_BAIL();
        }

        const uint64_t need = (uint64_t) workWidth * workHeight * kFeatureBytesPerPixel +
                              (uint64_t) width * height * 8ull * 4ull; // the RGBA16F working surfaces
        uint64_t usage = 0, budget = 0;
        if (!FitsInVideoMemory(device, need, &usage, &budget))
        {
            static uint64_t warnedAtBudget = 0;
            if (warnedAtBudget != budget)
            {
                warnedAtBudget = budget;
                LOG_WARN("DLSS-NR: not building the model yet -- video memory {} of {} MB in use, and a {}x{} model "
                         "needs about {} MB more. Waiting for room rather than risking a lost device.",
                         usage >> 20, budget >> 20, workWidth, workHeight, need >> 20);
            }

            device->Release();
            DLSSNR_BAIL();
        }
    }

    // A primary build starts here: measure its phases (teardown was summed as the old one was released).
    const bool buildingPrimary = g_nr.feature == nullptr;
    const auto buildStarted = std::chrono::steady_clock::now();
    uint64_t buildVramBefore = 0, buildBudget = 0;
    bool buildVramKnown = false;
    bool hadColorCopy = true, hadHdrCopy = true, hadActiveColor = true, hadOutputNative = true;

    if (buildingPrimary)
    {
        buildVramKnown = ReadVideoMemory(device, buildVramBefore, buildBudget);
        hadColorCopy = g_nr.colorCopy != nullptr;
        hadHdrCopy = g_nr.hdrCopy != nullptr;
        hadActiveColor = g_nr.activeColor != nullptr;
        hadOutputNative = g_nr.outputNative != nullptr;
    }

    if (g_nr.output == nullptr)
    {
        g_nr.output = CreateScratch(device, desc.Format, workWidth, workHeight);
        // The full-frame surfaces may still be live, so they are only made when missing -- overwriting
        // live ones would leak them mid-flight.
        if (g_nr.colorCopy == nullptr)
            g_nr.colorCopy = CreateScratch(device, desc.Format, width, height);
        if (g_nr.hdrCopy == nullptr)
            g_nr.hdrCopy = CreateScratch(device, desc.Format, width, height);
        g_nr.workWidth = workWidth;
        g_nr.workHeight = workHeight;
    }

    // Checked against the texture itself, not the rebuild bookkeeping above: that only reallocates
    // while a feature exists, and a staging texture smaller than the active rectangle would turn the
    // box copy below into an out-of-bounds CopyTextureRegion on the game's command list.
    if (cropColor && g_nr.activeColor != nullptr)
    {
        const D3D12_RESOURCE_DESC staged = g_nr.activeColor->GetDesc();
        if (staged.Width != width || staged.Height != height || staged.Format != desc.Format)
            ParkNrResource(g_nr.activeColor);
    }
    if (cropColor && g_nr.activeColor == nullptr)
        g_nr.activeColor = CreateScratch(device, desc.Format, width, height);
    if (cropColor && g_nr.activeColor == nullptr)
    {
        g_nr.failed = true;
        g_nr.reason = "the pre-SR active colour staging texture could not be allocated";
        LOG_ERROR("DLSS-NR unavailable: {}", g_nr.reason);
        device->Release();
        DLSSNR_BAIL();
    }

    if (requestedPasses == 1)
    {
        // Reclaim the extra raster and clear its failure latch. Raising the count later gets one fresh
        // allocation attempt; holding a failing allocation at two must not retry it every frame.
        ParkNrResource(g_nr.passScratch);
        g_nr.passScratchFailed = false;
    }
    else if (g_nr.passScratch == nullptr && !g_nr.passScratchFailed)
    {
        g_nr.passScratch = CreateScratch(device, desc.Format, workWidth, workHeight);
        g_nr.passScratchFailed = g_nr.passScratch == nullptr;

        if (g_nr.passScratchFailed)
            LOG_ERROR("DLSS-NR: could not allocate the model-output ping-pong; extra passes are disabled");
    }

    if (reduced && g_nr.colorSmall == nullptr)
        g_nr.colorSmall = CreateScratch(device, desc.Format, workWidth, workHeight);

    // The down-leg target is native (the answer is brought back to frame size before the resolve).
    if (workScale > 1.0f && g_nr.outputNative == nullptr)
        g_nr.outputNative = CreateScratch(device, desc.Format, width, height);

    // The up-leg's pair, for a model working below the frame: the enlarged answer and the enlarged
    // proxy it is measured against. Two frames of the colour format at frame size, allocated only
    // where they are used -- nothing is created for a model at or above 100%.
    if (reduced)
    {
        if (g_nr.editNative == nullptr)
            g_nr.editNative = CreateScratch(device, desc.Format, width, height);
        if (g_nr.proxyNative == nullptr)
            g_nr.proxyNative = CreateScratch(device, desc.Format, width, height);
    }

    if (g_nr.meter == nullptr)
    {
        g_nr.meter = CreateScratch(device, DXGI_FORMAT_R32_FLOAT, kDlssNrMeterGrid, kDlssNrMeterGrid);

        D3D12_HEAP_PROPERTIES readback {};
        readback.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC bufferDesc {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = kMeterBytes;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        for (auto& rb : g_nr.meterReadback)
        {
            if (FAILED(device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb))))
            {
                rb = nullptr;
                LOG_WARN("DLSS-NR: the white point meter could not allocate its readback; falling back "
                         "to the paper white slider");
            }
        }

        if (g_nr.meter != nullptr)
            LOG_INFO("DLSS-NR: white point meter up, {}x{} tiles", kDlssNrMeterGrid, kDlssNrMeterGrid);
    }

    if (g_nr.feature == nullptr && g_nr.output != nullptr && g_nr.colorCopy != nullptr && g_nr.hdrCopy != nullptr)
    {
        auto snippet = Util::FindFilePath(g_dllDir, "nvngx_dlssnr.dll");

        if (!snippet.has_value())
            snippet = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssnr.dll");

        if (!snippet.has_value())
        {
            g_nr.failed = true;
            g_nr.reason = "nvngx_dlssnr.dll was not found beside OptiScaler or the game";
            LOG_ERROR("DLSS-NR unavailable: {}", g_nr.reason);
            device->Release();
            DLSSNR_BAIL();
        }

        // Our own surfaces for this size are all made by now (output, copies, ping-pong, reduced input).
        const auto createStarted = std::chrono::steady_clock::now();
        const double surfacesMs =
            buildingPrimary ? std::chrono::duration<double, std::milli>(createStarted - buildStarted).count() : 0.0;

        SetExtras(cfg, nullptr, nullptr, 0, 0, 0, 0);
        g_nr.feature =
            g_nr.create(snippet->wstring().c_str(), State::Instance().NVNGX_ApplicationDataPath.c_str(), device,
                        cmdList, g_nr.capabilityParams, workWidth, workHeight, (int) PassPreset(cfg, 0),
                        cfg.DlssNrIntensity.value_or_default(), (int) PassStyle(cfg, 0),
                        cfg.DlssNrLocalStructure.value_or_default(), cfg.DlssNrLocalTone.value_or_default(),
                        cfg.DlssNrSkinStructure.value_or_default(), cfg.DlssNrAutoMask.value_or_default() ? 1 : 0,
                        cfg.DlssNrUICorrectionEffective() ? 1 : 0);

        const double createMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - createStarted).count();

        if (g_nr.feature != nullptr)
        {
            // Held until the first evaluate of this feature, which logs the whole build in one line.
            NrBuildMeasure m;
            m.awaitingFirstEval = true;
            m.feature = g_nr.feature;
            m.why = g_nextBuildWhy;
            m.workWidth = workWidth;
            m.workHeight = workHeight;
            m.width = width;
            m.height = height;
            m.teardownMs = g_teardownMs;
            m.teardownFeatures = g_teardownFeatures;
            m.surfacesMs = surfacesMs;
            m.createMs = createMs;
            m.vramKnown = buildingPrimary && buildVramKnown;
            m.vramBefore = buildVramBefore;

            uint64_t usageAfter = 0, budgetAfter = 0;
            if (m.vramKnown && ReadVideoMemory(device, usageAfter, budgetAfter))
                m.vramAfterCreate = usageAfter;
            else
                m.vramKnown = false;

            // Full-frame surfaces made by this build belong to the frame, not to this model size.
            if (!hadColorCopy)
                m.fullFrameBytes += ResourceBytes(device, g_nr.colorCopy);
            if (!hadHdrCopy)
                m.fullFrameBytes += ResourceBytes(device, g_nr.hdrCopy);
            if (!hadActiveColor)
                m.fullFrameBytes += ResourceBytes(device, g_nr.activeColor);
            if (!hadOutputNative)
                m.fullFrameBytes += ResourceBytes(device, g_nr.outputNative);

            g_buildMeasure = m;
            g_teardownMs = 0.0;
            g_teardownFeatures = 0;
            g_nextBuildWhy = "rebuild";
        }

        if (g_nr.feature == nullptr)
        {
            g_nr.featurePendingSubmission = false;
            g_nr.failed = true;
            g_nr.reason = "the model would not initialise";
            const auto initResult = (unsigned int) (g_nr.lastInit != nullptr ? *g_nr.lastInit : 0);
            const auto createResult = (unsigned int) (g_nr.lastCreate != nullptr ? *g_nr.lastCreate : 0);

            // Cast before formatting. These are ints, and "0x{:X}" on a negative int prints
            // 0x-452FFFFF, which no one can decode back to 0xBAD00001.
            LOG_ERROR("DLSS-NR create failed: init 0x{:X} ({}), create 0x{:X} ({})", initResult,
                      NgxResultName(initResult), createResult, NgxResultName(createResult));
            device->Release();
            DLSSNR_BAIL();
        }

        g_nr.width = width;
        g_nr.height = height;
        g_nr.beforeUpscale = frame.BeforeUpscale;
        g_nr.reset = true;
        g_nr.featurePendingSubmission = true;
        g_nr.featureCreateEpoch = frame.SubmissionEpoch;
        RecordBuiltPrimaryTuning(cfg);
        LOG_INFO("DLSS-NR running {} SR: target {}x{}, model {}x{}, guides {}x{} "
                 "(preset {}, intensity {}, style {}, build epoch {})",
                 frame.BeforeUpscale ? "before" : "after", width, height, workWidth, workHeight, guideWidth,
                 guideHeight, g_nr.builtPreset[0], g_nr.builtIntensity, g_nr.builtStyle[0], frame.SubmissionEpoch);

        // Creating and evaluating a feature in the same command list is the dice-roll that hung the
        // GPU (every crash died on a creation frame). The creation goes through the game's own submit
        // first; the first evaluate happens next frame. One frame without the model is invisible.
        device->Release();
        DLSSNR_BAIL();
    }

    if (g_nr.feature == nullptr)
    {
        device->Release();
        DLSSNR_BAIL();
    }

    // A later function call is not proof that the command list containing CreateFeature was
    // submitted: some engines record more than one upscale on the same list. Native DX12 supplies
    // the wrapped Present count and the bridges supply their post-Execute frame counter, so an epoch
    // change is the first point at which evaluating the feature is safe.
    if (g_nr.featurePendingSubmission)
    {
        if (frame.SubmissionEpoch == g_nr.featureCreateEpoch)
        {
            device->Release();
            DLSSNR_BAIL();
        }

        g_nr.featurePendingSubmission = false;
        LOG_INFO("DLSS-NR: primary feature ready after submitted epoch {}", g_nr.featureCreateEpoch);
    }

    // Park no-longer-requested feature histories immediately (their actual release remains deferred),
    // and clear their failure latch so a later 1 -> N change is a deliberate retry.
    for (unsigned int pass = 1; pass < DlssNr::MaxPassCount; ++pass)
    {
        if (pass >= requestedPasses)
        {
            ParkNrFeature(g_nr.passFeature[pass]);
            g_nr.passNeedsReset[pass] = false;
            g_nr.passCreateFailed[pass] = false;
            g_nr.passPendingSubmission[pass] = false;
        }
    }

    // Do not create another feature, and do not evaluate any feature, while a requested layer still
    // belongs to the current submission epoch. This keeps multiple upscaler evaluations recorded on
    // one command list from recreating the historical create/evaluate GPU hang.
    for (unsigned int pass = 1; pass < requestedPasses; ++pass)
    {
        if (!g_nr.passPendingSubmission[pass])
            continue;

        if (frame.SubmissionEpoch == g_nr.passCreateEpoch[pass])
        {
            device->Release();
            DLSSNR_BAIL();
        }

        g_nr.passPendingSubmission[pass] = false;
        LOG_INFO("DLSS-NR: feature for pass {} ready after submitted epoch {}", pass + 1, g_nr.passCreateEpoch[pass]);
    }

    // Build at most one missing extra feature on this invocation and evaluate nothing afterwards.
    // NGX feature creation records work on the supplied command list; evaluating that feature before
    // the list has been submitted is the creation-frame GPU hang that caused the old multi-pass path
    // to be removed. A new feature therefore gets an entire build-only frame and starts next time.
    if (g_nr.passScratch != nullptr)
    {
        for (unsigned int pass = 1; pass < requestedPasses; ++pass)
        {
            if (g_nr.passFeature[pass] != nullptr)
                continue;

            if (g_nr.passCreateFailed[pass])
                break;

            // An extra pass is optional. When a retired feature is still parked, or this one would not fit
            // in video memory, run with the passes already built and try again on a later frame.
            if (AnyFeatureParked())
                break;

            {
                const uint64_t need = (uint64_t) workWidth * workHeight * kFeatureBytesPerPixel;
                uint64_t usage = 0, budget = 0;
                if (!FitsInVideoMemory(device, need, &usage, &budget))
                {
                    static unsigned int warnedPass = 0;
                    if (warnedPass != pass + 1)
                    {
                        warnedPass = pass + 1;
                        LOG_WARN("DLSS-NR: model pass {} not built -- video memory {} of {} MB in use, and it needs "
                                 "about {} MB more. Running {} pass(es) until there is room.",
                                 pass + 1, usage >> 20, budget >> 20, need >> 20, pass);
                    }
                    break;
                }
            }

            auto snippet = Util::FindFilePath(g_dllDir, "nvngx_dlssnr.dll");
            if (!snippet.has_value())
                snippet = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssnr.dll");

            if (!snippet.has_value())
            {
                g_nr.passCreateFailed[pass] = true;
                LOG_ERROR("DLSS-NR: pass {} feature not built because nvngx_dlssnr.dll disappeared", pass + 1);
            }
            else
            {
                uint64_t passVramBefore = 0, passBudget = 0;
                const bool passVramKnown = ReadVideoMemory(device, passVramBefore, passBudget);
                const auto passStarted = std::chrono::steady_clock::now();

                SetExtras(cfg, nullptr, nullptr, 0, 0, 0, 0);
                g_nr.passFeature[pass] = g_nr.create(
                    snippet->wstring().c_str(), State::Instance().NVNGX_ApplicationDataPath.c_str(), device, cmdList,
                    g_nr.capabilityParams, workWidth, workHeight, (int) PassPreset(cfg, pass),
                    cfg.DlssNrIntensity.value_or_default(), (int) PassStyle(cfg, pass),
                    cfg.DlssNrLocalStructure.value_or_default(),
                    // Local tone belongs to the frame and is applied by pass zero only.
                    0.0f, cfg.DlssNrSkinStructure.value_or_default(), cfg.DlssNrAutoMask.value_or_default() ? 1 : 0, 1);

                {
                    const double passCreateMs =
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - passStarted)
                            .count();
                    uint64_t passVramAfter = 0, budgetAfter = 0;
                    uint64_t passBytes = 0;
                    if (passVramKnown && ReadVideoMemory(device, passVramAfter, budgetAfter) &&
                        passVramAfter > passVramBefore)
                        passBytes = passVramAfter - passVramBefore;

                    LOG_INFO("DLSS-NR build: pass {} feature {}x{} -- CreateFeature {:.1f} ms, +{} MB video memory",
                             pass + 1, workWidth, workHeight, passCreateMs, passBytes >> 20);
                }

                if (g_nr.passFeature[pass] != nullptr)
                {
                    g_nr.builtPreset[pass] = PassPreset(cfg, pass);
                    g_nr.builtStyle[pass] = PassStyle(cfg, pass);
                    g_nr.passNeedsReset[pass] = true;
                    g_nr.passPendingSubmission[pass] = true;
                    g_nr.passCreateEpoch[pass] = frame.SubmissionEpoch;
                    LOG_INFO("DLSS-NR: feature for pass {} built with preset {}, style {} at epoch {}; "
                             "waiting for submission",
                             pass + 1, g_nr.builtPreset[pass], g_nr.builtStyle[pass], frame.SubmissionEpoch);
                }
                else
                {
                    g_nr.passPendingSubmission[pass] = false;
                    g_nr.passCreateFailed[pass] = true;
                    LOG_ERROR("DLSS-NR: feature for pass {} failed to build; using {} ready pass(es)", pass + 1, pass);
                }
            }

            device->Release();
            DLSSNR_BAIL();
        }
    }

    // The upscaler has just written this, so it is a UAV. The model needs it readable.
    // Whether the buffer the upscaler just wrote is linear HDR or an already tone-mapped picture is not
    // something to assume: the game says so, in the flags it created its own DLSS feature with. Running
    // the colour transform over a frame that has already been through a tonemapper is pure damage, and
    // skipping it on one that has not leaves the model reading ordinary values as enormously bright.
    // EvaluateInternal has already combined the game's HDR flag with the authoritative output format.
    // That authority matters before SR: Color and Output may use different surface formats while still
    // representing the same frame colour space.
    const bool isHdrBuffer = frame.ColourIsLinearHdr;

    static bool reportedHdr = false;
    static bool reportedHdrValue = false;
    static bool reportedBefore = false;

    if (!reportedHdr || reportedHdrValue != isHdrBuffer || reportedBefore != frame.BeforeUpscale)
    {
        reportedHdr = true;
        reportedHdrValue = isHdrBuffer;
        reportedBefore = frame.BeforeUpscale;
        LOG_INFO("DLSS-NR {} SR: the game's DLSS colour space is {} so the colour transform is {}",
                 frame.BeforeUpscale ? "before" : "after", isHdrBuffer ? "linear HDR" : "already tone-mapped",
                 isHdrBuffer ? "on" : "off");
    }

    const bool haveCodec = IsInit();

    if (!haveCodec)
    {
        g_nr.failed = true;
        g_nr.reason = "the colour codec would not compile";
        LOG_ERROR("DLSS-NR unavailable: {}", g_nr.reason);
        device->Release();
        DLSSNR_BAIL();
    }

    // What the upscaler produces is linear HDR with an open-ended range; the model was trained on
    // finished, sRGB-encoded frames. The white point is what maps one to the other, and it is a property
    // of the game's exposure rather than a number worth asking anyone to guess: measured means of 0.065,
    // 1.8 and 185 have all been seen in this one game.
    ++g_frames;
    TickNrRetired();
    CheckCaptureTrigger();
    CheckCleanCaptureTrigger();

    if (g_cleanCapture.due(g_frames))
    {
        const auto written = g_cleanCapture.write(Util::DllPath().remove_filename() / "dlssnr-cleanup-capture");
        if (g_cleanCaptureBefore != nullptr)
            ParkNrResource(g_cleanCaptureBefore);
        if (!written.empty())
            LOG_INFO("DLSS-NR image clean up capture written to {}", written);
    }

    // Twice a second or so, for the panel's readout.
    if ((g_frames % 30) == 0)
    {
        uint64_t usage = 0, budget = 0;
        ReadVideoMemory(device, usage, budget);
    }

    if (g_captureWriteAtFrame != 0 && g_frames >= g_captureWriteAtFrame)
    {
        g_captureWriteAtFrame = 0;
        const auto captureDir = Util::DllPath().remove_filename() / "dlssnr-capture";
        const auto written = g_capture.write(captureDir);

        if (!written.empty())
            LOG_INFO("DLSS-NR wrote matched before/after frames to {}", written);
    }

    // Paper white, and nothing else. The frame is divided by this and encoded, and the soft knee
    // above 0.75 takes whatever is left over.
    //
    // It used to be divided by a white point measured from the frame -- around 3 in Cyberpunk -- which
    // was right for the old composition, where the encode had to be inverted and highlights therefore
    // had to survive it. Under the composition this now uses it is actively wrong twice over: the
    // model is handed a picture three times darker than it should see, and the highlight branch is
    // defeated. That branch hands back `originalLuma - proxyLuma`, the headroom the proxy could not
    // represent -- it exists precisely because the proxy is meant to clip. Normalising the highlights
    // away first leaves it nothing to give back.

    // On an engine that needs its compute state put back -- the bindless quirks -- the envelope can
    // only restore what was captured. If nothing was captured for this list, the upscaler decided
    // touching state was unsafe this frame, and binding the pass now would leave state the envelope
    // cannot clean up. So on those games, skip the frame rather than corrupt it. Ordinary games do
    // not require restore, so they are unaffected and the pass runs as before.
    // The Present route records into a list of its own, which has no game state on it to put back.
    const bool restoreRequired = !g_presentRouteDispatch && (cfg.RestoreComputeSignature.value_or_default() ||
                                                             cfg.RestoreGraphicSignature.value_or_default());

    // The same exception the upscaler already makes (NVNGX_DLSS_Dx12.cpp, TryEvaluateOptiFeature): a
    // command list that never has a root signature to track after a few frames is one the caller opened
    // only to run the upscale on, so there is no game state on it to protect. PureDark's plugin on
    // Resident Evil 2 hands over exactly such a list; the upscaler runs on it, and without this the pass
    // skipped every frame there. EXPERIMENTAL: the upscaler's version of this is the prime suspect in
    // that game's intermittent device removals shortly after start.
    static std::map<ID3D12GraphicsCommandList*, unsigned int> skippedForRestore;
    constexpr unsigned int kRestoreSkips = 3;
    bool untrackedDedicatedList = false;

    if (restoreRequired && !D3D12Hooks::CanRestoreRootSignature(cmdList))
    {
        auto& skips = skippedForRestore[cmdList];

        if (skips < kRestoreSkips)
        {
            ++skips;
        }
        else
        {
            untrackedDedicatedList = true;

            if (skips == kRestoreSkips)
            {
                ++skips;
                LOG_WARN("DLSS-NR: command list {:X} never had state to restore after {} frames -- treating it as "
                         "a dedicated upscaling list and running the pass on it without a restore",
                         (UINT64) cmdList, kRestoreSkips);
            }
        }
    }

    if (restoreRequired && !untrackedDedicatedList && !D3D12Hooks::CanRestoreRootSignature(cmdList))
    {
        ReportSkipOnce("the upscaler could not restore state this frame");

        // The device reference taken at the top of this function is released on every other path out.
        // It was not released here, and this is the one path a bindless game takes every single frame
        // -- so the game that most needed this skip was also leaking a device reference per frame.
        device->Release();
        DLSSNR_BAIL();
    }

    // From here on the pass binds its own root signature, heaps and pipeline. Everything below runs
    // inside the envelope so the game's compute state is restored no matter which way this returns.
    ScopedNrStateEnvelope stateEnvelope(cmdList);

    if (g_gpuTime == nullptr)
        g_gpuTime = std::make_unique<GpuTime_Dx12>(device);

    if (g_ngxTime == nullptr)
        g_ngxTime = std::make_unique<GpuTime_Dx12>(device);

    if (g_composeTime == nullptr)
        g_composeTime = std::make_unique<GpuTime_Dx12>(device);

    if (g_gpuTime != nullptr)
        g_gpuTime->Start(cmdList);

    // Copy just the live image, not the stale right/bottom margins. Do this only after model
    // creation/pending-submission early returns, and inside the measured GPU interval. The compact
    // texture lets every existing codec/compare/hold/capture path use unmodified pixel coordinates.
    ID3D12Resource* const gameColor = target;
    if (cropColor)
    {
        TransitionTarget(D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cmdList, g_nr.activeColor, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        DlssNr::CopyActiveColor(cmdList, g_nr.activeColor, gameColor, *active);
        TransitionTarget(outputArrival);
        Barrier(cmdList, g_nr.activeColor, D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        target = g_nr.activeColor;
        targetState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    const auto FinishColor = [&](bool copyBack)
    {
        if (cropColor)
        {
            if (copyBack)
            {
                TransitionTarget(D3D12_RESOURCE_STATE_COPY_SOURCE);
                Barrier(cmdList, gameColor, outputArrival, D3D12_RESOURCE_STATE_COPY_DEST);
                DlssNr::CopyActiveColor(cmdList, gameColor, target, *active, true);
                Barrier(cmdList, gameColor, D3D12_RESOURCE_STATE_COPY_DEST, outputArrival);
            }
            TransitionTarget(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        else
        {
            TransitionTarget(outputArrival);
        }
    };

    // Fetch the game's exposure, where the game supplies one and the user asked for it.
    //
    // This used to measure the white point off the frame as well, over a 64x64 grid of tile
    // luminances. That is gone: the pass writes the frame it was measuring, so the divisor chased its
    // own output -- one Enshrouded session walked it from 0.010 to 97.910, and toggling NR at a fixed
    // spot read 41.31 off against 0.46 on. What remains dispatches a single thread to copy the game's
    // 1x1 exposure texture into tile 0. That is a courier, not a measurement, and cannot feed back.
    // Gated on the source the menu actually writes. This read the retired WhitePointFromExposure
    // flag while consumption keyed on WhitePointSource == 1, so choosing "the game's own exposure"
    // never dispatched the meter and the white point silently fell back to the slider.
    const bool exposureSettingOn = cfg.DlssNrWhitePointSource.value_or_default() == 1;

    // Nothing held from before the option was switched off may survive switching it back on. See
    // InvalidateExposureMeter for what froze and why it read as a colour cast.
    if (exposureSettingOn && !g_nr.exposureSettingWasOn)
    {
        InvalidateExposureMeter();
        LOG_INFO("DLSS-NR exposure: option switched on, held reading discarded");
    }

    g_nr.exposureSettingWasOn = exposureSettingOn;

    const bool wantExposure = exposureSettingOn && frame.ExposureTexture != nullptr;

    if (g_nr.meter != nullptr && wantExposure)
    {
        DlssNrConstants meterParams {};
        meterParams.Mode = DlssNrMode_Meter;

        // One pixel. Only tile (0,0) is read back, and the tile-mean branch below it in the shader is
        // dead code the dispatch simply never reaches.
        meterParams.Width = 1;
        meterParams.Height = 1;

        const D3D12_RESOURCE_STATES priorTargetState = targetState;
        TransitionTarget(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        DispatchPass(cmdList, meterParams, target, nullptr, nullptr, (ID3D12Resource*) frame.ExposureTexture, nullptr,
                     g_nr.meter, nullptr);
        TransitionTarget(priorTargetState);

        CopyMeterToReadback(cmdList, device, true);
        ConsumeMeterReadback();
    }

    g_nr.gamePreExposure = frame.PreExposure;

    float whitePoint = ResolveWhitePoint(cfg, isHdrBuffer);

    // Zero-latency exposure (D3D12, source 1): when the game hands us a live exposure texture, the
    // white point is recomputed in-shader every frame from it (ExposurePreMul / exposure) instead of
    // the 3-4 frame CPU meter readback. whitePoint above still rides along in gWhitePoint as the
    // fallback the shader uses if the live sample is missing or absurd. Bound at t4 (InPrevEdit) below.
    ID3D12Resource* exposureTex = nullptr;
    uint32_t useGameExposure = 0;
    float exposurePreMul = 0.0f;

    if (cfg.DlssNrWhitePointSource.value_or_default() == 1 && frame.ExposureTexture != nullptr)
    {
        exposureTex = (ID3D12Resource*) frame.ExposureTexture;
        useGameExposure = 1;
        const float trim = std::clamp(cfg.DlssNrWhitePointTrim.value_or_default(), 0.25f, 4.0f);
        exposurePreMul = g_nr.gamePreExposure * trim;
    }

    // Auto brightness / Auto contrast: the frame measured as it arrives, before anything below writes to
    // it. The meter's tile-mean branch over the whole grid, onto its own surface; tile 0 is the exposure
    // courier and ConsumeToneReadback skips it. Nothing is created or dispatched while both are off.
    // With RenoDX in the game the trim is identity and Auto is off (DlssNrRenoDx::ToneTrimSuppressed).
    const bool toneTrimOff = DlssNrRenoDx::ToneTrimSuppressed();
    const bool autoBrightnessOn = !toneTrimOff && cfg.DlssNrAutoBrightness.value_or_default();
    const bool autoContrastOn = !toneTrimOff && cfg.DlssNrAutoContrast.value_or_default();
    if (autoBrightnessOn || autoContrastOn)
    {
        if (EnsureToneGrid(device))
        {
            DlssNrConstants toneParams {};
            toneParams.Mode = DlssNrMode_Meter;
            toneParams.Width = kDlssNrMeterGrid;
            toneParams.Height = kDlssNrMeterGrid;

            const D3D12_RESOURCE_STATES priorTargetState = targetState;
            TransitionTarget(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            DispatchPass(cmdList, toneParams, target, nullptr, nullptr, nullptr, nullptr, g_nr.tone, nullptr);
            TransitionTarget(priorTargetState);

            CopyToneToReadback(cmdList);
            ConsumeToneReadback(isHdrBuffer ? whitePoint : 1.0f, autoBrightnessOn, autoContrastOn);
        }
    }
    else if (g_nr.toneSettled)
    {
        // Off again: back to the sliders, and nothing measured before now survives switching it on.
        ResetAutoTone();
    }

    // Frame hold. Freeze the encode's input so a live setting change re-renders the same frame. This
    // is self-contained on purpose: it copies the output aside on hold-on and copies it BACK over the
    // live output before the encode reads it while held, so the encode's own path and barriers below
    // are untouched and the default (hold off) is byte-identical. See design/frame-hold.md.
    //
    // `target` is UAV here (normalised at entry, restored by the meter block above). The held copy is
    // left in COPY_SOURCE after capture and stays there for every restore.
    {
        const bool hold = cfg.DlssNrHoldFrame.value_or_default();

        if (hold)
        {
            const D3D12_RESOURCE_DESC td = target->GetDesc();
            const bool needCapture = !g_nr.heldActive || g_nr.heldColor == nullptr ||
                                     (unsigned int) td.Width != g_nr.heldWidth || td.Height != g_nr.heldHeight ||
                                     td.Format != g_nr.heldFormat;

            if (needCapture)
            {
                // Hold-on (or the output changed shape under a hold): capture THIS frame, do not
                // restore -- target already holds the frame to freeze, and the pass runs on it.
                if (g_nr.heldColor != nullptr)
                    ParkNrResource(g_nr.heldColor);

                g_nr.heldColor = CreateScratch(device, td.Format, (unsigned int) td.Width, td.Height);

                if (g_nr.heldColor != nullptr)
                {
                    const D3D12_RESOURCE_STATES priorTargetState = targetState;
                    TransitionTarget(D3D12_RESOURCE_STATE_COPY_SOURCE);
                    Barrier(cmdList, g_nr.heldColor, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_COPY_DEST);
                    cmdList->CopyResource(g_nr.heldColor, target);
                    Barrier(cmdList, g_nr.heldColor, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    TransitionTarget(priorTargetState);

                    g_nr.heldActive = true;
                    g_nr.heldWidth = (unsigned int) td.Width;
                    g_nr.heldHeight = td.Height;
                    g_nr.heldFormat = td.Format;
                    g_nr.heldWhitePoint = whitePoint;
                }
            }
            else
            {
                // Held: restore the frozen frame onto the live output before the encode reads it.
                const D3D12_RESOURCE_STATES priorTargetState = targetState;
                TransitionTarget(D3D12_RESOURCE_STATE_COPY_DEST);
                cmdList->CopyResource(target, g_nr.heldColor);
                TransitionTarget(priorTargetState);
            }

            // Suspend white-point measurement while held: use the snapshot so it cannot drift and
            // confound the comparison. (No-op on the capture frame, where the snapshot IS whitePoint.)
            if (g_nr.heldActive)
                whitePoint = g_nr.heldWhitePoint;
        }
        else if (g_nr.heldActive)
        {
            // Released: let go of the frozen frame and resume live input next frame.
            if (g_nr.heldColor != nullptr)
                ParkNrResource(g_nr.heldColor);
            g_nr.heldActive = false;
        }
    }

    DlssNrConstants encodeParams {};
    encodeParams.Mode = DlssNrMode_Encode;
    // A frame that is already display-referred is handed over untouched: the encode becomes a copy and
    // the resolve adds the model's edit back at full scale.
    encodeParams.Passthrough = isHdrBuffer ? 0u : 1u;
    encodeParams.WhitePoint = whitePoint;
    encodeParams.UseGameExposure = useGameExposure;
    encodeParams.ExposurePreMul = exposurePreMul;
    encodeParams.ReversibleMode = cfg.DlssNrReversibleMode.value_or_default();
    // Match only takes effect once a fit exists; until then the table is empty and the shader would
    // read a curve of zeros, so it falls back to the plain proxy.
    encodeParams.Width = width;
    encodeParams.Height = height;

    TransitionTarget(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    DispatchPass(cmdList, encodeParams, target, nullptr, nullptr, nullptr, exposureTex, g_nr.colorCopy, g_nr.hdrCopy);

    if (targetSupportsUav)
        TransitionTarget(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // The transitions double as the wait for the encode's writes.
    Barrier(cmdList, g_nr.colorCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    // Measure the buffer's scale from the copy the encode just kept -- untouched, so there is no path
    // (Calibration pass removed: it produced only a menu suggestion nothing consumed, at the cost
    // of a 4096-thread dispatch, a readback and an nth_element every frame.)

    Barrier(cmdList, g_nr.hdrCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Below full resolution the model is shown a filtered shrink of the proxy; the edit it returns is
    // enlarged during the resolve while the frame underneath stays full size and untouched.
    ID3D12Resource* modelInput = g_nr.colorCopy;

    if (reduced && g_nr.colorSmall != nullptr)
    {
        bool built = false;

        if (workScale > 1.0f)
        {
            // Supersample: enlarge the proxy to the larger working size with a real upscaling filter
            // (the Output Scaling upsampler) so the model sees a clean super-native input, rather than
            // the box minifier which only makes sense going down. colorCopy is NON_PIXEL_SHADER_RESOURCE
            // from the encode (SRV-ready); colorSmall is UNORDERED_ACCESS from last frame's resolve.
            // (Re)build the supersample scalers when missing or when the NR downscaler changed (the
            // filter is baked at construction). Both use NR's own DlssNrScalingDownscaler, independent
            // of Output Scaling, so the two can run different filters at once. superDown is built here
            // and used after the model (the down-leg below).
            const Scaler nrScaler = cfg.DlssNrScalingDownscaler.value_or_default();
            const Upsampler nrUpsampler = cfg.DlssNrScalingUpscaler.value_or_default();
            if (g_nr.nrScaler != nrScaler || g_nr.nrUpsampler != nrUpsampler)
            {
                // Parked rather than deleted, for the reason above: the GPU may still be reading
                // the pipeline a frame or two behind the change.
                ParkNrScaler(g_nr.superUp);
                ParkNrScaler(g_nr.superDown);
                g_nr.nrScaler = nrScaler;
                g_nr.nrUpsampler = nrUpsampler;
            }
            if (g_nr.superUp == nullptr)
                g_nr.superUp = new OS_Dx12("DLSS-NR supersample up", device, true, nrScaler, nrUpsampler);
            if (g_nr.superDown == nullptr)
                g_nr.superDown = new OS_Dx12("DLSS-NR supersample down", device, false, nrScaler);

            if (g_nr.superUp != nullptr && g_nr.superUp->Dispatch(cmdList, g_nr.colorCopy, g_nr.colorSmall))
            {
                Barrier(cmdList, g_nr.colorSmall, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                built = true;
            }
        }

        if (!built)
        {
            if (workScale > 1.0f)
            {
                // Wanted to supersample but the upscaler was not available -- warn once; the box path
                // below can only enlarge blockily, so the user should know the clean path is off.
                static bool warnedSuper = false;
                if (!warnedSuper)
                {
                    warnedSuper = true;
                    LOG_WARN("DLSS-NR supersample: upscaler unavailable, falling back to a blocky enlarge.");
                }
            }

            // Sub-native (or the upsampler could not be built): box-resample the proxy to the work size.
            DlssNrConstants down {};
            down.Mode = DlssNrMode_Downsample;
            down.Width = workWidth;
            down.Height = workHeight;
            DispatchPass(cmdList, down, modelInput, nullptr, nullptr, nullptr, nullptr, g_nr.colorSmall, nullptr);
            Barrier(cmdList, g_nr.colorSmall, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        modelInput = g_nr.colorSmall;
    }

    // Read the exposure scan's candidates on the pass's own command list, once a frame.
    DlssNr::ExposureScan::Tick(device, cmdList);

    ID3D12Resource* depthIn = ReadableGuide(device, cmdList, depth, &g_nr.depthClone);
    ID3D12Resource* motionIn = ReadableGuide(device, cmdList, motion, &g_nr.motionClone);

    if (depthIn == nullptr || motionIn == nullptr)
    {
        g_nr.failed = true;
        g_nr.reason = "the game's depth or motion vectors could not be made readable";
        LOG_ERROR("DLSS-NR unavailable: {}", g_nr.reason);
        FinishColor(false);
        device->Release();
        DLSSNR_BAIL();
    }

    // The vectors were scaled to full-frame pixels; the image the model reprojects is the working size.
    // The vectors were scaled to full-frame pixels; the image the model reprojects is the
    // working size.
    const float mvToWork = width != 0 ? (float) workWidth / (float) width : 1.0f;

    SetExtras(cfg, nullptr, nullptr, 0, 0, 0, 0);

    // The proxy path, when asked for. Same inputs, same model -- the difference is who calls it.
    //
    // Nothing falls back automatically. A silent fallback would mean never finding out the proxy
    // path was broken: the picture would look right either way, because the forwarder would be
    // quietly doing the work.
    if (cfg.DlssNrUseProxy.value_or_default())
    {
        const unsigned int proxyResult = DlssNr::Proxy::Run(
            cmdList, device, modelInput, depthIn, motionIn, g_nr.output, workWidth, workHeight, guideWidth, guideHeight,
            g_nr.guideDepthInverted, g_nr.reset, g_nr.guideMvScaleX * mvToWork, g_nr.guideMvScaleY * mvToWork);

        g_nr.reset = false;

        if (proxyResult != 1)
        {
            g_nr.failed = true;
            g_nr.reason = "the proxy path could not run the model";
            LOG_ERROR("DLSS-NR (proxy): evaluate returned 0x{:X} ({}), disabling for this session", proxyResult,
                      NgxResultName(proxyResult));
        }

        FinishColor(false);
        device->Release();
        DLSSNR_BAIL();
    }

    if (g_ngxTime != nullptr)
        g_ngxTime->Start(cmdList);

    // Count only a contiguous set of ready, separate feature histories. A failed extra creation never
    // falls back to reusing the main feature: that tells one temporal model several frames elapsed in
    // one game frame and makes its history fight the later layers.
    unsigned int effectivePasses = 1;
    if (g_nr.passScratch != nullptr)
    {
        for (unsigned int pass = 1; pass < requestedPasses; ++pass)
        {
            if (g_nr.passFeature[pass] == nullptr || g_nr.passPendingSubmission[pass])
                break;
            ++effectivePasses;
        }
    }

    {
        static unsigned int loggedConfigured = 0;
        static unsigned int loggedEffective = 0;
        if (loggedConfigured != configuredPasses || loggedEffective != effectivePasses)
        {
            loggedConfigured = configuredPasses;
            loggedEffective = effectivePasses;
            LOG_INFO("DLSS-NR model passes: configured {}, effective {}", configuredPasses, effectivePasses);
        }
    }

    // EXPERIMENTAL -- [DlssNr] PassRate. Run the stacked passes on only a fraction of frames, so the
    // cost of "two passes" can be paid partly. The features stay built either way: requestedPasses is
    // what TuningMatchesFeature above compares against, and that is untouched, so a skipped frame is
    // one evaluate call not made rather than a rebuild. Varying the built pass count per frame would
    // rebuild the feature every frame, which is why this reaches effectivePasses and nothing else.
    //
    // A credit accumulator rather than a frame counter: it spreads the skipped frames evenly at any
    // rate, where modulo only does so at rates that divide cleanly.
    //
    // The cost is in the picture, not the plumbing -- a skipped frame is genuinely less processed
    // than a run one, so the output alternates between two looks. See the note on DlssNrPassRate in
    // Config.h. Default 1.0 is every frame, which is byte-identical to before this existed.
    const float passRate = std::clamp(cfg.DlssNrPassRate.value_or_default(), 0.05f, 1.0f);
    if (passRate < 0.999f && effectivePasses > 1)
    {
        static float passCredit = 0.0f;
        passCredit += passRate;
        if (passCredit >= 1.0f)
            passCredit -= 1.0f;
        else
            effectivePasses = 1;
    }

    // Encode happened once above. Keep that base proxy immutable and ping-pong only model answers:
    //   pass 0: base -> A, pass 1: A -> B, pass 2: B -> A.
    // The final answer is resolved once against the original base, so matched-residual transfer is the
    // cumulative final-minus-base edit and colour/transfer controls are not compounded.
    ID3D12Resource* passInput = modelInput;
    ID3D12Resource* passOutput = g_nr.output;
    ID3D12Resource* finalAnswer = nullptr;
    bool outputReadable = false;
    bool scratchReadable = false;

    const auto MakeModelReadable = [&](ID3D12Resource* resource)
    {
        bool& readable = resource == g_nr.output ? outputReadable : scratchReadable;
        if (!readable)
        {
            Barrier(cmdList, resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            readable = true;
        }
    };

    const auto MakeModelWritable = [&](ID3D12Resource* resource)
    {
        bool& readable = resource == g_nr.output ? outputReadable : scratchReadable;
        if (readable)
        {
            Barrier(cmdList, resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            readable = false;
        }
    };

    int result = NVSDK_NGX_Result_Success;

    for (unsigned int pass = 0; pass < effectivePasses && result == NVSDK_NGX_Result_Success; ++pass)
    {
        void* const passFeature = pass == 0 ? g_nr.feature : g_nr.passFeature[pass];
        // Pass one always follows the frame's own reset. For the stacked passes, ChainedHistory
        // decides: on (the default, and what this engine has always done) they reset only on the
        // frame their feature was built and keep their temporal history from then on; off resets
        // them every frame, so each layer is a stateless refinement of the one below it.
        //
        // Neither is free. Keeping history lets a layer accumulate the one below it -- richer, and
        // able to compound ghosting behind fast movement. Resetting every frame cannot compound
        // anything, and NVIDIA documents Reset-per-frame as a flicker and aliasing risk, which is
        // what shimmering on two or three passes usually is.
        const bool chainedHistory = cfg.DlssNrChainedHistory.value_or_default();
        const bool passReset = g_nr.reset || (pass > 0 && (g_nr.passNeedsReset[pass] || !chainedHistory));
        const float passTone = pass == 0 ? cfg.DlssNrLocalTone.value_or_default() : 0.0f;

        MakeModelWritable(passOutput);
        const bool firstEvalOfBuild =
            pass == 0 && g_buildMeasure.awaitingFirstEval && g_buildMeasure.feature == passFeature;
        const auto evalStarted = std::chrono::steady_clock::now();
        result =
            g_nr.evaluate(cmdList, passFeature, g_nr.capabilityParams, passInput, depthIn, motionIn, passOutput,
                          workWidth, workHeight, guideWidth, guideHeight, g_nr.guideDepthInverted ? 1 : 0,
                          passReset ? 1 : 0, cfg.DlssNrIntensity.value_or_default(), (int) PassStyle(cfg, pass),
                          cfg.DlssNrLocalStructure.value_or_default(), passTone,
                          cfg.DlssNrSkinStructure.value_or_default(), cfg.DlssNrAutoMask.value_or_default() ? 1 : 0,
                          g_nr.guideMvScaleX * mvToWork, g_nr.guideMvScaleY * mvToWork);

        // The build's one line: every phase of it, then what the size costs in video memory. Written on the
        // first evaluate because that is the last phase, and the one the first build was never timed for
        // (Resident Evil 2, 2026-09-18). The evaluate is recorded, not waited on: this path has no GPU wait
        // of its own, so its cost on the GPU shows in the pass timer, not here.
        if (firstEvalOfBuild)
        {
            NrBuildMeasure& m = g_buildMeasure;
            m.awaitingFirstEval = false;

            const double evalMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - evalStarted).count();
            const float scale = m.width != 0 ? (float) m.workWidth / (float) m.width : 1.0f;

            LOG_INFO("DLSS-NR build: model {}x{} ({:.0f}% of {}x{}), {} -- teardown {:.1f} ms ({} feature(s) "
                     "released), surfaces {:.1f} ms, CreateFeature {:.1f} ms, first evaluate {:.1f} ms (CPU; no GPU "
                     "wait on this path), result {}",
                     m.workWidth, m.workHeight, scale * 100.0f, m.width, m.height, m.why, m.teardownMs,
                     m.teardownFeatures, m.surfacesMs, m.createMs, evalMs, NgxResultName((unsigned int) result));

            uint64_t usageNow = 0, budgetNow = 0;
            if (m.vramKnown && ReadVideoMemory(device, usageNow, budgetNow))
            {
                // The larger of after-create and after-first-evaluate: some of a feature's memory may only be
                // committed when it first runs. Full-frame surfaces made by the same build are not this size's.
                const uint64_t peak = std::max(usageNow, m.vramAfterCreate);
                const uint64_t delta = peak > m.vramBefore ? peak - m.vramBefore : 0;
                const uint64_t bytes = delta > m.fullFrameBytes ? delta - m.fullFrameBytes : 0;
                const uint64_t pixels = (uint64_t) m.workWidth * m.workHeight;

                // Extra-pass features are built between the create and this first evaluate, so they are in
                // the reading too; the per-pixel figure is per feature.
                unsigned int features = 1;
                for (unsigned int p = 1; p < DlssNr::MaxPassCount; ++p)
                    features += g_nr.passFeature[p] != nullptr ? 1u : 0u;

                // For the log only. The game allocates in the same window, so this is a reading, not a
                // measurement anything steers on.
                const double bpp = pixels != 0 ? (double) bytes / (double) pixels / (double) features : 0.0;

                LOG_INFO("DLSS-NR size {:.0f}%: {} MB ({}x{}; VRAM {} -> {} MB of {} MB, {} MB of it full-frame "
                         "surfaces not counted; {} feature(s), {:.0f} bytes per model pixel each)",
                         scale * 100.0f, bytes >> 20, m.workWidth, m.workHeight, m.vramBefore >> 20, peak >> 20,
                         budgetNow >> 20, m.fullFrameBytes >> 20, features, bpp);
            }
        }

        if (result != NVSDK_NGX_Result_Success)
            break;

        if (pass > 0)
            g_nr.passNeedsReset[pass] = false;

        finalAnswer = passOutput;
        MakeModelReadable(finalAnswer);

        if (pass + 1 < effectivePasses)
        {
            passInput = finalAnswer;
            passOutput = passOutput == g_nr.output ? g_nr.passScratch : g_nr.output;
        }
    }

    if (g_ngxTime != nullptr)
        g_ngxTime->End(cmdList);

    // A reset is a cut, or a new feature: Auto clean up holds still for a moment rather than chasing the
    // new scene's first frames. Not on the Present route while its vectors are zero, where every frame
    // resets and nothing would ever move.
    if (g_nr.reset && !(g_presentRouteDispatch && g_presentMotionBlind))
        g_nr.cleanHoldUntil = g_frames + 10;

    g_nr.reset = false;

    // Supersampling probe: report the model working ABOVE native so a test log tells us whether NGX even
    // accepts a super-native evaluate and what it returns. Once per working-size change, or on any error.
    if (workWidth > width || workHeight > height)
    {
        static unsigned int lastSuper = 0;
        if (lastSuper != workWidth || result != 1)
        {
            lastSuper = workWidth;
            LOG_INFO("DLSS-NR SUPERSAMPLE: model at {}x{} = {:.2f}x native {}x{}, evaluate result {} ({})", workWidth,
                     workHeight, (float) workWidth / (float) width, width, height, result,
                     NgxResultName((unsigned int) result));
        }
    }

    // Once, a few seconds in, so it lands after the values have been written at least once.
    static bool tuningReported = false;

    if (!tuningReported && g_frames > 240)
    {
        tuningReported = true;

        // At INFO, because whether the model actually took a value is the only way to tell a
        // control that does nothing from one that is not being written.
        auto report = [](const char* name, float wrote)
        {
            float value = 0.0f;
            const NVSDK_NGX_Result r = g_nr.capabilityParams->Get(name, &value);
            LOG_INFO("DLSS-NR readback {} -> {} (we wrote {}, result 0x{:X})", name, value, wrote, (uint32_t) r);
        };

        const Config& rcfg = *Config::Instance();
        report("DLSSNR.Intensity", rcfg.DlssNrIntensity.value_or_default());
        report("DLSSNR.LocalStructureStrength", rcfg.DlssNrLocalStructure.value_or_default());
        report("DLSSNR.LocalToneStrength", rcfg.DlssNrLocalTone.value_or_default());
        report("DLSSNR.SkinStructureStrength", rcfg.DlssNrSkinStructure.value_or_default());

        unsigned int style = 0;
        const NVSDK_NGX_Result styleResult = g_nr.capabilityParams->Get("DLSSNR.Style", &style);
        LOG_DEBUG("DLSS-NR readback DLSSNR.Style -> {} (result 0x{:X})", style, (uint32_t) styleResult);

        // The preset is the last control whose arrival has never been checked, and three of them look
        // identical in play. Either it is not landing or the presets really are alike.
        unsigned int preset = 0;
        const NVSDK_NGX_Result presetResult = g_nr.capabilityParams->Get("DLSSNR.Hint.Render.Preset", &preset);
        LOG_DEBUG("DLSS-NR readback DLSSNR.Hint.Render.Preset -> {} (result 0x{:X}, we wrote {})", preset,
                  (uint32_t) presetResult, PassPreset(cfg, 0));

        LOG_DEBUG("DLSS-NR wrote intensity {}, local structure {}, local tone {}, skin {}, style {}",
                  cfg.DlssNrIntensity.value_or_default(), cfg.DlssNrLocalStructure.value_or_default(),
                  cfg.DlssNrLocalTone.value_or_default(), cfg.DlssNrSkinStructure.value_or_default(),
                  PassStyle(cfg, 0));
    }

    if (result == NVSDK_NGX_Result_Success)
    {
        // Resolve takes the difference between what the model returned and what it was shown, and adds
        // that back to the frame. At strength zero the result is what the upscaler produced, exactly, and
        // anything the model left alone is untouched rather than round-tripped through the curve.
        DlssNrConstants resolveParams {};
        resolveParams.Mode = DlssNrMode_Resolve;
        resolveParams.WhitePoint = whitePoint;
        resolveParams.UseGameExposure = useGameExposure;
        resolveParams.ExposurePreMul = exposurePreMul;
        resolveParams.Width = width;
        resolveParams.Height = height;
        resolveParams.TransferStrength = cfg.DlssNrTransferStrength.value_or_default();
        resolveParams.ColourStrength = cfg.DlssNrColourStrength.value_or_default();
        // Auto, where it is on, in place of the slider; the slider's own value is kept for when it is off.
        // With RenoDX in the game, identity: RenoDX grades the picture (the ini values are kept).
        const bool trimOff = DlssNrRenoDx::ToneTrimSuppressed();
        const bool autoB = !trimOff && cfg.DlssNrAutoBrightness.value_or_default();
        const bool autoC = !trimOff && cfg.DlssNrAutoContrast.value_or_default();
        resolveParams.Brightness =
            trimOff ? 1.0f : (autoB ? g_nr.autoBrightness : cfg.DlssNrBrightness.value_or_default());
        resolveParams.Contrast = trimOff ? 1.0f : (autoC ? g_nr.autoContrast : cfg.DlssNrContrast.value_or_default());
        resolveParams.DebugView = cfg.DlssNrDebugView.value_or_default();
        resolveParams.MaxRatio = cfg.DlssNrMaxRatio.value_or_default();
        resolveParams.Transfer = cfg.DlssNrTransfer.value_or_default();
        resolveParams.DebugScale = cfg.DlssNrWhitePointScale.value_or_default();
        resolveParams.Passthrough = isHdrBuffer ? 0u : 1u;

        // Image Clean Up. Strength 0 when it is off, which the shader reads as "skip it all". The motion
        // slot holds the game's vectors here (motionIn), in the guide's pixels once scaled by the game's
        // own MV scale; the extra factor takes them to pixels of this dispatch. On the Present route with
        // no optical flow they are a field of zeros, and are then treated as absent.
        const uint32_t cleanMode = cfg.DlssNrCleanUpMode.value_or_default();
        const bool cleanOn = cleanMode == 1 || cleanMode == 2;
        const bool cleanAuto = cleanMode == 1;
        const bool cleanShown = cleanOn || resolveParams.DebugView == 4;
        const bool cleanBlind = g_presentRouteDispatch && g_presentMotionBlind;
        const bool haloMeter = cleanOn && EnsureHaloGrids(device);
        const float cleanCap = std::clamp(cfg.DlssNrCleanUpMaxStrength.value_or_default(), 0.0f, 1.0f);

        if (haloMeter)
        {
            unsigned int tiles = 0;
            const float before = ReadHalo(0, &tiles);
            const float after = ReadHalo(1, &tiles);
            const float model = ReadHalo(2, &tiles);
            if (before >= 0.0f)
                g_nr.haloBefore = before;
            if (after >= 0.0f)
                g_nr.haloAfter = after;
            if (model >= 0.0f)
                g_nr.haloModel = model;
            if (cleanAuto)
                UpdateCleanAuto(before, cleanCap);
        }
        else if (!cleanOn && g_nr.cleanAutoSettled)
        {
            ResetCleanUp();
        }

        float cleanStrength = 0.0f;
        if (cleanOn)
        {
            cleanStrength = cleanAuto ? g_nr.cleanAutoStrength
                                      : std::clamp(cfg.DlssNrCleanUpStrength.value_or_default(), 0.0f, 1.0f);
            // Never exactly zero while on, so the mask history keeps being written; a strength this small
            // moves nothing (the shader applies only a real move).
            resolveParams.CleanupStrength = std::max(cleanStrength, 1e-6f);
            resolveParams.CleanupHaveMotion =
                motionIn == nullptr || cleanBlind ? 0u : (g_presentRouteDispatch ? 2u : 1u);
        }
        g_nr.cleanApplied = cleanStrength;
        resolveParams.CleanupEdge =
            cleanAuto ? kCleanAutoEdge : std::clamp(cfg.DlssNrCleanUpEdge.value_or_default(), 0.25f, 4.0f);
        resolveParams.CleanupBalance =
            cleanAuto ? kCleanAutoBalance : std::clamp(cfg.DlssNrCleanUpBalance.value_or_default(), 0.0f, 1.0f);
        resolveParams.CleanupMotion =
            cleanAuto ? kCleanAutoMotion : std::clamp(cfg.DlssNrCleanUpMotion.value_or_default(), 0.0f, 1.0f);
        resolveParams.CleanupHaveDepth = cleanShown && DepthReadable(depthIn) ? 1u : 0u;
        resolveParams.CleanupDepthInverted = g_nr.guideDepthInverted ? 1u : 0u;
        resolveParams.CleanupProfile = cfg.DlssNrCleanUpProfile.value_or_default();

        // The mask history: written from the first frame, read from the second, dropped with the clean up.
        bool cleanHistory = false;
        if (cleanShown && EnsureCleanHistory(device, width, height))
        {
            cleanHistory = true;
            resolveParams.CleanupHistory = g_nr.cleanHistoryValid ? 2u : 1u;
        }
        else if (!cleanShown)
        {
            g_nr.cleanHistoryValid = false;
        }
        resolveParams.GuideWidth = guideWidth;
        resolveParams.GuideHeight = guideHeight;
        resolveParams.MvScaleX = guideWidth > 0 ? g_nr.guideMvScaleX * (float) width / (float) guideWidth : 0.0f;
        resolveParams.MvScaleY = guideHeight > 0 ? g_nr.guideMvScaleY * (float) height / (float) guideHeight : 0.0f;
        resolveParams.ReversibleMode = cfg.DlssNrReversibleMode.value_or_default();
        resolveParams.ApplyModel = cfg.DlssNrApplyModel.value_or_default() ? 1u : 0u;
        resolveParams.CompareMode = cfg.DlssNrCompare.value_or_default();
        resolveParams.CompareSplit = cfg.DlssNrCompareSplit.value_or_default();
        resolveParams.CompareZoom = std::max(1.0f, cfg.DlssNrCompareZoom.value_or_default());
        resolveParams.CompareSwap = cfg.DlssNrCompareSwap.value_or_default() ? 1u : 0u;

        // The numbers the composition actually ran with, logged when any of them changes.
        //
        // A colour report without these cannot be read. Paper white alone decides whether the model
        // was shown a sensible picture or a blown one, and it was absent from every log in the first
        // round of reports -- one tester's "much better at 16" had to be taken on trust because
        // nothing in the file said what the value was. Debug view and compare mode are here for the
        // same reason from the other direction: both change what is on screen, and a screenshot with
        // one left on is indistinguishable from a bug.
        struct ComposeReport
        {
            bool valid;
            float whitePoint;
            float transfer;
            float colour;
            float maxRatio;
            unsigned int passthrough;
            unsigned int debugView;
            unsigned int compareMode;
            unsigned int residual;
            unsigned int workW;
            unsigned int workH;
            unsigned int passes;
        };

        static ComposeReport loggedCompose {};

        // Quantised to the precision it is printed at. Comparing raw floats logged 2376 lines in one
        // Enshrouded session, because a measured white point drifts continuously and every drift was a
        // change. A line per meaningful change is the point; a line per frame is a different problem.
        const ComposeReport composeNow { true,
                                         std::round(resolveParams.WhitePoint * 100.0f) / 100.0f,
                                         resolveParams.TransferStrength,
                                         resolveParams.ColourStrength,
                                         resolveParams.MaxRatio,
                                         resolveParams.Passthrough,
                                         resolveParams.DebugView,
                                         resolveParams.CompareMode,
                                         resolveParams.Transfer,
                                         g_nr.workWidth,
                                         g_nr.workHeight,
                                         effectivePasses };

        if (!loggedCompose.valid || loggedCompose.whitePoint != composeNow.whitePoint ||
            loggedCompose.transfer != composeNow.transfer || loggedCompose.colour != composeNow.colour ||
            loggedCompose.maxRatio != composeNow.maxRatio || loggedCompose.passthrough != composeNow.passthrough ||
            loggedCompose.debugView != composeNow.debugView || loggedCompose.compareMode != composeNow.compareMode ||
            loggedCompose.residual != composeNow.residual || loggedCompose.workW != composeNow.workW ||
            loggedCompose.workH != composeNow.workH || loggedCompose.passes != composeNow.passes)
        {
            loggedCompose = composeNow;
            LOG_INFO("DLSS-NR composition: paper white {:.2f}x, detail {:.2f}, colour {:.2f}, guard "
                     "{:.1f}x, colour transform {}, transfer {}, model {}x{}, passes {}, debug view {}, compare {}",
                     composeNow.whitePoint, composeNow.transfer, composeNow.colour, composeNow.maxRatio,
                     composeNow.passthrough != 0 ? "off (frame already tone mapped)" : "on (linear HDR)",
                     composeNow.residual == 1 ? "matched residual" : "classic", composeNow.workW, composeNow.workH,
                     composeNow.passes, composeNow.debugView, composeNow.compareMode);
        }

        // Image Clean Up's settings, on their own line and at most once a second: a slider being dragged
        // changes them every frame, and one line per frame is noise. The last value is always written
        // once the second is up, so the log still ends on what was actually set.
        {
            struct CleanReport
            {
                uint32_t mode;
                float value; // the cap in Auto, the strength in Manual
                float edge;
                float balance;
                float motion;
                uint32_t depth;
                uint32_t vectors;
                bool history;
                bool present;

                bool operator==(const CleanReport&) const = default;
            };

            static CleanReport loggedClean { ~0u };
            static CleanReport pendingClean {};
            static bool cleanPending = false;
            static auto cleanLoggedAt = std::chrono::steady_clock::time_point {};

            const CleanReport cleanNow { cleanOn ? cleanMode : 0u,
                                         cleanOn ? std::round((cleanAuto ? cleanCap : cleanStrength) * 100.0f) / 100.0f
                                                 : 0.0f,
                                         cleanOn ? resolveParams.CleanupEdge : 0.0f,
                                         cleanOn ? resolveParams.CleanupBalance : 0.0f,
                                         cleanOn ? resolveParams.CleanupMotion : 0.0f,
                                         cleanOn ? resolveParams.CleanupHaveDepth : 0u,
                                         cleanOn ? resolveParams.CleanupHaveMotion + (cleanBlind ? 10u : 0u) : 0u,
                                         cleanOn && cleanHistory,
                                         cleanOn && g_presentRouteDispatch };

            if (!(cleanNow == loggedClean))
            {
                pendingClean = cleanNow;
                cleanPending = true;
            }

            const auto nowClean = std::chrono::steady_clock::now();
            if (cleanPending && nowClean - cleanLoggedAt >= std::chrono::seconds(1))
            {
                const CleanReport& c = pendingClean;
                if (c.mode == 0)
                    LOG_INFO("DLSS-NR image clean up: off");
                else
                    LOG_INFO("DLSS-NR image clean up: {} ({} {:.2f}), edge {:.2f} stops, reach {:.2f}, motion {:.2f}; "
                             "depth {}, motion vectors {}, mask history {}{}",
                             c.mode == 1 ? "auto" : "manual", c.mode == 1 ? "cap" : "strength", c.value, c.edge,
                             c.balance, c.motion, c.depth != 0 ? "read" : "absent",
                             c.vectors == 1    ? "read (game)"
                             : c.vectors == 2  ? "read (optical flow: only fast motion counts)"
                             : c.vectors >= 10 ? "zero (ignored)"
                                               : "absent",
                             c.history ? "on" : "off", c.present ? ", Present route" : "");
                loggedClean = pendingClean;
                cleanPending = false;
                cleanLoggedAt = nowClean;
            }
        }

        // Supersampling down-leg. Average the Nx model answer back to native with the chosen filter, so
        // the resolve composites a native answer against the native proxy 1:1 -- a real area resample,
        // not the single bilinear tap the Nx answer would otherwise get in the resolve (which aliases
        // the model's detail into noise, the "noisier above 100%" the probe showed). On success the
        // resolve reads the native proxy (colorCopy) and native answer (outputNative); on failure it
        // falls back to the Nx pair. finalAnswer is NPSR here; outputNative is UAV from last frame.
        bool superDownOk = false;
        if (workScale > 1.0f && g_nr.superDown != nullptr && g_nr.outputNative != nullptr &&
            g_nr.superDown->Dispatch(cmdList, finalAnswer, g_nr.outputNative))
        {
            Barrier(cmdList, g_nr.outputNative, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            superDownOk = true;
        }

        ID3D12Resource* resolveProxy = superDownOk ? g_nr.colorCopy : modelInput;
        ID3D12Resource* resolveAnswer = superDownOk ? g_nr.outputNative : finalAnswer;

        // Below full resolution: enlarge the answer and its proxy here, with the filter chosen in the
        // panel, instead of leaving the resolve to sample them bilinearly at full-size UVs. Both legs
        // take the same filter -- the composition is handed model minus proxy, and two filters would
        // put the difference between them inside that edit. On failure nothing changes: the resolve
        // reads the small pair and enlarges them itself, exactly as it did before.
        bool enlargedOk = false;
        if (reduced && !superDownOk && g_nr.editNative != nullptr && g_nr.proxyNative != nullptr)
        {
            const Scaler nrScaler = cfg.DlssNrScalingDownscaler.value_or_default();
            const Upsampler nrUpsampler = cfg.DlssNrScalingUpscaler.value_or_default();

            if (g_nr.editUp != nullptr && (g_nr.nrScaler != nrScaler || g_nr.nrUpsampler != nrUpsampler))
            {
                // Parked, not deleted: frames built with the old filter are still in flight.
                ParkNrScaler(g_nr.editUp);
                ParkNrScaler(g_nr.proxyUp);
            }
            g_nr.nrScaler = nrScaler;
            g_nr.nrUpsampler = nrUpsampler;

            if (g_nr.editUp == nullptr)
                g_nr.editUp = new OS_Dx12("DLSS-NR enlarge answer", device, true, nrScaler, nrUpsampler);
            if (g_nr.proxyUp == nullptr)
                g_nr.proxyUp = new OS_Dx12("DLSS-NR enlarge proxy", device, true, nrScaler, nrUpsampler);

            if (g_nr.editUp != nullptr && g_nr.proxyUp != nullptr &&
                g_nr.editUp->Dispatch(cmdList, finalAnswer, g_nr.editNative) &&
                g_nr.proxyUp->Dispatch(cmdList, modelInput, g_nr.proxyNative))
            {
                Barrier(cmdList, g_nr.editNative, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(cmdList, g_nr.proxyNative, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                resolveProxy = g_nr.proxyNative;
                resolveAnswer = g_nr.editNative;
                enlargedOk = true;
            }
        }

        // Matched residual re-bases an edit that came from a reduced raster, so it has to be told when
        // the pair it is handed has already been enlarged -- their sizes no longer say it. Transfer 2
        // is matched residual plus that fact; classic (0) needs nothing, because a pre-enlarged answer
        // IS the complete picture at frame size, which is what classic composes.
        if (enlargedOk && resolveParams.Transfer == 1)
            resolveParams.Transfer = 2;

        // Pre-SR Color is not guaranteed to have UAV support. Write directly when legal; otherwise
        // resolve into hdrCopy while the original Color remains readable, then copy the result back.
        ID3D12Resource* resolveOriginal = targetSupportsUav ? g_nr.hdrCopy : target;
        ID3D12Resource* resolveTarget = targetSupportsUav ? target : g_nr.hdrCopy;

        if (targetSupportsUav)
        {
            TransitionTarget(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        else
        {
            Barrier(cmdList, g_nr.hdrCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        if (g_composeTime != nullptr)
            g_composeTime->Start(cmdList);

        ID3D12Resource* historyRead = nullptr;
        ID3D12Resource* historyWrite = nullptr;
        if (cleanHistory)
        {
            const int write = (int) g_nr.cleanHistoryIndex;
            const int read = write ^ 1;
            MoveCleanHistory(cmdList, write, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            MoveCleanHistory(cmdList, read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            historyWrite = g_nr.cleanHistory[write];
            historyRead = g_nr.cleanHistory[read];
            MoveCleanModel(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        // Image Clean Up's prep, when the resolve will run the clean up (the shader's CleanupWanted): the
        // frame's log luminance, log depth and chroma worked out once per pixel, and each 8x8 block's range,
        // so the resolve's groups load their tile instead of computing it sixteen times over, and a group
        // with no edge near it skips its tile altogether. Nothing is dispatched or allocated with the clean
        // up off.
        ID3D12Resource* cleanPrepIn = nullptr;
        const bool cleanWanted = (resolveParams.CleanupStrength > 0.0f || resolveParams.DebugView == 4) &&
                                 resolveParams.CompareMode != 1 && resolveParams.ApplyModel != 0;
        if (cleanWanted && HasCleanupPrep() && (resolveParams.CleanupProfile & 32u) == 0u)
        {
            const D3D12_RESOURCE_DESC originalDesc = resolveOriginal->GetDesc();
            const unsigned int ow = (unsigned int) originalDesc.Width;
            const unsigned int oh = originalDesc.Height;

            if (EnsureCleanPrep(device, ow, oh))
            {
                DlssNrConstants prepParams = resolveParams;
                prepParams.Mode = DlssNrMode_CleanupPrep;
                prepParams.Width = ow;
                prepParams.Height = oh;
                MoveCleanPrep(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                if (DispatchPass(cmdList, prepParams, resolveProxy, nullptr, resolveOriginal, nullptr, nullptr,
                                 g_nr.cleanPrep, nullptr, resolveParams.CleanupHaveDepth != 0 ? depthIn : nullptr))
                {
                    MoveCleanPrep(cmdList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    resolveParams.CleanupPrepared = 1;
                    cleanPrepIn = g_nr.cleanPrep;
                }
            }
        }

        DispatchPass(cmdList, resolveParams, resolveProxy, resolveAnswer, resolveOriginal, motionIn, exposureTex,
                     resolveTarget, historyWrite, resolveParams.CleanupHaveDepth != 0 ? depthIn : nullptr, historyRead,
                     cleanHistory ? g_nr.cleanModel : nullptr, cleanPrepIn);

        // Image Clean Up capture: this frame's inputs and outputs, copied for the offline harness. The
        // composed picture before the clean up is a second resolve with the clean up off, into a scratch
        // surface of its own. Depth and motion are copied only when they are this pass's own copies, whose
        // state is known; the game's own resources are not touched.
        if (g_cleanCapture.wanted() && !g_cleanCapture.pending())
        {
            const D3D12_RESOURCE_DESC targetDesc = resolveTarget->GetDesc();
            if (g_cleanCaptureBefore != nullptr)
                ParkNrResource(g_cleanCaptureBefore);
            g_cleanCaptureBefore = CreateScratch(device, g_nr.hdrCopy->GetDesc().Format,
                                                 (unsigned int) targetDesc.Width, targetDesc.Height);

            if (g_cleanCaptureBefore != nullptr)
            {
                DlssNrConstants beforeParams = resolveParams;
                beforeParams.CleanupStrength = 0.0f;
                beforeParams.CleanupHistory = 0;
                beforeParams.DebugView = 0;
                DispatchPass(cmdList, beforeParams, resolveProxy, resolveAnswer, resolveOriginal, motionIn, exposureTex,
                             g_cleanCaptureBefore, nullptr);
            }

            const D3D12_RESOURCE_STATES originalState =
                targetSupportsUav ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : targetState;
            g_cleanCapture.copy(cmdList, device, "input", resolveOriginal, originalState);
            g_cleanCapture.copy(cmdList, device, "proxy", resolveProxy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            g_cleanCapture.copy(cmdList, device, "model", resolveAnswer,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            g_cleanCapture.copy(cmdList, device, "composed_before", g_cleanCaptureBefore,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            g_cleanCapture.copy(cmdList, device, "composed_after", resolveTarget,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            const bool ownDepth = depthIn != nullptr && (depthIn == g_nr.depthClone || g_presentRouteDispatch);
            g_cleanCapture.copy(cmdList, device, "depth", ownDepth ? depthIn : nullptr,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            const bool ownMotion = motionIn != nullptr && (motionIn == g_nr.motionClone || g_presentRouteDispatch);
            g_cleanCapture.copy(cmdList, device, "motion", ownMotion ? motionIn : nullptr,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            g_cleanCapture.copy(cmdList, device, "mask", historyWrite, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            g_cleanCapture.copy(cmdList, device, "model_reading", cleanHistory ? g_nr.cleanModel : nullptr,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            const auto& c = resolveParams;
            const std::string settings = std::format(
                "{{\"frame\":{},\"presentRoute\":{},\"passthrough\":{},\"whitePoint\":{},\"useGameExposure\":{},"
                "\"exposurePreMul\":{},\"transferStrength\":{},\"colourStrength\":{},\"maxRatio\":{},"
                "\"transfer\":{},\"reversibleMode\":{},\"brightness\":{},\"contrast\":{},\"debugView\":{},"
                "\"compareMode\":{},\"width\":{},\"height\":{},\"guideWidth\":{},\"guideHeight\":{},"
                "\"mvScaleX\":{},\"mvScaleY\":{},\"cleanupMode\":{},\"cleanupStrength\":{},\"cleanupEdge\":{},"
                "\"cleanupBalance\":{},\"cleanupMotion\":{},\"cleanupHaveMotion\":{},\"cleanupHaveDepth\":{},"
                "\"cleanupDepthInverted\":{},\"cleanupHistory\":{},\"cleanupProfile\":{},\"passes\":{},"
                "\"workWidth\":{},\"workHeight\":{},\"haloBefore\":{},\"haloAfter\":{},\"haloModel\":{}}}",
                g_frames, g_presentRouteDispatch, c.Passthrough, c.WhitePoint, c.UseGameExposure, c.ExposurePreMul,
                c.TransferStrength, c.ColourStrength, c.MaxRatio, c.Transfer, c.ReversibleMode, c.Brightness,
                c.Contrast, c.DebugView, c.CompareMode, c.Width, c.Height, c.GuideWidth, c.GuideHeight, c.MvScaleX,
                c.MvScaleY, cleanMode, c.CleanupStrength, c.CleanupEdge, c.CleanupBalance, c.CleanupMotion,
                c.CleanupHaveMotion, c.CleanupHaveDepth, c.CleanupDepthInverted, c.CleanupHistory, c.CleanupProfile,
                effectivePasses, g_nr.workWidth, g_nr.workHeight, g_nr.haloBefore, g_nr.haloAfter, g_nr.haloModel);
            g_cleanCapture.finish(settings, g_frames, 8);
            LOG_INFO("DLSS-NR image clean up capture recorded at frame {}; writing in 8 frames", g_frames);
        }

        // The halo meter: the per-pixel readings the resolve just wrote, reduced to three 64x64 grids.
        // Only when the resolve really ran the clean up -- not while a debug view other than the mask, the
        // side by side comparison or "Apply model" off made it leave before it, which would leave last
        // frame's readings in place. Timed with the composition, since it is the clean up's cost.
        const bool meterNow = haloMeter && cleanHistory && (resolveParams.CleanupProfile & 1u) == 0u &&
                              resolveParams.ApplyModel != 0 && resolveParams.CompareMode != 1 &&
                              (resolveParams.DebugView == 0 || resolveParams.DebugView == 4);
        if (meterNow)
        {
            MoveCleanHistory(cmdList, (int) g_nr.cleanHistoryIndex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            MoveCleanModel(cmdList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            DlssNrConstants haloParams = resolveParams;
            haloParams.Mode = DlssNrMode_HaloMeter;
            haloParams.Width = kDlssNrMeterGrid;
            haloParams.Height = kDlssNrMeterGrid;
            DispatchPass(cmdList, haloParams, historyWrite, nullptr, nullptr, nullptr, nullptr, g_nr.haloGrid[0],
                         g_nr.haloGrid[1], nullptr, historyWrite, g_nr.haloGrid[2], g_nr.cleanModel);
        }

        if (cleanHistory)
        {
            g_nr.cleanHistoryIndex ^= 1u;
            g_nr.cleanHistoryValid = true;
        }

        if (g_composeTime != nullptr)
            g_composeTime->End(cmdList);

        if (meterNow)
        {
            CopyHaloToReadback(cmdList, 0);
            CopyHaloToReadback(cmdList, 1);
            CopyHaloToReadback(cmdList, 2);
            g_nr.haloFrames++;
        }

        if (!targetSupportsUav)
        {
            Barrier(cmdList, g_nr.hdrCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            const D3D12_RESOURCE_STATES priorTargetState = targetState;
            TransitionTarget(D3D12_RESOURCE_STATE_COPY_DEST);
            cmdList->CopyResource(target, g_nr.hdrCopy);
            TransitionTarget(priorTargetState);
            Barrier(cmdList, g_nr.hdrCopy, D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        MakeModelWritable(g_nr.output);
        if (g_nr.passScratch != nullptr)
            MakeModelWritable(g_nr.passScratch);

        if (superDownOk)
            Barrier(cmdList, g_nr.outputNative, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        // The enlarged pair goes back to writable for the next frame, exactly as the supersample
        // target above does. Left in NON_PIXEL_SHADER_RESOURCE, the next frame's upscaler writes a UAV
        // to a resource that is not in that state -- which is a hung device a second after the filter
        // is chosen (DXGI_ERROR_DEVICE_HUNG, Shadow of the Tomb Raider, 2026-09-22).
        if (enlargedOk)
        {
            Barrier(cmdList, g_nr.editNative, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Barrier(cmdList, g_nr.proxyNative, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        // On-demand capture works in this path too: the staging copy still holds the frame as the
        // upscaler produced it, and the edited frame is the output itself. The write happens a few
        // frames later, once the GPU is certainly past these copies -- this path has no fence of its
        // own.
        if (g_capture.isActive())
        {
            g_capture.record(cmdList, device, g_nr.colorCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, target,
                             targetState);

            if (g_capture.readyToWrite() && g_captureWriteAtFrame == 0)
                g_captureWriteAtFrame = g_frames + 8;
        }
    }
    else
    {
        g_nr.failed = true;
        g_nr.reason = "the model refused to run";
        LOG_ERROR("DLSS-NR evaluate returned 0x{:X} ({}), disabling for this session", (uint32_t) result,
                  NgxResultName((unsigned int) result));
    }

    // On an evaluation failure, intermediate A/B inputs may still be readable. Restore both persistent
    // ping-pong surfaces to the UAV state the next frame starts from.
    MakeModelWritable(g_nr.output);
    if (g_nr.passScratch != nullptr)
        MakeModelWritable(g_nr.passScratch);

    Barrier(cmdList, g_nr.hdrCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Failed evaluations leave the game's original image intact. A successful copy-back writes
    // only the active rectangle and restores both resources before DLSS consumes the image.
    FinishColor(result == NVSDK_NGX_Result_Success);

    if (g_gpuTime != nullptr)
    {
        g_gpuTime->End(cmdList);

        // This path records into the game's own list, so there is no queue of ours to read from.
        // A caller that knows which queue the list goes to says so; otherwise the one the upscaler was
        // invoked on serves. The bridges have to say, because they run on a queue of their own that
        // State never learns about -- a Vulkan game creates no D3D12 swapchain, so nothing ever sets
        // currentCommandQueue and the cost went unreported.
        auto* queue =
            timingQueue != nullptr ? timingQueue : (ID3D12CommandQueue*) State::Instance().currentCommandQueue;

        // The queue is only asked for the timestamp frequency, which belongs to the GPU rather than to any
        // one queue, and the timestamps themselves come back through the readback whichever queue runs the
        // list. So with no queue known -- the DLSS5 Feeder's list, whose queue nothing here ever sees -- a
        // small queue of this pass's own answers the question. Without it the panel showed a green
        // "Running." with no cost on those games while others showed milliseconds (2026-09-14).
        if (queue == nullptr)
        {
            static ID3D12CommandQueue* frequencyQueue = nullptr;
            static ID3D12Device* frequencyDevice = nullptr;
            static bool frequencyQueueFailed = false;

            ID3D12Device* listDevice = nullptr;

            if (!frequencyQueueFailed && SUCCEEDED(cmdList->GetDevice(IID_PPV_ARGS(&listDevice))) &&
                listDevice != nullptr)
            {
                if (frequencyDevice != listDevice)
                {
                    // Retired rather than released if the device ever changes: cheap, and never in flight.
                    frequencyQueue = nullptr;
                    frequencyDevice = listDevice;

                    D3D12_COMMAND_QUEUE_DESC desc {};
                    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

                    if (FAILED(listDevice->CreateCommandQueue(&desc, IID_PPV_ARGS(&frequencyQueue))))
                    {
                        frequencyQueue = nullptr;
                        frequencyQueueFailed = true;
                    }
                }

                listDevice->Release();
            }

            queue = frequencyQueue;
        }

        if (queue != nullptr)
        {
            auto ms = g_gpuTime->ReadGpuTime(queue);

            if (ms.has_value() && NrTimingTrust::Plausible(ms.value()))
                g_lastGpuTime = ms;

            if (g_timingTrust.Add(ms))
                LOG_WARN("DLSS-NR: the GPU pass timer gives scattered readings on this card/route (last {:.2f} ms); "
                         "the panel shows DLSS 5 on without a cost this session",
                         ms.value_or(-1.0));

            if (g_ngxTime != nullptr)
            {
                if (auto ngx = g_ngxTime->ReadGpuTime(queue); ngx.has_value())
                    g_lastNgxTime = ngx;
            }

            if (g_composeTime != nullptr)
            {
                if (auto compose = g_composeTime->ReadGpuTime(queue);
                    compose.has_value() && NrTimingTrust::Plausible(compose.value()))
                    g_lastComposeTime = compose;
            }

            // The split, once every few hundred frames. What is worth reading is not the total but the
            // remainder: the model's cost is NVIDIA's to set, and everything else is ours.
            static unsigned long long lastSplitLog = 0;

            if (!g_timingTrust.Untrusted() && g_lastGpuTime.has_value() && g_lastNgxTime.has_value() &&
                g_frames - lastSplitLog > 600)
            {
                lastSplitLog = g_frames;
                const double total = g_lastGpuTime.value();
                const double ngx = g_lastNgxTime.value();
                const uint32_t cleanMode = Config::Instance()->DlssNrCleanUpMode.value_or_default();
                LOG_INFO("DLSS-NR cost: {:.2f} ms total = {:.2f} ms model + {:.2f} ms ours ({:.0f}% ours); "
                         "composition {} (image clean up {})",
                         total, ngx, total - ngx, total > 0.0 ? 100.0 * (total - ngx) / total : 0.0,
                         g_lastComposeTime.has_value() ? std::format("{:.3f} ms", g_lastComposeTime.value())
                                                       : std::string("n/a"),
                         cleanMode == 1 ? "auto" : (cleanMode == 2 ? "manual" : "off"));
            }
        }
    }

    // Heartbeat, every 600 frames the pass ran, whatever else is or is not available. The lines above
    // only ever say that a pass started (and the cost split needs a queue this app knows about, which
    // Resident Evil 2 through REFramework never gave it), so a log could not tell "running all session"
    // from "ran once" -- and a user comparing settings needs exactly that. Settings are the ones in
    // effect now, so a change made in the panel shows up here within 600 frames.
    {
        ++g_dispatchDone;
        static unsigned long long beatOk = 0;
        static unsigned long long beatFailed = 0;
        static unsigned long long beatLastTotal = 0;
        static auto beatLastTime = std::chrono::steady_clock::now();

        if (result == NVSDK_NGX_Result_Success)
            ++beatOk;
        else
            ++beatFailed;

        const unsigned long long total = beatOk + beatFailed;
        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - beatLastTime).count();

        // 600 frames, or two seconds, whichever comes first. The frame count alone made the interval
        // scale with slowness: at 48 fps a line every 12 s, at the 18 fps a three-pass Tomb Raider I-III
        // ran at, every 33 s -- and the manager's pop-out panel, which reads this line to show what
        // the pass costs, sat frozen for half a minute after a slider moved and then called the reading
        // stale. The one place a player most wants live cost figures is the game running slowest.
        if (total > beatLastTotal && (total - beatLastTotal >= 600 || seconds >= 2.0))
        {
            const double fps = seconds > 0.0 ? double(total - beatLastTotal) / seconds : 0.0;
            beatLastTotal = total;
            beatLastTime = now;

            const auto* cfg = Config::Instance();
            const bool queueKnown = timingQueue != nullptr || State::Instance().currentCommandQueue != nullptr;
            const std::string gpu = g_timingTrust.Untrusted() ? std::string("n/a (timer unreliable)")
                                    : g_lastGpuTime.has_value()
                                        ? std::format("{:.2f} ms", g_lastGpuTime.value())
                                        : (queueKnown ? std::string("not read yet") : std::string("n/a (no queue)"));

            LOG_INFO("DLSS-NR heartbeat: {} frames run ({} model failures), {:.0f} fps, GPU {} | "
                     "intensity {:.2f}, preset {}, style {}, passes {} ({} this frame), {}",
                     total, beatFailed, fps, gpu, cfg->DlssNrIntensity.value_or_default(),
                     cfg->DlssNrPreset.value_or_default(), cfg->DlssNrStyle.value_or_default(),
                     cfg->DlssNrPasses.value_or_default(), effectivePasses,
                     reduced ? "reduced resolution" : "full resolution");

            // Image Clean Up's measurement, beside the heartbeat so it reads against the same frames: the
            // model's glow, what is left of it after the clean up, and the strength that did it.
            const uint32_t cleanMode = cfg->DlssNrCleanUpMode.value_or_default();
            if (cleanMode == 1 || cleanMode == 2)
            {
                const auto stops = [](float v)
                { return v >= 0.0f ? std::format("{:.3f} stops", v) : std::string("not measured yet"); };
                LOG_INFO("DLSS-NR image clean up: {}, strength {:.2f}, halo {} in the composed picture, {} after "
                         "the clean up, {} in the model's own change{}",
                         cleanMode == 1 ? "auto" : "manual", g_nr.cleanApplied, stops(g_nr.haloBefore),
                         stops(g_nr.haloAfter), stops(g_nr.haloModel),
                         g_presentRouteDispatch ? " (Present route)" : "");
            }
        }
    }

    // Put any guide clones back where the next frame's copy expects to find them.
    // A clone left in NON_PIXEL_SHADER_RESOURCE by a frozen frame was never transitioned back to
    // COPY_DEST, because a frozen frame does not copy. Putting it back unconditionally would be a
    // barrier from a state it is not in, so the frozen case is skipped here and picked up by the
    // first live frame after the toggle goes off -- which is a copy, and copies transition it.
    if (g_nr.depthClone != nullptr)
        Barrier(cmdList, g_nr.depthClone, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);

    if (g_nr.motionClone != nullptr)
        Barrier(cmdList, g_nr.motionClone, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);

    if (reduced && g_nr.colorSmall != nullptr)
        Barrier(cmdList, g_nr.colorSmall, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Leave the staging copy as the next frame expects to find it.
    Barrier(cmdList, g_nr.colorCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    device->Release();
}

namespace DlssNr
{
void RetryAfterFailure()
{
    g_nr.failed = false;
    g_nr.reason = "";
    g_nr.reset = true;
}

// ---------------------------------------------------------------------------------------------------
// Present placement: what the upscale call leaves behind for Present, and the Present-side run.
//
// The guides the game hands its upscaler are only guaranteed for the length of that call; by Present the
// engine may have moved on to the next frame's targets. So they are copied into textures of our own on the
// game's list (a barrier and a CopyResource, no root signature, no pipeline state -- nothing the game has
// to have put back), together with the few values the pass reads off the parameter block.

struct PresentCapture
{
    ID3D12Device* device = nullptr; // not owned; the device the game's DLSS call is on
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motion = nullptr;
    D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_COPY_DEST;
    D3D12_RESOURCE_STATES motionState = D3D12_RESOURCE_STATE_COPY_DEST;

    unsigned int createFlags = 0;
    unsigned int subrectWidth = 0;
    unsigned int subrectHeight = 0;
    bool haveMvScale = false;
    float mvScaleX = 1.0f;
    float mvScaleY = 1.0f;
    float preExposure = 1.0f;
    unsigned int reset = 0; // sticky until a Present consumes it
    bool valid = false;
    unsigned long long capturedAtPresent = 0; // PresentRoute::PresentCount() when last captured
};

// With no upscale call at all -- the game running its own TAA -- the guides come from the depth tracker and a
// motion field of zeros, and the panel is drawn at Present because no upscaler exists to draw it.
struct TrackedGuides
{
    ID3D12Resource* depth = nullptr;
    D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_COPY_DEST;
    ID3D12Resource* zeroMotion = nullptr;
    bool wasActive = false;
    bool usingFlow = false;

    // Said once. The optical flow module logs its own failure; this is the OTHER way the route ends
    // up blind, and until now it said nothing at all.
    bool warnedGuideSize = false;

    // What the panel needs to answer "is motion actually reaching the model?". The evaluate path
    // already computes this per frame to decide whether to reset history; it just never kept it,
    // so nothing could report it and the symptom -- sharp when still, smears when moving -- was
    // only ever diagnosed from the outside, by reading files and inferring.
    //
    // Two counters rather than one flag, because the interesting case is intermittent: a provider
    // that feeds most frames and drops some is a different problem from one that never feeds at
    // all, and a single bool cannot tell them apart.
    unsigned long long evaluates = 0;
    unsigned long long blindEvaluates = 0;
    bool lastBlind = false;
};

TrackedGuides g_tracked;
std::unique_ptr<Menu_Dx12> g_presentMenu;

// The depth guide against the picture, from the log alone. Every five seconds a few strips of the tracked
// depth's copy and of the back buffer (eight rows and eight columns, three texels thick, the picture's band
// only) are copied to a readback buffer, and read eight Presents later: at every depth jump across a strip
// (over 0.7 stops of reciprocal depth) the strongest luminance step within 16 px is found, and the median
// offset along the strip is logged -- "colour edges sit X px across and Y px down from the depth edges".
// On a scene that moves, a depth guide from another frame shows here as a steady offset of several pixels
// (Resident Evil 2's captures, 2026-09-26: 5-11 px). Two checks running with an offset over 2 px move the
// depth tracker to its next pick rule (DepthTracker::NextPolicy), logged; after trying every rule without a
// good check the tracker goes back to the first and stops switching.
struct AlignStrip
{
    UINT64 depthOffset = 0;
    UINT64 colourOffset = 0;
    UINT depthPitch = 0;
    UINT colourPitch = 0;
    UINT width = 0;
    UINT height = 0;
    bool vertical = false;
};

struct AlignCheck
{
    ID3D12Resource* readback = nullptr;
    UINT64 size = 0;
    std::vector<AlignStrip> strips;
    DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT colourFormat = DXGI_FORMAT_UNKNOWN;
    bool reversed = true;
    bool pending = false;
    unsigned long long due = 0;
    std::chrono::steady_clock::time_point last {};
    int offBy = 0;        // checks running with the depth off the picture
    int switches = 0;     // rules tried since the last good check
    bool settled = false; // every rule tried without a good check: stop switching
    unsigned long long quietSaid = 0;
};

AlignCheck g_align;

// Bytes of one texel in the formats the strips can hold; 0 for a format this check does not read.
unsigned int AlignTexelBytes(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return 4;
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return 8;
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
        return 2;
    default:
        return 0;
    }
}

float AlignDepthAt(const unsigned char* p, DXGI_FORMAT f, bool reversed)
{
    float d = 0.0f;
    switch (f)
    {
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    {
        uint32_t v;
        std::memcpy(&v, p, 4);
        d = (float) (v & 0xFFFFFFu) / 16777215.0f;
        break;
    }
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
    {
        uint16_t v;
        std::memcpy(&v, p, 2);
        d = (float) v / 65535.0f;
        break;
    }
    default:
        std::memcpy(&d, p, 4);
        break;
    }
    if (!std::isfinite(d))
        d = 0.0f;
    return std::log2(std::max(reversed ? d : 1.0f - d, 1e-7f));
}

float AlignLumaAt(const unsigned char* p, DXGI_FORMAT f)
{
    float r = 0.0f, g = 0.0f, b = 0.0f;
    switch (f)
    {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        b = p[0] / 255.0f;
        g = p[1] / 255.0f;
        r = p[2] / 255.0f;
        break;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    {
        uint32_t v;
        std::memcpy(&v, p, 4);
        r = (v & 1023u) / 1023.0f;
        g = ((v >> 10) & 1023u) / 1023.0f;
        b = ((v >> 20) & 1023u) / 1023.0f;
        break;
    }
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    {
        uint16_t h[3];
        std::memcpy(h, p, 6);
        // Linear HDR: brought to a display-like scale so one threshold serves both.
        auto half = [](uint16_t v)
        {
            const int e = (v >> 10) & 31, m = v & 1023;
            const float mag = e == 0 ? m * 5.9604645e-8f : e == 31 ? 0.0f : std::ldexp(1.0f + m / 1024.0f, e - 15);
            return (v & 0x8000) ? 0.0f : mag;
        };
        r = std::pow(std::min(half(h[0]), 64.0f), 1.0f / 2.2f);
        g = std::pow(std::min(half(h[1]), 64.0f), 1.0f / 2.2f);
        b = std::pow(std::min(half(h[2]), 64.0f), 1.0f / 2.2f);
        break;
    }
    default:
        r = p[0] / 255.0f;
        g = p[1] / 255.0f;
        b = p[2] / 255.0f;
        break;
    }
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

// Records the strips on the Present route's list. depth rests in NON_PIXEL_SHADER_RESOURCE and the back
// buffer in PRESENT; both are left as they were.
void AlignRecord(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* depth, bool reversed,
                 ID3D12Resource* backBuffer, unsigned int pictureY, unsigned int width, unsigned int height,
                 unsigned long long presentIndex)
{
    const auto now = std::chrono::steady_clock::now();
    if (g_align.pending || width < 64 || height < 64 ||
        (g_align.last != std::chrono::steady_clock::time_point {} && now - g_align.last < std::chrono::seconds(5)))
        return;

    const D3D12_RESOURCE_DESC depthDesc = depth->GetDesc();
    const D3D12_RESOURCE_DESC colourDesc = backBuffer->GetDesc();
    if (AlignTexelBytes(depthDesc.Format) == 0 || AlignTexelBytes(colourDesc.Format) == 0 || depthDesc.Width < width ||
        depthDesc.Height < height)
        return;

    // Once per five seconds whether or not the strips can be laid out, so a format this cannot read costs
    // nothing per frame.
    g_align.last = now;

    // The strips' footprints, depth then colour, laid end to end.
    // Sixteen of each: a standing character's silhouette is mostly upright, so the columns (which measure
    // the vertical offset) cross it far less often than the rows do, and with eight the "down" figure never
    // had enough crossings to report (Resident Evil 2, 8fc46f3d: "? px down" every time).
    constexpr unsigned int kStrips = 16;
    constexpr unsigned int kThick = 3;
    std::vector<AlignStrip> strips;
    UINT64 offset = 0;
    // The footprint of a strip as the runtime lays it out for this format (GetCopyableFootprints on a
    // texture of the strip's size), so the copy's footprint is always one the format accepts.
    DXGI_FORMAT depthFootprint = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT colourFootprint = DXGI_FORMAT_UNKNOWN;
    auto place = [&](const D3D12_RESOURCE_DESC& source, UINT w, UINT h, UINT& pitch, DXGI_FORMAT& format)
    {
        D3D12_RESOURCE_DESC desc = source;
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.DepthOrArraySize = 1;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
        UINT rows = 0;
        UINT64 rowBytes = 0, total = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowBytes, &total);
        pitch = footprint.Footprint.RowPitch;
        format = footprint.Footprint.Format;
        offset = (offset + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
                 ~(UINT64) (D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
        const UINT64 at = offset;
        offset += total == UINT64_MAX ? 0 : total;
        return total == UINT64_MAX || total == 0 ? UINT64_MAX : at;
    };

    for (unsigned int k = 0; k < 2 * kStrips; ++k)
    {
        AlignStrip s;
        s.vertical = k >= kStrips;
        s.width = s.vertical ? kThick : width;
        s.height = s.vertical ? height : kThick;
        s.depthOffset = place(depthDesc, s.width, s.height, s.depthPitch, depthFootprint);
        s.colourOffset = place(colourDesc, s.width, s.height, s.colourPitch, colourFootprint);
        if (s.depthOffset == UINT64_MAX || s.colourOffset == UINT64_MAX)
            return;
        strips.push_back(s);
    }

    if (AlignTexelBytes(depthFootprint) == 0 || AlignTexelBytes(colourFootprint) == 0)
        return;

    if (g_align.readback == nullptr || g_align.size < offset)
    {
        if (g_align.readback != nullptr)
            ParkNrResource(g_align.readback);

        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer {};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = offset;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&g_align.readback))))
        {
            g_align.readback = nullptr;
            g_align.size = 0;
            return;
        }
        g_align.size = offset;
    }

    Barrier(list, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(list, backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);

    for (unsigned int k = 0; k < 2 * kStrips; ++k)
    {
        const AlignStrip& s = strips[k];
        const unsigned int i = k % kStrips;
        const UINT x0 = s.vertical ? (UINT) (((2 * i + 1) * width) / (2 * kStrips)) - 1 : 0;
        const UINT y0 = s.vertical ? 0 : (UINT) (((2 * i + 1) * height) / (2 * kStrips)) - 1;

        for (int which = 0; which < 2; ++which)
        {
            D3D12_TEXTURE_COPY_LOCATION from {};
            from.pResource = which == 0 ? depth : backBuffer;
            from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION to {};
            to.pResource = g_align.readback;
            to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            to.PlacedFootprint.Offset = which == 0 ? s.depthOffset : s.colourOffset;
            to.PlacedFootprint.Footprint = { which == 0 ? depthFootprint : colourFootprint, s.width, s.height, 1,
                                             which == 0 ? s.depthPitch : s.colourPitch };
            const UINT yBase = which == 0 ? 0u : pictureY;
            const D3D12_BOX box { x0, y0 + yBase, 0, x0 + s.width, y0 + yBase + s.height, 1 };
            list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
        }
    }

    Barrier(list, backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    Barrier(list, depth, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    g_align.strips = std::move(strips);
    g_align.depthFormat = depthFootprint;
    g_align.colourFormat = colourFootprint;
    g_align.reversed = reversed;
    g_align.pending = true;
    g_align.due = presentIndex + 8;
    g_align.last = now;
}

// Reads the strips recorded eight Presents ago -- the same no-fence pattern as the captures.
void AlignRead(unsigned long long presentIndex)
{
    if (!g_align.pending || presentIndex < g_align.due || g_align.readback == nullptr)
        return;

    g_align.pending = false;

    void* mapped = nullptr;
    const D3D12_RANGE range { 0, (SIZE_T) g_align.size };
    if (FAILED(g_align.readback->Map(0, &range, &mapped)) || mapped == nullptr)
        return;

    const auto* base = static_cast<const unsigned char*>(mapped);
    const unsigned int dBytes = AlignTexelBytes(g_align.depthFormat);
    const unsigned int cBytes = AlignTexelBytes(g_align.colourFormat);
    std::vector<int> across, down;

    for (const AlignStrip& s : g_align.strips)
    {
        const unsigned int n = s.vertical ? s.height : s.width;
        std::vector<float> d(n), y(n);

        for (unsigned int t = 0; t < n; ++t)
        {
            // The middle texel across the strip for depth; the three averaged for luminance, against dither.
            const unsigned int row = s.vertical ? t : 1;
            const unsigned int col = s.vertical ? 1 : t;
            d[t] = AlignDepthAt(base + s.depthOffset + (UINT64) row * s.depthPitch + (UINT64) col * dBytes,
                                g_align.depthFormat, g_align.reversed);
            float sum = 0.0f;
            for (unsigned int a = 0; a < 3; ++a)
            {
                const unsigned int r = s.vertical ? t : a;
                const unsigned int c = s.vertical ? a : t;
                sum += AlignLumaAt(base + s.colourOffset + (UINT64) r * s.colourPitch + (UINT64) c * cBytes,
                                   g_align.colourFormat);
            }
            y[t] = sum / 3.0f;
        }

        for (unsigned int t = 17; t + 17 < n; ++t)
        {
            if (std::abs(d[t] - d[t - 1]) <= 0.7f)
                continue;

            // The NEAREST luminance step at least half as strong as the strongest within 16 px, not the
            // strongest: a lit face or a shading band a few pixels inside an object is often a stronger step
            // than its silhouette, and read as an offset where there is none (the pillar in capture
            // 20260926-184409 read 7 px off while its silhouette was on the depth edge).
            float best = 0.0f;
            for (int k = -16; k <= 16; ++k)
                best = std::max(best, std::abs(y[t + k] - y[t + k - 1]));

            if (best <= 0.03f)
                continue;

            int nearest = 0;
            for (int r = 0; r <= 16; ++r)
            {
                if (std::abs(y[t + r] - y[t + r - 1]) >= 0.5f * best)
                {
                    nearest = r;
                    break;
                }
                if (std::abs(y[t - r] - y[t - r - 1]) >= 0.5f * best)
                {
                    nearest = -r;
                    break;
                }
            }

            (s.vertical ? down : across).push_back(nearest);
        }
    }

    const D3D12_RANGE none { 0, 0 };
    g_align.readback->Unmap(0, &none);

    auto median = [](std::vector<int>& v)
    {
        std::sort(v.begin(), v.end());
        return v.empty() ? 0 : v[v.size() / 2];
    };

    constexpr size_t kEnough = 12;
    const bool haveX = across.size() >= kEnough;
    const bool haveY = down.size() >= kEnough;

    if (!haveX && !haveY)
    {
        if (presentIndex - g_align.quietSaid > 3600 || g_align.quietSaid == 0)
        {
            g_align.quietSaid = presentIndex;
            LOG_INFO("DLSS-NR depth/colour alignment: too few silhouettes to judge ({} across, {} down)", across.size(),
                     down.size());
        }
        return;
    }

    const size_t nx = across.size();
    const size_t ny = down.size();
    const int mx = haveX ? median(across) : 0;
    const int my = haveY ? median(down) : 0;
    const int policy = DepthTracker::Policy();

    LOG_INFO("DLSS-NR depth/colour alignment: colour edges sit {} px across and {} px down from the depth edges "
             "(median of {} and {} silhouette crossings; depth picked by {}; {} frames so far took the newest "
             "buffer of an earlier frame)",
             haveX ? std::to_string(mx) : std::string("?"), haveY ? std::to_string(my) : std::string("?"), nx, ny,
             DepthTracker::PolicyName(policy), DepthTracker::HeldFrames());

    const bool off = (haveX && std::abs(mx) > 2) || (haveY && std::abs(my) > 2);

    if (!off)
    {
        g_align.offBy = 0;
        g_align.switches = 0;
        return;
    }

    if (g_align.settled || ++g_align.offBy < 2)
        return;

    g_align.offBy = 0;

    if (g_align.switches + 1 >= 3)
    {
        // Every rule tried without a good check: back to the first, and no more switching.
        while (DepthTracker::Policy() != 0)
            DepthTracker::NextPolicy();
        g_align.settled = true;
        LOG_WARN("DLSS-NR depth tracker: the depth sits off the picture whichever rule picks it -- back to {}, "
                 "and no more switching this session",
                 DepthTracker::PolicyName(0));
        return;
    }

    ++g_align.switches;
    const int next = DepthTracker::NextPolicy();
    LOG_WARN("DLSS-NR depth tracker: the depth sat {} px across and {} px down off the picture on two checks "
             "running -- trying the next pick: {}",
             mx, my, DepthTracker::PolicyName(next));
}

// Optical-flow motion vectors (dlssnr/opticalflow): a DLL beside OptiScaler, estimated from the finished
// frames on the Present route's own list. Anything missing or refused falls back to zero motion.
struct OpticalFlowModule
{
    bool tried = false;
    HMODULE module = nullptr;
    void* (*create)(ID3D12Device*, unsigned int, unsigned int, int) = nullptr;
    ID3D12Resource* (*record)(void*, ID3D12GraphicsCommandList*, ID3D12Resource*, int, int*) = nullptr;
    void (*destroy)(void*) = nullptr;
    const char* (*lastError)(void*) = nullptr;
    const char* (*staticError)() = nullptr;

    void* flow = nullptr;
    unsigned int width = 0;
    unsigned int height = 0;
    bool failed = false;
    unsigned int recordFailures = 0;

    ID3D12Resource* colour = nullptr; // the picture, copied out of the back buffer
    D3D12_RESOURCE_STATES colourState = D3D12_RESOURCE_STATE_COPY_DEST;

    // Estimators replaced on a size change, destroyed once nothing recorded with them can still be running.
    std::vector<std::pair<void*, unsigned int>> retired;
};

OpticalFlowModule g_flow;

bool EnsureOpticalFlowModule()
{
    if (g_flow.tried)
        return g_flow.module != nullptr;

    g_flow.tried = true;
    const constexpr char* kName = "OptiScaler_OpticalFlow.dll";

    auto found = Util::FindFilePath(Util::DllPath().remove_filename(), kName);
    if (!found.has_value())
        found = Util::FindFilePath(Util::ExePath().remove_filename(), kName);

    if (!found.has_value())
    {
        LOG_INFO("DLSS-NR Present route: {} is not beside OptiScaler -- zero motion vectors", kName);
        return false;
    }

    g_flow.module = LoadLibraryW(found.value().wstring().c_str());

    if (g_flow.module == nullptr)
    {
        LOG_WARN("DLSS-NR Present route: {} would not load (error {}) -- zero motion vectors", kName, GetLastError());
        return false;
    }

    const auto abi = (int (*)()) GetProcAddress(g_flow.module, "OptiOF_AbiVersion");
    g_flow.create = (decltype(g_flow.create)) GetProcAddress(g_flow.module, "OptiOF_Create");
    g_flow.record = (decltype(g_flow.record)) GetProcAddress(g_flow.module, "OptiOF_Record");
    g_flow.destroy = (decltype(g_flow.destroy)) GetProcAddress(g_flow.module, "OptiOF_Destroy");
    g_flow.lastError = (decltype(g_flow.lastError)) GetProcAddress(g_flow.module, "OptiOF_LastError");
    g_flow.staticError = (decltype(g_flow.staticError)) GetProcAddress(g_flow.module, "OptiOF_StaticError");

    if (abi == nullptr || abi() != 1 || g_flow.create == nullptr || g_flow.record == nullptr ||
        g_flow.destroy == nullptr || g_flow.lastError == nullptr || g_flow.staticError == nullptr)
    {
        LOG_WARN("DLSS-NR Present route: {} is not the version this OptiScaler expects -- zero motion vectors", kName);
        FreeLibrary(g_flow.module);
        g_flow.module = nullptr;
        return false;
    }

    LOG_INFO("DLSS-NR Present route: optical flow loaded from {} (AMD FidelityFX Optical Flow, as DXL uses it)",
             found.value().string());
    return true;
}

// Estimates this frame's motion from the picture inside the back buffer (at pictureY, width x height) and
// returns the per-pixel field, or null to use zero motion. Records on the Present route's own list, before
// the pass touches the back buffer, which arrives and leaves in PRESENT.
ID3D12Resource* PresentOpticalFlow(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* backBuffer,
                                   const D3D12_RESOURCE_DESC& frameDesc, unsigned int pictureY, unsigned int width,
                                   unsigned int height, bool reset, bool& sceneCut)
{
    sceneCut = false;

    for (size_t i = 0; i < g_flow.retired.size();)
    {
        if (--g_flow.retired[i].second == 0)
        {
            g_flow.destroy(g_flow.retired[i].first);
            g_flow.retired.erase(g_flow.retired.begin() + i);
        }
        else
        {
            ++i;
        }
    }

    if (!Config::Instance()->DlssNrOpticalFlow.value_or_default() || g_flow.failed || !EnsureOpticalFlowModule())
        return nullptr;

    if (g_flow.flow != nullptr && (g_flow.width != width || g_flow.height != height))
    {
        g_flow.retired.emplace_back(g_flow.flow, 16u);
        g_flow.flow = nullptr;
        ParkNrResource(g_flow.colour);
    }

    if (g_flow.flow == nullptr)
    {
        g_flow.flow = g_flow.create(device, width, height, 1);

        if (g_flow.flow == nullptr)
        {
            const char* why = g_flow.staticError();
            LOG_WARN("DLSS-NR Present route: optical flow unavailable ({}) -- zero motion vectors",
                     why != nullptr ? why : "unknown");
            g_flow.failed = true;
            return nullptr;
        }

        g_flow.width = width;
        g_flow.height = height;
        reset = true;
        LOG_INFO("DLSS-NR Present route: optical flow estimating {}x{} pictures", width, height);
    }

    // The copy below needs matching formats, and games change the swapchain's: Devil May Cry 5 starts in
    // R8G8B8A8 and moves to R10G10B10A2 a few seconds in. A mismatched copy puts the list in an error state,
    // so it fails to close -- every frame, from then on.
    if (g_flow.colour != nullptr && g_flow.colour->GetDesc().Format != frameDesc.Format)
    {
        ParkNrResource(g_flow.colour);
        reset = true;
    }

    if (g_flow.colour == nullptr)
    {
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = frameDesc.Format;
        desc.SampleDesc.Count = 1;

        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&g_flow.colour))))
        {
            g_flow.colour = nullptr;
            LOG_WARN("DLSS-NR Present route: the optical flow picture could not be allocated -- zero motion vectors");
            g_flow.failed = true;
            return nullptr;
        }

        g_flow.colourState = D3D12_RESOURCE_STATE_COPY_DEST;
    }

    Barrier(list, g_flow.colour, g_flow.colourState, D3D12_RESOURCE_STATE_COPY_DEST);
    Barrier(list, backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);

    D3D12_TEXTURE_COPY_LOCATION to {};
    to.pResource = g_flow.colour;
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION from {};
    from.pResource = backBuffer;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box { 0, pictureY, 0, width, pictureY + height, 1 };
    list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);

    Barrier(list, backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    Barrier(list, g_flow.colour, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g_flow.colourState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    int cut = 0;
    ID3D12Resource* const motion = g_flow.record(g_flow.flow, list, g_flow.colour, reset ? 1 : 0, &cut);

    if (motion == nullptr)
    {
        const char* why = g_flow.lastError(g_flow.flow);
        LOG_WARN("DLSS-NR Present route: optical flow did not record ({}) -- zero motion vectors this frame",
                 why != nullptr ? why : "unknown");

        if (++g_flow.recordFailures >= 3)
        {
            LOG_WARN("DLSS-NR Present route: optical flow failed three times -- zero motion vectors from here on");
            g_flow.failed = true;
        }

        return nullptr;
    }

    g_flow.recordFailures = 0;
    sceneCut = cut != 0;
    return motion;
}

// A motion field of zeros: a committed resource on a default heap is zero-filled when it is created.
ID3D12Resource* CreateZeroMotion(ID3D12Device* device, unsigned int width, unsigned int height)
{
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R16G16_FLOAT;
    desc.SampleDesc.Count = 1;

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    ID3D12Resource* res = nullptr;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                    nullptr, IID_PPV_ARGS(&res));
    return res;
}

PresentCapture g_presentCapture;
std::mutex g_presentCaptureMutex;

bool CopyGuideForPresent(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* source,
                         ID3D12Resource*& copy, D3D12_RESOURCE_STATES& copyState)
{
    if (source == nullptr)
        return false;

    const D3D12_RESOURCE_DESC want = source->GetDesc();

    if (copy != nullptr)
    {
        const D3D12_RESOURCE_DESC have = copy->GetDesc();

        // Retired, not released: a Present list may still be reading the old copy.
        if (have.Width != want.Width || have.Height != want.Height || have.Format != TypedGuideFormat(want.Format))
            ParkNrResource(copy);
    }

    if (copy == nullptr)
    {
        copy = CreateGuideClone(device, source);
        copyState = D3D12_RESOURCE_STATE_COPY_DEST;

        if (copy == nullptr)
            return false;
    }

    Barrier(cmdList, copy, copyState, D3D12_RESOURCE_STATE_COPY_DEST);
    Barrier(cmdList, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyResource(copy, source);
    Barrier(cmdList, source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmdList, copy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    copyState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    return true;
}

void CaptureForPresent(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params)
{
    ID3D12Resource* depth = GetResource(params, NVSDK_NGX_Parameter_Depth, "DLSSD.Depth");
    ID3D12Resource* motion = GetResource(params, NVSDK_NGX_Parameter_MotionVectors, "DLSSD.MotionVectors");

    if (depth == nullptr || motion == nullptr)
    {
        ReportSkipOnce(depth == nullptr ? "the parameters carried no depth"
                                        : "the parameters carried no motion vectors");
        return;
    }

    PresentRoute::NoteUpscaleList(cmdList);

    std::lock_guard<std::mutex> lock(g_presentCaptureMutex);
    auto& c = g_presentCapture;

    if (c.device != device)
    {
        ParkNrResource(c.depth);
        ParkNrResource(c.motion);
        c.device = device;
    }

    const bool haveDepth = CopyGuideForPresent(device, cmdList, depth, c.depth, c.depthState);
    const bool haveMotion = CopyGuideForPresent(device, cmdList, motion, c.motion, c.motionState);

    params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &c.createFlags);

    c.subrectWidth = 0;
    c.subrectHeight = 0;
    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &c.subrectWidth);
    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &c.subrectHeight);

    c.haveMvScale = params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &c.mvScaleX) == NVSDK_NGX_Result_Success &&
                    params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &c.mvScaleY) == NVSDK_NGX_Result_Success;

    float pre = 1.0f;
    c.preExposure = params->Get(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, &pre) == NVSDK_NGX_Result_Success && pre > 1e-6f
                        ? pre
                        : 1.0f;

    unsigned int reset = 0;
    if (params->Get(NVSDK_NGX_Parameter_Reset, &reset) == NVSDK_NGX_Result_Success && reset != 0)
        c.reset = 1;

    c.valid = haveDepth && haveMotion;
    c.capturedAtPresent = PresentRoute::PresentCount();

    // The hook is only proven by Presents arriving. Captures piling up with none means the game presents
    // through something the detour does not reach, and the pass is silently never running.
    {
        static unsigned long long captures = 0;
        static bool saidNoPresent = false;

        if (++captures == 300 && PresentRoute::PresentCount() == 0 && !saidNoPresent)
        {
            saidNoPresent = true;
            LOG_WARN("DLSS-NR Present route: 300 upscale calls and not one Present reached the hook -- the pass is "
                     "not running");
        }
    }

    static bool said = false;
    if (!said && c.valid)
    {
        said = true;
        const auto d = c.depth->GetDesc();
        LOG_INFO("DLSS-NR Present route: guides captured at the upscale call ({}x{} depth fmt {}), the model will "
                 "run at Present",
                 (UINT) d.Width, (UINT) d.Height, (int) d.Format);
    }
}

// Settings changed from outside this process reach the running game here. That is what makes the
// 32-bit route tunable at all -- OptiScaler runs inside the Feeder's 64-bit helper, which has no
// window, so its own panel cannot be shown over the game and the manager writes host64\OptiScaler.ini
// instead -- and it is what the manager's pop-out panel relies on everywhere else.
//
// It has to run before anything reads [DlssNr] Enabled, which is why it is its own function called at
// each entry point rather than a line inside the pass. It used to be only that line: a reload that
// switched the pass off meant the pass was never entered again, so nothing polled again, and no later
// change was read -- switching back on included. From the outside that is "the toggle does nothing
// and now the sliders do not either" (Tomb Raider I-III, 2026-09-16). The in-game toggle key never
// showed it because it flips the value in memory.
//
// The values in effect go on the same line, so a log can answer "did that slider land" on its own.
void PollSettingsFromDisk()
{
    // Frame Generation's on/off and HUD fix are read per frame, so a reload switches them live -- but
    // OptiScaler's own paths do one more thing when they change them, and a change arriving from the
    // pop-out panel or Edit has to do it too, or it is a different operation from the in-game one:
    //   on           the FG key (menu_common.cpp) sets fgChanged, so the generator starts clean
    //                instead of interpolating against a frame from before it was off;
    //   HUD fix      OptiScaler's menu sets clearCapturedHudlesses and fgChanged, so a stale HUD-less
    //                capture from before the switch is never used.
    const bool fgWas = Config::Instance()->FGEnabled.value_or_default();
    const bool hudfixWas = Config::Instance()->FGHUDFix.value_or_default();

    if (!Config::Instance()->ReloadIfChangedOnDisk())
        return;

    const Config& r = *Config::Instance();
    const bool fgNow = r.FGEnabled.value_or_default();
    const bool hudfixNow = r.FGHUDFix.value_or_default();
    if (fgNow && !fgWas)
        State::Instance().fgChanged = true;
    if (hudfixNow != hudfixWas)
    {
        State::Instance().clearCapturedHudlesses = true;
        State::Instance().fgChanged = true;
    }
    if (fgNow != fgWas || hudfixNow != hudfixWas)
        LOG_INFO("Frame Generation from disk mid-run: {} (was {}), HUD fix {} (was {})", fgNow ? "on" : "off",
                 fgWas ? "on" : "off", hudfixNow ? "on" : "off", hudfixWas ? "on" : "off");
    LOG_INFO("Settings reloaded from disk mid-run: enabled {}, apply model {}, preset {}, style {}, passes {}, "
             "pass rate {:.2f}, intensity {:.2f}, structure {:.3f}, tone {:.3f}, working scale {:.2f}",
             r.DlssNrEnabled.value_or_default(), r.DlssNrApplyModel.value_or_default(),
             r.DlssNrPreset.value_or_default(), r.DlssNrStyle.value_or_default(), r.DlssNrPasses.value_or_default(),
             r.DlssNrPassRate.value_or_default(), r.DlssNrIntensity.value_or_default(),
             r.DlssNrLocalStructure.value_or_default(), r.DlssNrLocalTone.value_or_default(),
             r.DlssNrWorkingScale.value_or_default());
}

void RunAtPresent(IDXGISwapChain3* swapChain, ID3D12CommandQueue* queue, unsigned long long presentIndex)
{
    if (swapChain == nullptr || queue == nullptr)
        return;

    // Asked every frame, not only when the hooks went in: the DLSS5 Feeder's add-on can load after the device
    // is created, and once it is here the pass rides its DLSS call instead (PresentRoute::Wanted).
    if (!PresentRoute::Wanted())
        return;

    PollSettingsFromDisk();

    const bool nrOn = Config::Instance()->DlssNrEnabled.value_or_default();

    std::lock_guard<std::mutex> lock(g_presentCaptureMutex);
    auto& c = g_presentCapture;

    // Guides from the game's upscale call while it keeps making one; otherwise from the depth tracker.
    const bool fromCapture = c.valid && c.depth != nullptr && c.motion != nullptr &&
                             presentIndex >= c.capturedAtPresent && presentIndex - c.capturedAtPresent <= 8;

    // The upscaler draws the panel when there is one. Without it the panel is drawn here every frame --
    // whether or not the pass runs, or switching Neural Rendering off in the panel would take the panel away.
    if (fromCapture && !nrOn)
        return;

    DepthTracker::Selection tracked {};
    const bool fromTracker = nrOn && !fromCapture && DepthTracker::Selected(tracked);

    if (nrOn && !fromCapture && !fromTracker && g_tracked.wasActive)
    {
        g_tracked.wasActive = false;
        LOG_INFO("DLSS-NR Present route: no scene depth this frame -- the pass pauses until one is found");
    }

    ID3D12Resource* backBuffer = nullptr;

    if (FAILED(swapChain->GetBuffer(swapChain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backBuffer))) ||
        backBuffer == nullptr)
    {
        ReportSkipOnce("the swapchain's back buffer could not be read at Present");
        return;
    }

    ID3D12Device* device = nullptr;
    backBuffer->GetDevice(IID_PPV_ARGS(&device));

    ID3D12Device* queueDevice = nullptr;
    if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&queueDevice))) && queueDevice != nullptr)
        queueDevice->Release();

    if (device == nullptr || (fromCapture && device != c.device) || queueDevice != device)
    {
        ReportSkipOnce("the swapchain or frame queue is on a different device from the game's upscale call");

        if (device != nullptr)
            device->Release();

        backBuffer->Release();
        return;
    }

    // A small ring of allocators, each reused only once the GPU has finished what it last recorded -- a
    // fence value per allocator says when. A frame that finds every allocator still in flight is skipped
    // rather than stalled on.
    struct AllocatorSlot
    {
        ID3D12CommandAllocator* allocator = nullptr;
        UINT64 fenceValue = 0;
    };

    static std::vector<AllocatorSlot> slots;
    static ID3D12Fence* fence = nullptr;
    static UINT64 fenceValue = 0;
    static ID3D12GraphicsCommandList* list = nullptr;
    static NVNGX_Parameters* presentParams = nullptr;
    static ID3D12Device* builtOn = nullptr;

    if (builtOn != device)
    {
        // A new device (the game recreated it): everything recorded for the old one is dropped.
        if (list != nullptr)
            list->Release();
        for (auto& s : slots)
            if (s.allocator != nullptr)
                s.allocator->Release();
        if (fence != nullptr)
            fence->Release();

        slots.clear();
        list = nullptr;
        fence = nullptr;
        fenceValue = 0;
        builtOn = device;

        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
            fence = nullptr;
    }

    if (fence == nullptr)
    {
        ReportSkipOnce("a fence for the Present route could not be created");
        device->Release();
        backBuffer->Release();
        return;
    }

    const UINT64 completed = fence->GetCompletedValue();
    AllocatorSlot* slot = nullptr;

    for (auto& s : slots)
    {
        if (s.fenceValue <= completed)
        {
            slot = &s;
            break;
        }
    }

    if (slot == nullptr && slots.size() < 8)
    {
        AllocatorSlot fresh;

        if (SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&fresh.allocator))))
        {
            slots.push_back(fresh);
            slot = &slots.back();
        }
    }

    if (slot == nullptr)
    {
        static bool saidBusy = false;
        if (!saidBusy)
        {
            saidBusy = true;
            LOG_WARN("DLSS-NR Present route: every command allocator still in flight -- skipping a frame");
        }

        device->Release();
        backBuffer->Release();
        return;
    }

    slot->allocator->Reset();

    if (list == nullptr)
    {
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot->allocator, nullptr,
                                             IID_PPV_ARGS(&list))))
        {
            list = nullptr;
            ReportSkipOnce("a command list for the Present route could not be created");
            device->Release();
            backBuffer->Release();
            return;
        }
    }
    else
    {
        list->Reset(slot->allocator, nullptr);
    }

    if (presentParams == nullptr)
        presentParams = new NVNGX_Parameters(API::DX12, true);

    presentParams->Set(NVSDK_NGX_Parameter_Output, backBuffer);
    presentParams->Set(NVSDK_NGX_Parameter_Color, backBuffer);
    presentParams->Set(NVSDK_NGX_Parameter_ExposureTexture, (void*) nullptr);
    presentParams->Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, 0u);
    presentParams->Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, 0u);
    presentParams->Set(kPresentActiveX, 0u);
    presentParams->Set(kPresentActiveY, 0u);
    presentParams->Set(kPresentActiveWidth, 0u);
    presentParams->Set(kPresentActiveHeight, 0u);

    bool haveGuides = fromCapture || fromTracker;

    if (!haveGuides)
    {
        // Panel only.
    }
    else if (fromCapture)
    {
        presentParams->Set(NVSDK_NGX_Parameter_Depth, c.depth);
        presentParams->Set(NVSDK_NGX_Parameter_MotionVectors, c.motion);
        presentParams->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, c.createFlags);
        presentParams->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, c.subrectWidth);
        presentParams->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, c.subrectHeight);
        presentParams->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, c.preExposure);
        presentParams->Set(NVSDK_NGX_Parameter_Reset, c.reset);
        presentParams->Set(NVSDK_NGX_Parameter_MV_Scale_X, c.haveMvScale ? c.mvScaleX : 1.0f);
        presentParams->Set(NVSDK_NGX_Parameter_MV_Scale_Y, c.haveMvScale ? c.mvScaleY : 1.0f);
        c.reset = 0;
    }
    else
    {
        // The tracked depth is copied on this list, from the state its last barrier left it in, into a typed
        // twin the model can read; the game's own buffer is back in that state before the list ends.
        ID3D12Resource*& copy = g_tracked.depth;

        if (copy != nullptr)
        {
            const D3D12_RESOURCE_DESC have = copy->GetDesc();
            if (have.Width != tracked.width || have.Height != tracked.height ||
                have.Format != TypedGuideFormat(tracked.format))
                ParkNrResource(copy);
        }

        if (copy == nullptr)
        {
            copy = CreateGuideClone(device, tracked.resource);
            g_tracked.depthState = D3D12_RESOURCE_STATE_COPY_DEST;
        }

        if (g_tracked.zeroMotion != nullptr)
        {
            const D3D12_RESOURCE_DESC have = g_tracked.zeroMotion->GetDesc();
            if (have.Width != tracked.width || have.Height != tracked.height)
                ParkNrResource(g_tracked.zeroMotion);
        }

        if (g_tracked.zeroMotion == nullptr)
            g_tracked.zeroMotion = CreateZeroMotion(device, tracked.width, tracked.height);

        haveGuides = copy != nullptr && g_tracked.zeroMotion != nullptr;

        if (haveGuides)
        {
            Barrier(list, copy, g_tracked.depthState, D3D12_RESOURCE_STATE_COPY_DEST);
            Barrier(list, tracked.resource, tracked.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->CopyResource(copy, tracked.resource);
            Barrier(list, tracked.resource, D3D12_RESOURCE_STATE_COPY_SOURCE, tracked.state);
            Barrier(list, copy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            g_tracked.depthState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

            // Same width, shorter depth: a letterboxed picture, centred between the bars (checked on Resident
            // Evil 2 at 16:10 -- 80 black rows above and below a 2560x1440 image). The pass runs on that band
            // so the guides line up with it; the bars are left untouched.
            const D3D12_RESOURCE_DESC frameDesc = backBuffer->GetDesc();
            const bool letterboxed = tracked.width == frameDesc.Width && tracked.height < frameDesc.Height;
            const unsigned int pictureY = letterboxed ? (unsigned int) ((frameDesc.Height - tracked.height) / 2) : 0u;

            // The depth guide against the picture, every few seconds, into the log (AlignCheck).
            AlignRead(presentIndex);
            if (tracked.width == frameDesc.Width && tracked.height <= frameDesc.Height)
                AlignRecord(device, list, copy, tracked.reversed, backBuffer, pictureY, tracked.width, tracked.height,
                            presentIndex);

            // Motion: estimated from the frames when the picture and the depth are the same size, zero otherwise.
            bool flowCut = false;
            const bool guidesLineUp = tracked.width == frameDesc.Width && tracked.height <= frameDesc.Height;
            ID3D12Resource* const motion =
                guidesLineUp ? PresentOpticalFlow(device, list, backBuffer, frameDesc, pictureY, tracked.width,
                                                  tracked.height, !g_tracked.wasActive, flowCut)
                             : nullptr;

            // The flow module logs when it cannot start. This is the other way to end up blind and it
            // was silent, which made the commonest cause of the artefact below invisible in a log.
            if (!guidesLineUp && !g_tracked.warnedGuideSize)
            {
                g_tracked.warnedGuideSize = true;
                LOG_WARN("DLSS-NR Present route: the depth guide is {}x{} against a {}x{} frame, so where the "
                         "picture sits inside it is unknown -- no optical flow, zero motion vectors",
                         tracked.width, tracked.height, (unsigned int) frameDesc.Width, frameDesc.Height);
            }

            presentParams->Set(NVSDK_NGX_Parameter_Depth, copy);
            presentParams->Set(NVSDK_NGX_Parameter_MotionVectors, motion != nullptr ? motion : g_tracked.zeroMotion);
            presentParams->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
                               (unsigned int) (tracked.reversed ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0));
            presentParams->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 0u);
            presentParams->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 0u);
            presentParams->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
            // The model's history goes with the estimator's: a scene cut it saw, or a switch between estimated
            // and zero motion, leaves nothing to reproject from.
            const bool motionSourceChanged = (motion != nullptr) != g_tracked.usingFlow;
            g_tracked.usingFlow = motion != nullptr;

            // ...and so does history kept against vectors that are all zero.
            //
            // Zero motion does not mean "nothing moved". It means "this route could not find out".
            // The model is temporal: it reprojects its history by those vectors and accumulates on
            // the result. Told every pixel stayed put, it fetches last frame's history from the same
            // screen position -- which is right only while the view is still. The moment the camera
            // moves it blends this frame's detail against history taken from the wrong surface, and
            // then keeps doing it, because nothing ever tells it otherwise. That is the slow pulsing
            // or swimming across textures reported on this route, in every game, and it is why no
            // setting made any difference to it: nothing here was a setting.
            //
            // There is no correct reprojection to be had without vectors, so the honest thing is to
            // keep no history at all and let each frame stand on its own. That gives up the temporal
            // stability history buys -- NVIDIA documents reset-per-frame as a flicker and aliasing
            // risk, and the same trade is written up under ChainedHistory -- which is why it is a
            // key and not a silent change. It costs nothing: a reset is a flag, not a rebuild.
            const bool blind = motion == nullptr;
            g_presentMotionBlind = blind;
            const bool resetWhileBlind = blind && Config::Instance()->DlssNrResetWhenBlind.value_or_default();

            // Kept for MotionState(), below. Free: the value is already in hand.
            g_tracked.evaluates++;
            if (blind)
                g_tracked.blindEvaluates++;
            g_tracked.lastBlind = blind;

            presentParams->Set(NVSDK_NGX_Parameter_Reset,
                               (!g_tracked.wasActive || flowCut || motionSourceChanged || resetWhileBlind) ? 1u : 0u);
            presentParams->Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.0f);
            presentParams->Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);

            if (letterboxed)
            {
                presentParams->Set(kPresentActiveY, pictureY);
                presentParams->Set(kPresentActiveWidth, tracked.width);
                presentParams->Set(kPresentActiveHeight, tracked.height);
            }

            if (!g_tracked.wasActive || motionSourceChanged)
            {
                LOG_INFO("DLSS-NR Present route: running on the tracked scene depth ({}x{}, {} depth) with {} motion "
                         "vectors -- no upscale call in this game",
                         tracked.width, tracked.height, tracked.reversed ? "reversed" : "standard",
                         motion != nullptr ? "optical-flow" : "zero");
            }

            g_tracked.wasActive = true;
        }
        else
        {
            ReportSkipOnce("the tracked depth's copy or the motion field could not be allocated");
        }
    }

    if (haveGuides)
    {
        g_presentRouteDispatch = true;
        EvaluateAfterUpscale(list, presentParams, queue, true, presentIndex);
        g_presentRouteDispatch = false;
    }

    // No upscaler means no upscaler menu: the panel is drawn here, over the finished frame and after the
    // pass, so the model never sees it.
    if (!fromCapture && !Config::Instance()->OverlayMenu.value_or_default())
    {
        if (g_presentMenu == nullptr)
            g_presentMenu = std::make_unique<Menu_Dx12>(Util::GetProcessWindow(), device);

        g_presentMenu->Render(list, backBuffer, D3D12_RESOURCE_STATE_PRESENT);
    }

    const HRESULT closed = list->Close();

    if (SUCCEEDED(closed))
    {
        ID3D12CommandList* lists[] = { list };
        queue->ExecuteCommandLists(1, lists);
        slot->fenceValue = ++fenceValue;
        queue->Signal(fence, fenceValue);
    }
    else
    {
        static unsigned int closeFailures = 0;
        if (++closeFailures <= 3)
        {
            LOG_ERROR("DLSS-NR Present route: our command list failed to close (0x{:X}) -- this frame was not "
                      "submitted; rebuilding the model and its guides",
                      (unsigned int) closed);
        }

        // Nothing on that list ran. A feature created on it exists only on the CPU side, and the next Present
        // would count it as submitted and evaluate it -- which throws inside the model (Devil May Cry 5). The
        // states recorded for our copies are wrong for the same reason. Everything is rebuilt from scratch.
        ReleaseSurfaces();
        ParkNrResource(g_tracked.depth);
        g_tracked.depthState = D3D12_RESOURCE_STATE_COPY_DEST;
        g_tracked.wasActive = false;

        if (g_flow.flow != nullptr)
        {
            g_flow.retired.emplace_back(g_flow.flow, 16u);
            g_flow.flow = nullptr;
        }
        ParkNrResource(g_flow.colour);
    }

    device->Release();
    backBuffer->Release();
}

// Reads the game's parameter block and runs the pass on what it finds.
//
// This is the call site's job, not the pass's. A caller that has the resources in hand -- a
// reprojection stage, a frame generation path, anything that is not the upscaler seam -- calls
// RunPass directly and never touches an NGX parameter block.
void EvaluateInternal(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params, bool beforeUpscale,
                      ID3D12CommandQueue* timingQueue, bool forcePost, unsigned long long submissionEpoch)
{
    // Before the Enabled check below, which is the return that used to starve the poll.
    PollSettingsFromDisk();

    const Config& cfg = *Config::Instance();

    if (!cfg.DlssNrEnabled.value_or_default())
    {
        ReportSkipOnce("it is switched off");
        return;
    }

    if (cmdList == nullptr || params == nullptr)
    {
        ReportSkipOnce("no command list or no parameter block");
        return;
    }

    // Present placement (dlssnr/DlssNr_PresentRoute.h). On the game's own upscale call -- no timing queue
    // of ours -- only the guides are copied; the model runs later, at Present, on a list of our own. The
    // Present route's own call arrives with its queue and falls through to the pass below.
    if (timingQueue == nullptr && PresentRoute::Wanted())
    {
        ID3D12Device* device = nullptr;

        if (SUCCEEDED(cmdList->GetDevice(IID_PPV_ARGS(&device))) && device != nullptr)
        {
            const bool installed = PresentRoute::EnsureInstalled(device);

            if (installed && !beforeUpscale)
                CaptureForPresent(device, cmdList, params);

            device->Release();

            if (installed)
                return;
        }
    }

    // Ray Reconstruction is forced post by its callers: PR #6 reports that pre-SR placement does not work
    // with DLSSD's input contract. [DlssNr] RunBeforeRR lifts that, experimentally -- the callers then pass
    // forcePost false and the colour read below is DLSSD.Color. Origin-zero padded inputs are staged at their active
    // size; offset, malformed or unsupported allocations still stay post. Rechecking on the post call makes this a real
    // fallback rather than dropping NR.
    bool preSrCompatible = true;
    if (cfg.DlssNrRunBeforeSr.value_or_default() && !forcePost)
    {
        ID3D12Resource* preColor = GetResource(params, NVSDK_NGX_Parameter_Color, "DLSSD.Color");
        unsigned int renderWidth = 0, renderHeight = 0, colorBaseX = 0, colorBaseY = 0;
        params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &renderWidth);
        params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &renderHeight);
        params->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, &colorBaseX);
        params->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, &colorBaseY);

        if (preColor == nullptr)
        {
            preSrCompatible = false;
        }
        else
        {
            const D3D12_RESOURCE_DESC colorDesc = preColor->GetDesc();
            const unsigned int allocationWidth = (unsigned int) colorDesc.Width;
            const unsigned int allocationHeight = colorDesc.Height;
            const auto active = PreSrColorExtent(colorDesc, renderWidth, renderHeight, colorBaseX, colorBaseY);
            preSrCompatible = active.has_value();

            if (active && (active->width != allocationWidth || active->height != allocationHeight))
            {
                static bool reportedPadding = false;
                if (!reportedPadding)
                {
                    reportedPadding = true;
                    LOG_INFO(
                        "DLSS-NR before SR: staging active {}x{} from padded Color allocation {}x{}; "
                        "only the active rectangle is copied back. Model size follows active size and WorkingScale.",
                        active->width, active->height, allocationWidth, allocationHeight);
                }
            }

            if (!preSrCompatible)
            {
                static bool warnedSubrect = false;
                if (!warnedSubrect)
                {
                    warnedSubrect = true;
                    LOG_WARN("DLSS-NR before SR requires a valid origin-zero active rectangle inside a "
                             "single-sample 2D Color texture; got allocation {}x{}, active {}x{} at {},{}. "
                             "Falling back after SR.",
                             allocationWidth, allocationHeight, renderWidth, renderHeight, colorBaseX, colorBaseY);
                }
            }
        }
    }

    const bool configuredBefore = cfg.DlssNrRunBeforeSr.value_or_default() && !forcePost && preSrCompatible;
    if (configuredBefore != beforeUpscale)
        return;

    // Which of the game's APIs this evaluate arrived through.
    //
    // Says out loud what was previously only reasoned about: an FSR or XeSS title reaches this pass
    // transitively, because those shims call OptiScaler's own NVSDK_NGX_D3D12_EvaluateFeature and
    // this pass hangs off that. Nothing needed adding to the shims -- a call there would run the
    // model twice -- but "nothing needed adding" is a claim, and this is the line that checks it.
    {
        static ApiUpscalerInput saidApi = (ApiUpscalerInput) -1;
        const ApiUpscalerInput api = State::Instance().currentInputApiName;

        if (saidApi != api)
        {
            saidApi = api;
            LOG_INFO("DLSS-NR reached through the game's {} input", ApiUpscalerInputName(api));
        }
    }

    ID3D12Resource* output = GetResource(params, NVSDK_NGX_Parameter_Output, "DLSSD.Output");
    ID3D12Resource* target = beforeUpscale ? GetResource(params, NVSDK_NGX_Parameter_Color, "DLSSD.Color") : output;
    ID3D12Resource* depth = GetResource(params, NVSDK_NGX_Parameter_Depth, "DLSSD.Depth");
    ID3D12Resource* motion = GetResource(params, NVSDK_NGX_Parameter_MotionVectors, "DLSSD.MotionVectors");

    // Without all three there is nothing to run on. This is not a failure -- some evaluates legitimately
    // carry none of it -- so it stays quiet and tries again next frame.
    if (target == nullptr || depth == nullptr || motion == nullptr)
    {
        ReportSkipOnce(target == nullptr  ? (beforeUpscale ? "the parameters carried no color texture"
                                                           : "the parameters carried no output texture")
                       : depth == nullptr ? "the parameters carried no depth"
                                          : "the parameters carried no motion vectors");
        return;
    }

    unsigned int createFlags = 0;
    params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &createFlags);

    DlssNrFrameInfo frame {};
    frame.DepthInverted = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
    frame.BeforeUpscale = beforeUpscale;
    frame.SubmissionEpoch = timingQueue != nullptr ? submissionEpoch : State::Instance().frameCount;

    // The Present count is the proof that a command list holding a new feature was submitted, and it
    // only moves when this app wraps the game's swapchain. It does not always: Resident Evil 2 through
    // REFramework and PureDark's plugin never advanced it past 0, so every feature waited forever for
    // an epoch change -- 9000 evaluates, not one frame of Neural Rendering, while the log said
    // "running". When the count stays put across this many evaluates, each evaluate becomes the epoch:
    // the game calls its upscaler once per frame, so the next call means last frame's list went out.
    if (timingQueue == nullptr)
    {
        static unsigned long long lastPresentCount = 0;
        static unsigned long long callsWithoutPresent = 0;
        static unsigned long long evaluateEpoch = 0;
        static bool usingEvaluateEpoch = false;
        constexpr unsigned long long kStuckCalls = 120;

        const unsigned long long presentCount = State::Instance().frameCount;
        ++evaluateEpoch;

        if (presentCount != lastPresentCount)
        {
            lastPresentCount = presentCount;
            callsWithoutPresent = 0;
        }
        else if (!usingEvaluateEpoch && ++callsWithoutPresent >= kStuckCalls)
        {
            usingEvaluateEpoch = true;
            LOG_WARN("DLSS-NR: the Present count has not moved in {} evaluates (stuck at {}) -- this game's "
                     "swapchain is not wrapped here, so each evaluate now counts as a submitted frame",
                     kStuckCalls, presentCount);
        }

        // Far above any Present count, so the switch can only ever move the epoch forward.
        if (usingEvaluateEpoch)
            frame.SubmissionEpoch = (1ull << 40) + evaluateEpoch;
    }

    // Color and Output may use different formats even though DLSS treats them as the same frame colour
    // space. Output is the stable authority across injection points; target is only a fallback for a
    // malformed parameter block.
    ID3D12Resource* colourAuthority = output != nullptr ? output : target;
    frame.ColourIsLinearHdr = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0 && colourAuthority != nullptr &&
                              FormatCanHoldLinearHdr(colourAuthority->GetDesc().Format);

    // The game telling the upscaler to forget everything it has accumulated: a cut, a teleport, a
    // load. Every upscaler in this tree reads it and this pass did not, so the model's history was
    // only ever reset by things that happened to us -- a resize, a rebuild, a recovery from failure
    // -- and never by anything that happened in the game. Across a cut the model was reprojecting
    // the previous scene onto the new one and being asked to reconcile them.
    //
    // Read the same way FFXFeature_Dx12 reads it, including leaving it alone when the parameter is
    // absent: a game that never sets it is not asking for a reset every frame.
    {
        unsigned int gameReset = 0;

        if (params->Get(NVSDK_NGX_Parameter_Reset, &gameReset) == NVSDK_NGX_Result_Success)
            frame.Reset = gameReset != 0;
    }

    // How much of the guides is real. See DlssNrFrameInfo -- zero means the game did not say.
    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &frame.RenderSubrectWidth);
    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &frame.RenderSubrectHeight);

    // The Present route's own block says where a letterboxed picture sits. No game ever sets these keys.
    if (g_presentRouteDispatch)
    {
        params->Get(kPresentActiveX, &frame.ActiveBaseX);
        params->Get(kPresentActiveY, &frame.ActiveBaseY);
        params->Get(kPresentActiveWidth, &frame.ActiveWidth);
        params->Get(kPresentActiveHeight, &frame.ActiveHeight);
    }

    if (params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &frame.MvScaleX) != NVSDK_NGX_Result_Success)
        frame.MvScaleX = 1.0f;

    if (params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &frame.MvScaleY) != NVSDK_NGX_Result_Success)
        frame.MvScaleY = 1.0f;

    // What the game says about its own exposure. Logged, used for nothing yet.
    //
    // The white point measured from the frame turned out to be a control loop rather than a
    // measurement: the pass writes into the buffer it reads, most games adapt their exposure to the
    // finished frame, and the two chase each other -- 0.01 to 97.9 in one Enshrouded session. Any
    // statistic taken from a frame we modify has that problem.
    //
    // These do not. DLSS.Pre.Exposure is the scale the game applied before handing the buffer over,
    // and ExposureTexture is a 1x1 the game fills with the exposure it is using; both are the game's
    // own numbers, decided upstream of anything here. Whether either is close to the divisor the model
    // actually wants is unknown, which is why this only prints them.
    //
    // The auto-exposure flag decides whether the texture means anything: with it set the game is
    // telling DLSS to work exposure out for itself and may supply nothing. OptiScaler forces that flag
    // on for eighteen games, so it is logged too -- reading a value whose flag has been overridden is
    // how the debug views lied earlier tonight.
    {
        float preExposure = 0.0f;
        const bool havePre =
            params->Get(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, &preExposure) == NVSDK_NGX_Result_Success;

        void* exposureTex = nullptr;
        params->Get(NVSDK_NGX_Parameter_ExposureTexture, &exposureTex);

        frame.ExposureTexture = exposureTex;
        frame.PreExposure = havePre && preExposure > 1e-6f ? preExposure : 1.0f;

        g_nr.exposureOfferedNow = exposureTex != nullptr;
        g_nr.exposureEverOffered = g_nr.exposureEverOffered || g_nr.exposureOfferedNow;
        g_nr.exposureFrames++;

        const bool autoExposureFlag = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_AutoExposure) != 0;

        struct ExposureReport
        {
            bool valid;
            float pre;
            bool havePre;
            bool haveTexture;
            bool autoFlag;
        };

        static ExposureReport logged {};
        const ExposureReport now { true, havePre ? preExposure : 0.0f, havePre, exposureTex != nullptr,
                                   autoExposureFlag };

        if (!logged.valid || logged.havePre != now.havePre || logged.haveTexture != now.haveTexture ||
            logged.autoFlag != now.autoFlag ||
            std::abs(logged.pre - now.pre) > std::max(0.01f * std::abs(now.pre), 1e-4f))
        {
            logged = now;
            LOG_INFO("DLSS-NR exposure from the game: DLSS.Pre.Exposure {}, ExposureTexture {}, "
                     "auto-exposure flag {}",
                     now.havePre ? std::to_string(now.pre) : std::string("not supplied"),
                     now.haveTexture ? "supplied" : "not supplied", now.autoFlag ? "set" : "clear");
        }

        // The value itself, once it has come back off the GPU. Separate from the line above because
        // that one says what the game offers and this one says what it actually reads -- and because
        // the reading arrives three frames after the offer.
        static float loggedExposure = -1.0f;

        if (g_nr.gameExposure > 1e-6f &&
            std::abs(loggedExposure - g_nr.gameExposure) > std::max(0.02f * g_nr.gameExposure, 1e-5f))
        {
            loggedExposure = g_nr.gameExposure;
            LOG_INFO("DLSS-NR game exposure {:.5f} (pre-exposure {:.3f}) -> white point would be {:.2f}",
                     g_nr.gameExposure, g_nr.gamePreExposure, g_nr.gamePreExposure / g_nr.gameExposure);
        }

        // The scan's number, on the same cadence, so one log carries both.
        //
        // This is the whole validation. In a game that hands over an exposure texture there is a
        // known-correct value; if the scan's candidate tracks it, the scan found the right buffer
        // rather than merely a moving one, and can be trusted where a game hands over nothing.
        // Comparing two numbers after the fact needs both written down, and until now the scan's
        // value existed only in a menu nobody can read while playing.
        {
            int which = 0;
            float low = 0.0f, high = 0.0f;
            const float scanned = DlssNr::ExposureScan::BestValue(&which, &low, &high);

            static float loggedScan = -1.0f;

            if (scanned > 0.0f && std::abs(loggedScan - scanned) > std::max(0.02f * scanned, 1e-6f))
            {
                loggedScan = scanned;

                if (g_nr.gameExposure > 1e-6f)
                    LOG_INFO("DLSS-NR exposure scan: candidate {} = {:.5f} ({:.5f}..{:.5f})  |  the "
                             "game's own exposure is {:.5f}  |  ratio {:.4f}",
                             which, scanned, low, high, g_nr.gameExposure, scanned / g_nr.gameExposure);
                else
                    LOG_INFO("DLSS-NR exposure scan: candidate {} = {:.5f} ({:.5f}..{:.5f})  |  this "
                             "game supplies no exposure to compare against",
                             which, scanned, low, high);
            }
        }
    }

    // The upscaler's inputs are at render resolution while colour and output are at display
    // resolution; the model takes that as a subrect per resource, which the pass reads from the
    // resources themselves.
    ID3D12Device* device = nullptr;

    if (FAILED(target->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
    {
        ReportSkipOnce("the output texture belongs to no D3D12 device");
        return;
    }

    // The pass is the object, so the caller holds it. Built once, on the device the frame is on.
    if (g_compose == nullptr)
        g_compose = std::make_unique<DlssNr_Dx12>("Neural Rendering", device);

    device->Release();

    if (g_compose == nullptr)
    {
        ReportSkipOnce("the pass could not be created");
        return;
    }

    g_compose->Dispatch(cmdList, target, depth, motion, target, frame, timingQueue);
}

void EvaluateAfterUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params,
                          ID3D12CommandQueue* timingQueue, bool forcePost, unsigned long long submissionEpoch)
{
    EvaluateInternal(cmdList, params, false, timingQueue, forcePost, submissionEpoch);
}

void EvaluateBeforeUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params,
                           ID3D12CommandQueue* timingQueue, unsigned long long submissionEpoch)
{
    EvaluateInternal(cmdList, params, true, timingQueue, false, submissionEpoch);
}

// The pass. Resources in, nothing read from anywhere the caller cannot see.

void ProbeD3D11(void* d3d11Device)
{
    static bool done = false;

    if (done || d3d11Device == nullptr)
        return;

    // Every other entry point in this file takes the lock before touching g_nr; this one was reaching
    // EnsureForwarder without it.
    std::lock_guard<std::mutex> nrLock(g_nrMutex);

    // Opt in only. See the note on DlssNrProbeD3D11: this is the one call in the pass that reaches
    // into a subsystem on the game's own device rather than reading something we already hold.
    if (!Config::Instance()->DlssNrProbeD3D11.value_or_default())
        return;

    done = true;

    if (!EnsureForwarder())
        return;

    auto probe = (int (*)(const wchar_t*)) GetProcAddress(g_nr.forwarder, "dlssnr_d3d11_probe");
    auto init = (int (*)(const wchar_t*, const wchar_t*, void*, int, int*, int*)) GetProcAddress(g_nr.forwarder,
                                                                                                 "dlssnr_d3d11_init");

    if (probe == nullptr || init == nullptr)
    {
        LOG_INFO("DLSS-NR D3D11: this forwarder has no D3D11 probe");
        return;
    }

    auto snippet = Util::FindFilePath(g_dllDir, "nvngx_dlssnr.dll");

    if (!snippet.has_value())
        snippet = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssnr.dll");

    if (!snippet.has_value())
        return;

    // Four bits, one per entry point: init 1, create 2, evaluate 4, release 8.
    const int bits = probe(snippet->wstring().c_str());

    // And the question NGX has an API for. Asked first because it creates nothing: if the feature
    // declines D3D11 here, that is the feature's own answer rather than our reading of a failed init.
    auto requirements = (int (*)(const wchar_t*, void*, unsigned int*, unsigned int*, unsigned int*)) GetProcAddress(
        g_nr.forwarder, "dlssnr_d3d11_requirements");

    if (requirements != nullptr)
    {
        // The adapter the game is actually running on. Without it the query answers
        // AdapterUnsupported, which looks like a verdict on the hardware and is really a verdict on
        // the question -- that is what the first attempt got, on a 5080.
        IDXGIAdapter* adapter = nullptr;
        IDXGIFactory1* factory = nullptr;

        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && factory != nullptr)
            factory->EnumAdapters(0, &adapter);

        unsigned int supported = 0xFFFFFFFFu;
        unsigned int minArch = 0;
        unsigned int minOs = 0;
        const int rc = requirements(snippet->wstring().c_str(), adapter, &supported, &minArch, &minOs);

        const char* meaning = supported == 0     ? "SUPPORTED"
                              : (supported & 16) ? "NotImplemented -- the feature has no D3D11 path"
                              : (supported & 4)  ? "AdapterUnsupported"
                              : (supported & 2)  ? "DriverVersionUnsupported"
                              : (supported & 8)  ? "OSVersionBelowMinimum"
                              : (supported & 1)  ? "CheckNotPresent"
                                                 : "unknown";

        LOG_WARN("DLSS-NR D3D11: GetFeatureRequirements {} ({}), FeatureSupported 0x{:X} -- {}. "
                 "minimum architecture 0x{:X}, minimum OS 0x{:X}",
                 rc, NgxResultName((unsigned int) rc), supported, meaning, minArch, minOs);

        if (adapter != nullptr)
            adapter->Release();

        if (factory != nullptr)
            factory->Release();
    }

    LOG_INFO("DLSS-NR D3D11: entry points resolved {}/15 (init {}, create {}, evaluate {}, release {})", bits,
             (bits & 1) ? "yes" : "no", (bits & 2) ? "yes" : "no", (bits & 4) ? "yes" : "no",
             (bits & 8) ? "yes" : "no");

    if (bits != 15)
    {
        LOG_INFO("DLSS-NR D3D11: incomplete surface, the bridge stays the only route");
        return;
    }

    // Four ways of asking, since the feature has already said it supports this platform.
    int attempt = 0;
    int results[4] = { -9, -9, -9, -9 };

    const int result = init(snippet->wstring().c_str(), State::Instance().NVNGX_ApplicationDataPath.c_str(),
                            d3d11Device, 0x0000015, &attempt, results);

    static const char* kNames[4] = { "Init_Ext on our own copy", "Init on our own copy",
                                     "Init_Ext on the shared module", "Init on the shared module" };

    for (int i = 0; i < 4; ++i)
    {
        LOG_INFO("DLSS-NR D3D11:   {} -> {} ({})", kNames[i], results[i],
                 results[i] == -2   ? "module not loaded"
                 : results[i] == -3 ? "export missing"
                 : results[i] == -9 ? "not reached"
                                    : NgxResultName((unsigned int) results[i]));
    }

    if (result == 1)
        LOG_WARN("DLSS-NR D3D11: initialised, via {}. The feature already said this platform is "
                 "supported; now the call works too. Next is a feature create on a device context.",
                 attempt > 0 ? kNames[attempt - 1] : "?");
    else
        // Deliberately not "so the bridge is required". GetFeatureRequirements answers 0x0 SUPPORTED
        // with a minimum architecture this card meets, so the platform is not the obstacle and saying
        // otherwise here would be printing a conclusion the evidence does not carry.
        LOG_WARN("DLSS-NR D3D11: every init variant refused, last {} ({}) -- though the feature itself "
                 "reports this platform as supported, so the obstacle is in how it is being called",
                 result, NgxResultName((unsigned int) result));
}

void RequestCleanUpCapture() { g_cleanCapture.request(); }
bool CleanUpCapturePending() { return g_cleanCapture.wanted() || g_cleanCapture.pending(); }

CleanUpReading CleanUpState()
{
    CleanUpReading r {};
    const uint32_t mode = Config::Instance()->DlssNrCleanUpMode.value_or_default();
    r.measuring = (mode == 1 || mode == 2) && g_nr.haloGrid[0] != nullptr;
    r.strength = g_nr.cleanApplied;
    r.haloBefore = g_nr.haloBefore;
    r.haloAfter = g_nr.haloAfter;
    r.haloModel = g_nr.haloModel;
    if (!g_timingTrust.Untrusted())
        r.composeMs = g_lastComposeTime;
    return r;
}

AutoToneReading AutoTone()
{
    AutoToneReading r {};
    r.measuring = g_nr.toneSettled;
    r.brightness = g_nr.autoBrightness;
    r.contrast = g_nr.autoContrast;
    r.mean = g_nr.toneMean;
    r.spreadStops = g_nr.toneSpreadStops;
    return r;
}

CalibrationReading Calibration()
{
    CalibrationReading r {};
    r.suggestion = g_nr.calibSuggestion;
    r.steadiness = g_nr.calibSteadiness;
    r.samples = g_nr.calibCount;
    r.usable = g_nr.calibUsable;
    r.why = g_nr.calibWhy;
    return r;
}

bool IsRunning() { return g_nr.feature != nullptr && !g_nr.failed; }

const char* FailureReason() { return g_nr.failed ? g_nr.reason : ""; }

// Same GetModuleHandleW-by-name check ConflictingNrAddon() uses, just not refusing anything --
// this is purely "what is this NR pass actually running on" for the overlay.
bool IsFeederPresent() { return GetModuleHandleW(L"dlss5-feed.addon64") != nullptr; }

MotionReading MotionState()
{
    MotionReading out {};
    out.evaluates = g_tracked.evaluates;
    out.blindEvaluates = g_tracked.blindEvaluates;
    out.lastBlind = g_tracked.lastBlind;
    out.usingFlow = g_tracked.usingFlow;
    return out;
}

// What the game offers by way of exposure, and what has been read from it. For the menu, so a user
// can see whether this game supplies one at all without having to read a log.
ExposureStatus GameExposureStatus()
{
    ExposureStatus s {};
    s.seenFrames = g_nr.exposureFrames;
    s.offeredNow = g_nr.exposureOfferedNow;
    s.everOffered = g_nr.exposureEverOffered;
    s.exposure = g_nr.gameExposure;
    s.preExposure = g_nr.gamePreExposure;
    return s;
}

std::optional<double> LastGpuTime() { return g_timingTrust.Untrusted() ? std::nullopt : g_lastGpuTime; }

bool VideoMemory(uint64_t* usedBytes, uint64_t* budgetBytes)
{
    const uint64_t budget = g_vramBudget.load();
    if (budget == 0)
        return false;

    if (usedBytes != nullptr)
        *usedBytes = g_vramUsage.load();
    if (budgetBytes != nullptr)
        *budgetBytes = budget;

    return true;
}

void RequestCapture(unsigned int frames)
{
    ClearCaptureDirectory();
    g_capture.request(frames);
}

bool CaptureInProgress() { return g_capture.isActive(); }

void Shutdown()
{
    std::lock_guard<std::mutex> nrLock(g_nrMutex);

    for (auto& r : g_nrRetired)
    {
        if (r.feature != nullptr && g_nr.release != nullptr)
            g_nr.release(r.feature);

        if (r.resource != nullptr)
            r.resource->Release();
    }

    g_nrRetired.clear();

    g_buildMeasure = {};

    if (g_nr.feature != nullptr && g_nr.release != nullptr)
        g_nr.release(g_nr.feature);

    g_nr.feature = nullptr;
    g_nr.featurePendingSubmission = false;

    for (unsigned int pass = 1; pass < DlssNr::MaxPassCount; ++pass)
    {
        void*& f = g_nr.passFeature[pass];
        if (f != nullptr && g_nr.release != nullptr)
            g_nr.release(f);

        f = nullptr;
        g_nr.passNeedsReset[pass] = false;
        g_nr.passCreateFailed[pass] = false;
        g_nr.passPendingSubmission[pass] = false;
    }

    if (g_nr.output != nullptr)
    {
        g_nr.output->Release();
        g_nr.output = nullptr;
    }

    if (g_nr.passScratch != nullptr)
    {
        g_nr.passScratch->Release();
        g_nr.passScratch = nullptr;
    }
    g_nr.passScratchFailed = false;

    if (g_nr.colorCopy != nullptr)
    {
        g_nr.colorCopy->Release();
        g_nr.colorCopy = nullptr;
    }

    if (g_nr.hdrCopy != nullptr)
    {
        g_nr.hdrCopy->Release();
        g_nr.hdrCopy = nullptr;
    }

    if (g_nr.activeColor != nullptr)
    {
        g_nr.activeColor->Release();
        g_nr.activeColor = nullptr;
    }

    if (g_nr.colorSmall != nullptr)
    {
        g_nr.colorSmall->Release();
        g_nr.colorSmall = nullptr;
    }

    if (g_nr.superUp != nullptr)
    {
        delete g_nr.superUp;
        g_nr.superUp = nullptr;
    }

    if (g_nr.superDown != nullptr)
    {
        delete g_nr.superDown;
        g_nr.superDown = nullptr;
    }

    // The pair that enlarges a below-size model answer and its proxy with the chosen filter.
    if (g_nr.editUp != nullptr)
    {
        delete g_nr.editUp;
        g_nr.editUp = nullptr;
    }

    if (g_nr.proxyUp != nullptr)
    {
        delete g_nr.proxyUp;
        g_nr.proxyUp = nullptr;
    }

    if (g_nr.outputNative != nullptr)
    {
        g_nr.outputNative->Release();
        g_nr.outputNative = nullptr;
    }

    if (g_nr.editNative != nullptr)
    {
        g_nr.editNative->Release();
        g_nr.editNative = nullptr;
    }

    if (g_nr.proxyNative != nullptr)
    {
        g_nr.proxyNative->Release();
        g_nr.proxyNative = nullptr;
    }

    if (g_nr.heldColor != nullptr)
    {
        g_nr.heldColor->Release();
        g_nr.heldColor = nullptr;
    }
    g_nr.heldActive = false;

    if (g_nr.meter != nullptr)
    {
        g_nr.meter->Release();
        g_nr.meter = nullptr;
    }

    if (g_nr.calib != nullptr)
    {
        g_nr.calib->Release();
        g_nr.calib = nullptr;
    }

    for (auto& r : g_nr.calibReadback)
    {
        if (r != nullptr)
        {
            r->Release();
            r = nullptr;
        }
    }

    g_nr.calibFrames = 0;
    g_nr.calibCount = 0;
    g_nr.calibSuggestion = 0.0f;
    g_nr.calibSteadiness = 0.0f;
    g_nr.calibUsable = false;
    g_nr.calibWhy = "measuring...";

    if (g_nr.tone != nullptr)
    {
        g_nr.tone->Release();
        g_nr.tone = nullptr;
    }

    for (auto& r : g_nr.toneReadback)
    {
        if (r != nullptr)
        {
            r->Release();
            r = nullptr;
        }
    }

    ResetAutoTone();

    for (int g = 0; g < 3; ++g)
    {
        if (g_nr.haloGrid[g] != nullptr)
        {
            g_nr.haloGrid[g]->Release();
            g_nr.haloGrid[g] = nullptr;
        }
        for (auto& rb : g_nr.haloReadback[g])
        {
            if (rb != nullptr)
            {
                rb->Release();
                rb = nullptr;
            }
        }
    }
    for (auto& h : g_nr.cleanHistory)
    {
        if (h != nullptr)
        {
            h->Release();
            h = nullptr;
        }
    }
    if (g_nr.cleanModel != nullptr)
    {
        g_nr.cleanModel->Release();
        g_nr.cleanModel = nullptr;
    }
    if (g_nr.cleanPrep != nullptr)
    {
        g_nr.cleanPrep->Release();
        g_nr.cleanPrep = nullptr;
    }
    g_nr.cleanPrepW = 0;
    g_nr.cleanPrepH = 0;
    g_nr.cleanHistoryW = 0;
    g_nr.cleanHistoryH = 0;
    g_nr.cleanHistoryValid = false;
    g_nr.cleanApplied = 0.0f;
    ResetCleanUp();

    for (auto& rb : g_nr.meterReadback)
    {
        if (rb != nullptr)
        {
            rb->Release();
            rb = nullptr;
        }
    }

    // The slots these flags describe have just been released, so nothing may vouch for what the next
    // buffers happen to contain. gameExposure is deliberately NOT cleared here: a recreate is a
    // transition within the same scene, and dropping to the slider for a few frames would be the
    // flicker the held value exists to prevent. The user switching the option off is the case where
    // the held value has to go, and that is handled at the edge in Dispatch.
    for (bool& valid : g_nr.meterExposureValid)
        valid = false;

    g_nr.meterFrames = 0;

    if (g_nr.depthClone != nullptr)
    {
        g_nr.depthClone->Release();
        g_nr.depthClone = nullptr;
    }

    if (g_nr.motionClone != nullptr)
    {
        g_nr.motionClone->Release();
        g_nr.motionClone = nullptr;
    }

    g_capture.release();
    g_gpuTime.reset();
    g_ngxTime.reset();
    g_composeTime.reset();
    g_lastNgxTime.reset();
    g_lastGpuTime.reset();

    g_compose.reset();
}
} // namespace DlssNr
