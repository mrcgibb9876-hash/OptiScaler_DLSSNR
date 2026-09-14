// FidelityFX Optical Flow wrapped for the DLSS-NR Present route. See dlssnr_opticalflow.h.
//
// Adapted from LCPD15's DXL, src/core/OpticalFlow.cpp (AGPL-3.0): the same FidelityFX Optical Flow 1.1.2
// subset, the same precompiled kernels, the same densify pass that turns the 8x8-block flow into a
// per-pixel field. What differs: the box downsample is a shader of this module's own, compiled at runtime,
// instead of DXL's shared compute-pass library, and there is a C interface in place of a class.

#include "dlssnr_opticalflow.h"

#include <FidelityFX/host/ffx_opticalflow.h>
#include <FidelityFX/host/backends/dx12/ffx_dx12.h>

#include <d3dcompiler.h>
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include "shaders/OpticalDensify.h"

namespace
{
const char* g_staticError = nullptr;

constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto kWrite = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES from,
                D3D12_RESOURCE_STATES to)
{
    if (from == to)
        return;

    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to };
    list->ResourceBarrier(1, &barrier);
}

FfxResource Wrap(ID3D12Resource* resource, FfxResourceStates state)
{
    return ffxGetResourceDX12(resource, ffxGetResourceDescriptionDX12(resource), L"OptiDlssNr optical flow", state);
}

void SafeRelease(IUnknown*& object)
{
    if (object != nullptr)
    {
        object->Release();
        object = nullptr;
    }
}

// Exact area average: each target pixel covers [id*Scale, (id+1)*Scale) of the source and every source
// pixel is weighted by its overlap -- a bilinear tap at a non-integer ratio folds high frequencies into
// moire, and this picture is what the flow is estimated from. Beyond the source (the estimation size is
// padded to whole 64-pixel tiles) the edge pixel repeats.
constexpr const char* kDownsampleHlsl = R"(
Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Target : register(u0);
cbuffer Params : register(b0)
{
    uint Width;
    uint Height;
    float ScaleX;
    float ScaleY;
    uint SourceWidth;
    uint SourceHeight;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height) return;
    const float2 scale = float2(ScaleX, ScaleY);
    const float2 lo = float2(id.xy) * scale;
    const float2 hi = lo + scale;
    const int2 first = int2(floor(lo));
    const int2 maxCoord = int2(SourceWidth, SourceHeight) - 1;
    float4 sum = 0.0f;
    float total = 0.0f;
    [loop] for (int y = 0; y < 6; ++y)
    {
        const int sy = first.y + y;
        const float wy = max(0.0f, min(hi.y, float(sy + 1)) - max(lo.y, float(sy)));
        if (wy <= 0.0f) continue;
        [loop] for (int x = 0; x < 6; ++x)
        {
            const int sx = first.x + x;
            const float wx = max(0.0f, min(hi.x, float(sx + 1)) - max(lo.x, float(sx)));
            if (wx <= 0.0f) continue;
            const int2 c = clamp(int2(sx, sy), int2(0, 0), maxCoord);
            sum += Source.Load(int3(c, 0)) * (wx * wy);
            total += wx * wy;
        }
    }
    Target[id.xy] = total > 0.0f ? sum / total : Source.Load(int3(clamp(first, int2(0, 0), maxCoord), 0));
}
)";

// Scene-change results come back through readback buffers, one per recent frame, so a buffer is only
// ever mapped once the list that filled it is several frames old.
constexpr unsigned kReadbackRing = 4;

struct Flow
{
    ID3D12Device* device = nullptr; // not owned
    unsigned sourceWidth = 0;
    unsigned sourceHeight = 0;
    unsigned flowWidth = 0;
    unsigned flowHeight = 0;

    std::unique_ptr<FfxOpticalflowContext> context;
    std::vector<unsigned char> scratch;

    ID3D12Resource* estimationInput = nullptr; // flowWidth x flowHeight RGBA16F, rests as UAV
    ID3D12Resource* sparse = nullptr;          // block vectors R16G16_SINT, rests as UAV
    ID3D12Resource* scene = nullptr;           // 3x1 R32_UINT scene-change data, rests as UAV
    ID3D12Resource* dense = nullptr;           // per-pixel R16G16_FLOAT, rests readable

    ID3D12DescriptorHeap* heap = nullptr; // 0 sparse SRV, 1 scene SRV, 2 dense UAV, 3 source SRV, 4 input UAV
    ID3D12RootSignature* densifyRoot = nullptr;
    ID3D12PipelineState* densifyPso = nullptr;
    ID3D12RootSignature* downsampleRoot = nullptr;
    ID3D12PipelineState* downsamplePso = nullptr;

    ID3D12Resource* readback[kReadbackRing] = {};
    unsigned frame = 0;

    bool history = false;
    const char* error = nullptr;

    ~Flow()
    {
        if (context)
            ffxOpticalflowContextDestroy(context.get());

        for (IUnknown** object :
             { (IUnknown**) &estimationInput, (IUnknown**) &sparse, (IUnknown**) &scene, (IUnknown**) &dense,
               (IUnknown**) &heap, (IUnknown**) &densifyRoot, (IUnknown**) &densifyPso, (IUnknown**) &downsampleRoot,
               (IUnknown**) &downsamplePso })
            SafeRelease(*object);

        for (ID3D12Resource*& buffer : readback)
            SafeRelease(reinterpret_cast<IUnknown*&>(buffer));
    }

    bool Texture(ID3D12Resource*& out, unsigned width, unsigned height, DXGI_FORMAT format, D3D12_RESOURCE_STATES state)
    {
        D3D12_HEAP_PROPERTIES heapProps {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Format = format;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return SUCCEEDED(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                         IID_PPV_ARGS(&out)));
    }

    bool ComputePipeline(const D3D12_ROOT_SIGNATURE_DESC& rootDesc, const void* code, size_t size,
                         ID3D12RootSignature*& root, ID3D12PipelineState*& pso)
    {
        ID3DBlob* blob = nullptr;
        ID3DBlob* errors = nullptr;
        const bool serialized =
            SUCCEEDED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors));

        if (errors != nullptr)
            errors->Release();

        if (!serialized || blob == nullptr ||
            FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root))))
        {
            if (blob != nullptr)
                blob->Release();
            return false;
        }

        blob->Release();

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc {};
        psoDesc.pRootSignature = root;
        psoDesc.CS = { code, size };
        return SUCCEEDED(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso)));
    }

    bool Create(int quality)
    {
        const unsigned limit = quality <= 0 ? 640u : quality == 1 ? 960u : 1280u;
        const float factor = std::min(1.0f, float(limit) / float(std::max(sourceWidth, sourceHeight)));

        // Seven-level luminance pyramid: whole 64-pixel tiles, both sides at least 128.
        flowWidth = std::max(128u, ((unsigned(sourceWidth * factor) + 63) / 64) * 64);
        flowHeight = std::max(128u, ((unsigned(sourceHeight * factor) + 63) / 64) * 64);

        D3D12_FEATURE_DATA_SHADER_MODEL shaderModel { D3D_SHADER_MODEL_6_2 };
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1 {};

        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel))) ||
            shaderModel.HighestShaderModel < D3D_SHADER_MODEL_6_2 ||
            FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1))) ||
            !options1.WaveOps)
        {
            error = "the GPU lacks shader model 6.2 with wave operations";
            return false;
        }

        if (!Texture(estimationInput, flowWidth, flowHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, kWrite) ||
            !Texture(dense, sourceWidth, sourceHeight, DXGI_FORMAT_R16G16_FLOAT, kRead) ||
            !Texture(sparse, (flowWidth + 7) / 8, (flowHeight + 7) / 8, DXGI_FORMAT_R16G16_SINT, kWrite) ||
            !Texture(scene, 3, 1, DXGI_FORMAT_R32_UINT, kWrite))
        {
            error = "the flow textures could not be allocated";
            return false;
        }

        scratch.resize(ffxGetScratchMemorySizeDX12(FFX_OPTICALFLOW_CONTEXT_COUNT));
        FfxOpticalflowContextDescription contextDesc {};
        contextDesc.resolution = { flowWidth, flowHeight };

        if (ffxGetInterfaceDX12(&contextDesc.backendInterface, ffxGetDeviceDX12(device), scratch.data(), scratch.size(),
                                FFX_OPTICALFLOW_CONTEXT_COUNT) != FFX_OK)
        {
            error = "the FidelityFX DX12 backend could not start";
            return false;
        }

        auto candidate = std::make_unique<FfxOpticalflowContext>();

        if (ffxOpticalflowContextCreate(candidate.get(), &contextDesc) != FFX_OK)
        {
            error = "the FidelityFX optical flow context could not be created";
            return false;
        }

        context = std::move(candidate);

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 5;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

        if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap))))
        {
            error = "the flow descriptor heap could not be created";
            return false;
        }

        const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE handle = heap->GetCPUDescriptorHandleForHeapStart();

        D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        srv.Format = DXGI_FORMAT_R16G16_SINT;
        device->CreateShaderResourceView(sparse, &srv, handle);
        handle.ptr += step;
        srv.Format = DXGI_FORMAT_R32_UINT;
        device->CreateShaderResourceView(scene, &srv, handle);
        handle.ptr += step;

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format = DXGI_FORMAT_R16G16_FLOAT;
        device->CreateUnorderedAccessView(dense, nullptr, &uav, handle);
        handle.ptr += step; // slot 3: the source SRV, written per record
        handle.ptr += step;
        uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        device->CreateUnorderedAccessView(estimationInput, nullptr, &uav, handle);

        // Densify: t0 sparse, t1 scene, u0 dense, b0 as eight constants (DXL's layout, which the kernel expects).
        {
            D3D12_DESCRIPTOR_RANGE ranges[2] {};
            ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0 };
            ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 2 };
            D3D12_ROOT_PARAMETER params[2] {};
            params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[0].DescriptorTable = { 2, ranges };
            params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            params[1].Constants = { 0, 0, 8 };
            D3D12_ROOT_SIGNATURE_DESC rootDesc {};
            rootDesc.NumParameters = 2;
            rootDesc.pParameters = params;

            if (!ComputePipeline(rootDesc, kOpticalDensify, sizeof(kOpticalDensify), densifyRoot, densifyPso))
            {
                error = "the densify pipeline could not be created";
                return false;
            }
        }

        // Downsample: t0 source (heap slot 3), u0 estimation input (slot 4), b0 as six constants.
        {
            ID3DBlob* code = nullptr;
            ID3DBlob* errors = nullptr;
            const HRESULT compiled = D3DCompile(kDownsampleHlsl, std::strlen(kDownsampleHlsl), "OptiOF_Downsample",
                                               nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                               &code, &errors);
            if (errors != nullptr)
                errors->Release();

            if (FAILED(compiled) || code == nullptr)
            {
                error = "the downsample shader did not compile";
                return false;
            }

            D3D12_DESCRIPTOR_RANGE ranges[2] {};
            ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 3 };
            ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 4 };
            D3D12_ROOT_PARAMETER params[2] {};
            params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[0].DescriptorTable = { 2, ranges };
            params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            params[1].Constants = { 0, 0, 6 };
            D3D12_ROOT_SIGNATURE_DESC rootDesc {};
            rootDesc.NumParameters = 2;
            rootDesc.pParameters = params;

            const bool ok = ComputePipeline(rootDesc, code->GetBufferPointer(), code->GetBufferSize(), downsampleRoot,
                                            downsamplePso);
            code->Release();

            if (!ok)
            {
                error = "the downsample pipeline could not be created";
                return false;
            }
        }

        D3D12_HEAP_PROPERTIES readbackHeap {};
        readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer {};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = 16;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        for (ID3D12Resource*& slot : readback)
        {
            if (FAILED(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&slot))))
            {
                error = "the scene-change readback could not be created";
                return false;
            }
        }

        return true;
    }

    ID3D12Resource* Record(ID3D12GraphicsCommandList* list, ID3D12Resource* input, bool reset, bool& sceneCut)
    {
        sceneCut = false;

        // The oldest buffer in the ring was filled kReadbackRing frames ago: read it, then reuse it.
        const unsigned slot = frame % kReadbackRing;

        if (frame >= kReadbackRing)
        {
            void* mapped = nullptr;
            D3D12_RANGE range { 0, 16 };

            if (SUCCEEDED(readback[slot]->Map(0, &range, &mapped)) && mapped != nullptr)
            {
                // The second value's low four bits are FidelityFX's recent-cut window, as DXL reads it.
                sceneCut = (static_cast<const uint32_t*>(mapped)[1] & 15u) != 0;
                D3D12_RANGE written { 0, 0 };
                readback[slot]->Unmap(0, &written);
            }
        }

        // Downsample the frame into the estimation input.
        D3D12_CPU_DESCRIPTOR_HANDLE sourceHandle = heap->GetCPUDescriptorHandleForHeapStart();
        const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        sourceHandle.ptr += 3 * step;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        srv.Format = input->GetDesc().Format;
        device->CreateShaderResourceView(input, &srv, sourceHandle);

        // The whole picture stretched over the whole estimation size, per axis. The densify kernel maps a pixel
        // to its block, and converts the block's motion back to pixels, with separate width and height ratios
        // (FlowWidth/Width, FlowHeight/Height), so the picture it was estimated from has to be laid out the
        // same way -- one width-based scale for both axes would skew vertical motion wherever the 64-pixel
        // padding differs between the axes.
        const float scaleX = float(sourceWidth) / float(flowWidth);
        const float scaleY = float(sourceHeight) / float(flowHeight);
        UINT constants[6] = { flowWidth, flowHeight, 0, 0, sourceWidth, sourceHeight };
        std::memcpy(&constants[2], &scaleX, sizeof(float));
        std::memcpy(&constants[3], &scaleY, sizeof(float));

        ID3D12DescriptorHeap* heaps[] = { heap };
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap->GetGPUDescriptorHandleForHeapStart();
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(downsampleRoot);
        list->SetPipelineState(downsamplePso);
        list->SetComputeRootDescriptorTable(0, { gpu.ptr + 3 * step });
        list->SetComputeRoot32BitConstants(1, 6, constants, 0);
        list->Dispatch((flowWidth + 7) / 8, (flowHeight + 7) / 8, 1);

        // The estimate itself.
        const bool first = !history || reset;
        Transition(list, estimationInput, kWrite, kRead);
        FfxOpticalflowDispatchDescription job {};
        job.commandList = ffxGetCommandListDX12(list);
        job.color = Wrap(estimationInput, FFX_RESOURCE_STATE_COMPUTE_READ);
        job.opticalFlowVector = Wrap(sparse, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        job.opticalFlowSCD = Wrap(scene, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        job.reset = first;
        job.backbufferTransferFunction = 0;
        job.minMaxLuminance = { 0.0f, 1.0f };
        const FfxErrorCode result = ffxOpticalflowContextDispatch(context.get(), &job);
        Transition(list, estimationInput, kRead, kWrite);

        if (result != FFX_OK)
        {
            error = "the optical flow dispatch failed";
            return nullptr;
        }

        // Block vectors to a per-pixel field.
        Transition(list, sparse, kWrite, kRead);
        Transition(list, scene, kWrite, kRead);
        Transition(list, dense, kRead, kWrite);
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(densifyRoot);
        list->SetPipelineState(densifyPso);
        list->SetComputeRootDescriptorTable(0, gpu);
        const UINT densify[8] = { sourceWidth, sourceHeight, flowWidth, flowHeight, first ? 1u : 0u, 0u, 0u, 0u };
        list->SetComputeRoot32BitConstants(1, 8, densify, 0);
        list->Dispatch((sourceWidth + 7) / 8, (sourceHeight + 7) / 8, 1);
        Transition(list, dense, kWrite, kRead);
        Transition(list, sparse, kRead, kWrite);

        // Scene-change data out to this frame's readback slot.
        Transition(list, scene, kRead, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from {};
        from.pResource = scene;
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION to {};
        to.pResource = readback[slot];
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_UINT, 3, 1, 1, 12 };
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        Transition(list, scene, D3D12_RESOURCE_STATE_COPY_SOURCE, kWrite);

        ++frame;
        history = true;
        error = nullptr;
        return dense;
    }
};
} // namespace

int OptiOF_AbiVersion(void) { return OPTI_OF_ABI_VERSION; }

void* OptiOF_Create(ID3D12Device* device, unsigned int width, unsigned int height, int quality)
{
    g_staticError = nullptr;

    if (device == nullptr || width == 0 || height == 0)
    {
        g_staticError = "no device or no size";
        return nullptr;
    }

    try
    {
        auto flow = std::make_unique<Flow>();
        flow->device = device;
        flow->sourceWidth = width;
        flow->sourceHeight = height;

        if (!flow->Create(std::clamp(quality, 0, 2)))
        {
            g_staticError = flow->error;
            return nullptr;
        }

        return flow.release();
    }
    catch (...)
    {
        g_staticError = "allocation failure";
        return nullptr;
    }
}

ID3D12Resource* OptiOF_Record(void* flow, ID3D12GraphicsCommandList* list, ID3D12Resource* input, int reset,
                              int* sceneCut)
{
    if (sceneCut != nullptr)
        *sceneCut = 0;

    if (flow == nullptr || list == nullptr || input == nullptr)
        return nullptr;

    auto* f = static_cast<Flow*>(flow);
    const D3D12_RESOURCE_DESC desc = input->GetDesc();

    if (desc.Width != f->sourceWidth || desc.Height != f->sourceHeight)
    {
        f->error = "the input is not the size the estimator was created for";
        return nullptr;
    }

    try
    {
        bool cut = false;
        ID3D12Resource* result = f->Record(list, input, reset != 0, cut);

        if (sceneCut != nullptr)
            *sceneCut = cut ? 1 : 0;

        return result;
    }
    catch (...)
    {
        f->error = "exception while recording";
        return nullptr;
    }
}

void OptiOF_Destroy(void* flow) { delete static_cast<Flow*>(flow); }

const char* OptiOF_LastError(void* flow) { return flow != nullptr ? static_cast<Flow*>(flow)->error : g_staticError; }

const char* OptiOF_StaticError(void) { return g_staticError; }
