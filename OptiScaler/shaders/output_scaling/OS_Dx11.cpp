#include "pch.h"
#include "OS_Dx11.h"

#include "OS_Common.h"
#include "OS_Upsamplers.h"
#include "../Shader_Common.h"

#define A_CPU
// FSR compute shader is from : https://github.com/fholger/vrperfkit/

#include "precompile/BCDS_bicubic_Shader_Dx11.h"
#include "precompile/BCDS_catmull_Shader_Dx11.h"
#include "precompile/BCDS_lanczos2_Shader_Dx11.h"
#include "precompile/BCDS_lanczos3_Shader_Dx11.h"
#include "precompile/BCDS_kaiser2_Shader_Dx11.h"
#include "precompile/BCDS_kaiser3_Shader_Dx11.h"
#include "precompile/BCDS_magc_Shader_Dx11.h"

#include "precompile/BCUS_Shader_Dx11.h"

#include "fsr1/ffx_fsr1.h"
#include "fsr1/FSR_EASU_Shader_Dx11.h"

#include <Config.h>

static Constants constants {};
static UpscaleShaderConstants fsr1Constants {};

#pragma warning(disable : 4244)

bool OS_Dx11::CreateBufferResource(ID3D11Device* InDevice, ID3D11Resource* InResource, uint32_t InWidth,
                                   uint32_t InHeight)
{
    return CreateBufferResourceCommon(InDevice, InResource, _buffer,
                                      [InWidth, InHeight](D3D11_TEXTURE2D_DESC& desc)
                                      {
                                          desc.Width = desc.Width > InWidth ? desc.Width : InWidth;
                                          desc.Height = desc.Height > InHeight ? desc.Height : InHeight;
                                          desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
                                      });
}

bool OS_Dx11::InitializeViews(ID3D11Texture2D* InResource, ID3D11Texture2D* OutResource)
{
    auto resultInput = InitializeSRV(InResource, _currentInResource, _srvInput);
    auto resultOutput = InitializeUAV(OutResource, _currentOutResource, _uavOutput);

    return resultInput && resultOutput;
}

bool OS_Dx11::Dispatch(ID3D11Device* InDevice, ID3D11DeviceContext* InContext, ID3D11Texture2D* InResource,
                       ID3D11Texture2D* OutResource)
{
    if (!_init || InDevice == nullptr || InContext == nullptr || InResource == nullptr || OutResource == nullptr)
        return false;

    LOG_DEBUG("[{0}] Start!", _name);

    ScopedGpuTime_Dx11 scopedGpuTime(GpuTime.get(), InContext);

    _device = InDevice;

    if (!InitializeViews(InResource, OutResource))
        return false;

    FsrEasuCon(fsr1Constants.const0, fsr1Constants.const1, fsr1Constants.const2, fsr1Constants.const3,
               State::Instance().currentFeature->TargetWidth(), State::Instance().currentFeature->TargetHeight(),
               State::Instance().currentFeature->TargetWidth(), State::Instance().currentFeature->TargetHeight(),
               State::Instance().currentFeature->DisplayWidth(), State::Instance().currentFeature->DisplayHeight());

    constants.srcWidth = State::Instance().currentFeature->TargetWidth();
    constants.srcHeight = State::Instance().currentFeature->TargetHeight();
    constants.destWidth = State::Instance().currentFeature->DisplayWidth();
    constants.destHeight = State::Instance().currentFeature->DisplayHeight();

    // See the matching comment in OS_Dx12.cpp. Always the Output Scaling keys here: nothing on this
    // backend builds a Neural Rendering pass.
    const auto tuning = UpsamplerTuningFor(false);
    constants.antiRinging = _upsample ? tuning.antiRinging : 0.0f;
    constants.sigmoid = (_upsample && tuning.sigmoid) ? 1 : 0;
    constants.sigmoidCentre = 0.75f;
    constants.sigmoidSlope = 6.5f;

    // fsr upscaling
    if (UsesFsr1())
    {
        // Copy the updated constant buffer data to the constant buffer resource
        D3D11_MAPPED_SUBRESOURCE mappedResource;
        auto hr = InContext->Map(_constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
        if (FAILED(hr))
        {
            LOG_ERROR("[{0}] Map error {1:x}", _name, hr);

            if (hr == DXGI_ERROR_DEVICE_REMOVED && _device != nullptr)
                Util::GetDeviceRemovedReason(_device);

            return false;
        }

        memcpy(mappedResource.pData, &fsr1Constants, sizeof(fsr1Constants));
        InContext->Unmap(_constantBuffer, 0);
    }
    else
    {
        D3D11_MAPPED_SUBRESOURCE mappedResource;
        auto hr = InContext->Map(_constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
        if (FAILED(hr))
        {
            LOG_ERROR("[{0}] Map error {1:x}", _name, hr);
            return false;
        }

        memcpy(mappedResource.pData, &constants, sizeof(constants));
        InContext->Unmap(_constantBuffer, 0);
    }

    // Set the compute shader and resources
    InContext->CSSetShader(_computeShader, nullptr, 0);
    InContext->CSSetConstantBuffers(0, 1, &_constantBuffer);
    InContext->CSSetShaderResources(0, 1, &_srvInput);
    InContext->CSSetUnorderedAccessViews(0, 1, &_uavOutput, nullptr);

    UINT dispatchWidth = 0;
    UINT dispatchHeight = 0;

    dispatchWidth =
        static_cast<UINT>((State::Instance().currentFeature->DisplayWidth() + InNumThreadsX - 1) / InNumThreadsX);
    dispatchHeight = (State::Instance().currentFeature->DisplayHeight() + InNumThreadsY - 1) / InNumThreadsY;

    InContext->Dispatch(dispatchWidth, dispatchHeight, 1);

    // Unbind resources
    ID3D11UnorderedAccessView* nullUAV = nullptr;
    InContext->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
    ID3D11ShaderResourceView* nullSRV[2] = { nullptr, nullptr };
    InContext->CSSetShaderResources(0, 2, nullSRV);

    return true;
}

bool OS_Dx11::UsesFsr1() const
{
    const auto downscaler = Config::Instance()->OutputScalingDownscaler.value_or_default();
    return _upsample ? (ConfiguredUpsampler(downscaler) == Upsampler::FSR1) : (downscaler == Scaler::FSR1);
}

OS_Dx11::OS_Dx11(std::string InName, ID3D11Device* InDevice, bool InUpsample)
    : Shader_Dx11(InName, InDevice), _upsample(InUpsample)
{
    if (InDevice == nullptr)
    {
        LOG_ERROR("InDevice is nullptr!");
        return;
    }

    LOG_DEBUG("{0} start!", _name);

    const void* csoData = nullptr;
    size_t csoSize = 0;
    const char* shaderCode = nullptr;

    const auto downscaler = Config::Instance()->OutputScalingDownscaler.value_or_default();

    std::string name = "OS: ";

    // Enlarging is settled first, for the reason spelled out in OS_Dx12.cpp: the Scaler::FSR1 test
    // used to come ahead of it, and FSR1 is the default downscaler.
    if (_upsample)
    {
        // No per-instance override here, unlike OS_Dx12: nothing on this backend builds an Output
        // Scaling pass with a filter of its own, so the global config is the only source.
        const auto upsampler = ConfiguredUpsampler(downscaler);
        const char* upsamplerSource = UpsamplerShaderSource(upsampler);

        if (upsampler == Upsampler::FSR1)
        {
            csoData = fsr_easu_cso;
            csoSize = sizeof(fsr_easu_cso);
            // FSR1 bypasses runtime compilation
        }
        else if (upsamplerSource != nullptr)
        {
            InNumThreadsY = 8;
            InNumThreadsX = 8;

            // No precompiled blob on purpose -- see OS_Upsamplers.h and CreateComputeShader.
            csoData = nullptr;
            csoSize = 0;
            shaderCode = upsamplerSource;
        }
        else
        {
            csoData = bcus_cso;
            csoSize = sizeof(bcus_cso);
            shaderCode = upsampleCode.c_str();
        }

        name += UpsamplerName(upsampler);
    }
    else if (downscaler == Scaler::FSR1)
    {
        csoData = fsr_easu_cso;
        csoSize = sizeof(fsr_easu_cso);
        name += "FSR1";
        // FSR1 bypasses runtime compilation
    }
    else
    {
        InNumThreadsY = 8;
        InNumThreadsX = 8;

        switch (downscaler)
        {
        case Scaler::CatmullRom:
            csoData = bcds_catmull_cso;
            csoSize = sizeof(bcds_catmull_cso);
            shaderCode = downsampleCodeCatmull.c_str();
            name += "CatmullRom";
            break;

        case Scaler::Lanczos2:
            csoData = bcds_lanczos2_cso;
            csoSize = sizeof(bcds_lanczos2_cso);
            shaderCode = downsampleCodeLanczos2.c_str();
            name += "Lanczos2";
            break;

        case Scaler::Lanczos3:
            csoData = bcds_lanczos3_cso;
            csoSize = sizeof(bcds_lanczos3_cso);
            shaderCode = downsampleCodeLanczos3.c_str();
            name += "Lanczos3";
            break;

        case Scaler::Kaiser2:
            csoData = bcds_kaiser2_cso;
            csoSize = sizeof(bcds_kaiser2_cso);
            shaderCode = downsampleCodeKaiser2.c_str();
            name += "Kaiser2";
            break;

        case Scaler::Kaiser3:
            csoData = bcds_kaiser3_cso;
            csoSize = sizeof(bcds_kaiser3_cso);
            shaderCode = downsampleCodeKaiser3.c_str();
            name += "Kaiser3";
            break;

        case Scaler::Magic:
            csoData = bcds_magc_cso;
            csoSize = sizeof(bcds_magc_cso);
            shaderCode = downsampleCodeMAGIC.c_str();
            name += "Magic";
            break;

        case Scaler::Bicubic:
        default:
            csoData = bcds_bicubic_cso;
            csoSize = sizeof(bcds_bicubic_cso);
            shaderCode = downsampleCodeBC.c_str();
            name += "Bicubic";
            break;
        }
    }

    _name = name;

    HRESULT result = CreateComputeShader(InDevice, _computeShader, csoData, csoSize, shaderCode);
    if (FAILED(result))
    {
        LOG_ERROR("[{0}] CreateComputeShader error: {1:X}", _name, result);
        return;
    }

    // CBV
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.ByteWidth = sizeof(Constants);
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    result = InDevice->CreateBuffer(&cbDesc, nullptr, &_constantBuffer);
    if (result != S_OK)
    {
        LOG_ERROR("CreateBuffer error: {0:X}", (UINT) result);
        return;
    }

    if (_upsample ? (ConfiguredUpsampler(downscaler) == Upsampler::FSR1) : (downscaler == Scaler::FSR1))
    {
        InNumThreadsX = 16;
        InNumThreadsY = 16;
    }

    _init = true;
}