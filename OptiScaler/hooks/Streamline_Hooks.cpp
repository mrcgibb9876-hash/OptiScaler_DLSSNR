#include <pch.h>

#include "Streamline_Hooks.h"

#include <Util.h>
#include <Config.h>

#include <nvapi/fakenvapi.h>
#include <misc/IdentifyGpu.h>
#include <hooks/Reflex_Hooks.h>
#include <menu/menu_overlay_base.h>
#include <framegen/nvngx/Nvngx_FG.h>
#include <proxies/KernelBase_Proxy.h>
#include <imgui/ImGuiNotify.hpp>
#include <dlssnr/DlssNr_RenoDx.h>

#include <json.hpp>
#include <sl1_reflex.h>
#include <magic_enum.hpp>
#include "detours/detours.h"

#include <algorithm>
#include <atomic>
#include <d3d12.h>
#include <dxgi1_6.h>

static bool IsSL1AndDLSSGActive()
{
    return State::Instance().streamlineVersion.major == 1 && State::Instance().activeFgInput == FGInput::DLSSG &&
           (State::Instance().activeFgOutput == FGOutput::FSRFG || State::Instance().activeFgOutput == FGOutput::XeFG);
}

static bool IsSL1AndFGActive()
{
    const auto& state = State::Instance();

    return state.streamlineVersion.major == 1 && state.activeFgInput == FGInput::DLSSG;
}

static void PatchSL1PluginJson(nlohmann::json& configJson)
{
    if (!IsSL1AndFGActive())
        return;

    LOG_DEBUG("Patching SL1 plugin JSON for external FG management");

    if (configJson.contains("/hooks"_json_pointer))
        configJson["hooks"].clear();

    if (configJson.contains("/exclusive_hooks"_json_pointer))
        configJson["exclusive_hooks"].clear();

    if (configJson.contains("/external/feature/tags"_json_pointer))
        configJson["external"]["feature"]["tags"].clear();

    if (configJson.contains("/vsync/supported"_json_pointer))
        configJson["vsync"]["supported"] = true;

    if (configJson.contains("/external/hws/required"_json_pointer))
        configJson["external"]["hws"]["required"] = false;
}

char* StreamlineHooks::trimStreamlineLog(const char* msg)
{
    char* result = (char*) malloc(strlen(msg) + 1);
    if (!result)
        return nullptr;

    strcpy(result, msg);

    size_t length = strlen(result);
    if (length > 0 && result[length - 1] == '\n')
    {
        result[length - 1] = '\0';
    }

    return result;
}

void StreamlineHooks::streamlineLogCallback(sl::LogType type, const char* msg)
{
    if (msg == nullptr)
        return;

    char* trimmed_msg = trimStreamlineLog(msg);
    if (trimmed_msg != nullptr)
    {
        switch (type)
        {
        case sl::LogType::eWarn:
            LOG_WARN("{}", trimmed_msg);
            break;
        case sl::LogType::eInfo:
            LOG_INFO("{}", trimmed_msg);
            break;
        case sl::LogType::eError:
            LOG_ERROR("{}", trimmed_msg);
            break;
        case sl::LogType::eCount:
            LOG_ERROR("{}", trimmed_msg);
            break;
        }

        free(trimmed_msg);
    }

    if (o_logCallback != nullptr)
        o_logCallback(type, msg);
}

sl::Result StreamlineHooks::hkslInit(const sl::Preferences& pref, uint64_t sdkVersion)
{
    LOG_FUNC();

    sl::Preferences localPref = pref;

    if (localPref.logMessageCallback != &streamlineLogCallback)
        o_logCallback = localPref.logMessageCallback;
    localPref.logLevel = sl::LogLevel::eCount;
    localPref.logMessageCallback = &streamlineLogCallback;

    // renderAPI is optional so need to be careful, should only matter for Vulkan
    renderApi = localPref.renderAPI;

    State::Instance().slFGInputs.reportEngineType(localPref.engine);

    // Treat engine type set in Streamline as ground truth
    if (localPref.engine == sl::EngineType::eUnreal)
        State::Instance().gameQuirks |= GameQuirk::ForceUnrealEngine;

    std::filesystem::path localSlPath(Config::Instance()->MainDllPath.value());
    localSlPath = localSlPath / L"streamline"; // Hardcoded streamline folder

    auto localSlPathStr = localSlPath.wstring();

    std::vector<const wchar_t*> storage;

    // Replace the SL files to allow for MFG
    if (State::Instance().activeFgInput == FGInput::NvngxFG && std::filesystem::exists(localSlPath / L"sl.common.dll"))
    {
        storage.assign(localPref.pathsToPlugins, localPref.pathsToPlugins + localPref.numPathsToPlugins);

        std::filesystem::path pluginsDir;

        // Find the first path that contains sl.common.dll
        // If storage is empty, look in the exe folder. pathsToPlugins is an optional field
        if (storage.empty())
        {
            std::filesystem::path exeFolder = Util::ExePath().parent_path();
            if (std::filesystem::exists(exeFolder / L"sl.common.dll"))
            {
                pluginsDir = exeFolder;
            }
        }
        else
        {
            for (const wchar_t* pathStr : storage)
            {
                if (!pathStr)
                    continue;

                std::filesystem::path p = pathStr;
                if (std::filesystem::exists(p / L"sl.common.dll"))
                {
                    pluginsDir = p;
                    break;
                }
            }
        }

        std::vector<std::string> missingDlls;
        bool hasNewerPlugin = false;

        // If we found the plugins folder, scan its contents
        if (!pluginsDir.empty() && std::filesystem::exists(pluginsDir))
        {
            for (const auto& entry : std::filesystem::directory_iterator(pluginsDir))
            {
                if (!entry.is_regular_file())
                    continue;

                std::wstring filename = entry.path().filename().wstring();

                std::wstring lowerName = filename;
                to_lower_in_place(lowerName);

                // Skip interposer
                if (lowerName == L"sl.interposer.dll")
                    continue;

                const bool isSlDll = lowerName.starts_with(L"sl.") && lowerName.ends_with(L".dll");
                const bool isNvLowLatency = lowerName == L"nvlowlatencyvk.dll";

                if (isSlDll || isNvLowLatency)
                {
                    std::filesystem::path localDllPath = localSlPath / filename;

                    // Check if localSlPath also has this DLL
                    if (!std::filesystem::exists(localDllPath))
                    {
                        missingDlls.push_back(entry.path().filename().string());
                    }
                    else
                    {
                        // Compare versions
                        version_t pluginVer, pluginProdVer;
                        version_t localVer, localProdVer;

                        bool gotPluginVer = Util::GetFileVersion(entry.path().wstring(), &pluginVer, &pluginProdVer);
                        bool gotLocalVer = Util::GetFileVersion(localDllPath.wstring(), &localVer, &localProdVer);

                        if (gotPluginVer && gotLocalVer)
                        {
                            if (localVer > pluginVer)
                            {
                                hasNewerPlugin = true;
                            }
                        }
                    }
                }
            }
        }

        // Insert local path only if a newer plugin was found
        if (hasNewerPlugin)
        {
            LOG_DEBUG("Making the game use local streamline files");

            storage.insert(storage.begin(), localSlPathStr.c_str());
            localPref.pathsToPlugins = storage.data();
            localPref.numPathsToPlugins = (uint32_t) storage.size();

            if (!missingDlls.empty())
            {
                std::string toastMsg = "You are missing the following dlls from the streamline folder:\n";
                for (const auto& missingDll : missingDlls)
                {
                    toastMsg += "- " + missingDll + "\n";
                }

                ImGui::InsertNotification({ ImGuiToastType::Warning, 20000, toastMsg.c_str() });
            }
        }
    }

    if (State::Instance().activeFgInput == FGInput::DLSSG || State::Instance().activeFgOutput == FGOutput::DLSSG)
    {
        std::vector<sl::Feature> localFeaturesToLoad(pref.featuresToLoad, pref.featuresToLoad + pref.numFeaturesToLoad);
        std::erase(localFeaturesToLoad, sl::kFeatureDLSS_G);

        localPref.featuresToLoad = localFeaturesToLoad.data();
        localPref.numFeaturesToLoad = localFeaturesToLoad.size();

        // return so that localFeaturesToLoad is valid
        return o_slInit(localPref, sdkVersion);
    }

    // bool hookSetTag =
    //     (State::Instance().activeFgInput == FGInput::NvngxFG || State::Instance().activeFgInput == FGInput::DLSSG);

    // if (hookSetTag)
    //     localPref->flags &= ~(sl::PreferenceFlags::eAllowOTA | sl::PreferenceFlags::eLoadDownloadedPlugins);

    // To prevent mixed up OTA situations
    // if (State::Instance().activeFgOutput == FGOutput::DLSSG)
    //{
    //    localPref.flags &= ~sl::PreferenceFlags::eAllowOTA;
    //    localPref.flags &= ~sl::PreferenceFlags::eLoadDownloadedPlugins;
    //}

    return o_slInit(localPref, sdkVersion);
}

sl::Result StreamlineHooks::hkslIsFeatureSupported(sl::Feature feature, const sl::AdapterInfo& adapterInfo)
{
    if (feature == sl::kFeatureDLSS_G)
        return sl::Result::eOk;

    return o_slIsFeatureSupported(feature, adapterInfo);
}

sl::Result StreamlineHooks::hkslIsFeatureLoaded(sl::Feature feature, bool& loaded)
{
    if (feature == sl::kFeatureDLSS_G)
    {
        loaded = true;
        return sl::Result::eOk;
    }

    return o_slIsFeatureLoaded(feature, loaded);
}

sl::Result StreamlineHooks::hkslGetFeatureRequirements(sl::Feature feature, sl::FeatureRequirements& requirements)
{
    if (feature == sl::kFeatureDLSS_G)
        return sl::Result::eOk;

    return o_slGetFeatureRequirements(feature, requirements);
}

sl::Result StreamlineHooks::hkslGetFeatureVersion(sl::Feature feature, sl::FeatureVersion& version)
{
    if (feature == sl::kFeatureDLSS_G)
    {
        version.versionSL = { State::Instance().streamlineVersion.major, State::Instance().streamlineVersion.minor,
                              State::Instance().streamlineVersion.patch };
        version.versionNGX = { 4, 2, 0 };

        return sl::Result::eOk;
    }

    return o_slGetFeatureVersion(feature, version);
}

static sl::Result dummy_slDLSSGGetState(const sl::ViewportHandle& viewport, sl::DLSSGState& state,
                                        const sl::DLSSGOptions* options)
{
    state.numFramesActuallyPresented = 1; // TODO: can do better
    state.numFramesToGenerateMax = 1;
    state.bIsVsyncSupportAvailable = sl::Boolean::eTrue;
    state.estimatedVRAMUsageInBytes = 300 * 1024 * 1024;

    return sl::Result::eOk;
}

static sl::Result dummy_slDLSSGSetOptions(const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options)
{
    return sl::Result::eOk;
}

sl::Result StreamlineHooks::hkslGetFeatureFunction(sl::Feature feature, const char* functionName, void*& function)
{
    if (feature == sl::kFeatureDLSS_G)
    {
        if (strcmp(functionName, "slDLSSGSetOptions") == 0)
        {
            function = &dummy_slDLSSGSetOptions;

            return sl::Result::eOk;
        }

        if (strcmp(functionName, "slDLSSGGetState") == 0)
        {
            function = &dummy_slDLSSGGetState;

            return sl::Result::eOk;
        }
    }

    return o_slGetFeatureFunction(feature, functionName, function);
}

sl::Result StreamlineHooks::hkslSetTag(const sl::ViewportHandle& viewport, const sl::ResourceTag* tags,
                                       uint32_t numTags, sl::CommandBuffer* cmdBuffer)
{
    if (renderApi == sl::RenderAPI::eD3D11 || renderApi == sl::RenderAPI::eVulkan)
    {
        LOG_ERROR("hkslSetTag only supports DX12");
        return o_slSetTag(viewport, tags, numTags, cmdBuffer);
    }

    if (renderApi == sl::RenderAPI::eCount)
        LOG_WARN("Incomplete Streamline hooks");

    if (tags == nullptr)
    {
        LOG_WARN("Game trying to remove a tag");
        return o_slSetTag(viewport, tags, numTags, cmdBuffer);
    }

    if (State::Instance().activeFgInput == FGInput::DLSSG &&
        State::Instance().gameQuirks[GameQuirk::IgnoreTagsWithoutHudlessForFG])
    {
        bool hasDepth = false;
        bool hasMVs = false;
        bool hasHudless = false;

        for (uint32_t i = 0; i < numTags; i++)
        {
            if (tags[i].resource == nullptr || tags[i].resource->native == nullptr)
                continue;

            if (tags[i].type == sl::kBufferTypeDepth)
                hasDepth = true;

            if (tags[i].type == sl::kBufferTypeMotionVectors)
                hasMVs = true;

            if (tags[i].type == sl::kBufferTypeHUDLessColor)
                hasHudless = true;
        }

        // Try to skip a DLSS call
        if (hasDepth && hasMVs && !hasHudless)
        {
            LOG_DEBUG("Skipping the FG tagging of potential DLSS resources");
            return o_slSetTag(viewport, tags, numTags, cmdBuffer);
        }
    }

    for (uint32_t i = 0; i < numTags; i++)
    {
        const auto typeEnum = (BufferType) tags[i].type;

        if (tags[i].resource == nullptr || tags[i].resource->native == nullptr)
        {
            LOG_TRACE("Resource of type: {} is null, continuing", magic_enum::enum_name(typeEnum));
            continue;
        }

        // Cyberpunk hudless state fix for RDNA 2
        if (State::Instance().gameQuirks & GameQuirk::CyberpunkHudlessState &&
            tags[i].resource->state ==
                (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) &&
            tags[i].type == sl::kBufferTypeHUDLessColor)
        {
            tags[i].resource->state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            LOG_TRACE("Changing hudless resource state");
        }

        if (State::Instance().activeFgInput == FGInput::DLSSG &&
            (tags[i].type == sl::kBufferTypeHUDLessColor || tags[i].type == sl::kBufferTypeDepth ||
             tags[i].type == sl::kBufferTypeHiResDepth || tags[i].type == sl::kBufferTypeLinearDepth ||
             tags[i].type == sl::kBufferTypeMotionVectors || tags[i].type == sl::kBufferTypeUIColorAndAlpha ||
             tags[i].type == sl::kBufferTypeBidirectionalDistortionField))
        {
            State::Instance().slFGInputs.reportResource(tags[i], (ID3D12GraphicsCommandList*) cmdBuffer, 0);
        }
        else if (State::Instance().activeFgInput == FGInput::NvngxFG)
        {
            LOG_TRACE("Tagging resource of type: {}", magic_enum::enum_name(typeEnum));
        }
    }

    auto result = o_slSetTag(viewport, tags, numTags, cmdBuffer);
    return result;
}

sl::Result StreamlineHooks::hkslSetTagForFrame(const sl::FrameToken& frame, const sl::ViewportHandle& viewport,
                                               const sl::ResourceTag* resources, uint32_t numResources,
                                               sl::CommandBuffer* cmdBuffer)
{
    if (renderApi == sl::RenderAPI::eD3D11 || renderApi == sl::RenderAPI::eVulkan)
    {
        LOG_ERROR("hkslSetTagForFrame only supports DX12");
        return o_slSetTagForFrame(frame, viewport, resources, numResources, cmdBuffer);
    }

    if (renderApi == sl::RenderAPI::eCount)
        LOG_WARN("Incomplete Streamline hooks");

    if (resources == nullptr)
    {
        LOG_WARN("Game trying to remove a tag");
        return o_slSetTagForFrame(frame, viewport, resources, numResources, cmdBuffer);
    }

    LOG_DEBUG("frameIndex: {}", static_cast<uint32_t>(frame));

    if (State::Instance().activeFgInput == FGInput::DLSSG &&
        State::Instance().gameQuirks[GameQuirk::IgnoreTagsWithoutHudlessForFG])
    {
        bool hasDepth = false;
        bool hasMVs = false;
        bool hasHudless = false;

        for (uint32_t i = 0; i < numResources; i++)
        {
            if (resources[i].resource == nullptr || resources[i].resource->native == nullptr)
                continue;

            if (resources[i].type == sl::kBufferTypeDepth)
                hasDepth = true;

            if (resources[i].type == sl::kBufferTypeMotionVectors)
                hasMVs = true;

            if (resources[i].type == sl::kBufferTypeHUDLessColor)
                hasHudless = true;
        }

        // Try to skip a DLSS call
        if (hasDepth && hasMVs && !hasHudless)
        {
            LOG_DEBUG("Skipping the FG tagging of potential DLSS resources");
            return o_slSetTagForFrame(frame, viewport, resources, numResources, cmdBuffer);
        }
    }

    for (uint32_t i = 0; i < numResources; i++)
    {
        const auto typeEnum = (BufferType) resources[i].type;

        if (resources[i].resource == nullptr || resources[i].resource->native == nullptr)
        {
            LOG_TRACE("Resource of type: {} is null, continuing", magic_enum::enum_name(typeEnum));
            continue;
        }

        if (State::Instance().activeFgInput == FGInput::DLSSG &&
            (resources[i].type == sl::kBufferTypeHUDLessColor || resources[i].type == sl::kBufferTypeDepth ||
             resources[i].type == sl::kBufferTypeHiResDepth || resources[i].type == sl::kBufferTypeLinearDepth ||
             resources[i].type == sl::kBufferTypeMotionVectors || resources[i].type == sl::kBufferTypeUIColorAndAlpha ||
             resources[i].type == sl::kBufferTypeBidirectionalDistortionField))
        {
            State::Instance().slFGInputs.reportResource(resources[i], (ID3D12GraphicsCommandList*) cmdBuffer,
                                                        (uint32_t) frame);
        }
        else if (State::Instance().activeFgInput == FGInput::NvngxFG)
        {
            LOG_TRACE("Tagging resource of type: {}", magic_enum::enum_name(typeEnum));
        }
    }

    auto result = o_slSetTagForFrame(frame, viewport, resources, numResources, cmdBuffer);
    return result;
}

sl::Result StreamlineHooks::hkslEvaluateFeature(sl::Feature feature, const sl::FrameToken& frame,
                                                const sl::BaseStructure** inputs, uint32_t numInputs,
                                                sl::CommandBuffer* cmdBuffer)
{
    LOG_DEBUG("frameIndex: {}", static_cast<uint32_t>(frame));

    if (State::Instance().activeFgInput == FGInput::DLSSG && numInputs > 0 && inputs != nullptr)
    {
        for (uint32_t i = 0; i < numInputs; i++)
        {
            if (inputs[i] == nullptr)
                continue;

            if (inputs[i]->structType == sl::ResourceTag::s_structType)
            {
                auto tag = (const sl::ResourceTag*) inputs[i];

                if (tag->type == sl::kBufferTypeHUDLessColor || tag->type == sl::kBufferTypeDepth ||
                    tag->type == sl::kBufferTypeHiResDepth || tag->type == sl::kBufferTypeLinearDepth ||
                    tag->type == sl::kBufferTypeMotionVectors || tag->type == sl::kBufferTypeUIColorAndAlpha ||
                    tag->type == sl::kBufferTypeBidirectionalDistortionField)
                {
                    State::Instance().slFGInputs.reportResource(*tag, (ID3D12GraphicsCommandList*) cmdBuffer,
                                                                (uint32_t) frame);
                }
            }
        }
    }

    auto result = o_slEvaluateFeature(feature, frame, inputs, numInputs, cmdBuffer);
    return result;
}

sl::Result StreamlineHooks::hkslAllocateResources(sl::CommandBuffer* cmdBuffer, sl::Feature feature,
                                                  const sl::ViewportHandle& viewport)
{
    LOG_FUNC();
    auto result = o_slAllocateResources(cmdBuffer, feature, viewport);
    return result;
}

sl::Result StreamlineHooks::hkslGetNativeInterface(void* proxyInterface, void** baseInterface)
{
    LOG_FUNC();
    auto result = o_slGetNativeInterface(proxyInterface, baseInterface);
    return result;
}

// [DlssNr] RenoDX beside the game's own DLSS Frame Generation -- ReShade has to sit ABOVE Streamline.
//
// What goes wrong without this (The Blood of Dawnwalker, UE5 + Streamline 2.7.30, 2026-09-25): the game
// upgrades the DXGI factory it got -- ReShade's proxy -- to a Streamline proxy, so SL wraps ReShade:
//
//   game -> SL swap chain proxy -> OptiScaler wrapper -> ReShade swap chain -> native swap chain
//
// With DLSS-G loaded, SL's GetBuffer hands the game its own "fake" back buffers (SL log: cloneFakeBuffers,
// nv.sl.dlss_g.tex2d.fake-swapchain-buffer) and at Present writes the real frame and the generated ones
// into the native back buffers itself. ReShade only knows the native ones, so RenoDX clones THOSE, the
// game never draws into the clone, and at each present RenoDX's swap chain proxy pass copies the unwritten
// clone over the frame SL just wrote. Black screen; the OptiScaler overlay, drawn through ReShade's
// device into the same clone and never cleared, leaves cursor trails. FG off is fine because the game
// then renders into buffers ReShade can see.
//
// The fix is the layering RenoDX's own "dlssfix" add-on builds, done here because this engine already
// hooks Streamline and a second hooker crashed: ReShade's factory proxy keeps the game's pointer, and
// only what it forwards to (its _orig) is replaced by an SL proxy, so
//
//   game -> OptiScaler wrapper -> ReShade swap chain -> SL swap chain proxy -> native swap chain
//
// with a ReShadeQueueFactory on each side of SL so that SL still receives the game's ReShade queue proxy
// and the native factory the native queue (the first version handed SL the native queue and crashed on
// DLSS-G's first Present).
//
// ReShade then tracks SL's fake buffers as the back buffers, the game renders into RenoDX's clone of
// them, and ReShade's present event -- RenoDX's proxy pass, ReLimiter, ReShade's own effects -- runs on
// the game's Present, before DLSS-G takes the buffer. Only when all of these hold: ReShade's factory
// proxy is what the game upgrades, its layout checks out, a RenoDX add-on is loaded, and OptiScaler is
// not running frame generation of its own on this game. Everything else is passed to SL untouched.
namespace
{
// ReShade's DXGIFactory proxy class (source/dxgi/dxgi_factory.hpp) and DXGISwapChain proxy class
// (source/dxgi/dxgi_swapchain.hpp); QueryInterface for either returns the proxy itself.
constexpr GUID kReShadeDXGIFactory = { 0x019778d4, 0xa03a, 0x7af4, { 0xb8, 0x89, 0xe9, 0x23, 0x62, 0xd2, 0x02, 0x38 } };
constexpr GUID kReShadeDXGISwapChain = {
    0x1f445f9f, 0x9887, 0x4c4c, { 0x90, 0x55, 0x4e, 0x3b, 0xad, 0xaf, 0xcc, 0xa8 }
};
// ReShade's IID_UnwrappedObject (source/com_utils.hpp): a proxy answers it with the object it wraps.
constexpr GUID kReShadeUnwrappedObject = {
    0x7f2c9a11, 0x3b4e, 0x4d6a, { 0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42 }
};

// ReShade's D3D12CommandQueue proxy class (source/d3d12/d3d12_command_queue.hpp).
constexpr GUID kReShadeD3D12CommandQueue = {
    0x2c576d2a, 0x0c1c, 0x4d1d, { 0xad, 0x7c, 0xbc, 0x4f, 0xae, 0xc1, 0x5a, 0xbc }
};

std::mutex s_reshadeAboveSlMutex;
std::vector<IUnknown*> s_reshadeAboveSlFactories; // ReShade factory proxies already re-layered
std::atomic<bool> s_reshadeAboveSl { false };

// The device the game passed to the swap chain creation call now running on this thread, see ScopedGameDevice.
thread_local IUnknown* t_gameDevice = nullptr;

// Once only, for a message that would otherwise repeat on every swap chain.
void LogOnce(std::atomic<bool>& said, const char* message)
{
    if (!said.exchange(true))
        LOG_WARN("RenoDX/DLSS-G: {}", message);
}

// Keeps Streamline's view of the command queue what it was before the re-layering.
//
// ReShade unwraps the game's queue to the native one before it calls the factory it wraps. When that was
// the native factory, below Streamline, it was right. Re-layered, the next thing down is Streamline, and
// it was handed the native queue while its own work is recorded on ReShade's device (the game gives SL its
// device, which is ReShade's proxy): ReShade's command lists submitted on a native queue. The first test
// crashed in the driver on DLSS-G's first Present (nvwgf2umx <- D3D12Core!CGraphicsCommandList::Present <-
// dxgi Present <- sl.dlss_g, SL minidump 2026-09-25). So two of these are put around Streamline, to give it
// exactly the view it had when it sat above ReShade:
//
//   ReShade -> [Rewrap] -> Streamline proxy -> [Unwrap] -> native factory
//
// Rewrap gets the native queue from ReShade and passes on the game's ReShade queue proxy instead, the one
// the swap chain creation hook recorded (ScopedGameDevice); Unwrap gets that proxy back from Streamline and
// gives the native factory the native queue, which is what ReShade did for it before. Anything else is
// forwarded unchanged. If Rewrap cannot match the queue it creates the swap chain on the native factory
// directly -- no frame generation for that swap chain, but never Streamline on a queue it cannot use.
class ReShadeQueueFactory final : public IDXGIFactory7
{
  public:
    enum class Mode
    {
        Rewrap,
        Unwrap
    };

    // Takes a reference of its own on both; bypass is only used by Rewrap.
    ReShadeQueueFactory(Mode mode, IDXGIFactory7* inner, IDXGIFactory7* bypass)
        : _mode(mode), _inner(inner), _bypass(bypass)
    {
        _inner->AddRef();
        if (_bypass != nullptr)
            _bypass->AddRef();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
    {
        if (ppvObject == nullptr)
            return E_POINTER;

        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIFactory) ||
            riid == __uuidof(IDXGIFactory1) || riid == __uuidof(IDXGIFactory2) || riid == __uuidof(IDXGIFactory3) ||
            riid == __uuidof(IDXGIFactory4) || riid == __uuidof(IDXGIFactory5) || riid == __uuidof(IDXGIFactory6) ||
            riid == __uuidof(IDXGIFactory7))
        {
            AddRef();
            *ppvObject = static_cast<IDXGIFactory7*>(this);
            return S_OK;
        }

        // Streamline's own "is this a proxy" query and ReShade's unwrap query are answered by what is below.
        return _inner->QueryInterface(riid, ppvObject);
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&_ref); }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG ref = InterlockedDecrement(&_ref);
        if (ref == 0)
        {
            _inner->Release();
            if (_bypass != nullptr)
                _bypass->Release();
            delete this;
        }
        return ref;
    }

    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override
    {
        return _inner->SetPrivateData(Name, DataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override
    {
        return _inner->SetPrivateDataInterface(Name, pUnknown);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override
    {
        return _inner->GetPrivateData(Name, pDataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override
    {
        return _inner->GetParent(riid, ppParent);
    }

    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT Adapter, IDXGIAdapter** ppAdapter) override
    {
        return _inner->EnumAdapters(Adapter, ppAdapter);
    }
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND WindowHandle, UINT Flags) override
    {
        return _inner->MakeWindowAssociation(WindowHandle, Flags);
    }
    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND* pWindowHandle) override
    {
        return _inner->GetWindowAssociation(pWindowHandle);
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc,
                                              IDXGISwapChain** ppSwapChain) override
    {
        IUnknown* device = pDevice;
        IDXGIFactory7* target = Translate(&device);
        const HRESULT hr = target->CreateSwapChain(device, pDesc, ppSwapChain);
        Done(device, pDevice);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE Module, IDXGIAdapter** ppAdapter) override
    {
        return _inner->CreateSoftwareAdapter(Module, ppAdapter);
    }

    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT Adapter, IDXGIAdapter1** ppAdapter) override
    {
        return _inner->EnumAdapters1(Adapter, ppAdapter);
    }
    BOOL STDMETHODCALLTYPE IsCurrent() override { return _inner->IsCurrent(); }

    BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override { return _inner->IsWindowedStereoEnabled(); }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                                                     IDXGIOutput* pRestrictToOutput,
                                                     IDXGISwapChain1** ppSwapChain) override
    {
        IUnknown* device = pDevice;
        IDXGIFactory7* target = Translate(&device);
        const HRESULT hr =
            target->CreateSwapChainForHwnd(device, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain);
        Done(device, pDevice);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown* pDevice, IUnknown* pWindow,
                                                           const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                           IDXGIOutput* pRestrictToOutput,
                                                           IDXGISwapChain1** ppSwapChain) override
    {
        IUnknown* device = pDevice;
        IDXGIFactory7* target = Translate(&device);
        const HRESULT hr = target->CreateSwapChainForCoreWindow(device, pWindow, pDesc, pRestrictToOutput, ppSwapChain);
        Done(device, pDevice);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE hResource, LUID* pLuid) override
    {
        return _inner->GetSharedResourceAdapterLuid(hResource, pLuid);
    }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND WindowHandle, UINT wMsg, DWORD* pdwCookie) override
    {
        return _inner->RegisterStereoStatusWindow(WindowHandle, wMsg, pdwCookie);
    }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE hEvent, DWORD* pdwCookie) override
    {
        return _inner->RegisterStereoStatusEvent(hEvent, pdwCookie);
    }
    void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD dwCookie) override { _inner->UnregisterStereoStatus(dwCookie); }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND WindowHandle, UINT wMsg, DWORD* pdwCookie) override
    {
        return _inner->RegisterOcclusionStatusWindow(WindowHandle, wMsg, pdwCookie);
    }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE hEvent, DWORD* pdwCookie) override
    {
        return _inner->RegisterOcclusionStatusEvent(hEvent, pdwCookie);
    }
    void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD dwCookie) override
    {
        _inner->UnregisterOcclusionStatus(dwCookie);
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown* pDevice, const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                            IDXGIOutput* pRestrictToOutput,
                                                            IDXGISwapChain1** ppSwapChain) override
    {
        IUnknown* device = pDevice;
        IDXGIFactory7* target = Translate(&device);
        const HRESULT hr = target->CreateSwapChainForComposition(device, pDesc, pRestrictToOutput, ppSwapChain);
        Done(device, pDevice);
        return hr;
    }

    UINT STDMETHODCALLTYPE GetCreationFlags() override { return _inner->GetCreationFlags(); }

    HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID AdapterLuid, REFIID riid, void** ppvAdapter) override
    {
        return _inner->EnumAdapterByLuid(AdapterLuid, riid, ppvAdapter);
    }
    HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID riid, void** ppvAdapter) override
    {
        return _inner->EnumWarpAdapter(riid, ppvAdapter);
    }

    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(DXGI_FEATURE Feature, void* pFeatureSupportData,
                                                  UINT FeatureSupportDataSize) override
    {
        return _inner->CheckFeatureSupport(Feature, pFeatureSupportData, FeatureSupportDataSize);
    }

    HRESULT STDMETHODCALLTYPE EnumAdapterByGpuPreference(UINT Adapter, DXGI_GPU_PREFERENCE GpuPreference, REFIID riid,
                                                         void** ppvAdapter) override
    {
        return _inner->EnumAdapterByGpuPreference(Adapter, GpuPreference, riid, ppvAdapter);
    }

    HRESULT STDMETHODCALLTYPE RegisterAdaptersChangedEvent(HANDLE hEvent, DWORD* pdwCookie) override
    {
        return _inner->RegisterAdaptersChangedEvent(hEvent, pdwCookie);
    }
    HRESULT STDMETHODCALLTYPE UnregisterAdaptersChangedEvent(DWORD dwCookie) override
    {
        return _inner->UnregisterAdaptersChangedEvent(dwCookie);
    }

  private:
    // Picks the factory to call and the device to hand it. *device may be replaced by a pointer that holds a
    // reference of its own; Done() releases it.
    IDXGIFactory7* Translate(IUnknown** device)
    {
        if (*device == nullptr)
            return _inner;

        if (_mode == Mode::Unwrap)
        {
            // From Streamline: ReShade's queue proxy, as the game created it. The native factory gets the queue
            // it wraps, which is what ReShade gave it when ReShade sat directly on top.
            IUnknown* proxy = nullptr;
            if ((*device)->QueryInterface(kReShadeD3D12CommandQueue, (void**) &proxy) != S_OK || proxy == nullptr)
                return _inner;
            proxy->Release();

            IUnknown* native = nullptr;
            if (proxy->QueryInterface(kReShadeUnwrappedObject, (void**) &native) == S_OK && native != nullptr)
            {
                *device = native;
                static std::atomic<bool> said { false };
                if (!said.exchange(true))
                    LOG_INFO("RenoDX/DLSS-G: native factory given the native queue {:X} for ReShade queue proxy {:X}",
                             (size_t) native, (size_t) proxy);
            }
            return _inner;
        }

        // From ReShade: the native queue. Streamline gets the game's ReShade queue proxy for it. Not a D3D12
        // queue (a D3D11 device): nothing of ReShade's to put back, forwarded as it is.
        ID3D12CommandQueue* queue = nullptr;
        if ((*device)->QueryInterface(IID_PPV_ARGS(&queue)) != S_OK || queue == nullptr)
            return _inner;
        queue->Release();

        IUnknown* game = t_gameDevice;
        IUnknown* native = nullptr;
        if (game != nullptr && game->QueryInterface(kReShadeUnwrappedObject, (void**) &native) == S_OK &&
            native != nullptr)
        {
            native->Release();
            if (native == *device)
            {
                game->AddRef();
                *device = game;
                static std::atomic<bool> said { false };
                if (!said.exchange(true))
                    LOG_INFO("RenoDX/DLSS-G: Streamline given the game's ReShade queue proxy {:X} for native queue "
                             "{:X}, as before the re-layering",
                             (size_t) game, (size_t) native);
                return _inner;
            }
        }

        static std::atomic<bool> said { false };
        LogOnce(said, "the game's queue could not be matched, swap chain created without Streamline (no frame "
                      "generation on it)");
        return _bypass != nullptr ? _bypass : _inner;
    }

    void Done(IUnknown* device, IUnknown* original)
    {
        if (device != original && device != nullptr)
            device->Release();
    }

    Mode _mode;
    IDXGIFactory7* _inner;
    IDXGIFactory7* _bypass;
    LONG _ref = 1;
};

// The module the object's vtable lives in exports ReShade's add-on entry point.
bool VTableIsReShades(IUnknown* object)
{
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(*reinterpret_cast<void**>(object)), &mod) ||
        mod == nullptr)
        return false;

    return KernelBaseProxy::GetProcAddress_()(mod, "ReShadeRegisterAddon") != nullptr;
}

// Why the factory was left as it is, logged once per reason so a player's log says which gate stopped it.
void LogLeftAlone(const char* reason)
{
    static std::mutex saidMutex;
    static std::vector<std::string> said;
    std::lock_guard<std::mutex> lock(saidMutex);
    if (std::find(said.begin(), said.end(), reason) != said.end())
        return;
    said.emplace_back(reason);
    LOG_INFO("RenoDX/DLSS-G: DXGI factory left under Streamline ({})", reason);
}
} // namespace

bool StreamlineHooks::isReShadeAboveStreamline(IUnknown* swapChain)
{
    if (!s_reshadeAboveSl || swapChain == nullptr)
        return false;

    IUnknown* proxy = nullptr;
    if (swapChain->QueryInterface(kReShadeDXGISwapChain, (void**) &proxy) != S_OK || proxy == nullptr)
        return false;

    proxy->Release();
    return true;
}

sl::Result StreamlineHooks::hkslUpgradeInterface(void** baseInterface)
{
    if (baseInterface == nullptr || *baseInterface == nullptr)
        return o_slUpgradeInterface(baseInterface);

    auto* object = static_cast<IUnknown*>(*baseInterface);

    // Only a DXGI factory that is, or wraps, ReShade's factory proxy. A device, a queue, a swap chain, a
    // factory without ReShade -- all of it goes to SL as before.
    IUnknown* reshadeFactory = nullptr;
    if (object->QueryInterface(kReShadeDXGIFactory, (void**) &reshadeFactory) != S_OK || reshadeFactory == nullptr)
    {
        IDXGIFactory* factory = nullptr;
        if (object->QueryInterface(IID_PPV_ARGS(&factory)) == S_OK && factory != nullptr)
        {
            factory->Release();
            LogLeftAlone("the factory is not a ReShade proxy");
        }

        return o_slUpgradeInterface(baseInterface);
    }

    reshadeFactory->Release(); // the game's reference keeps it alive

    std::lock_guard<std::mutex> lock(s_reshadeAboveSlMutex);

    // The game upgrades its factory again for a later swap chain: ReShade already forwards to SL's proxy,
    // and upgrading that would stack a second SL proxy on the first. Hand the same factory back.
    if (std::find(s_reshadeAboveSlFactories.begin(), s_reshadeAboveSlFactories.end(), reshadeFactory) !=
        s_reshadeAboveSlFactories.end())
        return sl::Result::eOk;

    if (State::Instance().activeFgInput == FGInput::DLSSG || State::Instance().activeFgInput == FGInput::NvngxFG ||
        State::Instance().activeFgOutput != FGOutput::NoFG)
    {
        // OptiScaler's own frame generation sits in this same swap chain chain; not re-layered.
        LogLeftAlone("OptiScaler frame generation is configured");
        return o_slUpgradeInterface(baseInterface);
    }

    if (!VTableIsReShades(reshadeFactory))
    {
        LogLeftAlone("the factory proxy is not ReShade's");
        return o_slUpgradeInterface(baseInterface);
    }

    const std::string renodx = DlssNrRenoDx::AddonInProcess();
    if (renodx.empty())
    {
        // ReShade under Streamline is what ReLimiter pacing beside native DLSS-G was verified with; without
        // RenoDX there is no reason to move it.
        LogLeftAlone("no RenoDX add-on loaded");
        return o_slUpgradeInterface(baseInterface);
    }

    // The member written below is ReShade's DXGIFactory::_orig, the first after the vtable pointer. That
    // is ReShade's private layout, so it is checked against what ReShade itself reports as the wrapped
    // object before anything is written; a ReShade that ever moves it is left alone rather than patched.
    IUnknown* wrapped = nullptr;
    if (reshadeFactory->QueryInterface(kReShadeUnwrappedObject, (void**) &wrapped) != S_OK || wrapped == nullptr)
    {
        LogLeftAlone("ReShade does not report the factory it wraps");
        return o_slUpgradeInterface(baseInterface);
    }

    wrapped->Release(); // ReShade's own reference keeps it alive

    auto** origSlot = reinterpret_cast<IUnknown**>(reshadeFactory) + 1;
    if (*origSlot != wrapped)
    {
        LogLeftAlone("ReShade's factory layout is not the expected one");
        return o_slUpgradeInterface(baseInterface);
    }

    IUnknown* alreadySl = nullptr;
    if (Util::CheckForRealObject(__FUNCTION__, wrapped, &alreadySl))
    {
        LogLeftAlone("ReShade already wraps a Streamline proxy");
        return o_slUpgradeInterface(baseInterface);
    }

    // Every step below either completes or undoes itself and hands the call to SL as if nothing had
    // happened: the game then keeps SL above ReShade, which is black with RenoDX but does not crash.
    IDXGIFactory7* native7 = nullptr;
    if (wrapped->QueryInterface(IID_PPV_ARGS(&native7)) != S_OK || native7 == nullptr)
    {
        LogLeftAlone("the native factory has no IDXGIFactory7");
        return o_slUpgradeInterface(baseInterface);
    }

    // Unwrap sits under SL. SL takes over the reference it is given, so on success the one Unwrap was
    // created with belongs to the SL proxy; on failure it is still ours and is dropped here.
    auto* unwrap = new ReShadeQueueFactory(ReShadeQueueFactory::Mode::Unwrap, native7, nullptr);
    void* upgraded = static_cast<IDXGIFactory7*>(unwrap);
    const auto result = o_slUpgradeInterface(&upgraded);
    if (result != sl::Result::eOk || upgraded == nullptr || upgraded == static_cast<IDXGIFactory7*>(unwrap))
    {
        if (upgraded != static_cast<IDXGIFactory7*>(unwrap) && upgraded != nullptr)
            static_cast<IUnknown*>(upgraded)->Release();
        else
            unwrap->Release();

        native7->Release();
        LOG_WARN("RenoDX/DLSS-G: Streamline would not upgrade the factory under ReShade ({}), left as it was",
                 magic_enum::enum_name(result));
        return o_slUpgradeInterface(baseInterface);
    }

    IDXGIFactory7* slProxy7 = nullptr;
    if (static_cast<IUnknown*>(upgraded)->QueryInterface(IID_PPV_ARGS(&slProxy7)) != S_OK || slProxy7 == nullptr)
    {
        static_cast<IUnknown*>(upgraded)->Release(); // releases Unwrap and, through it, its native reference
        native7->Release();
        LOG_WARN("RenoDX/DLSS-G: Streamline's factory proxy has no IDXGIFactory7, left as it was");
        return o_slUpgradeInterface(baseInterface);
    }

    // Rewrap is what ReShade forwards to. It holds its own references on the SL proxy and on the native
    // factory (for the no-Streamline fallback), so ours are dropped once it exists.
    auto* rewrap = new ReShadeQueueFactory(ReShadeQueueFactory::Mode::Rewrap, slProxy7, native7);
    slProxy7->Release();
    static_cast<IUnknown*>(upgraded)->Release();
    native7->Release();

    // ReShade's slot held one reference on the native factory; it now holds Rewrap's only reference, and
    // the native factory stays alive through Unwrap and Rewrap. The game keeps the reference it had on
    // ReShade's factory, which is what it is handed back.
    *origSlot = static_cast<IDXGIFactory7*>(rewrap);
    wrapped->Release();
    s_reshadeAboveSlFactories.push_back(reshadeFactory);
    s_reshadeAboveSl = true;

    LOG_INFO("RenoDX/DLSS-G present order fixed: ReShade factory {:X} now forwards to Streamline proxy {:X} "
             "(native {:X}); ReShade and {} see the game's Present before DLSS-G",
             (size_t) reshadeFactory, (size_t) upgraded, (size_t) wrapped, renodx);

    return sl::Result::eOk;
}

StreamlineHooks::ScopedGameDevice::ScopedGameDevice(IUnknown* device) : _previous(t_gameDevice)
{
    t_gameDevice = device;
}

StreamlineHooks::ScopedGameDevice::~ScopedGameDevice() { t_gameDevice = _previous; }

// [DlssNr] What of the game's DLSS-G tags reaches Streamline while ReShade sits above it for RenoDX.
//
// Re-layered, the frame DLSS-G receives at Present is RenoDX's output (its proxy pass has already encoded
// it for the HDR swap chain), but the game still tags its own HUD-less colour and UI colour/alpha buffers,
// which RenoDX never processed -- on Blood of Dawnwalker an R10G10B10A2 and a BGRA8 texture (SL's
// sl.common clones of them, 2026-09-25). DLSS-G interpolates the HUD-less buffer and lays the UI over it,
// so every generated frame came out in the game's encoding and every real one in RenoDX's: the picture
// pulsed. Withholding those tags (the first answer) only traded the pulsing for ghosting.
//
// What RenoDX's own dlssfix does, and what this does by default ([DlssNr] RenoDxDlssgHudless=redirect,
// also "auto"), is re-point every tag at RenoDX's own version of the resource through its host API
// (version 2, DlssNrRenoDx): a colour image DLSS-G compares with the presented frame -- HUD-less colour,
// the back buffer -- at a texture RenoDX fills at each present with its swap chain proxy pass over that
// image, so it is encoded exactly like the frame; anything else at RenoDX's clone of it when there is one
// (dlssfix's own rule). Without a version 2 RenoDX, or where RenoDX has nothing to substitute, the tag is
// forwarded as the game set it. drop / hudless / keep remain for comparison: withhold HUD-less and UI,
// withhold HUD-less only, forward everything. Nothing changes unless the re-layer engaged.
namespace
{
enum class TagFilter
{
    Redirect,
    Keep,
    HudlessOnly,
    HudlessAndUi
};

TagFilter CurrentTagFilter()
{
    const auto mode = Config::Instance()->RenoDxDlssgHudless.value_or_default();
    if (_wcsicmp(mode.c_str(), L"keep") == 0)
        return TagFilter::Keep;
    if (_wcsicmp(mode.c_str(), L"hudless") == 0)
        return TagFilter::HudlessOnly;
    if (_wcsicmp(mode.c_str(), L"drop") == 0)
        return TagFilter::HudlessAndUi;
    return TagFilter::Redirect; // auto, redirect, or anything unrecognised
}

const char* TagName(sl::BufferType type)
{
    switch (type)
    {
    case sl::kBufferTypeDepth:
        return "depth";
    case sl::kBufferTypeMotionVectors:
        return "motion vectors";
    case sl::kBufferTypeHUDLessColor:
        return "HUD-less colour";
    case sl::kBufferTypeUIColorAndAlpha:
        return "UI colour and alpha";
    case sl::kBufferTypeBackbuffer:
        return "back buffer";
    case sl::kBufferTypeNoWarpMask:
        return "no-warp mask";
    default:
        return "other";
    }
}

// Once per buffer type and outcome, so the log names what DLSS-G was given for each input.
void LogTagOnce(sl::BufferType type, const char* outcome)
{
    static std::mutex saidMutex;
    static std::vector<std::string> said;
    std::string key = std::to_string(type) + outcome;
    std::lock_guard<std::mutex> lock(saidMutex);
    if (std::find(said.begin(), said.end(), key) != said.end())
        return;
    said.push_back(std::move(key));
    LOG_INFO("RenoDX/DLSS-G: {} ({}) {}", TagName(type), type, outcome);
}

// The default: every tag at RenoDX's own version of its resource, see above. The copies live in *kept and
// *resources, which are sized first so the pointers into *resources stay valid.
const sl::ResourceTag* RedirectTags(const sl::ResourceTag* tags, uint32_t numTags, std::vector<sl::ResourceTag>* kept,
                                    std::vector<sl::Resource>* resources)
{
    if (!DlssNrRenoDx::TagApiAvailable())
    {
        static std::atomic<bool> said { false };
        if (!said.exchange(true))
            LOG_WARN("RenoDX/DLSS-G: the loaded RenoDX has no host API version 2, tags forwarded as the game set "
                     "them (expect generated frames to differ from real ones)");
        return tags;
    }

    kept->assign(tags, tags + numTags);
    resources->clear();
    resources->reserve(numTags);
    bool changed = false;

    for (uint32_t i = 0; i < numTags; i++)
    {
        const auto& tag = tags[i];
        if (tag.resource == nullptr || tag.resource->native == nullptr)
            continue;

        void* substitute = nullptr;
        uint32_t state = tag.resource->state;
        const bool colour = tag.type == sl::kBufferTypeHUDLessColor || tag.type == sl::kBufferTypeBackbuffer;

        if (colour && DlssNrRenoDx::EncodeForSwapchain(tag.resource->native, tag.resource->state, &substitute, &state))
            LogTagOnce(tag.type, "redirected to RenoDX's swap-chain-encoded copy");
        else if (DlssNrRenoDx::ResolveClone(tag.resource->native, &substitute))
            LogTagOnce(tag.type, "redirected to RenoDX's clone");
        else
        {
            LogTagOnce(tag.type, "has no RenoDX clone, forwarded as-is");
            continue;
        }

        resources->push_back(*tag.resource);
        resources->back().native = substitute;
        resources->back().state = state;
        (*kept)[i].resource = &resources->back();
        changed = true;
    }

    return changed ? kept->data() : tags;
}

bool Withheld(TagFilter filter, sl::BufferType type)
{
    if (type == sl::kBufferTypeHUDLessColor)
        return filter != TagFilter::Keep;
    if (type == sl::kBufferTypeUIColorAndAlpha)
        return filter == TagFilter::HudlessAndUi;
    return false;
}

// The tags to forward: tags itself when nothing is withheld, else a filtered copy in *kept. The first
// call also logs what the game tags, so a log shows the inputs DLSS-G was given.
const sl::ResourceTag* FilterTags(const sl::ResourceTag* tags, uint32_t* numTags, std::vector<sl::ResourceTag>* kept,
                                  std::vector<sl::Resource>* resources)
{
    if (!s_reshadeAboveSl || tags == nullptr || *numTags == 0)
        return tags;

    static std::atomic<bool> listed { false };
    if (!listed.exchange(true))
    {
        std::string types;
        for (uint32_t i = 0; i < *numTags; i++)
            types += (i == 0 ? "" : ",") + std::to_string(tags[i].type);
        LOG_INFO("RenoDX/DLSS-G: the game tags buffer types {} (2 HUD-less colour, 23 UI colour and alpha), "
                 "RenoDxDlssgHudless={}",
                 types, wstring_to_string(Config::Instance()->RenoDxDlssgHudless.value_or_default()));
    }

    const TagFilter filter = CurrentTagFilter();
    if (filter == TagFilter::Keep)
        return tags;

    if (filter == TagFilter::Redirect)
        return RedirectTags(tags, *numTags, kept, resources);

    kept->clear();
    bool hudless = false;
    bool ui = false;
    for (uint32_t i = 0; i < *numTags; i++)
    {
        if (Withheld(filter, tags[i].type))
        {
            hudless |= tags[i].type == sl::kBufferTypeHUDLessColor;
            ui |= tags[i].type == sl::kBufferTypeUIColorAndAlpha;
            continue;
        }
        kept->push_back(tags[i]);
    }

    if (kept->size() == *numTags)
        return tags;

    static std::atomic<bool> saidHudless { false };
    if (hudless && !saidHudless.exchange(true))
        LOG_INFO("RenoDX/DLSS-G: HUD-less tag withheld so generated frames match RenoDX's output");

    static std::atomic<bool> saidUi { false };
    if (ui && !saidUi.exchange(true))
        LOG_INFO("RenoDX/DLSS-G: UI colour and alpha tag withheld so generated frames match RenoDX's output");

    *numTags = static_cast<uint32_t>(kept->size());
    return kept->data();
}
} // namespace

sl::Result StreamlineHooks::hkslSetTag_renodx(const sl::ViewportHandle& viewport, const sl::ResourceTag* tags,
                                              uint32_t numTags, sl::CommandBuffer* cmdBuffer)
{
    thread_local std::vector<sl::ResourceTag> kept;
    thread_local std::vector<sl::Resource> resources;
    const sl::ResourceTag* forwarded = FilterTags(tags, &numTags, &kept, &resources);

    // Everything withheld: there is nothing left to set, and an empty call means "remove" to SL.
    if (forwarded != tags && numTags == 0)
        return sl::Result::eOk;

    return o_slSetTag_renodx(viewport, forwarded, numTags, cmdBuffer);
}

sl::Result StreamlineHooks::hkslSetTagForFrame_renodx(const sl::FrameToken& frame, const sl::ViewportHandle& viewport,
                                                      const sl::ResourceTag* tags, uint32_t numTags,
                                                      sl::CommandBuffer* cmdBuffer)
{
    thread_local std::vector<sl::ResourceTag> kept;
    thread_local std::vector<sl::Resource> resources;
    const sl::ResourceTag* forwarded = FilterTags(tags, &numTags, &kept, &resources);

    if (forwarded != tags && numTags == 0)
        return sl::Result::eOk;

    return o_slSetTagForFrame_renodx(frame, viewport, forwarded, numTags, cmdBuffer);
}

sl::Result StreamlineHooks::hkslSetD3DDevice(void* d3dDevice)
{
    LOG_FUNC();
    auto result = o_slSetD3DDevice(d3dDevice);
    return result;
}

void StreamlineHooks::streamlineLogCallback_sl1(sl1::LogType type, const char* msg)
{
    if (msg == nullptr)
        return;

    char* trimmed_msg = trimStreamlineLog(msg);

    if (trimmed_msg != nullptr)
    {
        switch (type)
        {
        case sl1::LogType::eLogTypeWarn:
            LOG_WARN("{}", trimmed_msg);
            break;
        case sl1::LogType::eLogTypeInfo:
            LOG_INFO("{}", trimmed_msg);
            break;
        case sl1::LogType::eLogTypeError:
            LOG_ERROR("{}", trimmed_msg);
            break;
        case sl1::LogType::eLogTypeCount:
            LOG_ERROR("{}", trimmed_msg);
            break;
        }

        free(trimmed_msg);
    }

    if (o_logCallback_sl1)
        o_logCallback_sl1(type, msg);
}

bool StreamlineHooks::hkslInit_sl1(const sl1::Preferences& pref, int applicationId)
{
    LOG_FUNC();

    sl1::Preferences localPref = pref;

    if (localPref.logMessageCallback != &streamlineLogCallback_sl1)
        o_logCallback_sl1 = localPref.logMessageCallback;
    localPref.logLevel = sl1::LogLevel::eLogLevelCount;
    localPref.logMessageCallback = &streamlineLogCallback_sl1;
    return o_slInit_sl1(localPref, applicationId);
}

bool StreamlineHooks::hkslSetTag_sl1(const sl1::Resource* resource, sl1::BufferType tag, uint32_t id,
                                     const sl1::Extent* extent)
{
    if (IsSL1AndFGActive())
        State::Instance().s_sl1FGInputs.setTag(resource, tag, id, extent);

    return o_slSetTag_sl1(resource, tag, id, extent);
}

bool StreamlineHooks::hkslSetConstants_sl1(const sl1::Constants& values, uint32_t frameIndex, uint32_t id)
{
    std::scoped_lock lock(setConstantsMutex);

    LOG_TRACE("SL1 slSetConstants frameIndex: {}, id: {}", frameIndex, id);

    if (IsSL1AndFGActive())
        State::Instance().s_sl1FGInputs.setConstants(values, frameIndex, id);

    return o_slSetConstants_interposer_sl1(values, frameIndex, id);
}

bool StreamlineHooks::hkslEvaluateFeature_sl1(sl1::CommandBuffer* cmdBuffer, sl1::Feature feature, uint32_t frameIndex,
                                              uint32_t id)
{
    LOG_TRACE("SL1 slEvaluateFeature feature: {}, frameIndex: {}, id: {}", magic_enum::enum_name(feature), frameIndex,
              id);

    if (IsSL1AndFGActive() && feature == sl1::Feature::eFeatureReflex)
    {
        const auto marker = (sl1::ReflexMarker) id;

        if (marker == sl1::ReflexMarker::eReflexMarkerRenderSubmitStart)
        {
            State::Instance().s_sl1FGInputs.evaluateState();
            State::Instance().s_sl1FGInputs.evaluateFeature(cmdBuffer, feature, frameIndex, id);
        }
        else if (marker == sl1::ReflexMarker::eReflexMarkerPresentStart)
        {
            State::Instance().s_sl1FGInputs.markPresent(frameIndex);
        }
    }

    return o_slEvaluateFeature_sl1(cmdBuffer, feature, frameIndex, id);
}

void StreamlineHooks::hookSystemCaps(sl::param::IParameters* params)
{
    if (State::Instance().streamlineVersion.major > 1)
    {
        if (!systemCaps)
            sl::param::getPointerParam(params, sl::param::common::kSystemCaps, &systemCaps);
    }
    else if (State::Instance().streamlineVersion.major == 1)
    {
        // This should be Streamline 1.5 as previous versions don't even have slOnPluginLoad
        if (!systemCapsSl15)
        {
            LOG_TRACE(
                "Attempting to get system caps for Streamline v1, this could fail depending on the exact version");
            sl::param::getPointerParam(params, sl::param::common::kSystemCaps, &systemCapsSl15);
        }
    }
}

uint32_t StreamlineHooks::getSystemCapsArch(SystemCaps* altSystemCaps)
{
    uint32_t highestArch = 0;

    auto primaryGpu = IdentifyGpu::getPrimaryGpu();
    if (!fakenvapi::isUsingAsMainNvapi() && primaryGpu.vendorId == VendorId::Nvidia)
    {
        if (State::Instance().streamlineVersion.major > 1)
        {
            auto caps = altSystemCaps != nullptr ? altSystemCaps : systemCaps;
            if (caps)
            {
                for (auto& adapter : caps->adapters)
                {
                    if (adapter.architecture > highestArch)
                        highestArch = adapter.architecture;
                }
            }
        }
        else if (State::Instance().streamlineVersion.major == 1)
        {
            if (systemCapsSl15)
            {
                for (uint32_t i = 0; i < systemCapsSl15->gpuCount; i++)
                {
                    if (systemCapsSl15->architecture[i] > highestArch)
                        highestArch = systemCapsSl15->architecture[i];
                }
            }
        }
    }

    // By default spoof Pascal, gets Reflex but not DLSSD
    // Could be problematic if not using fakenvapi but nvapi might not be initialized yet
    if (highestArch == 0)
        highestArch = NV_GPU_ARCHITECTURE_GP100;

    return highestArch;
}

void StreamlineHooks::setArch(uint32_t arch, SystemCaps* altSystemCaps)
{
    auto primaryGpu = IdentifyGpu::getPrimaryGpu();

    // altSystemCaps has to be sl2+
    if (State::Instance().streamlineVersion.major > 1 || altSystemCaps)
    {
        // Assumes that altCaps are always for SL2+
        auto caps = altSystemCaps != nullptr ? altSystemCaps : systemCaps;
        if (caps)
        {
            for (uint32_t i = 0; i < caps->gpuCount; i++)
            {
                caps->adapters[i].architecture = arch;
                caps->adapters[i].vendor = VendorId::Nvidia;
            }

            if (fakenvapi::isUsingAsMainNvapi() || primaryGpu.vendorId != VendorId::Nvidia)
                caps->driverVersionMajor = 999;

            caps->hwsSupported = true;
        }
    }
    else if (State::Instance().streamlineVersion.major == 1)
    {
        if (systemCapsSl15)
        {
            for (uint32_t i = 0; i < systemCapsSl15->gpuCount; i++)
                systemCapsSl15->architecture[i] = arch;

            if (fakenvapi::isUsingAsMainNvapi() || primaryGpu.vendorId != VendorId::Nvidia)
                systemCapsSl15->driverVersionMajor = 999;

            systemCapsSl15->hwSchedulingEnabled = true;
        }
    }
}

// Spoof arch based on feature and current arch
void StreamlineHooks::spoofArch(uint32_t currentArch, sl::Feature feature, SystemCaps* altSystemCaps)
{
    constexpr uint32_t maxArch = 0xFFFFFFFF;

    // Don't change arch for DLSS/DLSSD with turing and above
    if (feature == sl::kFeatureDLSS)
    {
        if (currentArch < NV_GPU_ARCHITECTURE_TU100)
            return setArch(maxArch, altSystemCaps);
    }

    // Don't spoof DLSSD at all
    else if (feature == sl::kFeatureDLSS_RR)
    {
        return;
    }

    // Don't change arch for DLSSG with ada and above
    else if (feature == sl::kFeatureDLSS_G)
    {
        if (State::Instance().activeFgNvngx != FGNvngxReplacement::None)
        {
            if (!Nvngx_FG::isDx12Available() && !Nvngx_FG::isVulkanAvailable())
                return setArch(0);
        }

        if (currentArch < NV_GPU_ARCHITECTURE_AD100)
            return setArch(maxArch, altSystemCaps);
    }

    else if (feature == sl::kFeatureReflex || feature == sl::kFeaturePCL)
    {
        if (fakenvapi::isUsingAsMainNvapi())
            return setArch(maxArch, altSystemCaps);
    }
}

bool StreamlineHooks::hkdlss_slOnPluginLoad(sl::param::IParameters* params, const char* loaderJSON,
                                            const char** pluginJSON)
{
    LOG_FUNC();

    // TODO: do it better than "static" and hoping for the best
    static std::string config;

    uint32_t currentArch = 0;
    if (Config::Instance()->StreamlineSpoofing.value_or_default())
    {
        hookSystemCaps(params);
        currentArch = getSystemCapsArch();
        spoofArch(currentArch, sl::kFeatureDLSS);
    }

    auto result = o_dlss_slOnPluginLoad(params, loaderJSON, pluginJSON);

    if (Config::Instance()->StreamlineSpoofing.value_or_default())
        setArch(currentArch);

    nlohmann::json configJson = nlohmann::json::parse(*pluginJSON);

    auto primaryGpu = IdentifyGpu::getPrimaryGpu();
    if (primaryGpu.vendorId != VendorId::Nvidia || !primaryGpu.dlssCapable)
    {
        if (configJson.contains("/external/vk/instance/extensions"_json_pointer))
            configJson["external"]["vk"]["instance"]["extensions"].clear();

        if (configJson.contains("/external/vk/device/extensions"_json_pointer))
            configJson["external"]["vk"]["device"]["extensions"].clear();

        if (configJson.contains("/external/vk/device/1.2_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.2_features"].clear();

        if (configJson.contains("/external/vk/device/1.3_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.3_features"].clear();
    }

    PatchSL1PluginJson(configJson);

    config = configJson.dump();

    *pluginJSON = config.c_str();

    return result;
}

sl::Result StreamlineHooks::hkslDLSSGetOptimalSettings(const sl::DLSSOptions& options,
                                                       sl::DLSSOptimalSettings& settings)
{
    static bool modesBroken = false;

    auto localOptions = options;

    if (localOptions.mode == sl::DLSSMode::eOff)
        modesBroken = true;

    if (modesBroken)
    {
        if (localOptions.mode == sl::DLSSMode::eMaxPerformance)
            localOptions.mode = sl::DLSSMode::eUltraPerformance;
        else if (localOptions.mode == sl::DLSSMode::eBalanced)
            localOptions.mode = sl::DLSSMode::eMaxPerformance;
        else if (localOptions.mode == sl::DLSSMode::eMaxQuality)
            localOptions.mode = sl::DLSSMode::eBalanced;
        else if (localOptions.mode == sl::DLSSMode::eUltraQuality)
            localOptions.mode = sl::DLSSMode::eMaxQuality;
        else if (localOptions.mode == sl::DLSSMode::eUltraPerformance)
            localOptions.mode = sl::DLSSMode::eDLAA;
    }

    return o_slDLSSGetOptimalSettings(localOptions, settings);
}

bool StreamlineHooks::hkdlssg_slOnPluginLoad(sl::param::IParameters* params, const char* loaderJSON,
                                             const char** pluginJSON)
{
    LOG_FUNC();

    // TODO: do it better than "static" and hoping for the best
    static std::string config;

    bool shouldSpoofArch =
        Config::Instance()->StreamlineSpoofing.value_or_default() &&
        (State::Instance().activeFgInput == FGInput::NvngxFG || State::Instance().activeFgInput == FGInput::DLSSG);

    uint32_t currentArch = 0;
    if (shouldSpoofArch)
    {
        hookSystemCaps(params);
        currentArch = getSystemCapsArch();
        spoofArch(currentArch, sl::kFeatureDLSS_G);
    }

    auto result = o_dlssg_slOnPluginLoad(params, loaderJSON, pluginJSON);

    if (shouldSpoofArch)
        setArch(currentArch);

    nlohmann::json configJson = nlohmann::json::parse(*pluginJSON);

    // Kill the DLSSG streamline swapchain hooks
    if (State::Instance().activeFgInput == FGInput::DLSSG || State::Instance().activeFgOutput == FGOutput::DLSSG)
    {
        if (configJson.contains("/hooks"_json_pointer))
            configJson["hooks"].clear();

        if (configJson.contains("/exclusive_hooks"_json_pointer))
            configJson["exclusive_hooks"].clear();

        if (configJson.contains("/external/feature/tags"_json_pointer))
            configJson["external"]["feature"]["tags"].clear(); // We handle the DLSSG resources

        if (configJson.contains("/external/vk/device/queues/compute/count"_json_pointer))
            configJson["external"]["vk"]["device"]["queues"]["compute"]["count"] = 0;

        if (configJson.contains("/external/vk/device/queues/graphics/count"_json_pointer))
            configJson["external"]["vk"]["device"]["queues"]["graphics"]["count"] = 0;

        if (configJson.contains("/external/vk/device/1.2_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.2_features"].clear();

        if (configJson.contains("/external/vk/device/1.3_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.3_features"].clear();
    }

    if (State::Instance().activeFgInput == FGInput::DLSSG || State::Instance().activeFgInput == FGInput::NvngxFG)
    {
        if (configJson.contains("/vsync/supported"_json_pointer))
            configJson["vsync"]["supported"] = true; // disable eVSyncOffRequired

        if (configJson.contains("/external/hws/required"_json_pointer))
            configJson["external"]["hws"]["required"] = false; // disable eHardwareSchedulingRequired

        // if (configJson.contains("/external/vk/opticalflow/supported"_json_pointer))
        //     configJson["external"]["vk"]["opticalflow"]["supported"] = true;
    }

    auto primaryGpu = IdentifyGpu::getPrimaryGpu();
    if (primaryGpu.vendorId != VendorId::Nvidia || !primaryGpu.dlssCapable)
    {
        if (configJson.contains("/external/vk/instance/extensions"_json_pointer))
            configJson["external"]["vk"]["instance"]["extensions"].clear();

        if (configJson.contains("/external/vk/device/extensions"_json_pointer))
            configJson["external"]["vk"]["device"]["extensions"].clear();

        if (configJson.contains("/external/vk/device/1.2_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.2_features"].clear();

        if (configJson.contains("/external/vk/device/1.3_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.3_features"].clear();
    }

    PatchSL1PluginJson(configJson);

    config = configJson.dump();

    *pluginJSON = config.c_str();

    return result;
}

const char* StreamlineHooks::hkdlssg_slGetPluginJSONConfig_sl1()
{
    static std::string patchedConfig;

    const char* originalConfig = o_dlssg_slGetPluginJSONConfig_sl1();

    if (originalConfig == nullptr)
        return originalConfig;

    try
    {
        auto configJson = nlohmann::json::parse(originalConfig);

        LOG_DEBUG("SL1 DLSSG JSON before patch: {}", configJson.dump());

        PatchSL1PluginJson(configJson);
        // RemoveSL1DLSSGHookEntriesRecursive(configJson);

        patchedConfig = configJson.dump();

        LOG_DEBUG("SL1 DLSSG JSON after patch: {}", patchedConfig);

        return patchedConfig.c_str();
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("Failed to patch SL1 DLSSG JSON config: {}", e.what());
        return originalConfig;
    }
}

bool StreamlineHooks::hklocal_dlssg_slOnPluginLoad(sl::param::IParameters* params, const char* loaderJSON,
                                                   const char** pluginJSON)
{
    LOG_FUNC();

    // TODO: do it better than "static" and hoping for the best
    static std::string config;

    bool shouldSpoofArch = Config::Instance()->StreamlineSpoofing.value_or_default();

    uint32_t currentArch = 0;
    SystemCaps* localSystemCaps = nullptr;
    if (shouldSpoofArch)
    {
        sl::param::getPointerParam(params, sl::param::common::kSystemCaps, &localSystemCaps);

        if (localSystemCaps)
        {
            currentArch = getSystemCapsArch(localSystemCaps);
            spoofArch(currentArch, sl::kFeatureDLSS_G, localSystemCaps);
        }
    }

    auto result = o_local_dlssg_slOnPluginLoad(params, loaderJSON, pluginJSON);

    if (shouldSpoofArch && localSystemCaps)
        setArch(currentArch, localSystemCaps);

    nlohmann::json configJson = nlohmann::json::parse(*pluginJSON);

    if (configJson.contains("/external/hws/required"_json_pointer))
        configJson["external"]["hws"]["required"] = false; // disable eHardwareSchedulingRequired

    auto primaryGpu = IdentifyGpu::getPrimaryGpu();
    if (primaryGpu.vendorId != VendorId::Nvidia || !primaryGpu.dlssCapable)
    {
        if (configJson.contains("/external/vk/instance/extensions"_json_pointer))
            configJson["external"]["vk"]["instance"]["extensions"].clear();

        if (configJson.contains("/external/vk/device/extensions"_json_pointer))
            configJson["external"]["vk"]["device"]["extensions"].clear();

        if (configJson.contains("/external/vk/device/1.2_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.2_features"].clear();

        if (configJson.contains("/external/vk/device/1.3_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.3_features"].clear();
    }

    config = configJson.dump();

    *pluginJSON = config.c_str();

    return result;
}

sl::Result StreamlineHooks::hkslSetConstants(const sl::Constants& values, const sl::FrameToken& frame,
                                             const sl::ViewportHandle& viewport)
{
    std::scoped_lock lock(setConstantsMutex);
    LOG_TRACE("called with frameIndex: {}, viewport: {}", (unsigned int) frame, (unsigned int) viewport);

    State::Instance().slFGInputs.setConstants(values, (uint32_t) frame);

    return o_slSetConstants(values, frame, viewport);
}

bool StreamlineHooks::hkcommon_slOnPluginLoad(sl::param::IParameters* params, const char* loaderJSON,
                                              const char** pluginJSON)
{
    LOG_FUNC();

    // TODO: do it better than "static" and hoping for the best
    static std::string config;

    auto result = o_common_slOnPluginLoad(params, loaderJSON, pluginJSON);

    nlohmann::json configJson = nlohmann::json::parse(*pluginJSON);

    auto& slVersion = State::Instance().streamlineVersion;

    // Grab a version of the potentially updated sl.common
    // Opti assumes that all plugins will have this version
    configJson.at("version").at("major").get_to(slVersion.major);
    configJson.at("version").at("minor").get_to(slVersion.minor);
    configJson.at("version").at("build").get_to(slVersion.patch);

    // Completely disables Streamline hooks
    // if (true)
    //    configJson["hooks"].clear();
    //    configJson["exclusive_hooks"].clear();
    //}

    PatchSL1PluginJson(configJson);

    config = configJson.dump();

    *pluginJSON = config.c_str();

    return result;
}

sl::Result StreamlineHooks::hkslDLSSGSetOptions(const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options)
{
    lastDlssgViewport = viewport;
    lastDlssgOptions = options;

    // Avoid reading past the game's struct's size
    sl::DLSSGOptions newOptions {};
    auto newStructVer = newOptions.structVersion;

    if (options.structVersion == 1)
        memcpy(&newOptions, &options, 104);
    else if (options.structVersion == 2 || options.structVersion == 3)
        memcpy(&newOptions, &options, 112);
    else if (options.structVersion == 4 || options.structVersion == 5)
        memcpy(&newOptions, &options, 120);
    else
        newOptions = options;

    newOptions.structVersion = newStructVer;

    auto& state = State::Instance();

    // Disable game's DLSSG when we are trying to create our own instance of DLSSG
    if (state.activeFgInput != FGInput::DLSSG && state.activeFgOutput == FGOutput::DLSSG)
    {
        newOptions.mode = sl::DLSSGMode::eOff;
        return o_slDLSSGSetOptions(viewport, newOptions);
    }

    // Make DLSSG auto always mean On
    if (newOptions.mode == sl::DLSSGMode::eAuto)
        newOptions.mode = sl::DLSSGMode::eOn;

    const auto dlssgPotentiallyActive = newOptions.mode == sl::DLSSGMode::eOn ||
                                        newOptions.mode == sl::DLSSGMode::eAuto ||
                                        newOptions.mode == sl::DLSSGMode::eDynamic;

    bool enableDynamicMode = Config::Instance()->FGDLSSGOverrideForceDMFG.value_or_default() &&
                             state.dlssgGameDMFGSupported && dlssgPotentiallyActive;

    if (enableDynamicMode)
    {
        newOptions.mode = sl::DLSSGMode::eDynamic;
    }

    if (newOptions.mode == sl::DLSSGMode::eDynamic && Config::Instance()->FGDLSSGFramerateTargetDMFG.has_value())
    {
        newOptions.dynamicTargetFrameRate = Config::Instance()->FGDLSSGFramerateTargetDMFG.value();
    }

    if (state.swapchainApi == API::Vulkan)
    {
        // Only matters for Vulkan, DX doesn't use this delay
        if (dlssgPotentiallyActive && !MenuOverlayBase::IsVisible())
            state.delayMenuRenderBy = 10;

        if (MenuOverlayBase::IsVisible())
        {
            newOptions.mode = sl::DLSSGMode::eOff;
            newOptions.flags |= sl::DLSSGFlags::eRetainResourcesWhenOff;
            ReflexHooks::setDlssgFrameCount(0);
        }
    }

    LOG_TRACE("DLSSG Modified Mode: {}", magic_enum::enum_name(newOptions.mode));

    if (dlssgPotentiallyActive && state.streamlineVersion >= feature_version { 2, 7, 1 })
    {
        // Populate dlssgMfgMax once
        if (!state.dlssgMfgMax.has_value())
        {
            sl::DLSSGState localState {};
            sl::DLSSGOptions localOptions {};
            if (o_slDLSSGGetState(viewport, localState, &localOptions) == sl::Result::eOk &&
                localState.numFramesToGenerateMax > 0 && localState.numFramesToGenerateMax < 6)
            {
                state.dlssgMfgMax = localState.numFramesToGenerateMax;
                LOG_TRACE("Saving original numFramesToGenerateMax: {}", state.dlssgMfgMax.value());

                if (Config::Instance()->FGDLSSGOverrideInterpolationCount.has_value() &&
                    Config::Instance()->FGDLSSGOverrideInterpolationCount.value() > state.dlssgMfgMax.value())
                {
                    Config::Instance()->FGDLSSGOverrideInterpolationCount = state.dlssgMfgMax.value();
                }
            }
        }

        // Won't take effect with Dynamic
        if (Config::Instance()->FGDLSSGOverrideInterpolationCount.has_value())
        {
            auto overrideCount = Config::Instance()->FGDLSSGOverrideInterpolationCount.value();
            if (overrideCount != 0)
                newOptions.numFramesToGenerate = overrideCount;
            else if (!enableDynamicMode)
                newOptions.mode = sl::DLSSGMode::eOff;
        }
    }

    state.dlssgLastSetMode = newOptions.mode;

    return o_slDLSSGSetOptions(viewport, newOptions);
}

sl::Result StreamlineHooks::hkslDLSSGGetState(const sl::ViewportHandle& viewport, sl::DLSSGState& state,
                                              const sl::DLSSGOptions* options)
{
    sl::Result result {};

    const auto originalStructVersion = state.structVersion;
    if (originalStructVersion < 4)
    {
        sl::DLSSGState newState {};

        // We might be feeding a newer struct to an older SL but that seems to work just fine for this Get function
        result = o_slDLSSGGetState(viewport, dynamic_cast<sl::DLSSGState&>(newState), options);

        // Copy back data to game's struct
        memcpy(&state, &newState, 56); // struct ver 1 size
        state.structVersion = originalStructVersion;

        if (originalStructVersion >= 2)
        {
            state.numFramesToGenerateMax = newState.numFramesToGenerateMax;
            state.bReserved4 = newState.bReserved4;
            state.bIsVsyncSupportAvailable = newState.bIsVsyncSupportAvailable;
        }

        if (originalStructVersion >= 3)
        {
            state.inputsProcessingCompletionFence = newState.inputsProcessingCompletionFence;
            state.lastPresentInputsProcessingCompletionFenceValue =
                newState.lastPresentInputsProcessingCompletionFenceValue;
        }

        State::Instance().dlssgGameDMFGSupported = newState.bIsDynamicMFGSupported == sl::eTrue;
    }
    else
    {
        result = o_slDLSSGGetState(viewport, state, options);
        State::Instance().dlssgGameDMFGSupported = state.bIsDynamicMFGSupported == sl::eTrue;
    }

    if (!State::Instance().dlssgGameDMFGSupported)
    {
        Config::Instance()->FGDLSSGOverrideForceDMFG.set_volatile_value(false);
    }

    auto& optiState = State::Instance();

    if (optiState.streamlineVersion >= feature_version { 2, 7, 1 })
    {
        if (!optiState.dlssgMfgMax.has_value())
        {
            sl::DLSSGState localState {};
            sl::DLSSGOptions localOptions {};
            if (o_slDLSSGGetState(viewport, localState, &localOptions) == sl::Result::eOk &&
                localState.numFramesToGenerateMax > 0 && localState.numFramesToGenerateMax < 6)
            {
                optiState.dlssgMfgMax = localState.numFramesToGenerateMax;
                LOG_TRACE("Saving original numFramesToGenerateMax: {}", optiState.dlssgMfgMax.value());

                if (Config::Instance()->FGDLSSGOverrideInterpolationCount.has_value() &&
                    Config::Instance()->FGDLSSGOverrideInterpolationCount.value() > optiState.dlssgMfgMax.value())
                {
                    Config::Instance()->FGDLSSGOverrideInterpolationCount = optiState.dlssgMfgMax.value();
                }
            }
        }
    }

    if (optiState.activeFgInput == FGInput::DLSSG)
    {
        auto fg = optiState.currentFG;

        if (fg != nullptr)
        {
            if (options != nullptr && options->flags & sl::DLSSGFlags::eRequestVRAMEstimate)
                state.estimatedVRAMUsageInBytes = static_cast<uint64_t>(256 * 1024) * 1024;

            if (fg->IsActive() && !fg->IsPaused())
            {
                state.numFramesActuallyPresented = fg->GetInterpolatedFrameCount() + 1;
            }
            else
            {
                state.numFramesActuallyPresented = 1;
            }
        }
        else
        {
            state.numFramesActuallyPresented = 1;
        }

        state.numFramesToGenerateMax = 1;

        LOG_DEBUG("Status: {}, numFramesActuallyPresented: {}", magic_enum::enum_name(state.status),
                  state.numFramesActuallyPresented);
    }

    return result;
}

bool StreamlineHooks::hkreflex_slOnPluginLoad(sl::param::IParameters* params, const char* loaderJSON,
                                              const char** pluginJSON)
{
    LOG_FUNC();

    // TODO: do it better than "static" and hoping for the best
    static std::string config;

    uint32_t currentArch = 0;
    if (Config::Instance()->StreamlineSpoofing.value_or_default())
    {
        hookSystemCaps(params);
        currentArch = getSystemCapsArch();
        spoofArch(currentArch, sl::kFeatureReflex);
    }

    auto result = o_reflex_slOnPluginLoad(params, loaderJSON, pluginJSON);

    if (Config::Instance()->StreamlineSpoofing.value_or_default())
        setArch(currentArch);

    nlohmann::json configJson = nlohmann::json::parse(*pluginJSON);

    auto primaryGpu = IdentifyGpu::getPrimaryGpu();
    if (primaryGpu.vendorId != VendorId::Nvidia || !primaryGpu.dlssCapable)
    {
        if (configJson.contains("/external/vk/instance/extensions"_json_pointer))
            configJson["external"]["vk"]["instance"]["extensions"].clear();

        if (configJson.contains("/external/vk/device/extensions"_json_pointer))
            configJson["external"]["vk"]["device"]["extensions"].clear();

        if (configJson.contains("/external/vk/device/1.2_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.2_features"].clear();

        if (configJson.contains("/external/vk/device/1.3_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.3_features"].clear();
    }

    PatchSL1PluginJson(configJson);

    config = configJson.dump();

    *pluginJSON = config.c_str();

    return result;
}

sl::Result StreamlineHooks::hkslReflexSetOptions(const sl::ReflexOptions& options)
{
    reflexGamesLastMode = options.mode;

    sl::ReflexOptions newOptions = options;

    if (Config::Instance()->FN_ForceReflex == ForceReflex::ForceEnable)
        newOptions.mode = sl::ReflexMode::eLowLatencyWithBoost;

    // Will cause a pink screen when used with DLSSG
    // if (Config::Instance()->FN_ForceReflex == 1)
    //     newOptions.mode = sl::ReflexMode::eOff;

    return o_slReflexSetOptions(newOptions);
}

sl::Result StreamlineHooks::hkslReflexSleep(const sl::FrameToken& frame)
{
    // if (State::Instance().activeFgOutput == FGOutput::DLSSG && StreamlineProxy::IsD3D12Inited() &&
    //     Config::Instance()->FGDLSSGUseGamesReflexMarkers.value_or_default())
    //{
    //     return StreamlineProxy::ReflexSleep()(frame);
    // }

    return o_slReflexSleep(frame);
}

void* StreamlineHooks::hkdlss_slGetPluginFunction(const char* functionName)
{
    LOG_DEBUG("{}", functionName);

    if (strcmp(functionName, "slOnPluginLoad") == 0)
    {
        o_dlss_slOnPluginLoad = (PFN_slOnPluginLoad) o_dlss_slGetPluginFunction(functionName);
        return &hkdlss_slOnPluginLoad;
    }

    if (strcmp(functionName, "slDLSSGetOptimalSettings") == 0 &&
        State::Instance().gameQuirks & GameQuirk::PregmataFixDLSSModes)
    {
        o_slDLSSGetOptimalSettings = (decltype(&slDLSSGetOptimalSettings)) o_dlss_slGetPluginFunction(functionName);
        return &hkslDLSSGetOptimalSettings;
    }

    return o_dlss_slGetPluginFunction(functionName);
}

void* StreamlineHooks::hkdlssg_slGetPluginFunction(const char* functionName)
{
    // LOG_DEBUG("{}", functionName);

    if (strcmp(functionName, "slOnPluginLoad") == 0)
    {
        o_dlssg_slOnPluginLoad = (PFN_slOnPluginLoad) o_dlssg_slGetPluginFunction(functionName);
        return &hkdlssg_slOnPluginLoad;
    }

    if (strcmp(functionName, "slDLSSGSetOptions") == 0)
    {
        o_slDLSSGSetOptions = (decltype(&slDLSSGSetOptions)) o_dlssg_slGetPluginFunction(functionName);

        // Give steam overlay the original as it seems to be hooking it
        auto steamOverlay = KernelBaseProxy::GetModuleHandleA_()("gameoverlayrenderer64.dll");
        if (steamOverlay != nullptr)
        {
            if (HMODULE callerModule = Util::GetCallerModule(_ReturnAddress()); callerModule == steamOverlay)
                return o_slDLSSGSetOptions;
        }

        return &hkslDLSSGSetOptions;
    }

    if (strcmp(functionName, "slDLSSGGetState") == 0)
    {
        o_slDLSSGGetState = (decltype(&slDLSSGGetState)) o_dlssg_slGetPluginFunction(functionName);

        // Give steam overlay the original as it seems to be hooking it
        auto steamOverlay = KernelBaseProxy::GetModuleHandleA_()("gameoverlayrenderer64.dll");
        if (steamOverlay != nullptr)
        {
            if (HMODULE callerModule = Util::GetCallerModule(_ReturnAddress()); callerModule == steamOverlay)
                return o_slDLSSGGetState;
        }

        return &hkslDLSSGGetState;
    }

    if (strcmp(functionName, "slGetPluginJSONConfig") == 0 && IsSL1AndDLSSGActive())
    {
        o_dlssg_slGetPluginJSONConfig_sl1 =
            reinterpret_cast<PFN_slGetPluginJSONConfig_sl1>(o_dlssg_slGetPluginFunction(functionName));

        if (o_dlssg_slGetPluginJSONConfig_sl1 != nullptr)
        {
            LOG_WARN("Hooking SL1 DLSSG slGetPluginJSONConfig");
            return &hkdlssg_slGetPluginJSONConfig_sl1;
        }
    }

    // Ensure that we have those DLSSG calls
    if (!o_slDLSSGSetOptions)
        o_slDLSSGSetOptions = (decltype(&slDLSSGSetOptions)) o_dlssg_slGetPluginFunction("slDLSSGSetOptions");

    if (!o_slDLSSGGetState)
        o_slDLSSGGetState = (decltype(&slDLSSGGetState)) o_dlssg_slGetPluginFunction("slDLSSGGetState");

    return o_dlssg_slGetPluginFunction(functionName);
}

void* StreamlineHooks::hklocal_dlssg_slGetPluginFunction(const char* functionName)
{
    // LOG_DEBUG("{}", functionName);

    if (strcmp(functionName, "slOnPluginLoad") == 0 && State::Instance().activeFgNvngx != FGNvngxReplacement::None)
    {
        o_local_dlssg_slOnPluginLoad = (PFN_slOnPluginLoad) o_local_dlssg_slGetPluginFunction(functionName);
        return &hklocal_dlssg_slOnPluginLoad;
    }

    return o_local_dlssg_slGetPluginFunction(functionName);
}

bool StreamlineHooks::hkreflex_slSetConstants_sl1(const void* data, uint32_t frameIndex, uint32_t id)
{
    // Streamline v1's version of slReflexSetOptions + slPCLSetMarker
    static sl1::ReflexConstants constants {};
    constants = *(const sl1::ReflexConstants*) data;

    reflexGamesLastMode = (sl::ReflexMode) constants.mode;

    LOG_DEBUG("mode: {}, frameIndex: {}, id: {}", (uint32_t) constants.mode, frameIndex, id);

    if (Config::Instance()->FN_ForceReflex == ForceReflex::ForceEnable)
        constants.mode = sl1::ReflexMode::eReflexModeLowLatencyWithBoost;

    // Will cause a pink screen when used with DLSSG
    // else if (Config::Instance()->FN_ForceReflex == 1)
    //     constants.mode = sl1::ReflexMode::eReflexModeOff;

    return o_reflex_slSetConstants_sl1(&constants, frameIndex, id);
}

void* StreamlineHooks::hkreflex_slGetPluginFunction(const char* functionName)
{
    // LOG_DEBUG("{}", functionName);

    if (strcmp(functionName, "slSetConstants") == 0 && State::Instance().streamlineVersion.major == 1)
    {
        o_reflex_slSetConstants_sl1 = (PFN_slSetConstants_sl1) o_reflex_slGetPluginFunction(functionName);
        return &hkreflex_slSetConstants_sl1;
    }

    if (strcmp(functionName, "slOnPluginLoad") == 0)
    {
        o_reflex_slOnPluginLoad = (PFN_slOnPluginLoad) o_reflex_slGetPluginFunction(functionName);
        return &hkreflex_slOnPluginLoad;
    }

    if (strcmp(functionName, "slReflexSetOptions") == 0)
    {
        o_slReflexSetOptions = (decltype(&slReflexSetOptions)) o_reflex_slGetPluginFunction(functionName);
        return &hkslReflexSetOptions;
    }

    if (strcmp(functionName, "slReflexSleep") == 0)
    {
        o_slReflexSleep = (decltype(&slReflexSleep)) o_reflex_slGetPluginFunction(functionName);
        return &hkslReflexSleep;
    }

    // TODO: Hopefully a game doesn't call both, maybe separate
    if (strcmp(functionName, "slReflexSetMarker") == 0 &&
        (State::Instance().gameQuirks & GameQuirk::FixSlSimulationMarkers ||
         State::Instance().activeFgInput == FGInput::DLSSG))
    {
        o_slPCLSetMarker = (decltype(&slPCLSetMarker)) o_reflex_slGetPluginFunction(functionName);
        return &hkslPCLSetMarker;
    }

    return o_reflex_slGetPluginFunction(functionName);
}

sl::Result StreamlineHooks::hkslPCLSetMarker(sl::PCLMarker marker, const sl::FrameToken& frame)
{
    // if (State::Instance().activeFgOutput == FGOutput::DLSSG && StreamlineProxy::IsD3D12Inited() &&
    //     Config::Instance()->FGDLSSGUseGamesReflexMarkers.value_or_default())
    //{
    //     return StreamlineProxy::PCLSetMarker()(marker, frame);
    // }

    // HACK for broken games
    if (State::Instance().gameQuirks & GameQuirk::FixSlSimulationMarkers)
    {
        static uint64_t last_simulation_end_id = 0;
        if (marker == sl::PCLMarker::eSimulationEnd)
        {
            last_simulation_end_id = frame;
        }

        if (marker == sl::PCLMarker::eSimulationStart && last_simulation_end_id >= frame && o_slGetNewFrameToken)
        {
            const uint64_t correction_offset = last_simulation_end_id - frame + 1;
            uint32_t newFrameId = static_cast<uint32_t>(frame + correction_offset);

            sl::FrameToken* newFramePointer {};
            auto result = o_slGetNewFrameToken(newFramePointer, &newFrameId);

            LOG_WARN("Simulation start marker sent after end marker, offset: {}", correction_offset);

            result = o_slPCLSetMarker(marker, *newFramePointer);
            return result;
        }
    }

    if (State::Instance().activeFgInput == FGInput::DLSSG)
    {
        if (State::Instance().streamlineVersion.major == 1)
        {
            if (marker == sl::PCLMarker::eRenderSubmitStart)
            {
                State::Instance().s_sl1FGInputs.evaluateState();
            }
            else if (marker == sl::PCLMarker::ePresentStart)
            {
                State::Instance().s_sl1FGInputs.markPresent(frame);
            }
        }
        else
        {
            if (marker == sl::PCLMarker::eRenderSubmitStart)
            {
                State::Instance().slFGInputs.evaluateState();
            }
            else if (marker == sl::PCLMarker::ePresentStart)
            {
                State::Instance().slFGInputs.markPresent(frame);
            }
        }
    }

    return o_slPCLSetMarker(marker, frame);
}

bool StreamlineHooks::hkpcl_slOnPluginLoad(sl::param::IParameters* params, const char* loaderJSON,
                                           const char** pluginJSON)
{
    LOG_FUNC();

    uint32_t currentArch = 0;
    if (Config::Instance()->StreamlineSpoofing.value_or_default())
    {
        hookSystemCaps(params);
        currentArch = getSystemCapsArch();
        spoofArch(currentArch, sl::kFeaturePCL);
    }

    auto result = o_pcl_slOnPluginLoad(params, loaderJSON, pluginJSON);

    if (Config::Instance()->StreamlineSpoofing.value_or_default())
        setArch(currentArch);

    return result;
}

void* StreamlineHooks::hkpcl_slGetPluginFunction(const char* functionName)
{
    // LOG_DEBUG("{}", functionName);

    if (strcmp(functionName, "slPCLSetMarker") == 0 &&
        (State::Instance().gameQuirks & GameQuirk::FixSlSimulationMarkers ||
         State::Instance().activeFgInput == FGInput::DLSSG))
    {
        o_slPCLSetMarker = (decltype(&slPCLSetMarker)) o_pcl_slGetPluginFunction(functionName);
        return &hkslPCLSetMarker;
    }

    if (strcmp(functionName, "slOnPluginLoad") == 0)
    {
        o_pcl_slOnPluginLoad = (PFN_slOnPluginLoad) o_pcl_slGetPluginFunction(functionName);
        return &hkpcl_slOnPluginLoad;
    }

    return o_pcl_slGetPluginFunction(functionName);
}

bool StreamlineHooks::hk_setVoid(void* self, const char* key, void** value)
{
    // LOG_DEBUG("{}", key);

    if (strcmp(key, sl::param::common::kSystemCaps) == 0)
    {
        LOG_TRACE("Attempting to change system caps for Streamline v1, this could fail depending on the exact version");

        // SystemCapsSl15 is not entirely correct for Streamline 1.3
        // But we here only use the beginning that matches + extra
        auto caps = (SystemCapsSl15*) value;

        if (caps)
        {
            caps->gpuCount = 1;
            caps->architecture[0] = UINT_MAX;
            caps->driverVersionMajor = 999;

            // HAGS
            *((char*) value + 56) = (char) 0x01;
        }
    }

    return o_setVoid(self, key, value);
}

void StreamlineHooks::hkcommon_slSetParameters_sl1(void* params)
{
    LOG_FUNC();

    if (o_setVoid == nullptr && params)
    {
        void** vtable = *(void***) params;

        // It's flipped, 0 -> set void*, 7 -> get void*
        o_setVoid = (PFN_setVoid) vtable[0];

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        if (o_setVoid != nullptr)
            DetourAttach(&(PVOID&) o_setVoid, hk_setVoid);

        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to hook setVoid: {:X}", detourResult);
            o_setVoid = nullptr;
        }
    }

    o_common_slSetParameters_sl1(params);
}

void* StreamlineHooks::hkcommon_slGetPluginFunction(const char* functionName)
{
    // LOG_DEBUG("{}", functionName);

    if (strcmp(functionName, "slOnPluginLoad") == 0)
    {
        o_common_slOnPluginLoad = (PFN_slOnPluginLoad) o_common_slGetPluginFunction(functionName);
        return &hkcommon_slOnPluginLoad;
    }

    // Used around Streamline v1.3, as 1.5 doesn't seem to have it anymore
    if (strcmp(functionName, "slSetParameters") == 0)
    {
        o_common_slSetParameters_sl1 = (PFN_slSetParameters_sl1) o_common_slGetPluginFunction(functionName);
        return &hkcommon_slSetParameters_sl1;
    }

    return o_common_slGetPluginFunction(functionName);
}

void StreamlineHooks::updateForceReflex()
{
    // Not needed for Streamline v1 as slSetConstants is sent every frame
    if (o_slReflexSetOptions)
    {
        sl::ReflexOptions options;

        auto forceReflex = Config::Instance()->FN_ForceReflex.value_or_default();

        if (forceReflex == ForceReflex::ForceEnable)
            options.mode = sl::ReflexMode::eLowLatencyWithBoost;
        else if (forceReflex == ForceReflex::ForceDisable)
            options.mode = sl::ReflexMode::eOff;
        else if (forceReflex == ForceReflex::InGame)
            options.mode = reflexGamesLastMode;

        auto result = o_slReflexSetOptions(options);
        if (result != sl::Result::eOk)
        {
            LOG_WARN("Failed to update Reflex mode with error code: {} ({:X})", magic_enum::enum_name(result),
                     (UINT) result);
        }
    }
}

void StreamlineHooks::updateDlssgOptions()
{
    if (o_slDLSSGSetOptions)
    {
        LOG_FUNC();
        hkslDLSSGSetOptions(lastDlssgViewport, lastDlssgOptions);
    }
}

// SL INTERPOSER

void StreamlineHooks::unhookInterposer()
{
    LOG_FUNC();

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (o_slSetTag)
        DetourDetach(&(PVOID&) o_slSetTag, hkslSetTag);

    if (o_slSetTagForFrame)
        DetourDetach(&(PVOID&) o_slSetTagForFrame, hkslSetTagForFrame);

    if (o_slSetConstants)
        DetourDetach(&(PVOID&) o_slSetConstants, hkslSetConstants);

    if (o_slEvaluateFeature)
        DetourDetach(&(PVOID&) o_slEvaluateFeature, hkslEvaluateFeature);

    if (o_slInit)
        DetourDetach(&(PVOID&) o_slInit, hkslInit);

    if (o_slSetTag_renodx)
        DetourDetach(&(PVOID&) o_slSetTag_renodx, hkslSetTag_renodx);

    if (o_slSetTagForFrame_renodx)
        DetourDetach(&(PVOID&) o_slSetTagForFrame_renodx, hkslSetTagForFrame_renodx);

    if (o_slUpgradeInterface)
        DetourDetach(&(PVOID&) o_slUpgradeInterface, hkslUpgradeInterface);

    if (o_slInit_sl1)
        DetourDetach(&(PVOID&) o_slInit_sl1, hkslInit_sl1);

    if (o_slSetTag_sl1)
        DetourDetach(&(PVOID&) o_slSetTag_sl1, hkslSetTag_sl1);

    if (o_slSetConstants_interposer_sl1)
        DetourDetach(&(PVOID&) o_slSetConstants_interposer_sl1, hkslSetConstants_sl1);

    if (o_slEvaluateFeature_sl1)
        DetourDetach(&(PVOID&) o_slEvaluateFeature_sl1, hkslEvaluateFeature_sl1);

    // if (o_logCallback)
    //     DetourDetach(&(PVOID&) o_logCallback, streamlineLogCallback);
    // else if (o_logCallback_sl1)
    //     DetourDetach(&(PVOID&) o_logCallback_sl1, streamlineLogCallback);

    auto detourResult = DetourTransactionCommit();
    if (detourResult != NO_ERROR)
    {
        LOG_ERROR("DetourTransactionCommit error: {:X}", detourResult);
    }
    else
    {
        o_slInit = nullptr;
        o_slInit_sl1 = nullptr;
        o_slSetTag = nullptr;
        o_slSetTagForFrame = nullptr;
        o_slEvaluateFeature = nullptr;
        o_slSetConstants = nullptr;
        o_slUpgradeInterface = nullptr;
        o_slSetTag_renodx = nullptr;
        o_slSetTagForFrame_renodx = nullptr;
        o_slSetTag_sl1 = nullptr;
        o_slSetConstants_interposer_sl1 = nullptr;
        o_slEvaluateFeature_sl1 = nullptr;
        o_logCallback = nullptr;
        o_logCallback_sl1 = nullptr;
    }
}

// Call it just after sl.interposer's load or if sl.interposer is already loaded
void StreamlineHooks::hookInterposer(HMODULE slInterposer)
{
    LOG_FUNC();

    if (!slInterposer)
    {
        LOG_WARN("Streamline module in NULL");
        return;
    }

    // Interposer needs this or it might end in an infinite loop calling itself
    static HMODULE last_slInterposer = nullptr;

    if (last_slInterposer == slInterposer)
        return;

    last_slInterposer = slInterposer;

    // Looks like when reading DLL version load methods are called
    // To prevent loops disabling checks for sl.interposer.dll
    auto owner = State::GetOwner();
    State::DisableChecks(owner, "sl.interposer");

    if (o_slSetTag || o_slInit || o_slInit_sl1 || o_slSetTag_sl1 || o_slSetConstants_interposer_sl1 ||
        o_slEvaluateFeature_sl1)
        unhookInterposer();

    {
        char dllPath[MAX_PATH];
        GetModuleFileNameA(slInterposer, dllPath, MAX_PATH);

        LOG_TRACE("slInterposer path: {}", dllPath);

        version_t sl_version;
        Util::GetFileVersion(string_to_wstring(dllPath), &sl_version);

        State::Instance().streamlineVersion.major = sl_version.major;
        State::Instance().streamlineVersion.minor = sl_version.minor;
        State::Instance().streamlineVersion.patch = sl_version.patch;

        LOG_INFO("Streamline version: {}.{}.{}", sl_version.major, sl_version.minor, sl_version.patch);

        if (sl_version.major >= 2)
        {
            o_slSetTag =
                reinterpret_cast<decltype(&slSetTag)>(KernelBaseProxy::GetProcAddress_()(slInterposer, "slSetTag"));
            o_slSetTagForFrame = reinterpret_cast<decltype(&slSetTagForFrame)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slSetTagForFrame"));
            o_slInit = reinterpret_cast<decltype(&slInit)>(KernelBaseProxy::GetProcAddress_()(slInterposer, "slInit"));
            o_slEvaluateFeature = reinterpret_cast<decltype(&slEvaluateFeature)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slEvaluateFeature"));
            o_slAllocateResources = reinterpret_cast<decltype(&slAllocateResources)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slAllocateResources"));
            o_slSetConstants = reinterpret_cast<decltype(&slSetConstants)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slSetConstants"));
            o_slGetNativeInterface = reinterpret_cast<decltype(&slGetNativeInterface)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slGetNativeInterface"));
            o_slUpgradeInterface = reinterpret_cast<decltype(&slUpgradeInterface)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slUpgradeInterface"));
            o_slSetD3DDevice = reinterpret_cast<decltype(&slSetD3DDevice)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slSetD3DDevice"));
            o_slGetNewFrameToken = reinterpret_cast<decltype(&slGetNewFrameToken)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slGetNewFrameToken")); // Not hooked

            // For making the game think DLSSG is loaded and supported
            // but making SL not actually load the plugin
            o_slIsFeatureSupported = reinterpret_cast<decltype(&slIsFeatureSupported)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slIsFeatureSupported"));
            o_slIsFeatureLoaded = reinterpret_cast<decltype(&slIsFeatureLoaded)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slIsFeatureLoaded"));
            o_slGetFeatureRequirements = reinterpret_cast<decltype(&slGetFeatureRequirements)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slGetFeatureRequirements"));
            o_slGetFeatureVersion = reinterpret_cast<decltype(&slGetFeatureVersion)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slGetFeatureVersion"));
            o_slGetFeatureFunction = reinterpret_cast<decltype(&slGetFeatureFunction)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slGetFeatureFunction"));

            if (o_slInit != nullptr)
            {
                LOG_TRACE("Hooking v2");
                DetourTransactionBegin();
                DetourUpdateThread(GetCurrentThread());

                DetourAttach(&(PVOID&) o_slInit, hkslInit);

                if (o_slEvaluateFeature != nullptr)
                    DetourAttach(&(PVOID&) o_slEvaluateFeature, hkslEvaluateFeature);

                // Pass-through unless RenoDX needs ReShade above Streamline, see hkslUpgradeInterface
                if (o_slUpgradeInterface != nullptr)
                    DetourAttach(&(PVOID&) o_slUpgradeInterface, hkslUpgradeInterface);

                if (State::Instance().activeFgInput == FGInput::NvngxFG ||
                    State::Instance().activeFgInput == FGInput::DLSSG)
                {
                    if (o_slSetTag != nullptr)
                        DetourAttach(&(PVOID&) o_slSetTag, hkslSetTag);

                    if (o_slSetTagForFrame != nullptr)
                        DetourAttach(&(PVOID&) o_slSetTagForFrame, hkslSetTagForFrame);

                    if (o_slSetConstants != nullptr)
                        DetourAttach(&(PVOID&) o_slSetConstants, hkslSetConstants);
                }
                else
                {
                    // Pass-through unless the RenoDX re-layer engaged, see hkslSetTag_renodx
                    o_slSetTag_renodx = o_slSetTag;
                    o_slSetTagForFrame_renodx = o_slSetTagForFrame;

                    if (o_slSetTag_renodx != nullptr)
                        DetourAttach(&(PVOID&) o_slSetTag_renodx, hkslSetTag_renodx);

                    if (o_slSetTagForFrame_renodx != nullptr)
                        DetourAttach(&(PVOID&) o_slSetTagForFrame_renodx, hkslSetTagForFrame_renodx);
                }

                if (State::Instance().activeFgInput == FGInput::DLSSG)
                {
                    if (o_slIsFeatureSupported != nullptr)
                        DetourAttach(&(PVOID&) o_slIsFeatureSupported, hkslIsFeatureSupported);

                    if (o_slIsFeatureLoaded != nullptr)
                        DetourAttach(&(PVOID&) o_slIsFeatureLoaded, hkslIsFeatureLoaded);

                    if (o_slGetFeatureRequirements != nullptr)
                        DetourAttach(&(PVOID&) o_slGetFeatureRequirements, hkslGetFeatureRequirements);

                    if (o_slGetFeatureVersion != nullptr)
                        DetourAttach(&(PVOID&) o_slGetFeatureVersion, hkslGetFeatureVersion);

                    if (o_slGetFeatureFunction != nullptr)
                        DetourAttach(&(PVOID&) o_slGetFeatureFunction, hkslGetFeatureFunction);
                }

                // if (o_slAllocateResources != nullptr)
                //     DetourAttach(&(PVOID&) o_slAllocateResources, hkslAllocateResources);

                // if (o_slGetNativeInterface != nullptr)
                //     DetourAttach(&(PVOID&) o_slGetNativeInterface, hkslGetNativeInterface);

                // if (o_slSetD3DDevice != nullptr)
                //     DetourAttach(&(PVOID&) o_slSetD3DDevice, hkslSetD3DDevice);

                auto detourResult = DetourTransactionCommit();
                if (detourResult != NO_ERROR)
                {
                    LOG_ERROR("Failed to hook sl.interposer v2: {:X}", detourResult);
                    o_slSetTag = nullptr;
                    o_slSetTagForFrame = nullptr;
                    o_slInit = nullptr;
                    o_slEvaluateFeature = nullptr;
                    o_slAllocateResources = nullptr;
                    o_slSetConstants = nullptr;
                    o_slGetNativeInterface = nullptr;
                    o_slUpgradeInterface = nullptr;
                    o_slSetTag_renodx = nullptr;
                    o_slSetTagForFrame_renodx = nullptr;
                    o_slSetD3DDevice = nullptr;
                    o_slIsFeatureSupported = nullptr;
                    o_slIsFeatureLoaded = nullptr;
                    o_slGetFeatureRequirements = nullptr;
                    o_slGetFeatureVersion = nullptr;
                    o_slGetFeatureFunction = nullptr;
                }
            }
        }
        else if (sl_version.major == 1)
        {
            o_slInit_sl1 =
                reinterpret_cast<decltype(&sl1::slInit)>(KernelBaseProxy::GetProcAddress_()(slInterposer, "slInit"));
            o_slSetTag_sl1 = reinterpret_cast<decltype(&sl1::slSetTag)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slSetTag"));
            o_slSetConstants_interposer_sl1 = reinterpret_cast<decltype(&sl1::slSetConstants)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slSetConstants"));
            o_slEvaluateFeature_sl1 = reinterpret_cast<decltype(&sl1::slEvaluateFeature)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slEvaluateFeature"));

            LOG_INFO("SL1 exports - slInit: {}, slSetTag: {}, slSetConstants: {}, slEvaluateFeature: {}",
                     o_slInit_sl1 != nullptr, o_slSetTag_sl1 != nullptr, o_slSetConstants_interposer_sl1 != nullptr,
                     o_slEvaluateFeature_sl1 != nullptr);

            if (o_slInit_sl1 || o_slSetTag_sl1 || o_slSetConstants_interposer_sl1 || o_slEvaluateFeature_sl1)
            {
                LOG_TRACE("Hooking v1");
                DetourTransactionBegin();
                DetourUpdateThread(GetCurrentThread());

                if (o_slInit_sl1)
                    DetourAttach(&(PVOID&) o_slInit_sl1, hkslInit_sl1);

                if (IsSL1AndFGActive())
                {
                    if (o_slSetTag_sl1)
                        DetourAttach(&(PVOID&) o_slSetTag_sl1, hkslSetTag_sl1);

                    if (o_slSetConstants_interposer_sl1)
                        DetourAttach(&(PVOID&) o_slSetConstants_interposer_sl1, hkslSetConstants_sl1);

                    if (o_slEvaluateFeature_sl1)
                        DetourAttach(&(PVOID&) o_slEvaluateFeature_sl1, hkslEvaluateFeature_sl1);
                }

                auto detourResult = DetourTransactionCommit();
                if (detourResult != NO_ERROR)
                {
                    LOG_ERROR("Failed to hook sl.interposer v1: {:X}", detourResult);
                    o_slInit_sl1 = nullptr;
                    o_slSetTag_sl1 = nullptr;
                    o_slSetConstants_interposer_sl1 = nullptr;
                    o_slEvaluateFeature_sl1 = nullptr;
                }
            }
        }
    }

    State::EnableChecks(owner);
}

// SL DLSS

void StreamlineHooks::unhookDlss()
{
    LOG_FUNC();

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (o_dlss_slGetPluginFunction)
        DetourDetach(&(PVOID&) o_dlss_slGetPluginFunction, hkdlss_slGetPluginFunction);

    auto detourResult = DetourTransactionCommit();
    if (detourResult != NO_ERROR)
    {
        LOG_ERROR("Failed to unhook DLSS: {:X}", detourResult);
    }
    else
    {
        o_dlss_slGetPluginFunction = nullptr;
    }
}

void StreamlineHooks::hookDlss(HMODULE slDlss)
{
    LOG_FUNC();

    if (!slDlss)
    {
        LOG_WARN("Dlss module in NULL");
        return;
    }

    if (o_dlss_slGetPluginFunction)
        unhookDlss();

    o_dlss_slGetPluginFunction =
        reinterpret_cast<PFN_slGetPluginFunction>(KernelBaseProxy::GetProcAddress_()(slDlss, "slGetPluginFunction"));

    if (o_dlss_slGetPluginFunction != nullptr)
    {
        LOG_TRACE("Hooking slGetPluginFunction in sl.dlss");
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        DetourAttach(&(PVOID&) o_dlss_slGetPluginFunction, hkdlss_slGetPluginFunction);

        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to hook DLSS: {:X}", detourResult);
            o_dlss_slGetPluginFunction = nullptr;
        }
    }
}

// SL DLSSG

void StreamlineHooks::unhookDlssg()
{
    LOG_FUNC();

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (o_dlssg_slGetPluginFunction)
        DetourDetach(&(PVOID&) o_dlssg_slGetPluginFunction, hkdlssg_slGetPluginFunction);

    auto detourResult = DetourTransactionCommit();
    if (detourResult != NO_ERROR)
    {
        LOG_ERROR("Failed to unhook DLSSG: {:X}", detourResult);
        o_dlssg_slGetPluginFunction = nullptr;
    }
}

void StreamlineHooks::hookDlssg(HMODULE slDlssg)
{
    LOG_FUNC();

    if (!slDlssg)
    {
        LOG_WARN("Dlssg module in NULL");
        return;
    }

    if (o_dlssg_slGetPluginFunction)
        unhookDlssg();

    o_dlssg_slGetPluginFunction =
        reinterpret_cast<PFN_slGetPluginFunction>(KernelBaseProxy::GetProcAddress_()(slDlssg, "slGetPluginFunction"));

    if (o_dlssg_slGetPluginFunction != nullptr)
    {
        LOG_TRACE("Hooking slGetPluginFunction in sl.dlssg");
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        DetourAttach(&(PVOID&) o_dlssg_slGetPluginFunction, hkdlssg_slGetPluginFunction);

        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to hook DLSSG: {:X}", detourResult);
            o_dlssg_slGetPluginFunction = nullptr;
        }
    }
}

// Local SL DLSSG

void StreamlineHooks::unhookLocalDlssg()
{
    LOG_FUNC();

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (o_local_dlssg_slGetPluginFunction)
    {
        DetourDetach(&(PVOID&) o_local_dlssg_slGetPluginFunction, hklocal_dlssg_slGetPluginFunction);
        o_local_dlssg_slGetPluginFunction = nullptr;
    }

    DetourTransactionCommit();
}

void StreamlineHooks::hookLocalDlssg(HMODULE slDlssg)
{
    LOG_FUNC();

    if (!slDlssg)
    {
        LOG_WARN("Dlssg module in NULL");
        return;
    }

    if (o_local_dlssg_slGetPluginFunction)
        unhookLocalDlssg();

    o_local_dlssg_slGetPluginFunction =
        reinterpret_cast<PFN_slGetPluginFunction>(KernelBaseProxy::GetProcAddress_()(slDlssg, "slGetPluginFunction"));

    if (o_local_dlssg_slGetPluginFunction != nullptr)
    {
        LOG_TRACE("Hooking slGetPluginFunction in local sl.dlssg");
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        DetourAttach(&(PVOID&) o_local_dlssg_slGetPluginFunction, hklocal_dlssg_slGetPluginFunction);

        DetourTransactionCommit();
    }
}

// SL REFLEX

void StreamlineHooks::unhookReflex()
{
    LOG_FUNC();

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (o_reflex_slGetPluginFunction)
        DetourDetach(&(PVOID&) o_reflex_slGetPluginFunction, hkreflex_slGetPluginFunction);

    auto detourResult = DetourTransactionCommit();
    if (detourResult != NO_ERROR)
    {
        LOG_ERROR("Failed to unhook Reflex: {:X}", detourResult);
    }
    else
    {
        o_reflex_slGetPluginFunction = nullptr;
    }
}

void StreamlineHooks::hookReflex(HMODULE slReflex)
{
    LOG_FUNC();

    if (!slReflex)
    {
        LOG_WARN("Reflex module in NULL");
        return;
    }

    if (o_reflex_slGetPluginFunction)
        unhookReflex();

    o_reflex_slGetPluginFunction =
        reinterpret_cast<PFN_slGetPluginFunction>(KernelBaseProxy::GetProcAddress_()(slReflex, "slGetPluginFunction"));

    if (o_reflex_slGetPluginFunction != nullptr)
    {
        LOG_TRACE("Hooking slGetPluginFunction in sl.reflex");
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        DetourAttach(&(PVOID&) o_reflex_slGetPluginFunction, hkreflex_slGetPluginFunction);

        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to hook Reflex: {:X}", detourResult);
            o_reflex_slGetPluginFunction = nullptr;
        }
    }
}

// SL PCL

void StreamlineHooks::unhookPcl()
{
    LOG_FUNC();

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (o_pcl_slGetPluginFunction)
        DetourDetach(&(PVOID&) o_pcl_slGetPluginFunction, hkpcl_slGetPluginFunction);

    auto detourResult = DetourTransactionCommit();
    if (detourResult != NO_ERROR)
    {
        LOG_ERROR("Failed to unhook PCL: {:X}", detourResult);
    }
    else
    {
        o_pcl_slGetPluginFunction = nullptr;
    }
}

void StreamlineHooks::hookPcl(HMODULE slPcl)
{
    LOG_FUNC();

    if (!slPcl)
    {
        LOG_WARN("Pcl module in NULL");
        return;
    }

    if (o_pcl_slGetPluginFunction)
        unhookPcl();

    o_pcl_slGetPluginFunction =
        reinterpret_cast<PFN_slGetPluginFunction>(KernelBaseProxy::GetProcAddress_()(slPcl, "slGetPluginFunction"));

    if (o_pcl_slGetPluginFunction != nullptr)
    {
        LOG_TRACE("Hooking slGetPluginFunction in sl.pcl");
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        DetourAttach(&(PVOID&) o_pcl_slGetPluginFunction, hkpcl_slGetPluginFunction);

        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to hook PCL: {:X}", detourResult);
            o_pcl_slGetPluginFunction = nullptr;
        }
    }
}

// SL COMMON

void StreamlineHooks::unhookCommon()
{
    LOG_FUNC();

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (o_common_slGetPluginFunction)
        DetourDetach(&(PVOID&) o_common_slGetPluginFunction, hkcommon_slGetPluginFunction);

    auto detourResult = DetourTransactionCommit();
    if (detourResult != NO_ERROR)
    {
        LOG_ERROR("Failed to unhook Common: {:X}", detourResult);
    }
    else
    {
        systemCaps = nullptr;
        systemCapsSl15 = nullptr;
        o_common_slGetPluginFunction = nullptr;
    }
}

void StreamlineHooks::hookCommon(HMODULE slCommon)
{
    LOG_FUNC();

    if (!slCommon)
    {
        LOG_WARN("Common module in NULL");
        return;
    }

    if (o_common_slGetPluginFunction)
        unhookCommon();

    o_common_slGetPluginFunction =
        reinterpret_cast<PFN_slGetPluginFunction>(KernelBaseProxy::GetProcAddress_()(slCommon, "slGetPluginFunction"));

    if (o_common_slGetPluginFunction != nullptr)
    {
        LOG_TRACE("Hooking slGetPluginFunction in sl.common");
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        DetourAttach(&(PVOID&) o_common_slGetPluginFunction, hkcommon_slGetPluginFunction);

        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to hook Common: {:X}", detourResult);
            o_common_slGetPluginFunction = nullptr;
        }
    }
}

bool StreamlineHooks::isInterposerHooked() { return o_slInit != nullptr || o_slInit_sl1 != nullptr; }

bool StreamlineHooks::isDlssHooked() { return o_dlss_slGetPluginFunction != nullptr; }

bool StreamlineHooks::isDlssgHooked() { return o_dlssg_slGetPluginFunction != nullptr; }

bool StreamlineHooks::isLocalDlssgHooked() { return o_local_dlssg_slGetPluginFunction != nullptr; }

bool StreamlineHooks::isCommonHooked() { return o_common_slGetPluginFunction != nullptr; }

bool StreamlineHooks::isPclHooked() { return o_pcl_slGetPluginFunction != nullptr; }

bool StreamlineHooks::isReflexHooked() { return o_reflex_slGetPluginFunction != nullptr; }
