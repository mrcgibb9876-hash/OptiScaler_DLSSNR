// Copyright (c) 2026 mrcgibb9876-hash. Original to this fork, not part of upstream OptiScaler.
// Distributed under the GNU General Public License version 3 -- see LICENSE.
#pragma once

// One frame of everything Image Clean Up sees, written raw for the offline harness
// (tools/cleanup-harness.js), so the clean up can be tuned on a real game's frames instead of synthetic
// ones. The game's input frame, the proxy the model was shown, the model's answer, the composed picture
// before and after the clean up, the depth and motion it read, its mask and model reading, and a
// manifest.json with each file's size, format and row pitch plus every setting and constant in effect.
//
// Recorded as copies into readback buffers on the frame's own command list, and written some frames
// later, once the GPU is certainly past them -- the same pattern as DlssNr_Capture.h, which has no fence
// either. Only plain single-plane colour and depth formats are copied; anything else is listed in the
// manifest as skipped rather than risked.

#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace cleancapture
{

inline const char* FormatName(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return "R32G32B32A32_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_UNORM:
        return "R16G16B16A16_UNORM";
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return "R11G11B10_FLOAT";
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return "B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
        return "R32_FLOAT";
    case DXGI_FORMAT_R16_FLOAT:
        return "R16_FLOAT";
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_TYPELESS:
        return "R16_UNORM";
    case DXGI_FORMAT_R32G32_FLOAT:
        return "R32G32_FLOAT";
    case DXGI_FORMAT_R16G16_FLOAT:
        return "R16G16_FLOAT";
    case DXGI_FORMAT_R16G16_SNORM:
        return "R16G16_SNORM";
    default:
        return nullptr;
    }
}

struct Item
{
    std::string name;
    ID3D12Resource* readback = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT rows = 0;
    UINT64 total = 0;
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

class CleanCapture
{
  public:
    void request() { requested_ = true; }
    bool wanted() const { return requested_ && !recorded_; }
    bool pending() const { return recorded_; }
    bool due(unsigned long long frame) const { return recorded_ && frame >= writeAt_; }

    // Copies subresource 0 of res, which is in state `state`, and puts it back in that state.
    void copy(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, const char* name, ID3D12Resource* res,
              D3D12_RESOURCE_STATES state)
    {
        if (res == nullptr)
        {
            skipped_ += std::string(skipped_.empty() ? "" : ",") + "\"" + name + " (not bound)\"";
            return;
        }

        const D3D12_RESOURCE_DESC desc = res->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 ||
            FormatName(desc.Format) == nullptr)
        {
            skipped_ += std::string(skipped_.empty() ? "" : ",") + "\"" + name + " (format " +
                        std::to_string((int) desc.Format) + ")\"";
            return;
        }

        Item item;
        item.name = name;
        item.width = (UINT) desc.Width;
        item.height = desc.Height;
        item.format = desc.Format;
        UINT64 rowBytes = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &item.footprint, &item.rows, &rowBytes, &item.total);

        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer = {};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = item.total;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.Format = DXGI_FORMAT_UNKNOWN;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&item.readback))))
        {
            skipped_ += std::string(skipped_.empty() ? "" : ",") + "\"" + name + " (no readback buffer)\"";
            return;
        }

        transition(cmd, res, state, D3D12_RESOURCE_STATE_COPY_SOURCE);

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = res;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = item.readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = item.footprint;

        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(cmd, res, D3D12_RESOURCE_STATE_COPY_SOURCE, state);

        items_.push_back(item);
    }

    // The frame is recorded; write it `delay` frames from `frame`. settingsJson is a JSON object.
    void finish(const std::string& settingsJson, unsigned long long frame, unsigned long long delay)
    {
        settings_ = settingsJson;
        writeAt_ = frame + delay;
        recorded_ = true;
        requested_ = false;
    }

    // Writes the files and releases everything. Returns the folder, or empty.
    std::string write(const std::filesystem::path& root)
    {
        if (!recorded_)
            return {};

        SYSTEMTIME t;
        GetLocalTime(&t);
        char stamp[64];
        std::snprintf(stamp, sizeof(stamp), "%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                      t.wSecond);
        const auto dir = root / stamp;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);

        std::string files;
        for (auto& item : items_)
        {
            void* mapped = nullptr;
            D3D12_RANGE range = { 0, (SIZE_T) item.total };
            if (SUCCEEDED(item.readback->Map(0, &range, &mapped)) && mapped != nullptr)
            {
                const auto path = dir / (item.name + ".raw");
                if (std::FILE* f = _wfopen(path.wstring().c_str(), L"wb"))
                {
                    // As the footprint lays the rows out; the manifest gives the pitch.
                    std::fwrite(mapped, 1, (size_t) item.total, f);
                    std::fclose(f);
                }
                D3D12_RANGE none = { 0, 0 };
                item.readback->Unmap(0, &none);
            }

            char line[512];
            std::snprintf(line, sizeof(line),
                          "%s\n    {\"name\":\"%s\",\"file\":\"%s.raw\",\"width\":%u,\"height\":%u,\"format\":\"%s\","
                          "\"dxgiFormat\":%d,\"rowPitch\":%u,\"offset\":%llu}",
                          files.empty() ? "" : ",", item.name.c_str(), item.name.c_str(), item.width, item.height,
                          FormatName(item.format), (int) item.format, item.footprint.Footprint.RowPitch,
                          (unsigned long long) item.footprint.Offset);
            files += line;
        }

        const auto manifest = dir / "manifest.json";
        if (std::FILE* f = _wfopen(manifest.wstring().c_str(), L"wt"))
        {
            std::fprintf(f,
                         "{\n  \"version\": 1,\n  \"files\": [%s\n  ],\n  \"skipped\": [%s],\n  \"settings\": %s\n}\n",
                         files.c_str(), skipped_.c_str(), settings_.c_str());
            std::fclose(f);
        }

        release();
        return dir.string();
    }

    void release()
    {
        for (auto& item : items_)
        {
            if (item.readback != nullptr)
                item.readback->Release();
        }
        items_.clear();
        skipped_.clear();
        settings_.clear();
        recorded_ = false;
    }

  private:
    static void transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res, D3D12_RESOURCE_STATES from,
                           D3D12_RESOURCE_STATES to)
    {
        if (from == to)
            return;
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        cmd->ResourceBarrier(1, &b);
    }

    std::vector<Item> items_;
    std::string skipped_;
    std::string settings_;
    bool requested_ = false;
    bool recorded_ = false;
    unsigned long long writeAt_ = 0;
};

} // namespace cleancapture
