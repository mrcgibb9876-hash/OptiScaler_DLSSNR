#pragma once
#include "SysUtils.h"
#include <d3d12.h>

class GpuTime_Dx12
{
    // The slot read back is the oldest in the ring. Three was too few: a game that queues frames ahead,
    // or the DLSS5 Feeder running two passes a frame, has not executed a list three passes old yet, and
    // the read could land mid-resolve -- a start from one pass against the end of another.
    static constexpr int QUERY_BUFFER_COUNT = 8;

    ID3D12QueryHeap* _queryHeap = nullptr;
    ID3D12Resource* _readbackBuffer = nullptr;
    std::array<bool, QUERY_BUFFER_COUNT> _trigger {};

    // Ticks per second belong to the GPU, not the queue, so they are asked once. The queue a caller
    // passes in can be the game's, and asking a queue the game has since released each frame is how
    // the frequency -- and every reading -- turns to garbage.
    UINT64 _frequency = 0;

    int _currentFrameIndex = 0;
    bool _init = false;

  public:
    GpuTime_Dx12(ID3D12Device* device);
    ~GpuTime_Dx12();

    void Start(ID3D12GraphicsCommandList* cmdList);
    void End(ID3D12GraphicsCommandList* cmdList);

    std::optional<double> ReadGpuTime(ID3D12CommandQueue* commandQueue);
};

class ScopedGpuTime_Dx12
{
    GpuTime_Dx12* _gpuTime;
    ID3D12GraphicsCommandList* _cmdList;

  public:
    ScopedGpuTime_Dx12(GpuTime_Dx12* gpuTime, ID3D12GraphicsCommandList* cmdList) : _gpuTime(gpuTime), _cmdList(cmdList)
    {
        if (_gpuTime && _cmdList)
            _gpuTime->Start(_cmdList);
    }

    ~ScopedGpuTime_Dx12()
    {
        if (_gpuTime && _cmdList)
            _gpuTime->End(_cmdList);
    }
};
