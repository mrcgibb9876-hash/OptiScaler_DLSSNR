// Copyright (c) 2026 mrcgibb9876-hash. Original to this fork, not part of upstream OptiScaler.
// Distributed under the GNU General Public License version 3 -- see LICENSE.
#pragma once

#include <d3d12.h>

#include <shaders/dlssnr/DlssNr_Common.h>
#include <nvsdk_ngx.h>

// DLSS 5 Neural Rendering, run over the upscaler's output.
//
// Neural Rendering is a post-process, not an upscaler and not a denoiser: it takes a finished frame plus
// depth and motion vectors and synthesises detail. NVIDIA ships no public integration for it, so it is
// driven directly through nvngx_dlssnr.dll as feature 18.
//
// OptiScaler is the right host for it because of one thing it knows that an external hook cannot: which
// NGX evaluate belongs to the upscaler and which to frame generation. Both are handed depth and motion
// vectors, so anything guessing from the parameter block alone attaches to both and runs the model twice
// per rendered frame. Here it is a lookup on the feature handle.
class Config;
struct IDXGISwapChain3;

namespace DlssNr
{
inline constexpr unsigned int MaxPassCount = 3;

// The model runs immediately after the game's upscaler, before the interface is drawn. It is shown a
// display-referred proxy of that frame -- the sort of picture it was trained on -- and its answer is
// composed back over the untouched original.
// Runs the model over Output on the same command list, immediately after the upscaler has written it.
// Called only for upscaler evaluates -- never for frame generation, which is the whole point.
//
// Safe to call every frame; it builds what it needs on first use and disables itself for the session if
// anything fails, rather than retrying into a crash.
// timingQueue is the queue this command list will be executed on, when the caller knows it.
// State::currentCommandQueue only exists once a D3D12 swapchain has been created, which a Vulkan
// game never does -- so without this the pass runs and never reports what it cost.
void EvaluateAfterUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params,
                          ID3D12CommandQueue* timingQueue = nullptr, bool forcePost = false,
                          unsigned long long submissionEpoch = 0);

// Present placement (DlssNr_PresentRoute.h): runs the pass over the back buffer about to be presented, on a
// command list of its own, from the guides the game's last upscale call left behind.
void RunAtPresent(IDXGISwapChain3* swapChain, ID3D12CommandQueue* queue, unsigned long long presentIndex);

// Re-reads OptiScaler.ini if it changed on disk (Config::ReloadIfChangedOnDisk, rate-limited inside) and logs
// the values now in effect. Called at every entry point BEFORE anything looks at [DlssNr] Enabled: the poll
// used to live only inside the pass, so once a reload turned the pass off nothing polled again and no later
// change -- switching it back on included -- was ever read.
void PollSettingsFromDisk();

// Runs the same pass over Color immediately before Super Resolution consumes it. The call is a no-op
// unless RunBeforeSR is enabled. Color is returned in its original readable state.
void EvaluateBeforeUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params,
                           ID3D12CommandQueue* timingQueue = nullptr, unsigned long long submissionEpoch = 0);

// Frame generation titles tag their UI layer through Streamline; a copy of it makes the HUD mask
// exact at the finished frame. Called at tag time.

// The settings panel, drawn inside OptiScaler's menu.
void RenderMenu(::Config* config, float menuResScale);

// Clears the session failure latch, so a failure caused by transient thrash does not cost a restart.
void RetryAfterFailure();

// Every [DlssNr] setting back to what it ships with, from the panel's Reset all button. Where the
// panel sits, the keys that open it and how it looks are the player's and are kept (DlssNr_Api.cpp).
void ResetSettingsToDefaults();

// Asks the model whether it will work on Direct3D 11 at all, once, and logs the answer.
//
// The bridge exists because of a claim nobody tested: "the model refuses on DX11, it answers
// FeatureNotSupported". Nothing in this project has ever called the snippet's own D3D11 entry points
// -- it exports ten of them, implemented in ngx_d3d11.cpp and sharing CreateFeatureCommon and
// EvaluateFeatureCommon with the D3D12 path. Nothing is created and nothing changes; it resolves the
// entry points and initialises on the game's own device, which is where a refusal would appear.
void ProbeD3D11(void* d3d11Device);

// What scale this game's buffer is on, measured from the untouched copy of each frame.
//
// A suggestion only. Nothing applies it: the menu shows it and the user takes it or does not, which
// keeps the number visible and adjustable rather than a value that moved on its own. Confidence is
// how settled recent readings are -- 1 means they agree, 0 means the scene is changing under the
// measurement and no single value would serve.
struct CalibrationReading
{
    float suggestion = 0.0f;

    // How much recent readings agree. This is steadiness, not correctness: a frozen frame agrees with
    // itself perfectly, so a loading screen scores full marks for a number that means nothing. Read it
    // together with usable.
    float steadiness = 0.0f;

    unsigned long long samples = 0;

    // Whether the scene is worth measuring at all. False when the frame is already tone mapped -- the
    // divisor does nothing there and the reading would be a meaningless 0.9 -- or when too little of
    // the picture is lit to say where the top of the range is. A dark cave gives a small number very
    // steadily, which is the trap this exists to close.
    bool usable = false;
    const char* why = "";
};

CalibrationReading Calibration();

// Auto brightness / Auto contrast (D3D12): the values the resolve is using right now, and what the last
// reading saw. measuring is false until a first reading lands -- the panel then shows the slider's value.
struct AutoToneReading
{
    bool measuring = false;
    float brightness = 1.0f;
    float contrast = 1.0f;
    float mean = 0.0f;        // the frame's average, 1 = paper white
    float spreadStops = 0.0f; // darkest tenth to brightest tenth, in stops
};

AutoToneReading AutoTone();

// Image Clean Up (D3D12): the strength the resolve ran with last (Auto's own choice in Auto), and the
// halo meter's two readings, in stops -- the model's glow before the clean up, and what is left of it in
// the finished picture. -1 until a reading lands; measuring is false while the clean up is off.
struct CleanUpReading
{
    bool measuring = false;
    float strength = 0.0f;
    float haloBefore = -1.0f;
    float haloAfter = -1.0f;
    float haloModel = -1.0f;         // the model's own change laid on the frame, before the composition
    std::optional<double> composeMs; // the composition pass, which the clean up runs inside
};

CleanUpReading CleanUpState();

// Whether the model is loaded and running, for the overlay.
bool IsRunning();

// Why it is not, if it is not. Empty while it is running or has not been tried yet.
const char* FailureReason();

// Whether the DLSS5 Feeder (dlss5-feed.addon64) is loaded in this process -- for the overlay, so
// it can say which evaluate call Neural Rendering is actually running on: the game's own native
// DLSS, or a synthetic one the Feeder built from ReShade depth + motion vectors for a game that
// has no DLSS of its own. A ReShade add-on is loaded with LoadLibrary, so it is an ordinary module
// and can be found by name, the same way ConflictingNrAddon() finds a competing consumer.
bool IsFeederPresent();

// Is motion actually reaching the model? Read-only, for the panel.
//
// The failure this answers is the one that has cost this project the most triage time, because it
// looks like nothing: when no motion vectors arrive, the model is handed a zero-motion texture and
// runs anyway. Every log line still says it ran. The picture is sharp when still and smears when
// moving, and until now the only way to find out why was to read ReShadePreset.ini from outside
// the game and infer.
//
// The panel shows it rather than acting on it. Which provider to use instead is a question about
// files, licences and downloads, and that belongs to the manager.
struct MotionReading
{
    unsigned long long evaluates = 0;

    // Of those, how many were handed no vectors. Equal to evaluates means nothing has ever fed it;
    // somewhere in between means a provider that drops frames, which is a different fault.
    unsigned long long blindEvaluates = 0;

    bool lastBlind = false;

    // Vectors came from the engine's own optical flow module rather than from a ReShade provider,
    // which is the Present route. Worth telling apart: on that route no ReShade provider is
    // involved at all, so "change the provider" would be the wrong advice.
    bool usingFlow = false;
};

MotionReading MotionState();

// What the game offers by way of exposure. Observed every frame whether or not the setting is on, so
// the menu can say whether turning it on would do anything here.
struct ExposureStatus
{
    unsigned long long seenFrames = 0; // evaluates observed; 0 means nothing has run yet
    bool offeredNow = false;           // a texture on the most recent frame
    bool everOffered = false;          // a texture on any frame so far
    float exposure = 0.0f;             // last value read back, 0 if none
    float preExposure = 1.0f;
};

ExposureStatus GameExposureStatus();

// The white point the exposure meter has settled on, or 0 if it has not taken a reading yet. For the
// overlay, so the number in use is visible rather than inferred.

// What the pass last cost on the GPU, in milliseconds, or nothing if it has not been measured yet.
std::optional<double> LastGpuTime();

// The game process's video memory use and the budget Windows gives it on the GPU the pass runs on, in
// bytes, as last read by the pass (DXGI QueryVideoMemoryInfo, local segment). False until the pass has
// read it once. For the panel.
bool VideoMemory(uint64_t* usedBytes, uint64_t* budgetBytes);

// What the white point meter last settled on, or 0 when it is not running. For the menu.

// Writes a run of consecutive frames, each as the upscaler produced it and again after the model's edit.
// The pair is a control: same frames, same run, one variable.
void RequestCapture(unsigned int frames);
bool CaptureInProgress();

void Shutdown();
} // namespace DlssNr
