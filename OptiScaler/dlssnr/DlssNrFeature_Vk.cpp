// Copyright (c) 2026 mrcgibb9876-hash. Original to this fork, not part of upstream OptiScaler.
// Distributed under the GNU General Public License version 3 -- see LICENSE.
#include "pch.h"

#include "DlssNrFeature_Vk.h"

#include <Config.h>
#include <State.h>
#include <Util.h>
#include <NVNGX_Parameter.h>

#include <shaders/dlssnr/DlssNr_Vk.h>
#include <shaders/output_scaling/OS_Vk.h>
#include <dlssnr/DlssNr_TimingTrust.h>

#include <algorithm>

namespace DlssNr
{
// DlssNr_Dx12.cpp. Declared here rather than by including DlssNrFeature_Dx12.h, which would pull the
// D3D12 headers into the Vulkan pass for one function that has nothing to do with D3D12.
void PollSettingsFromDisk();
} // namespace DlssNr
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <dxgi1_4.h>

namespace DlssNr
{

namespace
{

// The forwarder's Vulkan surface. The model checks its caller's module path and requires nvngx.dll in
// it, whichever API is being used, so these calls go through the same shim the D3D12 path does.
using PFN_VkProbe = int(__cdecl*)(const wchar_t*);
using PFN_VkInit = int(__cdecl*)(const wchar_t*, const wchar_t*, void*, void*, void*, int);
using PFN_VkCreate = void*(__cdecl*) (void*, void*, unsigned int, unsigned int, int, float, int, float, float, float,
                                      int, int);
using PFN_VkEvaluate = int(__cdecl*)(void*, void*, void*, void*, void*, void*, void*, unsigned int, unsigned int,
                                     unsigned int, unsigned int, int, int, float, int, float, float, float, int, float,
                                     float);
using PFN_VkRelease = void(__cdecl*)(void*);

// One image this pass owns: the storage, the view, and the NGX wrapper that describes it. Kept
// together because they are created, resized and destroyed as one thing.
struct OwnedImage
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    NVSDK_NGX_Resource_VK ngx {};
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;

    bool Valid() const { return image != VK_NULL_HANDLE && view != VK_NULL_HANDLE; }
};

// Stacked model passes: the same ceiling as DlssNr::MaxPassCount, which lives in the D3D12 header this
// file deliberately does not include. The menu's slider and the ini are clamped to that, and this path
// clamps to its own copy, so the two agreeing is what keeps a fourth pass from being asked of an array of
// three.
constexpr unsigned int kVkMaxPasses = 3;

struct VkState
{
    bool failed = false;
    const char* reason = "";

    HMODULE forwarder = nullptr;
    PFN_VkProbe probe = nullptr;
    PFN_VkInit init = nullptr;
    PFN_VkCreate create = nullptr;
    PFN_VkEvaluate evaluate = nullptr;
    PFN_VkRelease release = nullptr;

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;

    bool ngxInitialised = false;
    void* feature = nullptr;
    NVSDK_NGX_Parameter* capabilityParams = nullptr;

    // What the model writes, the proxy it is shown, and the frame as the upscaler left it.
    OwnedImage output;
    OwnedImage proxy;
    OwnedImage keep;

    // The proxy at the model's working size, when that is below the frame. The model -- 98% of the
    // cost -- then runs on this instead of the full proxy, which is the whole point of the working
    // scale slider. Unused (and never created) at scale 1, so the default path is unchanged.
    OwnedImage proxySmall;

    // Supersampling (working scale > 1): the model runs above native, superUp enlarges the proxy to
    // that size and superDown averages the answer (output) back into outputNative at native for a 1:1
    // composite. nrScaler is the filter both were built with, so a changed DlssNrScalingDownscaler
    // rebuilds them. Unused and never created at scale <= 1.
    OwnedImage outputNative;
    std::unique_ptr<OS_Vk> superUp;
    std::unique_ptr<OS_Vk> superDown;

    // Below full resolution: the model's answer and the small proxy it is measured against, both
    // enlarged to frame size with the chosen Upscaler before the resolve reads them. Without this the
    // resolve samples two small pictures bilinearly at full-size UVs and the filter choice does
    // nothing at all, which is what D3D12 did until 2026-09-22. Both legs take the SAME filter: the
    // composition is handed model minus proxy, and two filters would put the difference between them
    // inside that edit.
    OwnedImage editNative;
    OwnedImage proxyNative;
    std::unique_ptr<OS_Vk> editUp;
    std::unique_ptr<OS_Vk> proxyUp;
    Upsampler nrUpsampler = Upsampler::Count;
    Scaler nrScaler = Scaler::Count;

    std::unique_ptr<DlssNr_Vk> pass;

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t workWidth = 0;
    uint32_t workHeight = 0;
    bool reset = true;
    unsigned long long frames = 0;

    // Timing. A pair of timestamps per frame across a ring, read back three frames later: a query
    // read the frame it was written stalls the CPU on the GPU, which would cost more than the pass
    // it is measuring. Vulkan reports ticks, and timestampPeriod is how many nanoseconds a tick is.
    VkQueryPool queryPool = VK_NULL_HANDLE;
    float timestampPeriod = 0.0f;
    unsigned long long timedFrames = 0;
    std::optional<double> lastGpuTime;
    NrTimingTrust timingTrust;

    // Whether the game hands over an exposure texture, and what it said when it did.
    bool exposureOffered = false;

    // The game's own exposure, read off its 1x1 texture, and the scale it multiplied its buffer by.
    //
    // gameExposure holds its last good value rather than resetting when a frame arrives without a
    // texture: GTA V dropped it three times in one session on the D3D12 path, and falling back to a
    // default on those frames is a flicker, not a fallback.
    float gameExposure = 0.0f;
    float gamePreExposure = 1.0f;

    // The exposure's courier: an 8x8 R32_FLOAT image the meter writes, and a ring of host-visible
    // buffers it is copied into. Only texel (0,0) is ever read -- the rest of the grid belongs to the
    // frame-statistics meter that was removed from the shared shader, and 8x8 is here only so that a
    // single 8x8 thread group lands entirely inside the image.
    OwnedImage meter;
    VkBuffer meterReadback[4] = {};
    VkDeviceMemory meterReadbackMemory[4] = {};
    void* meterMapped[4] = {};
    unsigned long long meterFrames = 0;

    // WHICH upscaler feature this state belongs to.
    //
    // Everything above is one set of images at one size, and until now the only thing deciding when
    // to throw it away and build another was the size itself. That is fine while a game runs one
    // upscaler. Indiana Jones and the Great Circle runs two at once -- its own log says
    // "Created DLSSDContext feature (853,480) -> (1280,720) for viewport 2" beside a main feature at
    // 2560x1440 -> 3840x2160 -- and the two take turns calling us:
    //
    //   10:21:28.391  feature up at 3840x2160     the main viewport
    //   10:21:29.021  feature up at 1280x720      viewport 2, on the menu-to-game transition
    //   10:21:30.988  feature up at 3840x2160     back to the main one
    //   10:21:30.989  syncGPU submitToQueue failed - error -4   (VK_ERROR_DEVICE_LOST)
    //
    // Nothing resized. The size only looked like it changed because the question was being asked of
    // a different feature each time, and each answer tore down the whole resource set: a
    // vkDeviceWaitIdle, an NGX release and every image destroyed and rebuilt. The third one landed
    // 1.9 ms before the device was lost, on the game's Streamline frame-generation present thread.
    // vkDeviceWaitIdle requires host access to every VkQueue to be externally synchronised, and
    // that thread is submitting on one.
    //
    // Three guards, smallest first, all off together with DlssNrVkViewportGuard:
    //
    //  1. ONE VIEWPORT, BY SIZE. The largest output being drawn is the game; anything smaller is
    //     skipped rather than rebuilt for. Something larger is adopted at once, so a real resolution
    //     change is followed on the frame it happens, and a served size that stops arriving is handed
    //     over after kPrimaryGoneAfter evaluates, so the game dropping resolution for good is
    //     followed too.
    //  2. REBUILD GUARD. Even for the served size, a teardown within kRebuildGuardFrames evaluates of
    //     the last one is refused and the frame sits out. A genuine resolution change costs one
    //     unenhanced frame; a flap that reaches here another way cannot get into a vkDeviceWaitIdle
    //     loop.
    //
    // Both count `seen`, every evaluate that reaches the guard, NOT `frames`, which counts only the
    // ones actually composed. That distinction is load-bearing -- see `seen` below.
    // Keyed on the OUTPUT SIZE, not the feature id. The first attempt latched onto the id and it was
    // wrong: this game makes a NEW feature every time a DLSS setting changes -- handles 1000000,
    // 1000001, 1000002 ... one per change -- so the id is not a viewport's identity, it is a
    // serial number. The pass latched onto 1000000, that feature was released, and every later one
    // read as "a second viewport" and was skipped. No rebuilds, so no crash; no evaluates either, so
    // changing a DLSS 5 setting did nothing to the picture at all (reported 2026-09-24, the run after
    // the first fix shipped).
    //
    // The size is the stable thing. In that run the game drew 3840x2160 throughout while the
    // settings menu's preview came and went at 1280x720, 1129x635 and 960x540. The biggest output is
    // the game; the small ones are the preview.
    bool serving = false;
    uint32_t servingWidth = 0;
    uint32_t servingHeight = 0;
    unsigned long long servingLastSeen = 0;
    uint32_t skippedWidth = 0;
    uint32_t skippedHeight = 0;
    bool skippedLogged = false;

    // Every evaluate that reaches the guard, composed or skipped. Separate from `frames`, which only
    // counts composed ones -- gating the hand-over on that meant it stopped advancing the moment the
    // pass started skipping, so the hand-over could never fire. That was the deadlock.
    unsigned long long seen = 0;

    unsigned long long lastRebuildFrame = 0;
    bool rebuiltOnce = false;

    // Evaluates that got past the viewport guard -- the served viewport's frames. The clock for
    // everything below: a retired feature's countdown and a new one's wait before its first evaluate
    // are both measured in frames of the thing being served, not in calls from a preview beside it.
    unsigned long long served = 0;

    // What the live features were built with (issue #132).
    //
    // The model reads its tuning once, when the feature is built -- the forwarder says so beside
    // dlssnr_vk_create, and it is why the D3D12 path rebuilds on TuningMatchesFeature. This path read
    // cfg.DlssNrPreset exactly once, at the first create, and never compared again, so on No Man's Sky
    // picking Model A, B or C in the panel changed nothing until the resolution happened to move. Same
    // for style, intensity and the three strengths. These are what a change is detected against.
    struct Built
    {
        unsigned int preset[kVkMaxPasses] = {};
        unsigned int style[kVkMaxPasses] = {};
        float intensity = 0.0f;
        float localStructure = 0.0f;
        float localTone = 0.0f;
        float skinStructure = 0.0f;
        bool autoMask = false;
    } built;

    // A feature just created has only had its initialisation RECORDED into the game's command buffer.
    // Evaluating it in that same buffer, before the buffer has been submitted, is the creation-frame
    // dice roll that hung the GPU on D3D12 -- every one of those crashes died on a creation frame. D3D12
    // waits for a submission epoch; Vulkan has none here, so it waits for a served evaluate on another
    // command buffer, or kCreateSettleEvaluates of them on the same one (an engine that re-records a
    // single buffer every frame has necessarily submitted it by then).
    bool featurePending = false;
    unsigned long long featureCreatedAt = 0;
    VkCommandBuffer featureCreatedOn = VK_NULL_HANDLE;

    // Stacked model passes (Passes 2 and 3). Slot 0 is unused -- pass one is `feature` above -- so the
    // indices read the same as PassPreset(cfg, pass) on D3D12. Each layer is a feature of its own with
    // its own temporal history; reusing pass one's would tell one temporal model that several frames
    // elapsed in one game frame.
    void* passFeature[kVkMaxPasses] = {};
    bool passPending[kVkMaxPasses] = {};
    unsigned long long passCreatedAt[kVkMaxPasses] = {};
    VkCommandBuffer passCreatedOn[kVkMaxPasses] = {};
    bool passNeedsReset[kVkMaxPasses] = {};
    bool passCreateFailed[kVkMaxPasses] = {};

    // The other half of the ping-pong: pass one writes `output`, pass two reads it and writes this,
    // pass three reads this and writes `output`. Working size, built only when a second pass is asked
    // for, so a one-pass session allocates exactly what it did before.
    OwnedImage passScratch;

    // Features taken out of service, released kRetireAfter served evaluates later rather than on the
    // spot. The resize path can afford a vkDeviceWaitIdle -- a swapchain rebuild already stalls -- but a
    // settings change happens mid-flight, and vkDeviceWaitIdle needs every queue externally synchronised,
    // which a frame-generation present thread submitting on its own queue does not give it (Indiana
    // Jones lost its device 1.9 ms after one). Parking is what the D3D12 path does for the same reason.
    struct Retired
    {
        void* feature = nullptr;
        unsigned int left = 0;
    };
    std::vector<Retired> retired;

    // A settings change starts a short quiet period before the rebuild, so a slider being dragged is one
    // CreateFeature when it stops rather than one every half second while it moves -- the lesson of the
    // Before-SR run that rebuilt its model on every step and hitched each time.
    bool settling = false;
    std::chrono::steady_clock::time_point settleFrom {};

    // The model's own share of the pass, from a second timestamp pair around the evaluates -- the
    // "model" half of the cost line, which is what says whether the remainder (ours) is worth chasing.
    std::optional<double> lastModelTime;
};

// A teardown closer than this many evaluates to the last one is refused. Sized to cover a burst of
// alternating viewports without delaying a real resolution change by anything a player would see.
constexpr unsigned long long kRebuildGuardFrames = 8;

// The served feature has to go quiet for this many evaluates before another may take over. Long
// enough that ordinary alternation never re-latches, short enough that a released feature frees the
// pass within a second or so.
constexpr unsigned long long kPrimaryGoneAfter = 120;

// The grid the meter writes, and the size of one readback. 8 * 8 * sizeof(float).
constexpr uint32_t kMeterSide = 8;
constexpr VkDeviceSize kMeterBytes = kMeterSide * kMeterSide * sizeof(float);

// Four, so the slot being read is four frames behind the slot being written and the read never waits
// on the GPU. Same depth as the D3D12 meter's ring, for the same reason.
constexpr unsigned long long kMeterSlots = 4;

// Four frames of pairs. Three would do, four keeps the modulo cheap and the slot being written well
// clear of the slot being read.
constexpr uint32_t kTimingSlots = 4;

// Timestamps per slot: the whole pass (0, 1) and the model's evaluates inside it (2, 3). The second
// pair is the "model" of the cost line; the difference is the encode, resolve and resamples -- ours.
constexpr uint32_t kQueriesPerSlot = 4;

// A retired feature is released this many served evaluates after it was parked. The D3D12 path's figure
// (TickNrRetired), for the same reason: with frame generation the GPU can be several frames deep in work
// that still references it, and 32 is far past any queue depth a game runs.
constexpr unsigned int kRetireAfter = 32;

// See featurePending. Three served evaluates on one command buffer means it has been submitted and
// re-recorded at least twice, which it cannot be without the first submission having happened.
constexpr unsigned long long kCreateSettleEvaluates = 3;

// How long the model's settings have to hold still before the feature is rebuilt for them.
constexpr double kSettleMs = 300.0;

// What one feature is taken to cost per model pixel -- the D3D12 path's kFeatureBytesPerPixel, measured
// on Cyberpunk at 2560x1600 and rounded up. Used only to decide whether an extra pass fits.
constexpr uint64_t kFeatureBytesPerPixel = 192;

VkState g_vk;
std::mutex g_vkMutex;

void Fail(const char* why)
{
    if (g_vk.failed)
        return;

    g_vk.failed = true;
    g_vk.reason = why;
    LOG_ERROR("DLSS-NR Vulkan unavailable: {}", why);
}

// The same two lookups as DlssNr_Dx12.cpp's PassPreset / PassStyle, spelled out again rather than shared
// because those live in that file's anonymous namespace beside D3D12 state. Pass 2 and 3 inherit pass
// one's value unless the ini names one of their own. Clamped like D3D12: preset 0-3, style 0-2.
unsigned int VkPassPreset(const Config& cfg, unsigned int pass)
{
    if (pass == 1 && cfg.DlssNrPass2Preset.has_value())
        return std::min(cfg.DlssNrPass2Preset.value(), 3u);

    if (pass == 2 && cfg.DlssNrPass3Preset.has_value())
        return std::min(cfg.DlssNrPass3Preset.value(), 3u);

    return std::min(cfg.DlssNrPreset.value_or_default(), 3u);
}

unsigned int VkPassStyle(const Config& cfg, unsigned int pass)
{
    if (pass == 1 && cfg.DlssNrPass2Style.has_value())
        return std::min(cfg.DlssNrPass2Style.value(), 2u);

    if (pass == 2 && cfg.DlssNrPass3Style.has_value())
        return std::min(cfg.DlssNrPass3Style.value(), 2u);

    return std::min(cfg.DlssNrStyle.value_or_default(), 2u);
}

// Whether pass one -- and with it every layer, because the strengths are shared -- was built with what
// the config says now. Only what dlssnr_vk_create reads is compared: everything else is either set per
// evaluate or belongs to the encode and resolve, which read the config every frame anyway.
bool PrimaryTuningMatches(const Config& cfg)
{
    const VkState::Built& b = g_vk.built;

    return b.preset[0] == VkPassPreset(cfg, 0) && b.style[0] == VkPassStyle(cfg, 0) &&
           b.intensity == cfg.DlssNrIntensity.value_or_default() &&
           b.localStructure == cfg.DlssNrLocalStructure.value_or_default() &&
           b.localTone == cfg.DlssNrLocalTone.value_or_default() &&
           b.skinStructure == cfg.DlssNrSkinStructure.value_or_default() &&
           b.autoMask == cfg.DlssNrAutoMask.value_or_default();
}

void RecordPrimaryTuning(const Config& cfg)
{
    VkState::Built& b = g_vk.built;

    b.preset[0] = VkPassPreset(cfg, 0);
    b.style[0] = VkPassStyle(cfg, 0);
    b.intensity = cfg.DlssNrIntensity.value_or_default();
    b.localStructure = cfg.DlssNrLocalStructure.value_or_default();
    b.localTone = cfg.DlssNrLocalTone.value_or_default();
    b.skinStructure = cfg.DlssNrSkinStructure.value_or_default();
    b.autoMask = cfg.DlssNrAutoMask.value_or_default();
}

// Out of service now, released later. Never on the spot: see `retired`.
void ParkFeature(void*& feature)
{
    if (feature == nullptr)
        return;

    g_vk.retired.push_back({ feature, kRetireAfter });
    feature = nullptr;
}

// Once per served evaluate. A feature whose countdown has run out is released without a device wait:
// kRetireAfter frames on, nothing the GPU is still executing can reference it.
void TickRetired()
{
    unsigned int released = 0;

    for (size_t i = 0; i < g_vk.retired.size();)
    {
        if (--g_vk.retired[i].left > 0)
        {
            ++i;
            continue;
        }

        if (g_vk.release != nullptr)
            g_vk.release(g_vk.retired[i].feature);

        g_vk.retired.erase(g_vk.retired.begin() + (std::ptrdiff_t) i);
        ++released;
    }

    if (released > 0)
        LOG_INFO("DLSS-NR Vulkan: released {} retired feature(s) {} evaluates after they were parked", released,
                 kRetireAfter);
}

// Everything parked, now. Only after a vkDeviceWaitIdle (the resize path, a live shutdown), when the
// GPU is known to be done with all of it.
void ReleaseRetiredNow()
{
    for (const VkState::Retired& r : g_vk.retired)
        if (g_vk.release != nullptr)
            g_vk.release(r.feature);

    g_vk.retired.clear();
}

// Pass one's features, and every stacked layer's, out of service together.
void ParkAllFeatures()
{
    ParkFeature(g_vk.feature);
    g_vk.featurePending = false;

    for (unsigned int pass = 1; pass < kVkMaxPasses; ++pass)
    {
        ParkFeature(g_vk.passFeature[pass]);
        g_vk.passPending[pass] = false;
        g_vk.passNeedsReset[pass] = false;
        g_vk.passCreateFailed[pass] = false;
    }
}

// The bookkeeping for the features, once the handles themselves have been released or abandoned. Does
// not touch `feature` or `retired`: the caller decides whether those are released or abandoned.
void ForgetFeatureState()
{
    for (unsigned int pass = 1; pass < kVkMaxPasses; ++pass)
    {
        g_vk.passFeature[pass] = nullptr;
        g_vk.passPending[pass] = false;
        g_vk.passNeedsReset[pass] = false;
        g_vk.passCreateFailed[pass] = false;
    }

    g_vk.featurePending = false;
    g_vk.settling = false;
    g_vk.built = VkState::Built {};
    g_vk.lastModelTime.reset();
}

// Whether a feature created at `createdAt` on `createdOn` may be evaluated from `cmd` now. See
// featurePending.
bool ReadyAfterCreate(VkCommandBuffer createdOn, unsigned long long createdAt, VkCommandBuffer cmd)
{
    if (g_vk.served <= createdAt)
        return false;

    return cmd != createdOn || g_vk.served - createdAt >= kCreateSettleEvaluates;
}

// The process's video memory on this GPU, from DXGI. The budget is the operating system's (WDDM keeps
// one per process per adapter, whichever API allocated), so it is the same figure the D3D12 path guards
// with, and it needs nothing a Vulkan 1.0 instance lacks: VK_EXT_memory_budget would need the 1.1
// properties2 entry point, which a game on a 1.0 instance does not give us. The adapter is matched by
// vendor and device id. Unknown answers false, and the callers treat unknown as "fits" -- a driver
// without the query must not stop the pass from running at all.
bool ReadVideoMemoryVk(uint64_t& usage, uint64_t& budget)
{
    static IDXGIAdapter3* adapter = nullptr;
    static VkPhysicalDevice adapterFor = VK_NULL_HANDLE;
    static bool looked = false;

    if (g_vk.physicalDevice == VK_NULL_HANDLE)
        return false;

    if (!looked || adapterFor != g_vk.physicalDevice)
    {
        looked = true;
        adapterFor = g_vk.physicalDevice;

        if (adapter != nullptr)
        {
            adapter->Release();
            adapter = nullptr;
        }

        VkPhysicalDeviceProperties props {};
        vkGetPhysicalDeviceProperties(g_vk.physicalDevice, &props);

        IDXGIFactory1* factory = nullptr;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || factory == nullptr)
            return false;

        IDXGIAdapter1* candidate = nullptr;
        for (UINT i = 0; adapter == nullptr && factory->EnumAdapters1(i, &candidate) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 desc {};
            if (SUCCEEDED(candidate->GetDesc1(&desc)) && desc.VendorId == props.vendorID &&
                desc.DeviceId == props.deviceID)
                candidate->QueryInterface(IID_PPV_ARGS(&adapter));

            candidate->Release();
            candidate = nullptr;
        }

        factory->Release();
    }

    if (adapter == nullptr)
        return false;

    DXGI_QUERY_VIDEO_MEMORY_INFO info {};
    if (FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)) || info.Budget == 0)
        return false;

    usage = info.CurrentUsage;
    budget = info.Budget;
    return true;
}

// Whether `need` more bytes fit with the D3D12 path's standard reserve left over: 5% of the budget, at
// least 384 MB. Unknown counts as fitting.
bool FitsInVideoMemoryVk(uint64_t need, uint64_t& usage, uint64_t& budget)
{
    if (!ReadVideoMemoryVk(usage, budget))
        return true;

    const uint64_t reserve = std::max<uint64_t>(budget / 20, 384ull << 20);
    return usage + need + reserve <= budget;
}

// A memory dependency between two of the model's evaluates on the same images, with no layout change.
// Transition() records nothing when the layout already matches, and the ping-pong keeps both images in
// GENERAL throughout -- so without this, pass two could read `output` before pass one finished writing
// it, and pass three could overwrite it while pass two was still reading.
void ShaderBarrier(VkCommandBuffer cmd)
{
    VkMemoryBarrier barrier {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0,
                         nullptr, 0, nullptr);
}

// ---------------------------------------------------------------------------------------------
// Images this pass owns
// ---------------------------------------------------------------------------------------------

void DestroyImage(OwnedImage& img)
{
    if (g_vk.device == VK_NULL_HANDLE)
        return;

    if (img.view != VK_NULL_HANDLE)
        vkDestroyImageView(g_vk.device, img.view, nullptr);

    if (img.image != VK_NULL_HANDLE)
        vkDestroyImage(g_vk.device, img.image, nullptr);

    if (img.memory != VK_NULL_HANDLE)
        vkFreeMemory(g_vk.device, img.memory, nullptr);

    img = OwnedImage {};
}

uint32_t FindMemoryTypeIndex(uint32_t typeBits, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProps {};
    vkGetPhysicalDeviceMemoryProperties(g_vk.physicalDevice, &memProps);

    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
    {
        if ((typeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    }

    return UINT32_MAX;
}

// STORAGE and SAMPLED both, because every one of these is written by one dispatch and read by the
// next; TRANSFER_SRC so a capture can copy it out without a second surface.
// Build the OS_Vk resample descriptor for one of our own images. OS_Vk reads Width/Height/Format from
// this (the NR override makes it size from the images, not the current feature).
static VkImageInfo ImageInfoOf(const OwnedImage& img)
{
    VkImageInfo info {};
    info.ImageView = img.view;
    info.Image = img.image;
    info.SubresourceRange = VkImageSubresourceRange { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    info.Format = img.format;
    info.Width = img.width;
    info.Height = img.height;
    return info;
}

bool CreateImage(OwnedImage& img, uint32_t width, uint32_t height, VkFormat format, bool readWrite)
{
    DestroyImage(img);

    VkImageCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = { width, height, 1 };
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(g_vk.device, &info, nullptr, &img.image) != VK_SUCCESS)
    {
        LOG_ERROR("DLSS-NR Vulkan: could not create a {}x{} image", width, height);
        return false;
    }

    VkMemoryRequirements req {};
    vkGetImageMemoryRequirements(g_vk.device, img.image, &req);

    VkMemoryAllocateInfo alloc {};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = FindMemoryTypeIndex(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (alloc.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(g_vk.device, &alloc, nullptr, &img.memory) != VK_SUCCESS ||
        vkBindImageMemory(g_vk.device, img.image, img.memory, 0) != VK_SUCCESS)
    {
        LOG_ERROR("DLSS-NR Vulkan: could not back a {}x{} image", width, height);
        DestroyImage(img);
        return false;
    }

    VkImageViewCreateInfo view {};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = img.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    if (vkCreateImageView(g_vk.device, &view, nullptr, &img.view) != VK_SUCCESS)
    {
        LOG_ERROR("DLSS-NR Vulkan: could not view a {}x{} image", width, height);
        DestroyImage(img);
        return false;
    }

    img.width = width;
    img.height = height;
    img.format = format;
    img.layout = VK_IMAGE_LAYOUT_UNDEFINED;

    // The NGX wrapper. Filled once, because none of it changes until the image is recreated.
    img.ngx.Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;
    img.ngx.Resource.ImageViewInfo.ImageView = img.view;
    img.ngx.Resource.ImageViewInfo.Image = img.image;
    img.ngx.Resource.ImageViewInfo.SubresourceRange = view.subresourceRange;
    img.ngx.Resource.ImageViewInfo.Format = format;
    img.ngx.Resource.ImageViewInfo.Width = width;
    img.ngx.Resource.ImageViewInfo.Height = height;
    img.ngx.ReadWrite = readWrite;

    return true;
}

// The ring of host-visible buffers the meter's grid is copied into, created once and mapped for
// good. HOST_COHERENT so the read needs no invalidate; it is universally available for a buffer this
// small and the alternative is a vkInvalidateMappedMemoryRanges on a path that runs every frame.
bool CreateMeterReadback()
{
    for (unsigned long long i = 0; i < kMeterSlots; ++i)
    {
        if (g_vk.meterReadback[i] != VK_NULL_HANDLE)
            continue;

        VkBufferCreateInfo info {};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = kMeterBytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(g_vk.device, &info, nullptr, &g_vk.meterReadback[i]) != VK_SUCCESS)
        {
            LOG_WARN("DLSS-NR Vulkan: could not create the exposure readback buffer");
            return false;
        }

        VkMemoryRequirements req {};
        vkGetBufferMemoryRequirements(g_vk.device, g_vk.meterReadback[i], &req);

        VkMemoryAllocateInfo alloc {};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = FindMemoryTypeIndex(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        if (alloc.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(g_vk.device, &alloc, nullptr, &g_vk.meterReadbackMemory[i]) != VK_SUCCESS ||
            vkBindBufferMemory(g_vk.device, g_vk.meterReadback[i], g_vk.meterReadbackMemory[i], 0) != VK_SUCCESS ||
            vkMapMemory(g_vk.device, g_vk.meterReadbackMemory[i], 0, kMeterBytes, 0, &g_vk.meterMapped[i]) !=
                VK_SUCCESS)
        {
            LOG_WARN("DLSS-NR Vulkan: could not back the exposure readback buffer");
            return false;
        }
    }

    return true;
}

void DestroyMeterReadback()
{
    for (unsigned long long i = 0; i < kMeterSlots; ++i)
    {
        if (g_vk.meterReadbackMemory[i] != VK_NULL_HANDLE)
        {
            if (g_vk.meterMapped[i] != nullptr)
                vkUnmapMemory(g_vk.device, g_vk.meterReadbackMemory[i]);

            vkFreeMemory(g_vk.device, g_vk.meterReadbackMemory[i], nullptr);
        }

        if (g_vk.meterReadback[i] != VK_NULL_HANDLE)
            vkDestroyBuffer(g_vk.device, g_vk.meterReadback[i], nullptr);

        g_vk.meterMapped[i] = nullptr;
        g_vk.meterReadbackMemory[i] = VK_NULL_HANDLE;
        g_vk.meterReadback[i] = VK_NULL_HANDLE;
    }

    g_vk.meterFrames = 0;
}

// A layout transition with the access masks that go with it. Vulkan has no equivalent of D3D12's
// state promotion, so every read and every write says which layout it needs and this is how it gets
// there. Tracked per image so a no-op transition is not recorded.
void Transition(VkCommandBuffer cmd, OwnedImage& img, VkImageLayout to)
{
    if (img.image == VK_NULL_HANDLE || img.layout == to)
        return;

    VkImageMemoryBarrier barrier {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = img.layout;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = img.image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);

    img.layout = to;
}

// A resource the game owns. Its layout is the game's business, so this records the transition and
// puts it back exactly as it was rather than tracking it.
void TransitionForeign(VkCommandBuffer cmd, VkImage image, VkImageSubresourceRange range, VkImageLayout from,
                       VkImageLayout to)
{
    if (image == VK_NULL_HANDLE || from == to)
        return;

    VkImageMemoryBarrier barrier {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = range;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
}

// ---------------------------------------------------------------------------------------------
// Bring-up
// ---------------------------------------------------------------------------------------------

bool LoadForwarder()
{
    if (g_vk.forwarder != nullptr)
        return g_vk.create != nullptr;

    auto path = Util::FindFilePath(Util::DllPath().remove_filename(), "nvngx.dll_dlssnr.dll");

    if (!path.has_value())
        path = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx.dll_dlssnr.dll");

    if (!path.has_value())
    {
        Fail("nvngx.dll_dlssnr.dll was not found beside OptiScaler or the game");
        return false;
    }

    g_vk.forwarder = LoadLibraryW(path->wstring().c_str());

    if (g_vk.forwarder == nullptr)
    {
        Fail("the forwarder would not load");
        return false;
    }

    g_vk.probe = (PFN_VkProbe) GetProcAddress(g_vk.forwarder, "dlssnr_vk_probe");
    g_vk.init = (PFN_VkInit) GetProcAddress(g_vk.forwarder, "dlssnr_vk_init");
    g_vk.create = (PFN_VkCreate) GetProcAddress(g_vk.forwarder, "dlssnr_vk_create");
    g_vk.evaluate = (PFN_VkEvaluate) GetProcAddress(g_vk.forwarder, "dlssnr_vk_evaluate");
    g_vk.release = (PFN_VkRelease) GetProcAddress(g_vk.forwarder, "dlssnr_vk_release");

    if (g_vk.init == nullptr || g_vk.create == nullptr || g_vk.evaluate == nullptr)
    {
        Fail("the forwarder is missing its Vulkan entry points");
        return false;
    }

    return true;
}

// Whether a format can hold linear, open-ended light. A frame the game already tone mapped has white
// at 1 and must not be encoded a second time; an 8-bit or normalised format cannot be scene-referred
// whatever the game says. The D3D12 path asks the same question of DXGI formats.
bool FormatCanHoldLinearHdr(VkFormat format)
{
    switch (format)
    {
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32G32B32A32_SFLOAT:
    case VK_FORMAT_R16G16B16_SFLOAT:
    case VK_FORMAT_R32G32B32_SFLOAT:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
        return true;
    default:
        return false;
    }
}

// The create flags the game gave its own upscaler, which is where HDR and inverted depth are stated.
// Read from the parameter block rather than configured, because they describe the game's buffers and
// getting either wrong is silent: an encoded frame encoded twice, or depth read backwards.
unsigned int GameCreateFlags(NVSDK_NGX_Parameter* params)
{
    unsigned int flags = 0;

    if (params != nullptr)
        params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flags);

    return flags;
}

std::optional<std::filesystem::path> FindSnippet()
{
    auto snippet = Util::FindFilePath(Util::DllPath().remove_filename(), "nvngx_dlssnr.dll");

    if (!snippet.has_value())
        snippet = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssnr.dll");

    return snippet;
}

} // namespace

// ---------------------------------------------------------------------------------------------

bool IsRunningVk() { return g_vk.feature != nullptr && !g_vk.failed; }

const char* FailureReasonVk() { return g_vk.failed ? g_vk.reason : ""; }

unsigned long long FramesVk() { return g_vk.frames; }

bool ExposureOfferedVk() { return g_vk.exposureOffered; }

std::optional<double> LastGpuTimeVk() { return g_vk.timingTrust.Untrusted() ? std::nullopt : g_vk.lastGpuTime; }

void EvaluateAfterUpscaleVk(VkCommandBuffer cmdBuffer, NVSDK_NGX_Parameter* params, VkInstance instance,
                            VkPhysicalDevice physicalDevice, VkDevice device, unsigned int featureId)
{
    // Before [DlssNr] Enabled is read, as on every D3D entry point: the native Vulkan pass never polled
    // at all, so on a Vulkan game the manager's pop-out panel and Edit changed nothing whatever
    // LiveReload said. Rate-limited inside (one file-time check per 250 ms).
    PollSettingsFromDisk();

    auto& cfg = *Config::Instance();

    if (!cfg.DlssNrEnabled.value_or_default())
        return;

    if (cmdBuffer == VK_NULL_HANDLE || params == nullptr || device == VK_NULL_HANDLE ||
        physicalDevice == VK_NULL_HANDLE)
        return;

    std::lock_guard<std::mutex> lock(g_vkMutex);

    // Every call, before any guard below can return: the "has the served viewport gone quiet" test
    // counts these, and counting composed frames instead is what deadlocked the first attempt.
    g_vk.seen++;

    if (g_vk.failed)
        return;

    // The game's own resources, already wrapped: NGX hands Vulkan resources over as
    // NVSDK_NGX_Resource_VK, so only this pass's own images need building.
    NVSDK_NGX_Resource_VK* colour = nullptr;
    NVSDK_NGX_Resource_VK* depth = nullptr;
    NVSDK_NGX_Resource_VK* motion = nullptr;

    params->Get(NVSDK_NGX_Parameter_Output, (void**) &colour);
    params->Get(NVSDK_NGX_Parameter_Depth, (void**) &depth);
    params->Get(NVSDK_NGX_Parameter_MotionVectors, (void**) &motion);

    // The game's exposure, now read rather than only counted.
    //
    // What blocked this was the layout: a descriptor names the layout its image will be in when the
    // shader runs, NVIDIA's Vulkan header does not use the word "layout" once, and a barrier is no
    // safer because it needs the layout it is coming from. Three things in this tree answer it, and
    // they agree. FSR2Feature_Vk hands this same texture to FidelityFX as COMPUTE_READ, which its
    // Vulkan backend maps to SHADER_READ_ONLY_OPTIMAL, on a path that works in these games. The
    // D3D12-on-Vulkan bridge transitions the game's exposure image out of SHADER_READ_ONLY_OPTIMAL,
    // on a path that works. And the header stating nothing means there is no contract to break --
    // the convention is the contract.
    //
    // So it is bound in SHADER_READ_ONLY_OPTIMAL and no barrier is recorded: this never transitions a
    // resource it does not own. If a game turns out to leave it somewhere else the cost is a wrong
    // number, not a lost device, and the gate on the readback throws a wrong number away.
    NVSDK_NGX_Resource_VK* exposure = nullptr;
    float preExposure = 1.0f;
    const bool havePre = params->Get(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, &preExposure) == NVSDK_NGX_Result_Success;

    params->Get(NVSDK_NGX_Parameter_ExposureTexture, (void**) &exposure);

    static bool saidExposure = false;

    if (!saidExposure)
    {
        saidExposure = true;
        LOG_INFO("DLSS-NR Vulkan: exposure from the game: DLSS.Pre.Exposure {}, ExposureTexture {}",
                 havePre ? std::to_string(preExposure) : std::string("not supplied"),
                 exposure != nullptr ? "supplied" : "not supplied");
    }

    g_vk.exposureOffered = exposure != nullptr;

    if (havePre && std::isfinite(preExposure) && preExposure > 0.0f)
        g_vk.gamePreExposure = preExposure;

    // Take the grid written four frames ago. Retired by now, so this reads mapped memory rather than
    // waiting on the GPU -- which is the whole reason for the ring.
    if (g_vk.meterFrames >= kMeterSlots)
    {
        const void* mapped = g_vk.meterMapped[g_vk.meterFrames % kMeterSlots];

        if (mapped != nullptr)
        {
            float measured = 0.0f;
            std::memcpy(&measured, mapped, sizeof(float));

            // Believed only if it could be an exposure. A texel read through a layout the game did
            // not leave it in, or a slot the game stopped filling, fails here and the last good
            // value stands.
            if (std::isfinite(measured) && measured > 0.0f)
                g_vk.gameExposure = measured;
        }
    }

    // Said when it moves by more than a fiftieth, not every frame. Enough to see in a log that the
    // number is the game's and that it tracks the scene, without a line per frame.
    static float loggedExposure = -1.0f;

    if (g_vk.gameExposure > 1e-6f &&
        std::abs(loggedExposure - g_vk.gameExposure) > std::max(0.02f * g_vk.gameExposure, 1e-5f))
    {
        loggedExposure = g_vk.gameExposure;
        LOG_INFO("DLSS-NR Vulkan: the game's exposure is {}, pre-exposure {}, so white point {}", g_vk.gameExposure,
                 g_vk.gamePreExposure, g_vk.gamePreExposure / g_vk.gameExposure);
    }

    if (colour == nullptr || depth == nullptr || motion == nullptr)
    {
        static bool said = false;

        if (!said)
        {
            said = true;
            LOG_INFO("DLSS-NR Vulkan: the parameter block carried no {}",
                     colour == nullptr ? "output" : (depth == nullptr ? "depth" : "motion vectors"));
        }

        return;
    }

    const uint32_t width = colour->Resource.ImageViewInfo.Width;
    const uint32_t height = colour->Resource.ImageViewInfo.Height;
    const uint32_t guideWidth = depth->Resource.ImageViewInfo.Width;
    const uint32_t guideHeight = depth->Resource.ImageViewInfo.Height;

    if (width == 0 || height == 0)
        return;

    // One viewport, chosen by OUTPUT SIZE. See the serving block in VkState.
    if (cfg.DlssNrVkViewportGuard.value_or_default())
    {
        const unsigned long long area = (unsigned long long) width * height;
        const unsigned long long servingArea = (unsigned long long) g_vk.servingWidth * g_vk.servingHeight;

        if (!g_vk.serving || area > servingArea)
        {
            // The biggest thing being drawn is the game. Adopted immediately when something larger
            // turns up, so a real resolution change is followed on the frame it happens.
            if (g_vk.serving)
                LOG_INFO("DLSS-NR Vulkan: {}x{} (feature {}) is larger than the {}x{} being served; "
                         "following it",
                         width, height, featureId, g_vk.servingWidth, g_vk.servingHeight);
            else
                LOG_INFO("DLSS-NR Vulkan: serving the {}x{} viewport (feature {})", width, height, featureId);
            g_vk.serving = true;
            g_vk.servingWidth = width;
            g_vk.servingHeight = height;
            g_vk.servingLastSeen = g_vk.seen;
            g_vk.skippedLogged = false;
        }
        else if (width != g_vk.servingWidth || height != g_vk.servingHeight)
        {
            // Smaller. Either a second viewport drawn beside the game -- the settings-menu preview
            // is one -- or the game itself having dropped resolution and the old size never coming
            // back. Told apart by whether the served size is still arriving.
            if (g_vk.seen - g_vk.servingLastSeen > kPrimaryGoneAfter)
            {
                LOG_INFO("DLSS-NR Vulkan: {}x{} has not been drawn for {} evaluates; serving {}x{} "
                         "(feature {}) instead",
                         g_vk.servingWidth, g_vk.servingHeight, kPrimaryGoneAfter, width, height, featureId);
                g_vk.servingWidth = width;
                g_vk.servingHeight = height;
                g_vk.servingLastSeen = g_vk.seen;
                g_vk.skippedLogged = false;
            }
            else
            {
                // Once per size, not once per frame: the other viewport draws every frame it is up.
                if (!g_vk.skippedLogged || g_vk.skippedWidth != width || g_vk.skippedHeight != height)
                {
                    LOG_INFO("DLSS-NR Vulkan: {}x{} (feature {}) is a second viewport ({}x{} is being "
                             "served); skipping it rather than rebuilding for its size",
                             width, height, featureId, g_vk.servingWidth, g_vk.servingHeight);
                    g_vk.skippedWidth = width;
                    g_vk.skippedHeight = height;
                    g_vk.skippedLogged = true;
                }
                return;
            }
        }
        else
        {
            g_vk.servingLastSeen = g_vk.seen;
        }
    }

    // Past the guard: this is a frame of the viewport being served.
    g_vk.served++;

    // The model's working size. The slider is a fraction of the frame; at 1 it is the frame, and the
    // reduced path below never runs, so the default is byte-for-byte what it was.
    // Above 1 the model supersamples (up to 2x): the proxy is enlarged, the model runs above native,
    // and superDown averages the answer back. Vulkan matches the D3D12 cap.
    const float workScale = std::clamp(cfg.DlssNrWorkingScale.value_or_default(), 0.25f, 2.0f);
    const uint32_t workWidth = (uint32_t) (width * workScale + 0.5f);
    const uint32_t workHeight = (uint32_t) (height * workScale + 0.5f);
    const bool reduced = workWidth != width || workHeight != height;

    g_vk.instance = instance;
    g_vk.physicalDevice = physicalDevice;

    // A device change invalidates everything. The OLD device is presumed dead here -- the game
    // destroyed it, which already freed every resource made on it -- so abandon those handles rather
    // than call vkDestroy*/wait-idle on a dead device (that would be use-after-free). Rebuild fresh.
    if (g_vk.device != device)
    {
        ShutdownVk(false);
        g_vk.device = device;
        g_vk.instance = instance;
        g_vk.physicalDevice = physicalDevice;
    }

    if (!LoadForwarder())
        return;

    // Initialise NGX on this device, once. The snippet path is the model itself; the forwarder loads
    // it so the caller gate sees a module named nvngx.dll.
    if (!g_vk.ngxInitialised)
    {
        auto snippet = FindSnippet();

        if (!snippet.has_value())
        {
            Fail("nvngx_dlssnr.dll was not found beside OptiScaler or the game");
            return;
        }

        const int probe = g_vk.probe != nullptr ? g_vk.probe(snippet->wstring().c_str()) : 0;

        // Four bits, one per entry point. Anything short of fifteen means the model's Vulkan surface
        // is not entirely reachable and there is no point going further.
        if (probe != 15)
        {
            LOG_ERROR("DLSS-NR Vulkan: the model's Vulkan surface is incomplete (probe {})", probe);
            Fail("the model does not expose a complete Vulkan surface");
            return;
        }

        const int result = g_vk.init(snippet->wstring().c_str(), State::Instance().NVNGX_ApplicationDataPath.c_str(),
                                     (void*) instance, (void*) physicalDevice, (void*) device, 0x0000015);

        if (result != 1)
        {
            LOG_ERROR("DLSS-NR Vulkan: NVSDK_NGX_VULKAN_Init_Ext returned {}", result);
            Fail("the model would not initialise on this Vulkan device");
            return;
        }

        g_vk.ngxInitialised = true;
        LOG_INFO("DLSS-NR Vulkan: the model initialised on this device");
    }

    if (g_vk.capabilityParams == nullptr)
    {
        if (NVSDK_NGX_VULKAN_AllocateParameters(&g_vk.capabilityParams) != NVSDK_NGX_Result_Success ||
            g_vk.capabilityParams == nullptr)
        {
            Fail("a parameter block could not be allocated");
            return;
        }
    }

    if (g_vk.queryPool == VK_NULL_HANDLE)
    {
        VkPhysicalDeviceProperties props {};
        vkGetPhysicalDeviceProperties(physicalDevice, &props);

        // A period of zero means the device does not support timestamps on this queue. The pass runs
        // regardless; it simply reports no cost, which is what the D3D12 path does when its heap is
        // unavailable.
        g_vk.timestampPeriod = props.limits.timestampPeriod;

        if (g_vk.timestampPeriod > 0.0f)
        {
            VkQueryPoolCreateInfo info {};
            info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            info.queryType = VK_QUERY_TYPE_TIMESTAMP;
            info.queryCount = kTimingSlots * kQueriesPerSlot;

            if (vkCreateQueryPool(device, &info, nullptr, &g_vk.queryPool) != VK_SUCCESS)
            {
                g_vk.queryPool = VK_NULL_HANDLE;
                LOG_INFO("DLSS-NR Vulkan: no timestamp pool, the pass will not report its cost");
            }
        }
    }

    if (g_vk.pass == nullptr)
    {
        g_vk.pass = std::make_unique<DlssNr_Vk>("Neural Rendering", device, physicalDevice);

        if (!g_vk.pass->IsInit())
        {
            g_vk.pass.reset();
            Fail("the composition pass could not be created");
            return;
        }
    }

    // Retired features count down in served frames, and are released here when theirs runs out.
    TickRetired();

    // Resize. The feature is built for a size and has to be rebuilt when the frame OR the working
    // size changes -- moving the slider is a rebuild, which is why it is compared here.
    if (g_vk.width != width || g_vk.height != height || g_vk.workWidth != workWidth || g_vk.workHeight != workHeight)
    {
        // Not this soon after the last one. Two viewports taking turns is what made these teardowns
        // repeat (see servingId), and the guard above stops that at the source -- this is the
        // backstop for a flap that reaches here another way. Sitting the frame out costs one
        // unenhanced frame; tearing the device down costs the session.
        if (cfg.DlssNrVkViewportGuard.value_or_default() && g_vk.rebuiltOnce &&
            g_vk.seen - g_vk.lastRebuildFrame < kRebuildGuardFrames)
        {
            LOG_WARN("DLSS-NR Vulkan: {}x{} wanted {} evaluates after the last rebuild; skipping this "
                     "frame rather than tearing the resources down again",
                     width, height, g_vk.seen - g_vk.lastRebuildFrame);
            return;
        }

        g_vk.lastRebuildFrame = g_vk.seen;
        g_vk.rebuiltOnce = true;

        // This block releases the feature and frees the surfaces below IMMEDIATELY. A frame-size
        // change is already fenced by the game -- it recreates the swapchain around it -- but moving
        // the working-scale slider is not: the game is mid-flight and previous frames' command
        // buffers still reference the feature and images about to be destroyed. Freeing a Vulkan
        // resource that in-flight GPU work still touches is device removal (ERR_GFX_STATE, reproduced
        // on RDR2 and Enshrouded by dragging the model-resolution slider). Drain the device first.
        // Only the rare resize path reaches here, so the CPU stall is a one-off hitch, not per-frame.
        if (g_vk.device != VK_NULL_HANDLE)
            vkDeviceWaitIdle(g_vk.device);

        if (g_vk.feature != nullptr && g_vk.release != nullptr)
        {
            g_vk.release(g_vk.feature);
            g_vk.feature = nullptr;
        }

        // The device is drained, so the stacked layers, anything parked and the ping-pong image can go
        // now too: all of them were built for the old working size. The layers come back one per frame
        // through the pass build below, at the new size.
        g_vk.featurePending = false;

        for (unsigned int pass = 1; pass < kVkMaxPasses; ++pass)
        {
            if (g_vk.passFeature[pass] != nullptr && g_vk.release != nullptr)
                g_vk.release(g_vk.passFeature[pass]);

            g_vk.passFeature[pass] = nullptr;
            g_vk.passPending[pass] = false;
            g_vk.passNeedsReset[pass] = false;
            g_vk.passCreateFailed[pass] = false;
        }

        ReleaseRetiredNow();
        DestroyImage(g_vk.passScratch);

        const VkFormat working = VK_FORMAT_R16G16B16A16_SFLOAT;

        // The meter is a fixed 8x8 whatever the frame is, so it is only built the once -- but it is
        // built alongside the rest so that a failure here is caught by the same check.
        const bool meterReady =
            (g_vk.meter.Valid() || CreateImage(g_vk.meter, kMeterSide, kMeterSide, VK_FORMAT_R32_SFLOAT, true)) &&
            CreateMeterReadback();

        if (!meterReady)
            LOG_WARN("DLSS-NR Vulkan: no exposure meter; the white point stays on the slider");

        DestroyImage(g_vk.proxySmall);
        DestroyImage(g_vk.outputNative);
        DestroyImage(g_vk.editNative);
        DestroyImage(g_vk.proxyNative);

        // output is the model's target, so it is the working size. proxy and keep are full: proxy is
        // the source the downsample reads, keep is the untouched frame the resolve composites onto.
        // outputNative is the native buffer the supersample down-leg averages the answer into.
        const bool ok = CreateImage(g_vk.output, workWidth, workHeight, working, true) &&
                        CreateImage(g_vk.proxy, width, height, working, true) &&
                        CreateImage(g_vk.keep, width, height, working, true) &&
                        (!reduced || CreateImage(g_vk.proxySmall, workWidth, workHeight, working, true)) &&
                        (workScale <= 1.0f || CreateImage(g_vk.outputNative, width, height, working, true)) &&
                        (!reduced || (CreateImage(g_vk.editNative, width, height, working, true) &&
                                      CreateImage(g_vk.proxyNative, width, height, working, true)));

        if (!ok)
        {
            Fail("the pass could not allocate its own surfaces");
            return;
        }

        g_vk.width = width;
        g_vk.height = height;
        g_vk.workWidth = workWidth;
        g_vk.workHeight = workHeight;
        g_vk.reset = true;
    }

    const unsigned int configuredPasses = std::clamp(cfg.DlssNrPasses.value_or_default(), 1u, kVkMaxPasses);

    // -----------------------------------------------------------------------------------------
    // Settings the model reads only at create time: a change retires the feature and builds another
    // -----------------------------------------------------------------------------------------

    const auto now = std::chrono::steady_clock::now();

    if (g_vk.feature != nullptr && !PrimaryTuningMatches(cfg))
    {
        // Pass one's settings, or the strengths every layer shares: every feature was built with the
        // old ones, so all of them go. Parked, not released -- see `retired` -- and the rebuild waits
        // below until they are actually gone, so the old set and the new never sit in video memory
        // together (the overlap that took the Cyberpunk run over its 12 GB).
        LOG_INFO("DLSS-NR Vulkan: the model's settings changed (preset {} -> {}, style {} -> {}, intensity {:.2f} -> "
                 "{:.2f}); rebuilding the feature once they hold still for {:.0f} ms",
                 g_vk.built.preset[0], VkPassPreset(cfg, 0), g_vk.built.style[0], VkPassStyle(cfg, 0),
                 g_vk.built.intensity, cfg.DlssNrIntensity.value_or_default(), kSettleMs);

        ParkAllFeatures();
        g_vk.settling = true;
        g_vk.settleFrom = now;
    }

    for (unsigned int pass = 1; pass < kVkMaxPasses; ++pass)
    {
        if (pass >= configuredPasses)
        {
            // No longer asked for. Its history goes with it, and a failure latch is cleared so a later
            // 1 -> N is a deliberate retry rather than a remembered no.
            if (g_vk.passFeature[pass] != nullptr)
                LOG_INFO("DLSS-NR Vulkan: pass {} no longer requested; retiring its feature", pass + 1);

            ParkFeature(g_vk.passFeature[pass]);
            g_vk.passPending[pass] = false;
            g_vk.passNeedsReset[pass] = false;
            g_vk.passCreateFailed[pass] = false;
        }
        else if (g_vk.passFeature[pass] != nullptr && (g_vk.built.preset[pass] != VkPassPreset(cfg, pass) ||
                                                       g_vk.built.style[pass] != VkPassStyle(cfg, pass)))
        {
            // Only this layer's own model or style moved: only this layer is rebuilt, which is what the
            // panel's help for Pass 2/3 model promises. Pass one keeps running meanwhile.
            LOG_INFO("DLSS-NR Vulkan: pass {} settings changed (preset {} -> {}, style {} -> {}); rebuilding that "
                     "layer only",
                     pass + 1, g_vk.built.preset[pass], VkPassPreset(cfg, pass), g_vk.built.style[pass],
                     VkPassStyle(cfg, pass));

            ParkFeature(g_vk.passFeature[pass]);
            g_vk.passPending[pass] = false;
            g_vk.passNeedsReset[pass] = false;
            g_vk.passCreateFailed[pass] = false;
            g_vk.settling = true;
            g_vk.settleFrom = now;
        }
    }

    const bool settled =
        !g_vk.settling || std::chrono::duration<double, std::milli>(now - g_vk.settleFrom).count() >= kSettleMs;

    if (g_vk.feature == nullptr)
    {
        // The old features have to be gone before a new one is built -- see above. About half a second
        // of unenhanced frames after a change, the same wait the D3D12 path has.
        if (!g_vk.retired.empty() || !settled)
            return;

        g_vk.settling = false;

        g_vk.feature =
            g_vk.create((void*) cmdBuffer, g_vk.capabilityParams, workWidth, workHeight, (int) VkPassPreset(cfg, 0),
                        cfg.DlssNrIntensity.value_or_default(), (int) VkPassStyle(cfg, 0),
                        cfg.DlssNrLocalStructure.value_or_default(), cfg.DlssNrLocalTone.value_or_default(),
                        cfg.DlssNrSkinStructure.value_or_default(), cfg.DlssNrAutoMask.value_or_default() ? 1 : 0, 1);

        if (g_vk.feature == nullptr)
        {
            Fail("the model would not build a feature on this device");
            return;
        }

        RecordPrimaryTuning(cfg);
        g_vk.featurePending = true;
        g_vk.featureCreatedAt = g_vk.served;
        g_vk.featureCreatedOn = cmdBuffer;

        LOG_INFO("DLSS-NR Vulkan: feature up at {}x{} (frame {}x{}) -- preset {}, style {}, intensity {:.2f}",
                 workWidth, workHeight, width, height, g_vk.built.preset[0], g_vk.built.style[0], g_vk.built.intensity);
        g_vk.reset = true;

        // Nothing is evaluated on the frame a feature is created. See featurePending.
        return;
    }

    if (g_vk.featurePending)
    {
        if (!ReadyAfterCreate(g_vk.featureCreatedOn, g_vk.featureCreatedAt, cmdBuffer))
            return;

        g_vk.featurePending = false;
        LOG_INFO("DLSS-NR Vulkan: feature ready after its creation was submitted ({} served evaluates later)",
                 g_vk.served - g_vk.featureCreatedAt);
    }

    // -----------------------------------------------------------------------------------------
    // Stacked passes: at most one new layer per frame, and nothing evaluated on that frame
    // -----------------------------------------------------------------------------------------

    for (unsigned int pass = 1; pass < configuredPasses; ++pass)
    {
        if (g_vk.passFeature[pass] != nullptr)
            continue;

        // A layer that would not build stays off until Passes is changed; the ones below it run.
        if (g_vk.passCreateFailed[pass] || !g_vk.retired.empty() || !settled)
            break;

        g_vk.settling = false;

        if (!g_vk.passScratch.Valid() || g_vk.passScratch.width != workWidth || g_vk.passScratch.height != workHeight)
        {
            // New, so nothing in flight can reference it; no drain needed.
            if (!CreateImage(g_vk.passScratch, workWidth, workHeight, VK_FORMAT_R16G16B16A16_SFLOAT, true))
            {
                g_vk.passCreateFailed[pass] = true;
                LOG_WARN("DLSS-NR Vulkan: no image for a second model pass; running one pass");
                break;
            }
        }

        // An extra layer is optional, so it waits for room rather than risk the eviction that lost the
        // device in Cyberpunk. The feature is the cost; the scratch image above is a few MB beside it.
        {
            const uint64_t need = (uint64_t) workWidth * workHeight * kFeatureBytesPerPixel;
            uint64_t usage = 0, budget = 0;

            if (!FitsInVideoMemoryVk(need, usage, budget))
            {
                static unsigned int warnedPass = 0;

                if (warnedPass != pass + 1)
                {
                    warnedPass = pass + 1;
                    LOG_WARN("DLSS-NR Vulkan: model pass {} not built -- video memory {} of {} MB in use, and it "
                             "needs about {} MB more. Running {} pass(es) until there is room.",
                             pass + 1, usage >> 20, budget >> 20, need >> 20, pass);
                }
                break;
            }
        }

        // Local tone belongs to the frame and is applied by pass one only, as on D3D12.
        g_vk.passFeature[pass] =
            g_vk.create((void*) cmdBuffer, g_vk.capabilityParams, workWidth, workHeight, (int) VkPassPreset(cfg, pass),
                        cfg.DlssNrIntensity.value_or_default(), (int) VkPassStyle(cfg, pass),
                        cfg.DlssNrLocalStructure.value_or_default(), 0.0f, cfg.DlssNrSkinStructure.value_or_default(),
                        cfg.DlssNrAutoMask.value_or_default() ? 1 : 0, 1);

        if (g_vk.passFeature[pass] != nullptr)
        {
            g_vk.built.preset[pass] = VkPassPreset(cfg, pass);
            g_vk.built.style[pass] = VkPassStyle(cfg, pass);
            g_vk.passPending[pass] = true;
            g_vk.passCreatedAt[pass] = g_vk.served;
            g_vk.passCreatedOn[pass] = cmdBuffer;
            g_vk.passNeedsReset[pass] = true;
            LOG_INFO("DLSS-NR Vulkan: feature for pass {} built at {}x{} with preset {}, style {}; waiting for "
                     "submission",
                     pass + 1, workWidth, workHeight, g_vk.built.preset[pass], g_vk.built.style[pass]);
        }
        else
        {
            // Not Fail(): pass one is fine and keeps running. Only this layer is given up on.
            g_vk.passCreateFailed[pass] = true;
            LOG_ERROR("DLSS-NR Vulkan: feature for pass {} failed to build; using {} pass(es)", pass + 1, pass);
        }

        // A creation frame evaluates nothing, including the layers that are ready. The frame goes out
        // as the upscaler left it -- one unenhanced frame per layer built.
        return;
    }

    // The layers that are built, ready and contiguous from pass one. A gap stops the count: a layer is
    // fed the one below it, never pass one's answer twice.
    unsigned int effectivePasses = 1;

    if (g_vk.passScratch.Valid())
    {
        for (unsigned int pass = 1; pass < configuredPasses; ++pass)
        {
            if (g_vk.passFeature[pass] == nullptr)
                break;

            if (g_vk.passPending[pass])
            {
                if (!ReadyAfterCreate(g_vk.passCreatedOn[pass], g_vk.passCreatedAt[pass], cmdBuffer))
                    break;

                g_vk.passPending[pass] = false;
                LOG_INFO("DLSS-NR Vulkan: feature for pass {} ready after its creation was submitted", pass + 1);
            }

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
            LOG_INFO("DLSS-NR Vulkan model passes: configured {}, effective {}", configuredPasses, effectivePasses);
        }
    }

    // [DlssNr] PassRate, as on D3D12: the stacked layers run on that fraction of frames and pass one on
    // all of them. A credit accumulator spreads the skipped frames evenly at any rate. The layers stay
    // built either way, so a skipped frame is an evaluate not made, never a rebuild.
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

    // -----------------------------------------------------------------------------------------
    // Encode: the frame the upscaler wrote -> a display-referred proxy, plus an untouched copy
    // -----------------------------------------------------------------------------------------

    const unsigned int createFlags = GameCreateFlags(params);
    const bool gameSaysHdr = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0;
    const bool depthInverted = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;

    // The game asking the upscaler to forget its history -- a cut, a teleport, a load. Same omission
    // as the D3D12 path had: the model's history was only ever reset by things that happened to us,
    // never by anything that happened in the game.
    {
        unsigned int gameReset = 0;

        if (params->Get(NVSDK_NGX_Parameter_Reset, &gameReset) == NVSDK_NGX_Result_Success && gameReset != 0)
        {
            g_vk.reset = true;

            static unsigned long long resets = 0;
            ++resets;

            if (resets <= 3 || resets % 100 == 0)
                LOG_INFO("DLSS-NR Vulkan: the game asked for a history reset ({} so far)", resets);
        }
    }

    // Both have to agree. A game can set the HDR flag on a buffer that cannot hold open-ended light,
    // and encoding an already tone-mapped frame a second time looks washed out and banded.
    const bool linearHdr = gameSaysHdr && FormatCanHoldLinearHdr(colour->Resource.ImageViewInfo.Format);

    // The same rule as the D3D12 path, deliberately spelled the same way: the game divides its frame
    // by preExposure and multiplies by exposure, so undoing that is the divisor this pass wants, and
    // the slider becomes a trim on top rather than the answer.
    //
    // The trim is bounded here, at the point of use, rather than at the slider. Someone who found 64
    // by hand on the manual path and then switches the exposure source on keeps that 64 in their ini;
    // bounding it in the menu would leave the picture wrong for a reason the menu no longer showed.
    // Their value stays in the config untouched, so switching back to manual restores it.
    float whitePoint = cfg.DlssNrWhitePointScale.value_or_default();

    if (cfg.DlssNrWhitePointSource.value_or_default() == 1 && g_vk.gameExposure > 1e-6f)
    {
        const float trim = std::clamp(cfg.DlssNrWhitePointTrim.value_or_default(), 0.25f, 4.0f);
        whitePoint = std::clamp(g_vk.gamePreExposure / g_vk.gameExposure * trim, 0.01f, 4096.0f);
    }

    static bool saidEncoding = false;

    if (!saidEncoding)
    {
        saidEncoding = true;
        LOG_INFO("DLSS-NR Vulkan: the game's buffer is {} (flag {}, format {}), depth {}",
                 linearHdr ? "linear HDR" : "already tone-mapped", gameSaysHdr ? "set" : "clear",
                 (int) colour->Resource.ImageViewInfo.Format, depthInverted ? "inverted" : "normal");
    }

    DlssNrConstants encode {};
    encode.Mode = DlssNrMode_Encode;
    encode.Width = width;
    encode.Height = height;
    encode.WhitePoint = whitePoint;
    encode.Passthrough = linearHdr ? 0u : 1u;
    encode.ReversibleMode = cfg.DlssNrReversibleMode.value_or_default();
    encode.ApplyModel = cfg.DlssNrApplyModel.value_or_default() ? 1u : 0u;
    encode.TransferStrength = cfg.DlssNrTransferStrength.value_or_default();
    encode.ColourStrength = cfg.DlssNrColourStrength.value_or_default();
    encode.Brightness = cfg.DlssNrBrightness.value_or_default();
    encode.Contrast = cfg.DlssNrContrast.value_or_default();
    encode.MaxRatio = cfg.DlssNrMaxRatio.value_or_default();
    encode.Transfer = cfg.DlssNrTransfer.value_or_default();
    encode.DebugScale = cfg.DlssNrWhitePointScale.value_or_default();
    encode.GuideWidth = guideWidth;
    encode.GuideHeight = guideHeight;

    const VkImageSubresourceRange colourRange = colour->Resource.ImageViewInfo.SubresourceRange;

    // Open the measurement. Reset immediately before writing: a query pool slot must be reset before
    // it is written again, and doing it here rather than at the end keeps the two in one place.
    const uint32_t timingSlot = (uint32_t) (g_vk.timedFrames % kTimingSlots);

    if (g_vk.queryPool != VK_NULL_HANDLE)
    {
        vkCmdResetQueryPool(cmdBuffer, g_vk.queryPool, timingSlot * kQueriesPerSlot, kQueriesPerSlot);
        vkCmdWriteTimestamp(cmdBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_vk.queryPool, timingSlot * kQueriesPerSlot);
    }

    // The game's colour is read here and written at the end. Its layout on arrival is GENERAL, which
    // is what NGX requires of a resource it is handed, so it is left alone.
    Transition(cmdBuffer, g_vk.proxy, VK_IMAGE_LAYOUT_GENERAL);
    Transition(cmdBuffer, g_vk.keep, VK_IMAGE_LAYOUT_GENERAL);

    // Read in GENERAL, which is the layout it is actually in.
    //
    // This slot used to take the default and declare SHADER_READ_ONLY_OPTIMAL, which disagreed with
    // the comment four lines up and with the resolve below -- the resolve writes this same image as a
    // storage image, which is only legal in GENERAL, and nothing transitions it in between. It is the
    // upscaler's output, a storage image the upscaler has just written, so GENERAL is what it is.
    // Inert on the only hardware this model runs on, wrong everywhere it is read.
    if (!g_vk.pass->Dispatch(cmdBuffer, encode, width, height, colour->Resource.ImageViewInfo.ImageView, VK_NULL_HANDLE,
                             VK_NULL_HANDLE, VK_NULL_HANDLE, g_vk.proxy.view, g_vk.keep.view, VK_IMAGE_LAYOUT_GENERAL))
    {
        Fail("the encode dispatch failed");
        return;
    }

    // The model's input: the full proxy, or a downsampled copy of it when the working scale is below
    // the frame. Mirrors the D3D12 path -- the encode always writes a full proxy, and a separate
    // downsample makes the small one the model actually reads.
    OwnedImage* modelInput = &g_vk.proxy;

    if (reduced && g_vk.proxySmall.Valid())
    {
        bool built = false;

        if (workScale > 1.0f)
        {
            // Supersample: upscale the proxy to the super-native working size with the chosen filter so
            // the model sees a clean input. Rebuild both scalers when the NR downscaler changed (baked
            // at construction). proxy -> SHADER_READ_ONLY (sampled), proxySmall -> GENERAL (storage).
            const Scaler wantScaler = cfg.DlssNrScalingDownscaler.value_or_default();
            if (g_vk.nrScaler != wantScaler)
            {
                // Rebuilding frees the old scalers' pipelines/descriptors. The filter dropdown changes
                // no size, so this does NOT go through the resize block's drain -- and prior frames'
                // submitted command buffers still bind these pipelines. Freeing them under in-flight GPU
                // work is device removal (the same hazard the resize path drains for). Drain first. A
                // filter change is rare, so the one-off stall is a hitch, not a per-frame cost.
                if (g_vk.device != VK_NULL_HANDLE)
                    vkDeviceWaitIdle(g_vk.device);
                g_vk.superUp.reset();
                g_vk.superDown.reset();
                g_vk.nrScaler = wantScaler;
            }
            if (!g_vk.superUp)
                g_vk.superUp =
                    std::make_unique<OS_Vk>("DLSS-NR VK supersample up", device, physicalDevice, true, wantScaler);
            if (!g_vk.superDown)
                g_vk.superDown =
                    std::make_unique<OS_Vk>("DLSS-NR VK supersample down", device, physicalDevice, false, wantScaler);

            Transition(cmdBuffer, g_vk.proxy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            Transition(cmdBuffer, g_vk.proxySmall, VK_IMAGE_LAYOUT_GENERAL);

            VkImageInfo upin = ImageInfoOf(g_vk.proxy);
            VkImageInfo upout = ImageInfoOf(g_vk.proxySmall);

            if (g_vk.superUp && g_vk.superUp->IsInit() && g_vk.superUp->Dispatch(cmdBuffer, upin, upout))
                built = true;
            else
            {
                static bool warnedVkSuper = false;
                if (!warnedVkSuper)
                {
                    warnedVkSuper = true;
                    LOG_WARN("DLSS-NR Vulkan supersample: upscaler unavailable, falling back to box enlarge.");
                }
            }
        }

        if (!built)
        {
            DlssNrConstants down = encode;
            down.Mode = DlssNrMode_Downsample;
            down.Width = workWidth;
            down.Height = workHeight;

            Transition(cmdBuffer, g_vk.proxy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            Transition(cmdBuffer, g_vk.proxySmall, VK_IMAGE_LAYOUT_GENERAL);

            if (!g_vk.pass->Dispatch(cmdBuffer, down, workWidth, workHeight, g_vk.proxy.view, VK_NULL_HANDLE,
                                     VK_NULL_HANDLE, VK_NULL_HANDLE, g_vk.proxySmall.view, VK_NULL_HANDLE,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
            {
                Fail("the downsample dispatch failed");
                return;
            }
        }

        modelInput = &g_vk.proxySmall;
    }

    // -----------------------------------------------------------------------------------------
    // The meter: the game's 1x1 exposure -> texel (0,0) of the grid -> a buffer the CPU can read
    // -----------------------------------------------------------------------------------------

    // The motion slot carries it, because the meter has no use for motion vectors and the shader is
    // one shader with a fixed set of bindings. The source slot is left empty and gets the dummy.
    //
    // Gated on the setting that consumes the answer, which is not merely tidy. This is the only place
    // the pass binds a resource it does not own on a guess about its layout, and the guess is good
    // but it is still a guess. A user who has not asked for the exposure source never has the game's
    // image touched at all, so if some engine does leave it somewhere unexpected, the blast radius is
    // people who turned the thing on rather than everyone on Vulkan.
    if (cfg.DlssNrWhitePointSource.value_or_default() == 1 && exposure != nullptr &&
        exposure->Type == NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW &&
        exposure->Resource.ImageViewInfo.ImageView != VK_NULL_HANDLE && g_vk.meter.Valid())
    {
        const unsigned long long slot = g_vk.meterFrames % kMeterSlots;

        if (g_vk.meterReadback[slot] != VK_NULL_HANDLE)
        {
            DlssNrConstants meter = encode;
            meter.Mode = DlssNrMode_Meter;
            meter.Width = kMeterSide;
            meter.Height = kMeterSide;

            Transition(cmdBuffer, g_vk.meter, VK_IMAGE_LAYOUT_GENERAL);

            if (g_vk.pass->Dispatch(cmdBuffer, meter, kMeterSide, kMeterSide, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                    VK_NULL_HANDLE, exposure->Resource.ImageViewInfo.ImageView, g_vk.meter.view,
                                    VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
            {
                Transition(cmdBuffer, g_vk.meter, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

                VkBufferImageCopy region {};
                region.bufferOffset = 0;
                region.bufferRowLength = 0;
                region.bufferImageHeight = 0;
                region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                region.imageOffset = { 0, 0, 0 };
                region.imageExtent = { kMeterSide, kMeterSide, 1 };

                vkCmdCopyImageToBuffer(cmdBuffer, g_vk.meter.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       g_vk.meterReadback[slot], 1, &region);

                // The copy has to be visible to a host read, and only the host will read it.
                VkBufferMemoryBarrier toHost {};
                toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toHost.buffer = g_vk.meterReadback[slot];
                toHost.offset = 0;
                toHost.size = kMeterBytes;

                vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                                     nullptr, 1, &toHost, 0, nullptr);

                g_vk.meterFrames++;
            }
        }
    }

    // -----------------------------------------------------------------------------------------
    // The model
    // -----------------------------------------------------------------------------------------

    Transition(cmdBuffer, g_vk.output, VK_IMAGE_LAYOUT_GENERAL);

    if (effectivePasses > 1)
        Transition(cmdBuffer, g_vk.passScratch, VK_IMAGE_LAYOUT_GENERAL);

    // Whatever last touched these -- last frame's resolve reading `output`, or last frame's pass two
    // writing the scratch -- is finished before the model starts. A layout transition would have said
    // so, but an image that stays in GENERAL from one frame to the next gets none.
    if (effectivePasses > 1)
        ShaderBarrier(cmdBuffer);

    if (g_vk.queryPool != VK_NULL_HANDLE)
        vkCmdWriteTimestamp(cmdBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_vk.queryPool,
                            timingSlot * kQueriesPerSlot + 2);

    // The encode ran once above. Its proxy stays untouched and only the model's answers ping-pong:
    //   pass 1: proxy -> output, pass 2: output -> scratch, pass 3: scratch -> output.
    // The final answer is resolved once, against the original proxy, so the transfer is the cumulative
    // final-minus-base edit and the colour controls are not applied once per layer. D3D12's order.
    OwnedImage* passInput = modelInput;
    OwnedImage* passOutput = &g_vk.output;
    OwnedImage* finalAnswer = nullptr;
    unsigned int passesRun = 0;
    const bool chainedHistory = cfg.DlssNrChainedHistory.value_or_default();

    for (unsigned int pass = 0; pass < effectivePasses; ++pass)
    {
        void* const passFeature = pass == 0 ? g_vk.feature : g_vk.passFeature[pass];

        // Pass one follows the frame's own reset. The stacked layers follow ChainedHistory: on, they
        // reset only on the frame they were built and keep their history after; off, every frame.
        const bool passReset = g_vk.reset || (pass > 0 && (g_vk.passNeedsReset[pass] || !chainedHistory));
        const float passTone = pass == 0 ? cfg.DlssNrLocalTone.value_or_default() : 0.0f;

        // Pass N's answer is pass N+1's input: the write has to land before the read.
        if (pass > 0)
            ShaderBarrier(cmdBuffer);

        const int evaluated = g_vk.evaluate(
            (void*) cmdBuffer, passFeature, g_vk.capabilityParams, &passInput->ngx, depth, motion, &passOutput->ngx,
            workWidth, workHeight, guideWidth, guideHeight, depthInverted ? 1 : 0, passReset ? 1 : 0,
            cfg.DlssNrIntensity.value_or_default(), (int) VkPassStyle(cfg, pass),
            cfg.DlssNrLocalStructure.value_or_default(), passTone, cfg.DlssNrSkinStructure.value_or_default(),
            cfg.DlssNrAutoMask.value_or_default() ? 1 : 0, 1.0f, 1.0f);

        if (evaluated != 1)
        {
            LOG_ERROR("DLSS-NR Vulkan: evaluate returned {} on pass {}", evaluated, pass + 1);

            if (pass == 0)
            {
                g_vk.frames++;
                Fail("the model refused to evaluate");
                return;
            }

            // A stacked layer refusing is not a reason to lose pass one. That layer is retired and
            // latched off until Passes changes; this frame resolves what the layers below it made.
            ParkFeature(g_vk.passFeature[pass]);
            g_vk.passPending[pass] = false;
            g_vk.passCreateFailed[pass] = true;
            break;
        }

        if (pass > 0)
            g_vk.passNeedsReset[pass] = false;

        finalAnswer = passOutput;
        ++passesRun;

        if (pass + 1 < effectivePasses)
        {
            passInput = passOutput;
            passOutput = passOutput == &g_vk.output ? &g_vk.passScratch : &g_vk.output;
        }
    }

    if (g_vk.queryPool != VK_NULL_HANDLE)
        vkCmdWriteTimestamp(cmdBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_vk.queryPool,
                            timingSlot * kQueriesPerSlot + 3);

    // What reached the resolve, for the heartbeat's "this frame".
    effectivePasses = passesRun;

    g_vk.reset = false;
    g_vk.frames++;

    // -----------------------------------------------------------------------------------------
    // Resolve: proxy + the model's answer + the untouched copy -> the frame
    // -----------------------------------------------------------------------------------------

    DlssNrConstants resolve = encode;
    resolve.Mode = DlssNrMode_Resolve;

    // Supersampling down-leg (Vulkan). Average the Nx model answer back to native with the chosen
    // filter so the resolve composites a native answer against the native proxy 1:1 -- not the single
    // bilinear tap the Nx answer would otherwise get, which aliases the model's detail into noise. On
    // failure it falls back to the Nx pair (modelInput + output), the old behaviour.
    OwnedImage* resolveProxy = modelInput;
    OwnedImage* resolveAnswer = finalAnswer;

    if (workScale > 1.0f && g_vk.superDown && g_vk.superDown->IsInit() && g_vk.outputNative.Valid())
    {
        Transition(cmdBuffer, *finalAnswer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Transition(cmdBuffer, g_vk.outputNative, VK_IMAGE_LAYOUT_GENERAL);

        VkImageInfo dsin = ImageInfoOf(*finalAnswer);
        VkImageInfo dsout = ImageInfoOf(g_vk.outputNative);

        if (g_vk.superDown->Dispatch(cmdBuffer, dsin, dsout))
        {
            resolveProxy = &g_vk.proxy;
            resolveAnswer = &g_vk.outputNative;
        }
    }

    // Below full resolution: enlarge the answer and its proxy here, with the filter chosen in the panel,
    // rather than leaving the resolve to sample two small pictures bilinearly at full-size UVs -- which is
    // what made the filter choice do nothing at all below 100%. Both legs take the same filter: the
    // composition is handed model minus proxy, and two filters would put the difference between them
    // inside that edit. Matched residual has to be told the edit still came from a reduced raster, since
    // the sizes no longer say so; Transfer 2 carries that, as on D3D12.
    if (reduced && resolveAnswer == finalAnswer && g_vk.editNative.Valid() && g_vk.proxyNative.Valid())
    {
        const Scaler wantScaler = cfg.DlssNrScalingDownscaler.value_or_default();
        const Upsampler wantUpsampler = cfg.DlssNrScalingUpscaler.value_or_default();

        if (g_vk.editUp && (g_vk.nrUpsampler != wantUpsampler || g_vk.nrScaler != wantScaler))
        {
            // The shader IS the filter, so a change means new pipelines and freeing the old ones. Frames
            // already submitted still bind them, and freeing those under in-flight work is device removal
            // -- the hazard the supersample rebuild above drains for, and the one that hung D3D12 twice
            // before its scalers were parked. A filter change is rare; the stall is a hitch, not a cost.
            if (g_vk.device != VK_NULL_HANDLE)
                vkDeviceWaitIdle(g_vk.device);
            g_vk.editUp.reset();
            g_vk.proxyUp.reset();
        }
        g_vk.nrUpsampler = wantUpsampler;
        // Both halves of the comparison above, or it never settles: nrScaler was only ever set by the
        // supersample branch, so below 100% it stayed Scaler::Count and every frame drained the device
        // and rebuilt both enlarge pipelines. Safe to share: each branch creates its own scalers when
        // they are missing, whatever this says.
        g_vk.nrScaler = wantScaler;

        if (!g_vk.editUp)
            g_vk.editUp = std::make_unique<OS_Vk>("DLSS-NR VK enlarge answer", device, physicalDevice, true, wantScaler,
                                                  wantUpsampler);
        if (!g_vk.proxyUp)
            g_vk.proxyUp = std::make_unique<OS_Vk>("DLSS-NR VK enlarge proxy", device, physicalDevice, true, wantScaler,
                                                   wantUpsampler);

        if (g_vk.editUp && g_vk.editUp->IsInit() && g_vk.proxyUp && g_vk.proxyUp->IsInit())
        {
            Transition(cmdBuffer, *finalAnswer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            Transition(cmdBuffer, g_vk.editNative, VK_IMAGE_LAYOUT_GENERAL);
            Transition(cmdBuffer, *modelInput, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            Transition(cmdBuffer, g_vk.proxyNative, VK_IMAGE_LAYOUT_GENERAL);

            VkImageInfo editIn = ImageInfoOf(*finalAnswer);
            VkImageInfo editOut = ImageInfoOf(g_vk.editNative);
            VkImageInfo proxyIn = ImageInfoOf(*modelInput);
            VkImageInfo proxyOut = ImageInfoOf(g_vk.proxyNative);

            if (g_vk.editUp->Dispatch(cmdBuffer, editIn, editOut) &&
                g_vk.proxyUp->Dispatch(cmdBuffer, proxyIn, proxyOut))
            {
                resolveProxy = &g_vk.proxyNative;
                resolveAnswer = &g_vk.editNative;

                if (resolve.Transfer == 1)
                    resolve.Transfer = 2;
            }
        }
    }

    Transition(cmdBuffer, *resolveProxy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(cmdBuffer, *resolveAnswer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(cmdBuffer, g_vk.keep, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    if (!g_vk.pass->Dispatch(cmdBuffer, resolve, width, height, resolveProxy->view, resolveAnswer->view, g_vk.keep.view,
                             VK_NULL_HANDLE, colour->Resource.ImageViewInfo.ImageView, VK_NULL_HANDLE,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
    {
        Fail("the resolve dispatch failed");
        return;
    }

    // Close it, and read the slot from three frames ago -- retired by now, so the read does not wait.
    if (g_vk.queryPool != VK_NULL_HANDLE)
    {
        vkCmdWriteTimestamp(cmdBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_vk.queryPool,
                            timingSlot * kQueriesPerSlot + 1);
        g_vk.timedFrames++;

        if (g_vk.timedFrames > kTimingSlots)
        {
            const uint32_t readSlot = (uint32_t) (g_vk.timedFrames % kTimingSlots);
            uint64_t ticks[kQueriesPerSlot] = {};
            std::optional<double> reading;

            // Without WAIT: a slot this old is retired, and if it somehow is not, NOT_READY is the
            // right answer rather than a stall. All four were written together, so they retire together.
            if (vkGetQueryPoolResults(device, g_vk.queryPool, readSlot * kQueriesPerSlot, kQueriesPerSlot,
                                      sizeof(ticks), ticks, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
                ticks[0] != 0 && ticks[1] > ticks[0])
            {
                const double ms = (double) (ticks[1] - ticks[0]) * (double) g_vk.timestampPeriod / 1e6;
                reading = ms;

                // A pass that appears to have taken over a second did not; the queue was reset under
                // it or the pair straddled a device change.
                if (NrTimingTrust::Plausible(ms))
                    g_vk.lastGpuTime = ms;

                // The model's pair sits inside the whole pass's, so anything outside it is not a reading.
                if (ticks[2] >= ticks[0] && ticks[3] > ticks[2] && ticks[3] <= ticks[1])
                {
                    const double model = (double) (ticks[3] - ticks[2]) * (double) g_vk.timestampPeriod / 1e6;

                    if (NrTimingTrust::Plausible(model))
                        g_vk.lastModelTime = model;
                }
            }

            if (g_vk.timingTrust.Add(reading))
                LOG_WARN("DLSS-NR Vulkan: the GPU pass timer gives scattered readings on this card; the panel "
                         "shows DLSS 5 on without a cost this session");
        }

        // The split, every 600 composed frames: the D3D12 path's line, word for word, because the
        // manager's run log reads it (runlog.js nrTiming, which takes the last one after the last
        // heartbeat). What is worth reading is the remainder -- the model's cost is NVIDIA's to set,
        // everything else is ours.
        static unsigned long long lastSplitLog = 0;

        if (!g_vk.timingTrust.Untrusted() && g_vk.lastGpuTime.has_value() && g_vk.lastModelTime.has_value() &&
            g_vk.frames - lastSplitLog > 600)
        {
            lastSplitLog = g_vk.frames;
            const double total = g_vk.lastGpuTime.value();
            const double model = std::min(g_vk.lastModelTime.value(), total);
            LOG_INFO("DLSS-NR cost: {:.2f} ms total = {:.2f} ms model + {:.2f} ms ours ({:.0f}% ours)", total, model,
                     total - model, total > 0.0 ? 100.0 * (total - model) / total : 0.0);
        }
    }

    // Heartbeat, the D3D12 path's line in the D3D12 path's words (issue #132). This path wrote none, so
    // a No Man's Sky log that had run the model for ten minutes read to the manager as a pass that
    // never ran, and its pop-out showed no cost: runlog.js matches
    //   DLSS-NR heartbeat: (\d+) frames run \((\d+) model failures\), ([\d.]+) fps, GPU ([^|]+?) \|
    // and anything reworded before the "|" silently stops matching. Every 600 composed frames or two
    // seconds, whichever is first -- the D3D12 cadence, so the pop-out's readout is as live here as
    // there. Failures are always 0 on this path: pass one refusing disables it for the session (Fail)
    // and a stacked layer refusing is dropped, not counted per frame.
    {
        static unsigned long long beatLastTotal = 0;
        static auto beatLastTime = std::chrono::steady_clock::now();

        const unsigned long long total = g_vk.frames;
        const auto beatNow = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(beatNow - beatLastTime).count();

        if (total > beatLastTotal && (total - beatLastTotal >= 600 || seconds >= 2.0))
        {
            const double fps = seconds > 0.0 ? double(total - beatLastTotal) / seconds : 0.0;
            beatLastTotal = total;
            beatLastTime = beatNow;

            const std::string gpu = g_vk.timingTrust.Untrusted() ? std::string("n/a (timer unreliable)")
                                    : g_vk.lastGpuTime.has_value()
                                        ? std::format("{:.2f} ms", g_vk.lastGpuTime.value())
                                        : (g_vk.queryPool != VK_NULL_HANDLE ? std::string("not read yet")
                                                                            : std::string("n/a (no timestamps)"));

            LOG_INFO("DLSS-NR heartbeat: {} frames run ({} model failures), {:.0f} fps, GPU {} | "
                     "intensity {:.2f}, preset {}, style {}, passes {} ({} this frame), {}, native Vulkan",
                     total, 0, fps, gpu, cfg.DlssNrIntensity.value_or_default(), cfg.DlssNrPreset.value_or_default(),
                     cfg.DlssNrStyle.value_or_default(), cfg.DlssNrPasses.value_or_default(), effectivePasses,
                     reduced ? "reduced resolution" : "full resolution");
        }
    }

    static bool reported = false;

    if (!reported && g_vk.frames > 2)
    {
        reported = true;
        LOG_INFO("DLSS-NR Vulkan: running natively at {}x{}, guides {}x{}", width, height, guideWidth, guideHeight);
    }
}

void ShutdownVk(bool deviceAlive)
{
    if (!deviceAlive)
    {
        // The device these handles belong to is gone (a device change was detected). Destroying a
        // VkDevice already frees every resource created on it, so touch NOTHING on the old device --
        // no wait-idle, no vkDestroy*, no NGX release, and crucially no DlssNr_Vk destructor (it would
        // vkDestroy its pipelines on the dead device). Abandon the handles; the driver reclaimed them
        // when the device died. The one-off CPU-side leak of the pass object is the rare cost of a
        // device recreation, and far cheaper than the use-after-free it replaces. Zeroing the
        // OwnedImage/meter handles matters: the resize path gates on `.Valid()`, so a stale non-null
        // handle from the dead device would be reused on the NEW device and crash.
        g_vk.pass.release();
        g_vk.superUp.release();
        g_vk.superDown.release();
        g_vk.nrScaler = Scaler::Count;
        g_vk.feature = nullptr;
        g_vk.capabilityParams = nullptr;
        g_vk.queryPool = VK_NULL_HANDLE;
        g_vk.output = OwnedImage {};
        g_vk.proxy = OwnedImage {};
        g_vk.proxySmall = OwnedImage {};
        g_vk.outputNative = OwnedImage {};
        g_vk.keep = OwnedImage {};
        g_vk.meter = OwnedImage {};

        // The same rule for the stacked layers, the parked features and the ping-pong image -- and for
        // the two enlarge targets and their scalers, which this branch used to miss: a stale editNative
        // is `.Valid()`, so the next resize would have vkDestroyed a dead device's image on the new one.
        g_vk.passScratch = OwnedImage {};
        g_vk.editNative = OwnedImage {};
        g_vk.proxyNative = OwnedImage {};
        g_vk.editUp.release();
        g_vk.proxyUp.release();
        g_vk.nrUpsampler = Upsampler::Count;
        g_vk.retired.clear();
        ForgetFeatureState();

        for (int i = 0; i < 4; ++i)
        {
            g_vk.meterReadback[i] = VK_NULL_HANDLE;
            g_vk.meterReadbackMemory[i] = VK_NULL_HANDLE;
            g_vk.meterMapped[i] = nullptr;
        }

        g_vk.device = VK_NULL_HANDLE;
        g_vk.width = 0;
        g_vk.height = 0;
        g_vk.workWidth = 0;
        g_vk.workHeight = 0;
        g_vk.timedFrames = 0;
        g_vk.meterFrames = 0;
        g_vk.lastGpuTime.reset();
        g_vk.ngxInitialised = false;
        g_vk.reset = true;
        return;
    }

    // The device is alive (real teardown): drain before freeing so nothing the GPU is still using is
    // destroyed under it, the same rule as the resize path.
    if (g_vk.device != VK_NULL_HANDLE)
        vkDeviceWaitIdle(g_vk.device);

    if (g_vk.feature != nullptr && g_vk.release != nullptr)
        g_vk.release(g_vk.feature);

    g_vk.feature = nullptr;

    for (unsigned int pass = 1; pass < kVkMaxPasses; ++pass)
        if (g_vk.passFeature[pass] != nullptr && g_vk.release != nullptr)
            g_vk.release(g_vk.passFeature[pass]);

    ReleaseRetiredNow();
    ForgetFeatureState();
    DestroyImage(g_vk.passScratch);

    DestroyImage(g_vk.output);
    DestroyImage(g_vk.proxy);
    DestroyImage(g_vk.proxySmall);
    DestroyImage(g_vk.outputNative);
    DestroyImage(g_vk.editNative);
    DestroyImage(g_vk.proxyNative);
    DestroyImage(g_vk.keep);
    DestroyImage(g_vk.meter);
    DestroyMeterReadback();

    g_vk.pass.reset();
    g_vk.superUp.reset();
    g_vk.superDown.reset();
    g_vk.editUp.reset();
    g_vk.proxyUp.reset();
    g_vk.nrScaler = Scaler::Count;
    g_vk.nrUpsampler = Upsampler::Count;

    if (g_vk.capabilityParams != nullptr)
    {
        NVSDK_NGX_VULKAN_DestroyParameters(g_vk.capabilityParams);
        g_vk.capabilityParams = nullptr;
    }

    if (g_vk.queryPool != VK_NULL_HANDLE && g_vk.device != VK_NULL_HANDLE)
    {
        vkDestroyQueryPool(g_vk.device, g_vk.queryPool, nullptr);
        g_vk.queryPool = VK_NULL_HANDLE;
    }

    g_vk.timedFrames = 0;
    g_vk.lastGpuTime.reset();

    g_vk.device = VK_NULL_HANDLE;
    g_vk.width = 0;
    g_vk.height = 0;
    g_vk.ngxInitialised = false;
    g_vk.reset = true;
}

} // namespace DlssNr
