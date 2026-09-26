// Copyright (c) 2026 mrcgibb9876-hash. Original to this fork, not part of upstream OptiScaler.
// Distributed under the GNU General Public License version 3 -- see LICENSE.
#include "pch.h"

#include "DlssNr_DepthTracker.h"

#include <detours/detours.h>

#include <algorithm>
#include <atomic>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
// ID3D12Device: IUnknown 0-2, ID3D12Object 3-6, then GetNodeCount 7 ... CreateDepthStencilView 21.
constexpr size_t kSlotCreateDepthStencilView = 21;
// CreateSampler is 22. Leaving it out of the count put these two hooks on CreateSampler and CopyDescriptors
// with the wrong signatures, and Resident Evil 2 faulted before its first frame.
constexpr size_t kSlotCopyDescriptors = 23;
constexpr size_t kSlotCopyDescriptorsSimple = 24;
// ID3D12GraphicsCommandList: ... ResourceBarrier 26, OMSetRenderTargets 46, ClearDepthStencilView 47.
constexpr size_t kSlotResetList = 10;
constexpr size_t kSlotResourceBarrier = 26;
constexpr size_t kSlotOMSetRenderTargets = 46;
constexpr size_t kSlotClearDepthStencilView = 47;

using CreateDepthStencilViewFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*,
                                                          const D3D12_DEPTH_STENCIL_VIEW_DESC*,
                                                          D3D12_CPU_DESCRIPTOR_HANDLE);
using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT,
                                                      const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL,
                                                      const D3D12_CPU_DESCRIPTOR_HANDLE*);
using ClearDepthStencilViewFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE,
                                                         D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT*);
using ResourceBarrierFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
using ResetListFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*,
                                                ID3D12PipelineState*);

CreateDepthStencilViewFn o_CreateDepthStencilView = nullptr;
OMSetRenderTargetsFn o_OMSetRenderTargets = nullptr;
ClearDepthStencilViewFn o_ClearDepthStencilView = nullptr;
ResourceBarrierFn o_ResourceBarrier = nullptr;
ResetListFn o_ResetList = nullptr;

bool IsDepthFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R16_TYPELESS:
        return true;
    default:
        return false;
    }
}

// A candidate. The resource is deliberately not referenced: holding it would keep the game's depth
// alive past its own release. It is only ever compared as an identity, and a candidate that goes
// unused for long enough is dropped, so a new resource at a recycled address is not matched against
// an old size for long.
struct Entry
{
    std::atomic<ID3D12Resource*> resource { nullptr };
    unsigned int width = 0;
    unsigned int height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::atomic<unsigned int> binds { 0 };
    std::atomic<unsigned int> clears { 0 };
    unsigned int bindsLastFrame = 0;
    unsigned int clearsLastFrame = 0;
    unsigned int staleFrames = 0;
    std::atomic<unsigned int> state { D3D12_RESOURCE_STATE_COMMON };
    std::atomic<bool> stateKnown { false };
    std::atomic<bool> reversed { true };

    // The same, counted when the list that recorded it is SUBMITTED rather than when it is recorded. A
    // game that records its next frame before presenting this one -- or hands out a fresh, identical depth
    // buffer every frame, as RE Engine does -- has two buffers bound in the same Present-to-Present window,
    // and record-time counts cannot tell which one the GPU will have written last when the pass copies at
    // Present. Written under g_listMutex.
    unsigned int writesSubmitted = 0; // DSV binds for writing and depth clears, since the last Present
    unsigned int writesLastFrame = 0;
    unsigned long long lastWriteSeq = 0; // submission order of the last of them
    unsigned int submittedState = D3D12_RESOURCE_STATE_COMMON;
    bool submittedStateKnown = false;
};

constexpr unsigned int kMaxCandidates = 96;
constexpr unsigned int kMaxStaleFrames = 240;

// DSV handle -> candidate, an open-addressed table read without a lock. DXL keeps 128 in a list, which
// Resident Evil 2 filled before it had drawn a frame: every scene depth view it made afterwards had
// nowhere to go, no bind ever matched, and no depth was ever chosen (2026-09-14).
constexpr size_t kDsvTableBits = 14;
constexpr size_t kDsvTableSize = size_t(1) << kDsvTableBits;
constexpr size_t kDsvMaxProbe = 64;

struct DsvSlot
{
    std::atomic<SIZE_T> handle { 0 };
    std::atomic<int> candidate { -1 };
    std::atomic<bool> readOnly { false }; // a read-only depth view: bound, but nothing written through it
};

Entry g_entries[kMaxCandidates];
std::atomic<unsigned int> g_count { 0 };
DsvSlot g_dsvTable[kDsvTableSize];
std::atomic<unsigned int> g_dsvCount { 0 };
UINT g_dsvIncrement = 0;

// Why nothing is chosen, when nothing is: binds with a depth attached, and how many of them resolved.
std::atomic<unsigned int> g_depthBinds { 0 };
std::atomic<unsigned int> g_depthBindsMatched { 0 };

// Adding is rare (a new depth buffer or view); serialising it keeps two threads from claiming one slot.
std::mutex g_addMutex;

// What each open command list did to the candidates, in order, applied when the list is submitted
// (OnExecute) and dropped when it is reset without having been.
struct ListEvent
{
    int candidate;
    unsigned char kind; // 0 bound for writing, 1 cleared, 2 transitioned
    unsigned int state;
};
std::mutex g_listMutex;
std::unordered_map<const void*, std::vector<ListEvent>> g_pending;
unsigned long long g_submitSeq = 0;

void Note(const void* list, int candidate, unsigned char kind, unsigned int state)
{
    std::lock_guard<std::mutex> lock(g_listMutex);
    g_pending[list].push_back(ListEvent { candidate, kind, state });
}

// Which rule picks among the candidates (DlssNr::DepthTracker::Policy). Written at Present only.
constexpr int kPolicyCount = 3;
std::atomic<int> g_policy { 0 };
bool g_lastPickSubmitted = false; // the current pick came from submitted work (Present thread only)

size_t DsvHash(SIZE_T handle)
{
    return static_cast<size_t>((static_cast<unsigned long long>(handle) * 0x9E3779B97F4A7C15ull) >>
                               (64 - kDsvTableBits));
}

int CandidateForHandle(SIZE_T handle)
{
    size_t i = DsvHash(handle);

    for (size_t probe = 0; probe < kDsvMaxProbe; ++probe)
    {
        const SIZE_T key = g_dsvTable[i].handle.load(std::memory_order_relaxed);

        if (key == handle)
            return g_dsvTable[i].candidate.load(std::memory_order_relaxed);

        if (key == 0)
            return -1;

        i = (i + 1) & (kDsvTableSize - 1);
    }

    return -1;
}

// Under g_addMutex. A handle pointed at a new resource takes the new candidate; a full probe run evicts
// the slot the handle hashes to.
void MapHandle(SIZE_T handle, int candidate)
{
    if (handle == 0)
        return;

    const size_t home = DsvHash(handle);
    size_t i = home;

    for (size_t probe = 0; probe < kDsvMaxProbe; ++probe)
    {
        const SIZE_T key = g_dsvTable[i].handle.load(std::memory_order_relaxed);

        if (key == handle)
        {
            g_dsvTable[i].candidate.store(candidate, std::memory_order_relaxed);
            return;
        }

        if (key == 0)
        {
            g_dsvTable[i].candidate.store(candidate, std::memory_order_relaxed);
            g_dsvTable[i].handle.store(handle, std::memory_order_relaxed);
            g_dsvCount.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        i = (i + 1) & (kDsvTableSize - 1);
    }

    g_dsvTable[home].handle.store(handle, std::memory_order_relaxed);
    g_dsvTable[home].candidate.store(candidate, std::memory_order_relaxed);
}

bool HandleReadOnly(SIZE_T handle)
{
    size_t i = DsvHash(handle);

    for (size_t probe = 0; probe < kDsvMaxProbe; ++probe)
    {
        const SIZE_T key = g_dsvTable[i].handle.load(std::memory_order_relaxed);

        if (key == handle)
            return g_dsvTable[i].readOnly.load(std::memory_order_relaxed);

        if (key == 0)
            return false;

        i = (i + 1) & (kDsvTableSize - 1);
    }

    return false;
}

void SetHandleReadOnly(SIZE_T handle, bool readOnly)
{
    size_t i = DsvHash(handle);

    for (size_t probe = 0; probe < kDsvMaxProbe; ++probe)
    {
        const SIZE_T key = g_dsvTable[i].handle.load(std::memory_order_relaxed);

        if (key == handle)
        {
            g_dsvTable[i].readOnly.store(readOnly, std::memory_order_relaxed);
            return;
        }

        if (key == 0)
            return;

        i = (i + 1) & (kDsvTableSize - 1);
    }
}

void UnmapCandidate(int candidate)
{
    for (size_t i = 0; i < kDsvTableSize; ++i)
    {
        if (g_dsvTable[i].candidate.load(std::memory_order_relaxed) == candidate)
            g_dsvTable[i].candidate.store(-1, std::memory_order_relaxed);
    }
}

std::atomic<bool> g_installed { false };
std::mutex g_installMutex;

// Written at Present only.
int g_selectedIndex = -1;
unsigned int g_selectedWidth = 0;
unsigned int g_selectedHeight = 0;
DXGI_FORMAT g_selectedFormat = DXGI_FORMAT_UNKNOWN;

int FindCandidate(ID3D12Resource* resource)
{
    const unsigned int count = g_count.load(std::memory_order_acquire);

    for (unsigned int i = 0; i < count; ++i)
    {
        if (g_entries[i].resource.load(std::memory_order_relaxed) == resource)
            return static_cast<int>(i);
    }

    return -1;
}

int FindOrAddCandidate(ID3D12Resource* resource)
{
    if (resource == nullptr)
        return -1;

    // Only ever called with a live resource (a view is being made of it), so its description is current. An
    // existing entry at this address takes it: the address may belong to a new resource now.
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    const bool isDepth = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && IsDepthFormat(desc.Format);

    std::lock_guard<std::mutex> lock(g_addMutex);

    if (const int found = FindCandidate(resource); found >= 0)
    {
        Entry& entry = g_entries[found];

        if (!isDepth)
        {
            UnmapCandidate(found);
            entry.resource.store(nullptr, std::memory_order_release);
            entry.stateKnown.store(false, std::memory_order_relaxed);
            return -1;
        }

        if (entry.width != desc.Width || entry.height != desc.Height || entry.format != desc.Format)
        {
            entry.width = static_cast<unsigned int>(desc.Width);
            entry.height = desc.Height;
            entry.format = desc.Format;
            entry.stateKnown.store(false, std::memory_order_relaxed);
        }

        entry.staleFrames = 0;
        return found;
    }

    if (!isDepth)
        return -1;

    const unsigned int count = g_count.load(std::memory_order_acquire);
    unsigned int slot = count;

    // Reuse a slot EndFrame emptied before growing, or the table fills with buffers that once existed.
    for (unsigned int i = 0; i < count; ++i)
    {
        if (g_entries[i].resource.load(std::memory_order_relaxed) == nullptr)
        {
            slot = i;
            break;
        }
    }

    if (slot == count && count >= kMaxCandidates)
    {
        // Full: evict the one unused the longest that is not the current choice.
        slot = kMaxCandidates;
        unsigned int stalest = 0;

        for (unsigned int i = 0; i < kMaxCandidates; ++i)
        {
            if (g_entries[i].bindsLastFrame == 0 && g_entries[i].clearsLastFrame == 0 &&
                static_cast<int>(i) != g_selectedIndex &&
                (slot == kMaxCandidates || g_entries[i].staleFrames > stalest))
            {
                slot = i;
                stalest = g_entries[i].staleFrames;
            }
        }

        if (slot == kMaxCandidates)
            return -1;

        UnmapCandidate(static_cast<int>(slot));
    }

    Entry& entry = g_entries[slot];
    entry.width = static_cast<unsigned int>(desc.Width);
    entry.height = desc.Height;
    entry.format = desc.Format;
    entry.binds.store(0, std::memory_order_relaxed);
    entry.clears.store(0, std::memory_order_relaxed);
    entry.bindsLastFrame = 0;
    entry.clearsLastFrame = 0;
    entry.staleFrames = 0;
    entry.stateKnown.store(false, std::memory_order_relaxed);
    entry.resource.store(resource, std::memory_order_release);

    if (slot == count)
        g_count.store(count + 1, std::memory_order_release);

    return static_cast<int>(slot);
}

void STDMETHODCALLTYPE hkCreateDepthStencilView(ID3D12Device* device, ID3D12Resource* resource,
                                                const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
                                                D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    // A handle pointed at a non-depth resource, or at nothing, loses whatever it mapped to before.
    const int candidate = resource != nullptr ? FindOrAddCandidate(resource) : -1;

    {
        std::lock_guard<std::mutex> lock(g_addMutex);

        if (candidate >= 0 || CandidateForHandle(handle.ptr) >= 0)
        {
            MapHandle(handle.ptr, candidate);
            SetHandleReadOnly(handle.ptr, desc != nullptr && (desc->Flags & D3D12_DSV_FLAG_READ_ONLY_DEPTH) != 0);
        }
    }

    o_CreateDepthStencilView(device, resource, desc, handle);
}

// An engine can build its views in one heap and copy them into the heap it binds from. The copy carries the
// view, so it carries the mapping.
void CopyDsvMappings(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE dest, D3D12_CPU_DESCRIPTOR_HANDLE source)
{
    if (g_dsvIncrement == 0)
        return;

    std::lock_guard<std::mutex> lock(g_addMutex);

    for (UINT k = 0; k < count; ++k)
    {
        const SIZE_T from = source.ptr + static_cast<SIZE_T>(k) * g_dsvIncrement;
        const SIZE_T to = dest.ptr + static_cast<SIZE_T>(k) * g_dsvIncrement;
        const int candidate = CandidateForHandle(from);

        if (candidate >= 0 || CandidateForHandle(to) >= 0)
        {
            MapHandle(to, candidate);
            SetHandleReadOnly(to, HandleReadOnly(from));
        }
    }
}

using CopyDescriptorsSimpleFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE,
                                                         D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
using CopyDescriptorsFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*,
                                                   UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*,
                                                   D3D12_DESCRIPTOR_HEAP_TYPE);
CopyDescriptorsSimpleFn o_CopyDescriptorsSimple = nullptr;
CopyDescriptorsFn o_CopyDescriptors = nullptr;

void STDMETHODCALLTYPE hkCopyDescriptorsSimple(ID3D12Device* device, UINT count, D3D12_CPU_DESCRIPTOR_HANDLE dest,
                                               D3D12_CPU_DESCRIPTOR_HANDLE source, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    if (type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV)
        CopyDsvMappings(count, dest, source);

    o_CopyDescriptorsSimple(device, count, dest, source, type);
}

void STDMETHODCALLTYPE hkCopyDescriptors(ID3D12Device* device, UINT destCount, const D3D12_CPU_DESCRIPTOR_HANDLE* dests,
                                         const UINT* destSizes, UINT sourceCount,
                                         const D3D12_CPU_DESCRIPTOR_HANDLE* sources, const UINT* sourceSizes,
                                         D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    if (type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV && dests != nullptr && destSizes != nullptr && sources != nullptr &&
        sourceSizes != nullptr)
    {
        // Both sides are ranges laid end to end over the same total count.
        UINT d = 0, dOff = 0, s = 0, sOff = 0;

        while (d < destCount && s < sourceCount)
        {
            if (dOff >= destSizes[d])
            {
                ++d;
                dOff = 0;
                continue;
            }

            if (sOff >= sourceSizes[s])
            {
                ++s;
                sOff = 0;
                continue;
            }

            D3D12_CPU_DESCRIPTOR_HANDLE to { dests[d].ptr + static_cast<SIZE_T>(dOff) * g_dsvIncrement };
            D3D12_CPU_DESCRIPTOR_HANDLE from { sources[s].ptr + static_cast<SIZE_T>(sOff) * g_dsvIncrement };
            CopyDsvMappings(1, to, from);
            ++dOff;
            ++sOff;
        }
    }

    o_CopyDescriptors(device, destCount, dests, destSizes, sourceCount, sources, sourceSizes, type);
}

void STDMETHODCALLTYPE hkOMSetRenderTargets(ID3D12GraphicsCommandList* list, UINT count,
                                            const D3D12_CPU_DESCRIPTOR_HANDLE* targets, BOOL singleHandle,
                                            const D3D12_CPU_DESCRIPTOR_HANDLE* depthStencil)
{
    if (depthStencil != nullptr)
    {
        const int candidate = CandidateForHandle(depthStencil->ptr);
        g_depthBinds.fetch_add(1, std::memory_order_relaxed);

        if (candidate >= 0)
        {
            g_entries[candidate].binds.fetch_add(1, std::memory_order_relaxed);
            g_depthBindsMatched.fetch_add(1, std::memory_order_relaxed);

            if (!HandleReadOnly(depthStencil->ptr))
                Note(list, candidate, 0, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        }
    }

    o_OMSetRenderTargets(list, count, targets, singleHandle, depthStencil);
}

void STDMETHODCALLTYPE hkClearDepthStencilView(ID3D12GraphicsCommandList* list,
                                               D3D12_CPU_DESCRIPTOR_HANDLE depthStencil, D3D12_CLEAR_FLAGS flags,
                                               FLOAT depth, UINT8 stencil, UINT rectCount, const D3D12_RECT* rects)
{
    const int candidate = CandidateForHandle(depthStencil.ptr);

    if (candidate >= 0)
    {
        Entry& entry = g_entries[candidate];
        entry.clears.fetch_add(1, std::memory_order_relaxed);

        // A clear is only legal in DEPTH_WRITE, so this is one of the few moments the state is certain.
        entry.state.store(D3D12_RESOURCE_STATE_DEPTH_WRITE, std::memory_order_relaxed);
        entry.stateKnown.store(true, std::memory_order_relaxed);

        if ((flags & D3D12_CLEAR_FLAG_DEPTH) != 0)
            entry.reversed.store(depth < 0.5f, std::memory_order_relaxed);

        Note(list, candidate, 1, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }

    o_ClearDepthStencilView(list, depthStencil, flags, depth, stencil, rectCount, rects);
}

void STDMETHODCALLTYPE hkResourceBarrier(ID3D12GraphicsCommandList* list, UINT barrierCount,
                                         const D3D12_RESOURCE_BARRIER* barriers)
{
    if (barriers != nullptr)
    {
        const unsigned int count = g_count.load(std::memory_order_acquire);

        for (UINT i = 0; i < barrierCount; ++i)
        {
            const D3D12_RESOURCE_BARRIER& barrier = barriers[i];

            if (barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
                continue;

            for (unsigned int c = 0; c < count; ++c)
            {
                if (g_entries[c].resource.load(std::memory_order_relaxed) == barrier.Transition.pResource)
                {
                    g_entries[c].state.store(barrier.Transition.StateAfter, std::memory_order_relaxed);
                    g_entries[c].stateKnown.store(true, std::memory_order_relaxed);
                    Note(list, static_cast<int>(c), 2, barrier.Transition.StateAfter);
                    break;
                }
            }
        }
    }

    o_ResourceBarrier(list, barrierCount, barriers);
}

// A list reset without having been submitted: what it recorded never runs.
HRESULT STDMETHODCALLTYPE hkResetList(ID3D12GraphicsCommandList* list, ID3D12CommandAllocator* allocator,
                                      ID3D12PipelineState* state)
{
    {
        std::lock_guard<std::mutex> lock(g_listMutex);
        g_pending.erase(list);
    }

    return o_ResetList(list, allocator, state);
}

void** VtableOf(IUnknown* object) { return *static_cast<void***>(static_cast<void*>(object)); }
} // namespace

namespace DlssNr::DepthTracker
{
bool Install(ID3D12Device* device)
{
    if (g_installed.load(std::memory_order_acquire))
        return true;

    if (device == nullptr)
        return false;

    std::lock_guard<std::mutex> lock(g_installMutex);

    if (g_installed.load(std::memory_order_acquire))
        return true;

    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;

    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list))))
    {
        if (allocator != nullptr)
            allocator->Release();

        LOG_WARN("DLSS-NR depth tracker: no probe command list -- scene depth will not be found");
        return false;
    }

    list->Close();

    o_CreateDepthStencilView = static_cast<CreateDepthStencilViewFn>(VtableOf(device)[kSlotCreateDepthStencilView]);
    o_CopyDescriptors = static_cast<CopyDescriptorsFn>(VtableOf(device)[kSlotCopyDescriptors]);
    o_CopyDescriptorsSimple = static_cast<CopyDescriptorsSimpleFn>(VtableOf(device)[kSlotCopyDescriptorsSimple]);
    g_dsvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    o_ResourceBarrier = static_cast<ResourceBarrierFn>(VtableOf(list)[kSlotResourceBarrier]);
    o_OMSetRenderTargets = static_cast<OMSetRenderTargetsFn>(VtableOf(list)[kSlotOMSetRenderTargets]);
    o_ClearDepthStencilView = static_cast<ClearDepthStencilViewFn>(VtableOf(list)[kSlotClearDepthStencilView]);
    o_ResetList = static_cast<ResetListFn>(VtableOf(list)[kSlotResetList]);

    list->Release();
    allocator->Release();

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&) o_CreateDepthStencilView, hkCreateDepthStencilView);
    DetourAttach(&(PVOID&) o_CopyDescriptors, hkCopyDescriptors);
    DetourAttach(&(PVOID&) o_CopyDescriptorsSimple, hkCopyDescriptorsSimple);
    DetourAttach(&(PVOID&) o_ResourceBarrier, hkResourceBarrier);
    DetourAttach(&(PVOID&) o_OMSetRenderTargets, hkOMSetRenderTargets);
    DetourAttach(&(PVOID&) o_ClearDepthStencilView, hkClearDepthStencilView);
    DetourAttach(&(PVOID&) o_ResetList, hkResetList);
    const LONG committed = DetourTransactionCommit();

    if (committed != NO_ERROR)
    {
        LOG_WARN("DLSS-NR depth tracker: hooks could not be attached ({}) -- scene depth will not be found", committed);
        return false;
    }

    g_installed.store(true, std::memory_order_release);
    LOG_INFO("DLSS-NR depth tracker installed (method adapted from DXL's DepthTracker)");
    return true;
}

bool Installed() { return g_installed.load(std::memory_order_acquire); }

void OnExecute(unsigned int count, ID3D12CommandList* const* lists)
{
    if (!Installed() || lists == nullptr)
        return;

    std::lock_guard<std::mutex> lock(g_listMutex);

    if (g_pending.empty())
        return;

    for (unsigned int l = 0; l < count; ++l)
    {
        const auto found = g_pending.find(lists[l]);
        if (found == g_pending.end())
            continue;

        for (const ListEvent& event : found->second)
        {
            if (event.candidate < 0 || event.candidate >= static_cast<int>(kMaxCandidates))
                continue;

            Entry& entry = g_entries[event.candidate];
            ++g_submitSeq;

            if (event.kind != 2)
            {
                ++entry.writesSubmitted;
                entry.lastWriteSeq = g_submitSeq;
            }

            if (event.kind != 0)
            {
                entry.submittedState = event.state;
                entry.submittedStateKnown = true;
            }
        }

        g_pending.erase(found);
    }
}

int Policy() { return g_policy.load(std::memory_order_relaxed); }

const char* PolicyName(int policy)
{
    switch (policy)
    {
    case 0:
        return "most written in the submitted frame";
    case 1:
        return "last written in the submitted frame";
    default:
        return "most bound while recording (the old rule)";
    }
}

int NextPolicy()
{
    const int next = (g_policy.load(std::memory_order_relaxed) + 1) % kPolicyCount;
    g_policy.store(next, std::memory_order_relaxed);
    return next;
}

void EndFrame(unsigned int renderWidth, unsigned int renderHeight)
{
    if (!Installed())
        return;

    const unsigned int count = g_count.load(std::memory_order_acquire);

    {
        std::lock_guard<std::mutex> lock(g_listMutex);

        for (unsigned int i = 0; i < count; ++i)
        {
            g_entries[i].writesLastFrame = g_entries[i].writesSubmitted;
            g_entries[i].writesSubmitted = 0;
        }

        // A list left open across many frames (never submitted, never reset) is dropped rather than kept.
        if (g_pending.size() > 4096)
            g_pending.clear();
    }

    for (unsigned int i = 0; i < count; ++i)
    {
        Entry& entry = g_entries[i];
        entry.bindsLastFrame = entry.binds.exchange(0, std::memory_order_relaxed);
        entry.clearsLastFrame = entry.clears.exchange(0, std::memory_order_relaxed);

        if (entry.bindsLastFrame != 0 || entry.clearsLastFrame != 0)
        {
            entry.staleFrames = 0;
            continue;
        }

        // Counted, not acted on. DXL drops a candidate unused for 240 frames, handle mappings and all; a game
        // that makes its views once -- Resident Evil 2 -- then loses its scene depth for good the first time a
        // menu or a load screen goes that long without drawing a scene. Slots are reclaimed only when the table
        // is full, and a recycled address is caught by FindOrAddCandidate refreshing the entry.
        if (entry.resource.load(std::memory_order_relaxed) != nullptr && entry.staleFrames < kMaxStaleFrames)
            ++entry.staleFrames;
    }

    // Score, highest wins: the render size exactly, then bound this frame, then how often. Square powers of
    // two are shadow maps. A candidate whose width is not the render width is never taken: a wrong-sized
    // depth handed to the model is worse than none.
    //
    // The size given is the swapchain's, which a letterboxed game draws only part of: Resident Evil 2 draws
    // 1920x1080 inside a 1920x1200 swapchain, so no candidate was ever "the render size exactly" and the
    // choice fell to bind counts alone. A candidate of the full width and a shorter height -- the picture
    // between the bars -- now scores as the render size, the tallest such first.
    //
    // Then, unless the old rule is asked for (Policy 2), only candidates the SUBMITTED work wrote this frame
    // are in the running, ordered by how many writes (Policy 0) or by which was written last (Policy 1), the
    // other breaking ties. The copy at Present then takes the buffer the GPU has just finished writing for
    // this very frame. Record-time counts, with the previous choice winning a tie, could take the buffer of
    // the frame before -- the depth guide sitting 5-11 px off the picture on Resident Evil 2's captures
    // (2026-09-26) whenever anything moved. When nothing was submitted with a candidate in it (a game that
    // submits through a path these hooks do not see), the old rule stands in.
    const int policy = g_policy.load(std::memory_order_relaxed);
    bool anySubmitted = false;

    if (policy != 2)
    {
        for (unsigned int i = 0; i < count; ++i)
        {
            const Entry& entry = g_entries[i];
            if (entry.resource.load(std::memory_order_relaxed) != nullptr && entry.writesLastFrame != 0 &&
                (renderWidth == 0 || entry.width == renderWidth))
                anySubmitted = true;
        }
    }

    g_lastPickSubmitted = anySubmitted;

    int best = -1;
    long long bestScore = -1;
    unsigned long long bestSeq = 0;

    for (unsigned int i = 0; i < count; ++i)
    {
        const Entry& entry = g_entries[i];

        if (entry.resource.load(std::memory_order_relaxed) == nullptr)
            continue;

        if (anySubmitted ? entry.writesLastFrame == 0 : (entry.bindsLastFrame == 0 && entry.clearsLastFrame == 0))
            continue;

        if (renderWidth != 0 && entry.width != renderWidth)
            continue;

        long long score = 0;

        if (renderWidth != 0 && renderHeight != 0 && entry.height == renderHeight)
            score += 1000000;
        else if (renderHeight != 0 && entry.height < renderHeight && entry.height * 2 > renderHeight)
            score += 1000000 - static_cast<long long>(renderHeight - entry.height);
        else
            score += 200000;

        const bool squarePowerOfTwo =
            entry.width == entry.height && entry.width != 0 && (entry.width & (entry.width - 1)) == 0;

        if (squarePowerOfTwo)
            score -= 500000;

        if (anySubmitted)
        {
            // Writes decide (Policy 0), or only break ties (Policy 1, where the last written wins).
            if (policy == 0)
                score += static_cast<long long>(std::min(entry.writesLastFrame, 5000u)) * 10;

            if (score > bestScore || (score == bestScore && entry.lastWriteSeq > bestSeq))
            {
                bestScore = score;
                bestSeq = entry.lastWriteSeq;
                best = static_cast<int>(i);
            }
            continue;
        }

        score += static_cast<long long>(entry.bindsLastFrame) * 10;
        score += entry.clearsLastFrame;

        // Keep the previous choice on a tie: RE Engine hands out a new, identical depth buffer each frame,
        // and without this the choice jumps between dozens of equal candidates.
        if (static_cast<int>(i) == g_selectedIndex)
            score += 5000;

        if (score > bestScore)
        {
            bestScore = score;
            best = static_cast<int>(i);
        }
    }

    if (best < 0)
    {
        g_selectedIndex = -1;

        // What was there instead, every ten seconds or so while nothing qualifies. The heuristic will be wrong
        // for some game; this is where that shows.
        static unsigned int framesWithout = 0;

        if (++framesWithout % 600 == 1)
        {
            std::string seen;
            unsigned int listed = 0;

            for (unsigned int i = 0; i < count && listed < 12; ++i)
            {
                const Entry& entry = g_entries[i];
                if (entry.resource.load(std::memory_order_relaxed) == nullptr)
                    continue;

                seen += std::format("{}{}x{} fmt {} b{} c{}", listed == 0 ? "" : ", ", entry.width, entry.height,
                                    (int) entry.format, entry.bindsLastFrame, entry.clearsLastFrame);
                ++listed;
            }

            LOG_INFO("DLSS-NR depth tracker: no scene depth for a {}x{} frame -- {} candidates, {} DSV handles, {} "
                     "depth binds of which {} matched since the last report: {}",
                     renderWidth, renderHeight, count, g_dsvCount.load(std::memory_order_acquire),
                     g_depthBinds.exchange(0), g_depthBindsMatched.exchange(0),
                     seen.empty() ? std::string("none seen") : seen);
        }

        return;
    }

    const Entry& chosen = g_entries[best];

    // Logged on a change of size, format or rule only: the index moves every frame on RE Engine.
    static int saidPolicy = -1;
    static int saidSubmitted = -1;
    if (chosen.width != g_selectedWidth || chosen.height != g_selectedHeight || chosen.format != g_selectedFormat ||
        saidPolicy != policy || saidSubmitted != (anySubmitted ? 1 : 0))
    {
        saidPolicy = policy;
        saidSubmitted = anySubmitted ? 1 : 0;
        LOG_INFO("DLSS-NR depth tracker: scene depth is {}x{} format {} ({} binds, {} clears last frame, {} "
                 "writes submitted, {} candidates; picked by {}{})",
                 chosen.width, chosen.height, (int) chosen.format, chosen.bindsLastFrame, chosen.clearsLastFrame,
                 chosen.writesLastFrame, count, anySubmitted ? PolicyName(policy) : PolicyName(2),
                 anySubmitted || policy == 2 ? "" : " -- nothing submitted with a candidate in it");
    }

    g_selectedIndex = best;
    g_selectedWidth = chosen.width;
    g_selectedHeight = chosen.height;
    g_selectedFormat = chosen.format;
}

bool Selected(Selection& out)
{
    if (g_selectedIndex < 0)
        return false;

    const Entry& entry = g_entries[g_selectedIndex];
    ID3D12Resource* const resource = entry.resource.load(std::memory_order_relaxed);

    if (resource == nullptr || !entry.stateKnown.load(std::memory_order_relaxed))
        return false;

    out.resource = resource;
    out.width = entry.width;
    out.height = entry.height;
    out.format = entry.format;
    // The state the submitted work left it in, when the pick came from submitted work: the record-time
    // state may already be the next frame's.
    {
        std::lock_guard<std::mutex> lock(g_listMutex);
        out.state = g_lastPickSubmitted && entry.submittedStateKnown
                        ? static_cast<D3D12_RESOURCE_STATES>(entry.submittedState)
                        : static_cast<D3D12_RESOURCE_STATES>(entry.state.load(std::memory_order_relaxed));
    }
    out.reversed = entry.reversed.load(std::memory_order_relaxed);
    return true;
}

void Invalidate()
{
    std::lock_guard<std::mutex> lock(g_addMutex);
    const unsigned int count = g_count.load(std::memory_order_acquire);

    for (unsigned int i = 0; i < count; ++i)
    {
        g_entries[i].resource.store(nullptr, std::memory_order_release);
        g_entries[i].stateKnown.store(false, std::memory_order_relaxed);
        g_entries[i].bindsLastFrame = 0;
        g_entries[i].clearsLastFrame = 0;
        g_entries[i].staleFrames = 0;
    }

    {
        std::lock_guard<std::mutex> lock(g_listMutex);
        g_pending.clear();
        for (unsigned int i = 0; i < count; ++i)
        {
            g_entries[i].writesSubmitted = 0;
            g_entries[i].writesLastFrame = 0;
            g_entries[i].submittedStateKnown = false;
        }
    }

    for (size_t i = 0; i < kDsvTableSize; ++i)
        g_dsvTable[i].candidate.store(-1, std::memory_order_relaxed);

    g_selectedIndex = -1;
    g_selectedWidth = 0;
    g_selectedHeight = 0;
    g_selectedFormat = DXGI_FORMAT_UNKNOWN;
}
} // namespace DlssNr::DepthTracker
