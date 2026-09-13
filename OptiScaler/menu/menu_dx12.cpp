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

static DescriptorHeapAllocator g_pd3dSrvDescHeapAlloc;

bool Menu_Dx12::Render(ID3D12GraphicsCommandList* pCmdList, ID3D12Resource* outTexture)
{
    if (Config::Instance()->OverlayMenu.value_or_default())
        return false;

    if (pCmdList == nullptr || outTexture == nullptr)
        return false;

    // Nothing on screen to draw, so do not touch the caller's texture or command list at all.
    //
    // This early-out exists a few lines below as dead commented-out code; restoring it matters
    // because everything after this point runs unconditionally today, once per frame, whether or
    // not a menu is open: it transitions outTexture, rebinds descriptor heaps on the caller's
    // command list, binds a render target view and runs a full ImGui pass. That is wasted work
    // when nothing is visible, and on a texture this code does not own it is fatal -- Resident
    // Evil 2 through PureDark's plugin died at ~444 upscaled frames with nobody having pressed a
    // key, faulting in nvwgf2umx.dll (2026-09-13).
    //
    // Reading the flags needs no ImGui context (they are two statics), and menu shortcuts are
    // handled off this path, so keys still work while this returns early.
    if (!MenuDxBase::IsVisible())
        return true;

    // Make sure there is an ImGui context before ImGui::GetIO() below dereferences the null global
    // and takes the process with it.
    //
    // It can genuinely be missing here, because two pieces of code disagree about which overlay is
    // in use. MenuDxBase's constructor skips MenuCommon::Init -- the only thing that creates the
    // context -- when OverlayMenu is true, and a per-game quirk can then force the old overlay by
    // setting OverlayMenu to false afterwards. Once it does, the early-out at the top of this
    // function stops firing and we arrive here with no context at all.
    //
    // Resident Evil 2 through REFramework and PureDark's plugin hits exactly that: the quirk logs
    // "Using old overlay (draws on upscaled image)" and the first menu frame crashes on the null
    // context (2026-09-13). Whether it happens at all varies by launch, because when OptiScaler wins
    // the race to wrap the swapchain the overlay path initialises the context first and this is moot.
    //
    // Deliberately bail rather than create the context here. Creating it was tried, and it works --
    // the context comes up, the panel toggles, input arrives -- but the draw that follows then dies
    // inside nvwgf2umx.dll instead. This path assumes it owns the texture it is drawing into: it
    // transitions it with StateBefore=UNORDERED_ACCESS and rebinds descriptor heaps on the caller's
    // command list. Both are true for OptiScaler's own upscaler output and neither is guaranteed for
    // a texture some other plugin created and still owns. Fixing that is a bigger change than a
    // missing null check, so until then this leaves the game running without an overlay on this
    // path, instead of taking it down.
    if (ImGui::GetCurrentContext() == nullptr)
    {
        LOG_WARN("No ImGui context on the old overlay path -- not drawing the menu this frame");
        return false;
    }

    frameCounter++;

    // if (!IsVisible())
    //	return true;

    auto outDesc = outTexture->GetDesc();

    CreateRenderTarget(outDesc);

    ImGuiIO& io = ImGui::GetIO();
    (void) io;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

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
        initInfo.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu_handle,
                                           D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu_handle)
        { return g_pd3dSrvDescHeapAlloc.Alloc(out_cpu_handle, out_gpu_handle); };
        initInfo.SrvDescriptorFreeFn =
            [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle, D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle)
        { return g_pd3dSrvDescHeapAlloc.Free(cpu_handle, gpu_handle); };

        _dx12Init = ImGui_ImplDX12_Init(&initInfo);

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

    if ((outDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) > 0)
    {
        D3D12_RESOURCE_BARRIER outBarrier = {};
        outBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        outBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        outBarrier.Transition.pResource = outTexture;
        outBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        outBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        outBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        pCmdList->ResourceBarrier(1, &outBarrier);

        // Create RTV for out
        pCmdList->SetDescriptorHeaps(1, &_srvDescHeap);

        _device->CreateRenderTargetView(outTexture, &rtDesc, _renderTargetDescriptor[backbuf]);
        pCmdList->OMSetRenderTargets(1, &_renderTargetDescriptor[backbuf], FALSE, NULL);

        ImGui_ImplDX12_NewFrame();
        // ImGui_ImplWin32_NewFrame();

        // Render
        if (MenuDxBase::RenderMenu())
            ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), pCmdList);

        outBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        outBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        pCmdList->ResourceBarrier(1, &outBarrier);

        return true;
    }

    D3D12_RESOURCE_BARRIER outBarrier = {};
    outBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    outBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    outBarrier.Transition.pResource = outTexture;
    outBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    outBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
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

    ImGui_ImplDX12_NewFrame();
    // ImGui_ImplWin32_NewFrame();

    // Render to buffer
    if (MenuDxBase::RenderMenu())
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
        outBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
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

    g_pd3dSrvDescHeapAlloc.Destroy();
    g_pd3dSrvDescHeapAlloc.Create(pDevice, _srvDescHeap);

    Dx12Ready();

    _srvDescHeap->SetName(L"Imgui_Dx12_srvDescHeap");
}

Menu_Dx12::~Menu_Dx12()
{
    // g_pd3dSrvDescHeapAlloc.Destroy(); // Can cause a crash on app close, unsure why

    if (!_dx12Init)
        return;

    // On shutting down don't invalidate device objects
    ImGui_ImplDX12_Shutdown(true, !State::Instance().isShuttingDown);
    MenuCommon::Shutdown();

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
