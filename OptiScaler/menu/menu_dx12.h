#pragma once

#include "menu_dx_base.h"
#include <d3d12.h>
#include <imgui/imgui_impl_dx12.h>

class Menu_Dx12 : public MenuDxBase
{
  private:
    bool _dx12Init = false;
    ID3D12Device* _device = nullptr;

    // Old resources
    ID3D12Resource* _renderTargetResource[2] = { nullptr, nullptr };
    ID3D12DescriptorHeap* _rtvDescHeap = nullptr;
    ID3D12DescriptorHeap* _srvDescHeap = nullptr;

    // Hands out slots in _srvDescHeap and nowhere else -- one per object, see the constructor.
    DescriptorHeapAllocator _srvDescHeapAlloc;
    D3D12_CPU_DESCRIPTOR_HANDLE _renderTargetDescriptor[2] = {};

    void CreateRenderTarget(const D3D12_RESOURCE_DESC& InDesc);

  public:
    // outState is the state outTexture arrives in and is left in: an upscaler's output is a UAV, a back
    // buffer drawn on at Present (the DLSS-NR Present route) is in PRESENT.
    bool Render(ID3D12GraphicsCommandList* pCmdList, ID3D12Resource* outTexture,
                D3D12_RESOURCE_STATES outState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    Menu_Dx12(HWND handle, ID3D12Device* pDevice);

    ~Menu_Dx12();
};
