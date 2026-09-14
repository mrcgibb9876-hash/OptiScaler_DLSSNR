#include "pch.h"
#include "menu_dx12.h"

#include <d3dx/d3dx12.h>

#include "Config.h"
#include "menu_common.h"
#include <imgui/imgui_impl_dx12.h>
#include <imgui/imgui_impl_win32.h>

long frameCounter = 0;
static int const SRV_HEAP_SIZE = 64;

struct ImGui_ImplDX12_Texture
{
    ID3D12Resource* pTextureResource;
    D3D12_CPU_DESCRIPTOR_HANDLE hFontSrvCpuDescHandle;
    D3D12_GPU_DESCRIPTOR_HANDLE hFontSrvGpuDescHandle;

    ImGui_ImplDX12_Texture() { memset((void*) this, 0, sizeof(*this)); }
};

struct ImGui_ImplDX12_Data
{
    ImGui_ImplDX12_InitInfo InitInfo;
    ID3D12Device* pd3dDevice;
    ID3D12RootSignature* pRootSignature;
    ID3D12PipelineState* pPipelineState;
    ID3D12CommandQueue* pCommandQueue;
    bool commandQueueOwned;
    DXGI_FORMAT RTVFormat;
    DXGI_FORMAT DSVFormat;
    ID3D12DescriptorHeap* pd3dSrvDescHeap;
    UINT numFramesInFlight;
    ImGui_ImplDX12_Texture FontTexture;
    bool LegacySingleDescriptorUsed;

    ImGui_ImplDX12_Data() { memset((void*) this, 0, sizeof(*this)); }
};

// The menu object whose descriptor heap and device the shared DX12 backend was built on. Only that
// object may shut the backend down, and any other object has to build its own before drawing: its
// render path binds its own _srvDescHeap, so borrowing another object's backend would draw with the
// wrong heap. Before this, a menu created while an older one's backend was still up never
// initialised at all (the init only ran when no backend existed), and the older one's destructor
// then shut that backend down anyway.
static Menu_Dx12* s_dx12BackendOwner = nullptr;

bool Menu_Dx12::Render(ID3D12GraphicsCommandList* pCmdList, ID3D12Resource* outTexture, D3D12_RESOURCE_STATES outState)
{
    if (Config::Instance()->OverlayMenu.value_or_default())
        return false;

    if (pCmdList == nullptr || outTexture == nullptr)
        return false;

    // ImGui::GetIO() below dereferences the global context, so there has to be one. It can genuinely
    // be missing: MenuDxBase's constructor skips MenuCommon::Init -- the only thing that creates the
    // context -- when OverlayMenu is true, and a per-game quirk can then force this old overlay by
    // setting OverlayMenu to false afterwards, at which point the early-out above stops firing and
    // we arrive here with nothing set up. Whether it happens varies by launch, because when the
    // swapchain overlay gets there first it creates the context and the question never arises.
    if (ImGui::GetCurrentContext() == nullptr)
    {
        LOG_WARN("No ImGui context on the old overlay path -- not drawing the menu this frame");
        return false;
    }

    frameCounter++;

    auto outDesc = outTexture->GetDesc();

    CreateRenderTarget(outDesc);

    ImGuiIO& io = ImGui::GetIO();
    (void) io;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

    // Our backend was shut down under us (by the object that owned it): build it again before drawing.
    // A backend that belongs to another, still-live menu object is never taken over -- replacing it
    // while that object still held its heap crashed the game on the spot (2026-09-14). This object just
    // waits, drawing nothing, until the other lets go; the shared context keeps the keys working.
    if (_dx12Init && (io.BackendRendererUserData == nullptr || s_dx12BackendOwner != this))
        _dx12Init = false;

    if (!_dx12Init && io.BackendRendererUserData != nullptr)
    {
        static bool saidWaiting = false;
        if (!saidWaiting)
        {
            saidWaiting = true;
            LOG_DEBUG("Menu waiting for the previous menu object to release its DX12 backend");
        }

        return false;
    }

    if (!_dx12Init && io.BackendRendererUserData == nullptr)
    {
        ImGui_ImplDX12_InitInfo initInfo {};
        initInfo.Device = _device;

        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queueDesc.NodeMask = 1;
        HRESULT hr = _device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&initInfo.CommandQueue));
        IM_ASSERT(SUCCEEDED(hr));

        initInfo.NumFramesInFlight = 2;
        initInfo.RTVFormat = outDesc.Format;
        initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
        initInfo.SrvDescriptorHeap = _srvDescHeap;
        initInfo.UserData = &_srvDescHeapAlloc;
        initInfo.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu_handle,
                                           D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu_handle)
        { return static_cast<DescriptorHeapAllocator*>(info->UserData)->Alloc(out_cpu_handle, out_gpu_handle); };
        initInfo.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle,
                                          D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle)
        { return static_cast<DescriptorHeapAllocator*>(info->UserData)->Free(cpu_handle, gpu_handle); };

        _dx12Init = ImGui_ImplDX12_Init(&initInfo);

        if (_dx12Init)
            s_dx12BackendOwner = this;

        ImGui_ImplDX12_Data* bd =
            ImGui::GetCurrentContext() ? (ImGui_ImplDX12_Data*) ImGui::GetIO().BackendRendererUserData : nullptr;
        if (bd)
            bd->commandQueueOwned = true;
    }

    if (!_dx12Init)
        return false;

    frameCounter++;

    auto backbuf = frameCounter % 2;

    D3D12_RENDER_TARGET_VIEW_DESC rtDesc = {};
    rtDesc.Format = outDesc.Format;
    rtDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

    // What the caller actually handed us, and which of the two paths below that selects. Worth one
    // line because the two paths make very different demands of a texture this code does not own.
    static bool loggedOutTexture = false;
    if (!loggedOutTexture)
    {
        loggedOutTexture = true;
        LOG_WARN("Old overlay target: {}x{} fmt {} flags {:X} -> {} path", (UINT) outDesc.Width, (UINT) outDesc.Height,
                 (UINT) outDesc.Format, (UINT) outDesc.Flags,
                 (outDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) > 0 ? "render-target" : "copy");
    }

    if ((outDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) > 0)
    {
        ImGui_ImplDX12_NewFrame();
        // ImGui_ImplWin32_NewFrame();

        // Build the frame BEFORE touching the caller's command list, and stop here if there was
        // nothing to draw. RenderMenu() has to run every frame either way: it is also where the
        // menu shortcut keys are read, so gating it on the menu already being visible means no key
        // can ever open anything.
        //
        // Everything below used to run unconditionally, once per frame, open menu or not: a
        // transition of outTexture, a descriptor-heap rebind on the caller's command list, a render
        // target swap and a full ImGui pass. Wasted work at best, and on a texture this code does
        // not own it is fatal -- a game whose upscaler output belongs to another plugin died a few
        // hundred frames in with nobody having pressed a key, faulting inside the D3D12 driver
        // (2026-09-13).
        if (!MenuDxBase::RenderMenu())
            return true;

        D3D12_RESOURCE_BARRIER outBarrier = {};
        outBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        outBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        outBarrier.Transition.pResource = outTexture;
        outBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        outBarrier.Transition.StateBefore = outState;
        outBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        pCmdList->ResourceBarrier(1, &outBarrier);

        // Create RTV for out
        pCmdList->SetDescriptorHeaps(1, &_srvDescHeap);

        _device->CreateRenderTargetView(outTexture, &rtDesc, _renderTargetDescriptor[backbuf]);
        pCmdList->OMSetRenderTargets(1, &_renderTargetDescriptor[backbuf], FALSE, NULL);

        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), pCmdList);

        outBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        outBarrier.Transition.StateAfter = outState;
        pCmdList->ResourceBarrier(1, &outBarrier);

        return true;
    }

    // Same order, and for the same reasons, as the render-target path above. This one additionally
    // used to strand outTexture in COPY_SOURCE whenever there was nothing to draw: it transitioned
    // first and only restored UNORDERED_ACCESS inside the branch that actually drew.
    ImGui_ImplDX12_NewFrame();
    // ImGui_ImplWin32_NewFrame();

    if (!MenuDxBase::RenderMenu())
        return true;

    D3D12_RESOURCE_BARRIER outBarrier = {};
    outBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    outBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    outBarrier.Transition.pResource = outTexture;
    outBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    outBarrier.Transition.StateBefore = outState;
    outBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

    D3D12_RESOURCE_BARRIER bufferBarrier = {};
    bufferBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bufferBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    bufferBarrier.Transition.pResource = _renderTargetResource[backbuf];
    bufferBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bufferBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bufferBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;

    D3D12_RESOURCE_BARRIER barriers[] = { bufferBarrier, outBarrier };
    pCmdList->ResourceBarrier(2, barriers);

    // Copy out to buffer
    pCmdList->CopyResource(_renderTargetResource[backbuf], outTexture);

    // Set as render target
    bufferBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    bufferBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    pCmdList->ResourceBarrier(1, &bufferBarrier);

    // Create RTV for buffer
    pCmdList->SetDescriptorHeaps(1, &_srvDescHeap);

    _device->CreateRenderTargetView(_renderTargetResource[backbuf], &rtDesc, _renderTargetDescriptor[backbuf]);
    pCmdList->OMSetRenderTargets(1, &_renderTargetDescriptor[backbuf], FALSE, nullptr);

    // Render to buffer
    {
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), pCmdList);

        outBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        outBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;

        bufferBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bufferBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        pCmdList->ResourceBarrier(2, barriers);

        // Copy back buffer to out
        pCmdList->CopyResource(outTexture, _renderTargetResource[backbuf]);

        // fix states
        bufferBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        bufferBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

        outBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        outBarrier.Transition.StateAfter = outState;
        pCmdList->ResourceBarrier(2, barriers);
    }

    return true;
}

Menu_Dx12::Menu_Dx12(HWND handle, ID3D12Device* pDevice) : MenuDxBase(handle), _device(pDevice)
{
    if (Config::Instance()->OverlayMenu.value_or_default())
        return;

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = 2;
    rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    rtvDesc.NodeMask = 1;

    if (pDevice->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&_rtvDescHeap)) != S_OK)
        return;

    _rtvDescHeap->SetName(L"Imgui_Dx12_rtvHeap");

    SIZE_T rtvDescriptorSize = pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = _rtvDescHeap->GetCPUDescriptorHandleForHeapStart();

    for (UINT i = 0; i < 2; ++i)
    {
        _renderTargetDescriptor[i] = rtvHandle;
        rtvHandle.ptr += rtvDescriptorSize;
    }

    D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.NumDescriptors = SRV_HEAP_SIZE;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    {
        ScopedSkipHeapCapture skipHeapCapture {};

        if (pDevice->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&_srvDescHeap)) != S_OK)
            return;
    }

    // This used to be one allocator shared by every menu object, pointed at the newest object's heap
    // here. When an upscaler is recreated, the new feature's menu is built while the old one's backend
    // is still up; the old one then shuts that backend down and frees its font descriptors -- slots in
    // its own heap -- into an allocator that now indexes the new heap. The new backend is then built on
    // a corrupted free list and draws with descriptors that point nowhere. Resident Evil 2 recreates its
    // upscaler on the first real frame and died about two seconds later, every launch, with the device
    // removed or an access violation in this module, whichever came first (2026-09-14).
    _srvDescHeapAlloc.Create(pDevice, _srvDescHeap);

    Dx12Ready();

    _srvDescHeap->SetName(L"Imgui_Dx12_srvDescHeap");
}

Menu_Dx12::~Menu_Dx12()
{
    // g_pd3dSrvDescHeapAlloc.Destroy(); // Can cause a crash on app close, unsure why

    if (!_dx12Init)
        return;

    // On shutting down don't invalidate device objects. Only the object that built the backend may
    // tear it down -- a newer menu may already be drawing with its own.
    if (s_dx12BackendOwner == this && ImGui::GetCurrentContext() != nullptr)
    {
        // shutdown_platform stays false, as it is in the DX11 menu: true runs DestroyPlatformWindows, which
        // frees the main viewport's Win32 data under whichever menu object is still using the shared
        // context. The next frame's DPI query then reads a null window and faults (Resident Evil 2,
        // c0000005 two seconds after its upscaler was recreated, 2026-09-14). The Win32 backend does its
        // own DestroyPlatformWindows when the last menu object lets the context go.
        ImGui_ImplDX12_Shutdown(false, !State::Instance().isShuttingDown);
        s_dx12BackendOwner = nullptr;
    }

    // MenuCommon::Shutdown is ~MenuDxBase's to call, once per menu object. Calling it here as well
    // counted this object twice against the shared context and destroyed it under the next menu.

    SAFE_RELEASE(_rtvDescHeap);
    SAFE_RELEASE(_srvDescHeap);
    SAFE_RELEASE(_renderTargetResource[0]);
    SAFE_RELEASE(_renderTargetResource[1]);
}

void Menu_Dx12::CreateRenderTarget(const D3D12_RESOURCE_DESC& InDesc)
{
    if (_renderTargetResource[0] != nullptr)
    {
        auto rtDesc = _renderTargetResource[0]->GetDesc();

        if (InDesc.Width != rtDesc.Width || InDesc.Height != rtDesc.Height || InDesc.Format != rtDesc.Format)
        {
            SAFE_RELEASE(_renderTargetResource[0]);
            SAFE_RELEASE(_renderTargetResource[1]);
        }
        else
            return;
    }

    for (UINT i = 0; i < 2; ++i)
    {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = InDesc.Width;
        desc.Height = InDesc.Height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = TranslateTypelessFormats(InDesc.Format);
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_HEAP_PROPERTIES heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

        ID3D12Resource* renderTarget;
        auto result =
            _device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&renderTarget));

        if (result == S_OK)
        {
            renderTarget->SetName(L"Imgui_Dx12_renderTarget");

            D3D12_RENDER_TARGET_VIEW_DESC rtDesc = {};
            rtDesc.Format = InDesc.Format;
            rtDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

            _device->CreateRenderTargetView(renderTarget, &rtDesc, _renderTargetDescriptor[i]);
            _renderTargetResource[i] = renderTarget;
        }
    }
}
