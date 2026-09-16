#include "pch.h"

#include "DlssNr_PresentRoute.h"
#include "DlssNrFeature_Dx12.h"
#include "DlssNr_DepthTracker.h"

#include <Config.h>
#include <State.h>
#include <SysUtils.h>
#include <misc/Quirks.h>
#include <proxies/Dxgi_Proxy.h>

#include <dxgi1_4.h>

#include <detours/detours.h>

#include <atomic>
#include <chrono>
#include <format>
#include <mutex>
#include <string>

namespace
{
// IDXGISwapChain vtable slots (IUnknown 0-2, IDXGIObject 3-6, IDXGIDeviceSubObject 7, IDXGISwapChain 8-17,
// IDXGISwapChain1 18-28). Present is 8, Present1 is 22.
constexpr size_t kSlotPresent = 8;
constexpr size_t kSlotPresent1 = 22;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

PresentFn g_originalPresent = nullptr;
Present1Fn g_originalPresent1 = nullptr;

// ID3D12CommandQueue vtable: IUnknown 0-2, ID3D12Object 3-6, ID3D12DeviceChild 7, then
// UpdateTileMappings 8, CopyTileMappings 9, ExecuteCommandLists 10.
constexpr size_t kSlotExecuteCommandLists = 10;
using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
ExecuteCommandListsFn g_originalExecuteCommandLists = nullptr;

std::atomic<unsigned long long> g_presents { 0 };
std::atomic<bool> g_installed { false };
std::atomic<bool> g_gaveUp { false };
std::mutex g_installMutex;

// The game's upscale list, and the queue it was last seen going out on. The queue is held with a
// reference of our own; a replaced one is deliberately never released, because a Present on another
// thread may be using it at that moment and a queue leaked on the rare switch costs nothing.
std::atomic<ID3D12CommandList*> g_upscaleList { nullptr };

// Every queue the game has submitted work on, as identities only -- never dereferenced and never referenced.
// Holding them kept the game's queues, and so its device, alive past its own teardown, and Resident Evil 2
// then faulted in the driver on exit. The queue actually used is the one read out of the swapchain being
// presented, which the swapchain itself keeps alive; this list only confirms that pointer is a real queue,
// so a wrong offset can never hand the pass a stray pointer.
constexpr size_t kMaxQueues = 16;
std::atomic<ID3D12CommandQueue*> g_seenQueues[kMaxQueues] {};
std::atomic<size_t> g_seenQueueCount { 0 };
std::mutex g_seenQueueMutex;

bool QueueSeen(const ID3D12CommandQueue* queue)
{
    const size_t count = g_seenQueueCount.load(std::memory_order_acquire);

    for (size_t i = 0; i < count; ++i)
    {
        if (g_seenQueues[i].load(std::memory_order_relaxed) == queue)
            return true;
    }

    return false;
}

void STDMETHODCALLTYPE HookedExecuteCommandLists(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    if (queue != nullptr && !QueueSeen(queue))
    {
        std::lock_guard<std::mutex> lock(g_seenQueueMutex);
        const size_t seen = g_seenQueueCount.load(std::memory_order_acquire);

        if (!QueueSeen(queue) && seen < kMaxQueues && queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
        {
            g_seenQueues[seen].store(queue, std::memory_order_relaxed);
            g_seenQueueCount.store(seen + 1, std::memory_order_release);
            LOG_DEBUG("DLSS-NR Present route: direct queue {} seen submitting", static_cast<void*>(queue));
        }
    }

    g_originalExecuteCommandLists(queue, count, lists);
}

// Where a D3D12 swapchain keeps the queue it presents on, found on the probe by looking for the probe's
// own queue -- the way REFramework finds it ("Found command queue offset"). The upscale list's queue is
// only a guess at the present queue: an engine with more than one direct queue submits the frame on one
// and presents on another, and a pass that writes the back buffer from the wrong one races the game's
// own writes to it and hangs the GPU. That is what DEVICE_HUNG after 133 frames of Resident Evil 2 was.
size_t g_queueOffset = 0;
constexpr size_t kQueueScanSlots = 512;

// Plain function: __try cannot share a frame with objects that unwind.
size_t FindPointerOffset(void* object, void* value)
{
    __try
    {
        const auto* const slots = static_cast<void**>(object);

        for (size_t i = 1; i < kQueueScanSlots; ++i)
        {
            if (slots[i] == value)
                return i * sizeof(void*);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    return 0;
}

ID3D12CommandQueue* ReadPointerAt(void* object, size_t offset)
{
    __try
    {
        return *static_cast<ID3D12CommandQueue**>(static_cast<void*>(static_cast<char*>(object) + offset));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

// Present1 may call Present inside DXGI, and a game can present from inside its own hooks; the pass
// must run once per frame whichever way round that happens.
thread_local bool t_inPresent = false;

void OnPresent(IDXGISwapChain* swapChain, UINT flags)
{
    if (t_inPresent || swapChain == nullptr || (flags & DXGI_PRESENT_TEST) != 0)
        return;

    t_inPresent = true;

    // Only D3D12 swapchains have a command queue behind them. A D3D11 swapchain of the same class goes
    // through untouched.
    IDXGISwapChain3* swapChain3 = nullptr;

    if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3))) && swapChain3 != nullptr)
    {
        // A D3D12 swapchain answers GetDevice with the device, not the queue. The queue is read out of the
        // swapchain where the probe showed it is kept; it is only trusted when ExecuteCommandLists has
        // seen that same queue carry the game's upscale list, so a wrong offset can never hand the pass a
        // stray pointer. A D3D11 swapchain has no D3D12 device and goes through.
        ID3D12CommandQueue* queue = nullptr;
        ID3D12Device* device = nullptr;

        {
            DXGI_SWAP_CHAIN_DESC1 desc {};
            if (SUCCEEDED(swapChain3->GetDesc1(&desc)))
                DlssNr::DepthTracker::EndFrame(desc.Width, desc.Height);
        }

        if (g_queueOffset != 0)
        {
            ID3D12CommandQueue* const presentQueue = ReadPointerAt(swapChain, g_queueOffset);

            if (presentQueue != nullptr && QueueSeen(presentQueue))
            {
                queue = presentQueue;

                static ID3D12CommandQueue* said = nullptr;
                if (said != presentQueue)
                {
                    said = presentQueue;
                    LOG_INFO("DLSS-NR Present route: the swapchain presents on queue {}", static_cast<void*>(queue));
                }
            }
        }

        if (queue != nullptr && SUCCEEDED(swapChain3->GetDevice(IID_PPV_ARGS(&device))) && device != nullptr)
        {
            device->Release();
            queue->AddRef();

            const unsigned long long presentIndex = ++g_presents;

            if (presentIndex == 1)
                LOG_INFO("DLSS-NR Present route: first Present seen (swapchain {}, thread {})",
                         static_cast<void*>(swapChain), GetCurrentThreadId());

            const auto started = std::chrono::steady_clock::now();
            DlssNr::RunAtPresent(swapChain3, queue, presentIndex);
            const auto ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

            static unsigned slowReports = 0;
            if (ms > 50.0 && slowReports < 5)
            {
                ++slowReports;
                LOG_WARN("DLSS-NR Present route: the pass held Present {} for {:.1f} ms", presentIndex, ms);
            }

            queue->Release();
        }
        else
        {
            static bool saidNoQueue = false;
            if (!saidNoQueue)
            {
                saidNoQueue = true;
                LOG_INFO("DLSS-NR Present route: a Present went through untouched ({})",
                         queue == nullptr ? "the swapchain's queue has not been seen submitting yet"
                                          : "not a D3D12 swapchain");
            }
        }

        swapChain3->Release();
    }

    t_inPresent = false;
}

// A removed device is otherwise only "Present failed" in someone else's log. The reason code, and
// whether this pass had run at all by then, is what tells a fault of ours from one that was already
// there.
void ReportPresentResult(IDXGISwapChain* swapChain, HRESULT hr)
{
    if (hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET && hr != DXGI_ERROR_DEVICE_HUNG)
        return;

    static std::atomic<bool> said { false };
    if (said.exchange(true))
        return;

    HRESULT reason = S_OK;
    ID3D12Device* device = nullptr;

    if (swapChain != nullptr && SUCCEEDED(swapChain->GetDevice(IID_PPV_ARGS(&device))) && device != nullptr)
    {
        reason = device->GetDeviceRemovedReason();
        device->Release();
    }

    LOG_ERROR("DLSS-NR Present route: Present returned 0x{:X}, device removed reason 0x{:X}, after {} Presents "
              "through the hook",
              (unsigned int) hr, (unsigned int) reason, g_presents.load());
}

HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
{
    OnPresent(swapChain, flags);
    const HRESULT hr = g_originalPresent(swapChain, syncInterval, flags);
    ReportPresentResult(swapChain, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookedPresent1(IDXGISwapChain1* swapChain, UINT syncInterval, UINT flags,
                                         const DXGI_PRESENT_PARAMETERS* parameters)
{
    OnPresent(swapChain, flags);
    const HRESULT hr = g_originalPresent1(swapChain, syncInterval, flags, parameters);
    ReportPresentResult(swapChain, hr);
    return hr;
}

HMODULE ModuleOf(void* address)
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(address), &module);
    return module;
}

// "dxgi.dll" or "dinput8.dll" rather than an address nobody can look up after the process has gone.
std::string ModuleName(void* address)
{
    const HMODULE module = ModuleOf(address);
    wchar_t path[MAX_PATH] {};

    if (module == nullptr || GetModuleFileNameW(module, path, MAX_PATH) == 0)
        return std::format("unknown module ({})", address);

    std::wstring name(path);
    const size_t slash = name.find_last_of(L"\\/");
    return wstring_to_string(slash == std::wstring::npos ? name : name.substr(slash + 1));
}

bool GiveUp(const char* why)
{
    g_gaveUp = true;
    LOG_WARN("DLSS-NR Present route unavailable: {} -- the pass stays at the upscaler call", why);
    return false;
}
} // namespace

namespace DlssNr::PresentRoute
{
bool Wanted()
{
    const std::wstring placement = Config::Instance()->DlssNrPlacement.value_or_default();

    if (placement == L"present")
        return true;

    if (placement == L"evaluate")
        return false;

    // auto: the games whose swapchain this app is known not to wrap -- unless the DLSS5 Feeder is loaded. The
    // Feeder makes a DLSS call of its own for the pass to ride, and its ReShade wraps the D3D12 device, so the
    // Present route would find the swapchain on a device that does not match and run nothing at all: Devil
    // May Cry 5 with the Feeder deployed, 2026-09-14. "present" in the ini still forces it.
    if (DlssNr::IsFeederPresent())
        return false;

    return static_cast<bool>(State::Instance().gameQuirks & GameQuirk::OldOverlayMenu);
}

bool EnsureInstalled(ID3D12Device* device)
{
    if (g_installed)
        return true;

    if (g_gaveUp || device == nullptr)
        return false;

    std::lock_guard<std::mutex> lock(g_installMutex);

    if (g_installed)
        return true;

    if (g_gaveUp)
        return false;

    if (DxgiProxy::Module() == nullptr || DxgiProxy::CreateDxgiFactory2_() == nullptr)
        return GiveUp("the system DXGI could not be reached");

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const wchar_t* className = L"OptiScalerDlssNrPresentProbe";

    WNDCLASSEXW wc {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = instance;
    wc.lpszClassName = className;
    RegisterClassExW(&wc);

    HWND window =
        CreateWindowExW(0, className, L"", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, nullptr, nullptr, instance, nullptr);

    if (window == nullptr)
    {
        UnregisterClassW(className, instance);
        return GiveUp("a probe window could not be created");
    }

    ID3D12CommandQueue* queue = nullptr;
    IDXGIFactory2* factory = nullptr;
    IDXGISwapChain1* probe = nullptr;

    const auto cleanup = [&]()
    {
        if (probe != nullptr)
            probe->Release();
        if (factory != nullptr)
            factory->Release();
        if (queue != nullptr)
            queue->Release();
        DestroyWindow(window);
        UnregisterClassW(className, instance);
    };

    D3D12_COMMAND_QUEUE_DESC queueDesc {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))) || queue == nullptr)
    {
        cleanup();
        return GiveUp("a probe command queue could not be created");
    }

    if (FAILED(DxgiProxy::CreateDxgiFactory2_()(0, __uuidof(IDXGIFactory2), &factory)) || factory == nullptr)
    {
        cleanup();
        return GiveUp("a DXGI factory could not be created");
    }

    DXGI_SWAP_CHAIN_DESC1 desc {};
    desc.Width = 16;
    desc.Height = 16;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    // Nothing else is allowed to learn about this swapchain: a hook that wraps it would hand back a
    // wrapper's vtable, not DXGI's.
    IDXGISwapChain* const wrappedBefore = State::Instance().currentWrappedSwapchain;
    IDXGISwapChain* const swapchainBefore = State::Instance().currentSwapchain;

    if (FAILED(factory->CreateSwapChainForHwnd(queue, window, &desc, nullptr, nullptr, &probe)) || probe == nullptr)
    {
        cleanup();
        return GiveUp("a probe swapchain could not be created");
    }

    State::Instance().currentWrappedSwapchain = wrappedBefore;
    State::Instance().currentSwapchain = swapchainBefore;

    void** vtable = *static_cast<void***>(static_cast<void*>(probe));

    // Patched only if the slot is not this app's own wrapper: patching that would hook the wrapper class
    // and miss the game's real swapchain. Windows' dxgi.dll is expected; another module is logged.
    const HMODULE presentOwner = ModuleOf(vtable[kSlotPresent]);

    if (presentOwner == ModuleOf(static_cast<void*>(&GiveUp)))
    {
        cleanup();
        return GiveUp("the probe swapchain came back wrapped by this app");
    }

    // The functions are hooked, not the table. Writing the shared vtable entry was the first attempt and it
    // took Resident Evil 2 to one frame a second: REFramework owns that entry and watches it. Every caller --
    // the shared table, a per-object copy of it, an overlay that looked the address up once and kept it --
    // ends up in these functions, so detouring them catches all of them and changes nothing anyone can see.
    // This is how DXL hooks Present (PresentInlineHooks.h).
    g_originalPresent = static_cast<PresentFn>(vtable[kSlotPresent]);
    g_originalPresent1 = static_cast<Present1Fn>(vtable[kSlotPresent1]);

    const std::string presentModule = ModuleName(g_originalPresent);
    const std::string present1Module = ModuleName(g_originalPresent1);

    g_queueOffset = FindPointerOffset(probe, queue);

    if (g_queueOffset == 0)
    {
        cleanup();
        return GiveUp("the swapchain's command queue could not be located in the probe");
    }

    // Every D3D12 queue runs the same ExecuteCommandLists, so the probe queue shows where it is.
    g_originalExecuteCommandLists = static_cast<ExecuteCommandListsFn>(
        (*static_cast<void***>(static_cast<void*>(queue)))[kSlotExecuteCommandLists]);

    cleanup();

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    const LONG attachPresent = DetourAttach(&(PVOID&) g_originalPresent, HookedPresent);
    const LONG attachPresent1 = DetourAttach(&(PVOID&) g_originalPresent1, HookedPresent1);
    const LONG attachExecute = DetourAttach(&(PVOID&) g_originalExecuteCommandLists, HookedExecuteCommandLists);

    if (attachPresent != NO_ERROR || attachPresent1 != NO_ERROR || attachExecute != NO_ERROR)
    {
        DetourTransactionAbort();
        LOG_WARN("DLSS-NR Present route: detour attach failed ({} / {} / {})", attachPresent, attachPresent1,
                 attachExecute);
        return GiveUp("the Present functions could not be detoured");
    }

    const LONG committed = DetourTransactionCommit();

    if (committed != NO_ERROR)
    {
        LOG_WARN("DLSS-NR Present route: detour commit failed ({})", committed);
        return GiveUp("the Present functions could not be detoured");
    }

    g_installed = true;

    LOG_INFO("DLSS-NR Present route: Present in {} and Present1 in {} detoured, swapchain queue at offset 0x{:X} -- "
             "the pass now runs at Present on a command list of its own",
             presentModule, present1Module, g_queueOffset);
    return true;
}

bool Active() { return g_installed && g_presents > 0; }

unsigned long long PresentCount() { return g_presents; }

void NoteUpscaleList(ID3D12GraphicsCommandList* cmdList) { g_upscaleList.store(cmdList, std::memory_order_relaxed); }

void PrepareForDevice(ID3D12Device* device)
{
    if (device == nullptr || !Config::Instance()->DlssNrEnabled.value_or_default() || !Wanted())
        return;

    // A game with no upscale call never reaches the evaluate-side install, so both go in as soon as its device
    // exists: the depth tracker has to be watching before the first frame is drawn.
    DlssNr::DepthTracker::Install(device);
    EnsureInstalled(device);
}
} // namespace DlssNr::PresentRoute
