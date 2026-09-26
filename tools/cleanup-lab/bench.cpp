// GPU timing bench for the DLSS-NR resolve (mode 1), Image Clean Up's prep (CSPrep) and the halo meter
// (mode 5) on a frame captured in game, with GPU timestamps -- the real shader, the capture's inputs and
// settings, the depth taken from the mask's log-depth channel. Also writes each config's picture as
// <name>.rgba (RGBA8), which grade.js --variant reads.
//
//   cl /O2 /EHsc /std:c++17 bench.cpp            (x64 Native Tools prompt)
//   set BENCH_PREP=OptiScaler/shaders/dlssnr/precompile/DlssNr_ShaderPrep.cso
//   bench.exe OptiScaler/shaders/dlssnr/precompile/DlssNr_Shader.cso <capture dir> 100 off:strength=0 on:prep=1
//
// config: name:key=value,...  keys: strength edge balance motion history profile depth meter contrast colour detail brightness prep
// (prep=1 runs the prep dispatch first and binds it, as the engine does).
#define NOMINMAX
#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <map>
#include <algorithm>
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAILED %s: %08x (line %d)\n", #x, (unsigned) hr_, __LINE__); exit(1); } } while (0)

static std::vector<char> ReadAll(const std::string& p) {
  std::ifstream f(p, std::ios::binary); if (!f) { printf("cannot read %s\n", p.c_str()); exit(1); }
  return std::vector<char>((std::istreambuf_iterator<char>(f)), {});
}
static std::string Str(const std::string& json, const std::string& key) {
  auto k = json.find("\"" + key + "\":"); if (k == std::string::npos) return "0";
  k += key.size() + 3; auto e = json.find_first_of(",}", k); return json.substr(k, e - k);
}
static float F(const std::string& j, const char* k) { return (float) atof(Str(j, k).c_str()); }
static uint32_t U(const std::string& j, const char* k) { std::string s = Str(j, k); if (s == "true") return 1; if (s == "false") return 0; return (uint32_t) atof(s.c_str()); }

struct Constants {
  uint32_t Mode; float WhitePoint; uint32_t Width, Height; float TransferStrength, ColourStrength; uint32_t DebugView; float MaxRatio;
  uint32_t Passthrough; float MvScaleX, MvScaleY; uint32_t GuideWidth, GuideHeight, CompareMode; float CompareSplit, CompareZoom;
  uint32_t CompareSwap, Transfer; float DebugScale; uint32_t ReversibleMode, ApplyModel, UseGameExposure; float ExposurePreMul, Brightness, Contrast;
  float CleanupStrength, CleanupEdge, CleanupBalance, CleanupMotion; uint32_t CleanupHaveMotion, CleanupHaveDepth, CleanupDepthInverted, CleanupHistory, CleanupProfile, CleanupPrepared;
};

ID3D12Device* dev;
ID3D12CommandQueue* queue;
ID3D12CommandAllocator* alloc;
ID3D12GraphicsCommandList* list;
ID3D12Fence* fence; UINT64 fenceValue = 0; HANDLE fenceEvent;

void Flush() { queue->Signal(fence, ++fenceValue); if (fence->GetCompletedValue() < fenceValue) { fence->SetEventOnCompletion(fenceValue, fenceEvent); WaitForSingleObject(fenceEvent, INFINITE); } }
void Submit() { CHECK(list->Close()); ID3D12CommandList* l[] = { list }; queue->ExecuteCommandLists(1, l); Flush(); CHECK(alloc->Reset()); CHECK(list->Reset(alloc, nullptr)); }

ID3D12Resource* Tex(DXGI_FORMAT fmt, UINT w, UINT h, bool uav) {
  D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_DEFAULT };
  D3D12_RESOURCE_DESC d {}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = fmt; d.SampleDesc.Count = 1;
  d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
  ID3D12Resource* r; CHECK(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r))); return r;
}
ID3D12Resource* Upload(DXGI_FORMAT fmt, UINT w, UINT h, UINT bpp, const void* data, UINT srcPitch, bool uav) {
  ID3D12Resource* t = Tex(fmt, w, h, uav);
  UINT pitch = (w * bpp + 255) & ~255u;
  D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_UPLOAD };
  D3D12_RESOURCE_DESC d {}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = (UINT64) pitch * h; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Resource* up; CHECK(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&up)));
  char* m; CHECK(up->Map(0, nullptr, (void**) &m));
  for (UINT y = 0; y < h; y++) memcpy(m + (size_t) y * pitch, (const char*) data + (size_t) y * srcPitch, (size_t) w * bpp);
  up->Unmap(0, nullptr);
  D3D12_TEXTURE_COPY_LOCATION dst { t, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; dst.SubresourceIndex = 0;
  D3D12_TEXTURE_COPY_LOCATION src { up, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; src.PlacedFootprint.Footprint = { fmt, w, h, 1, pitch };
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  Submit(); up->Release();
  return t;
}

int main(int argc, char** argv) {
  if (argc < 5) { printf("usage: bench <cso> <capture> <iters> <config>...\n"); return 1; }
  std::string dir = argv[2]; int iters = atoi(argv[3]);
  auto cso = ReadAll(argv[1]);
  auto manifestRaw = ReadAll(dir + "/manifest.json"); std::string mj(manifestRaw.begin(), manifestRaw.end());
  auto settingsAt = mj.find("\"settings\""); std::string sj = mj.substr(settingsAt);

  IDXGIFactory6* fac; CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&fac)));
  IDXGIAdapter1* ad; CHECK(fac->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&ad)));
  DXGI_ADAPTER_DESC1 add; ad->GetDesc1(&add); printf("adapter: %ls\n", add.Description);
  CHECK(D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)));

  D3D12_COMMAND_QUEUE_DESC qd { D3D12_COMMAND_LIST_TYPE_DIRECT }; CHECK(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
  CHECK(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)));
  CHECK(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list)));
  CHECK(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))); fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

  const UINT W = U(sj, "width"), H = U(sj, "height");
  auto rgba8 = [&](const char* n) { auto b = ReadAll(dir + "/" + n + ".raw"); return Upload(DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 4, b.data(), W * 4, false); };
  ID3D12Resource* proxy = rgba8("proxy"); ID3D12Resource* model = rgba8("model"); ID3D12Resource* input = rgba8("input");
  const bool haveMask = std::ifstream(dir + "/mask.raw").good();
  const bool haveDepthFile = std::ifstream(dir + "/depth.raw").good();
  std::vector<char> maskRaw = haveMask ? ReadAll(dir + "/mask.raw") : std::vector<char>((size_t) W * H * 8, 0);
  ID3D12Resource* history = Upload(DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, 8, maskRaw.data(), W * 8, true);
  // depth: inverted depth from the mask's log (green, half) -> R32_FLOAT
  std::vector<float> depth((size_t) W * H);
  for (size_t i = 0; i < depth.size(); i++) {
    uint16_t h = *(const uint16_t*) (maskRaw.data() + i * 8 + 2);
    int e = (h >> 10) & 31, m = h & 1023; float s = (h & 0x8000) ? -1.f : 1.f;
    float v = e == 0 ? s * m * powf(2, -24) : s * (1 + m / 1024.f) * powf(2, (float) e - 15);
    depth[i] = powf(2.0f, v);
  }
  if (haveDepthFile) { // the captured depth guide (R32_FLOAT rows, pitch W*4)
    auto dr = ReadAll(dir + "/depth.raw");
    if (dr.size() >= depth.size() * 4) memcpy(depth.data(), dr.data(), depth.size() * 4);
  }
  ID3D12Resource* depthTex = Upload(DXGI_FORMAT_R32_FLOAT, W, H, 4, depth.data(), W * 4, false);
  ID3D12Resource* target = Tex(DXGI_FORMAT_R8G8B8A8_UNORM, W, H, true);
  ID3D12Resource* keep = Tex(DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, true);
  ID3D12Resource* aux = Tex(DXGI_FORMAT_R16_FLOAT, W, H, true);
  ID3D12Resource* prep = Tex(DXGI_FORMAT_R16G16B16A16_FLOAT, W, H + 2 * ((H + 7) / 8), true);
  ID3D12Resource* grid[3] = { Tex(DXGI_FORMAT_R32G32B32A32_FLOAT, 64, 64, true), Tex(DXGI_FORMAT_R32G32B32A32_FLOAT, 64, 64, true), Tex(DXGI_FORMAT_R32G32B32A32_FLOAT, 64, 64, true) };

  // root signature: one table, 8 SRV, 3 UAV, 1 CBV; static linear clamp sampler
  D3D12_DESCRIPTOR_RANGE1 ranges[3] = {
    { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 8, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 0 },
    { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 8 },
    { D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 11 } };
  D3D12_ROOT_PARAMETER1 rp {}; rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp.DescriptorTable = { 3, ranges };
  D3D12_STATIC_SAMPLER_DESC samp {}; samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR; samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP; samp.MaxLOD = D3D12_FLOAT32_MAX; samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsd {}; rsd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1; rsd.Desc_1_1 = { 1, &rp, 1, &samp, D3D12_ROOT_SIGNATURE_FLAG_NONE };
  ID3DBlob *sig, *err; CHECK(D3D12SerializeVersionedRootSignature(&rsd, &sig, &err));
  ID3D12RootSignature* root; CHECK(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root)));
  D3D12_COMPUTE_PIPELINE_STATE_DESC pd {}; pd.pRootSignature = root; pd.CS = { cso.data(), cso.size() };
  ID3D12PipelineState* pso; CHECK(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)));
  ID3D12PipelineState* prepPso = pso; std::vector<char> prepCso;
  if (const char* pp = getenv("BENCH_PREP")) { prepCso = ReadAll(pp); D3D12_COMPUTE_PIPELINE_STATE_DESC p2 = pd; p2.CS = { prepCso.data(), prepCso.size() }; CHECK(dev->CreateComputePipelineState(&p2, IID_PPV_ARGS(&prepPso))); printf("prep entry from %s\n", pp); }

  // two descriptor tables: resolve and meter
  D3D12_DESCRIPTOR_HEAP_DESC hd { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 60, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
  ID3D12DescriptorHeap* heap; CHECK(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
  UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto cpu = [&](UINT i) { auto h = heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += (SIZE_T) i * inc; return h; };
  auto gpu = [&](UINT i) { auto h = heap->GetGPUDescriptorHandleForHeapStart(); h.ptr += (UINT64) i * inc; return h; };
  auto srv = [&](ID3D12Resource* r, UINT i) { dev->CreateShaderResourceView(r, nullptr, cpu(i)); };
  auto uav = [&](ID3D12Resource* r, UINT i) { dev->CreateUnorderedAccessView(r, nullptr, nullptr, cpu(i)); };
  // constant buffers
  D3D12_HEAP_PROPERTIES uhp { D3D12_HEAP_TYPE_UPLOAD };
  D3D12_RESOURCE_DESC cbd {}; cbd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; cbd.Width = 1024; cbd.Height = 1; cbd.DepthOrArraySize = 1; cbd.MipLevels = 1; cbd.SampleDesc.Count = 1; cbd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Resource* cb; CHECK(dev->CreateCommittedResource(&uhp, D3D12_HEAP_FLAG_NONE, &cbd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&cb)));
  char* cbMap; CHECK(cb->Map(0, nullptr, (void**) &cbMap));
  // resolve table 0..11
  srv(proxy, 0); srv(model, 1); srv(input, 2); srv(proxy, 3); srv(proxy, 4); srv(depthTex, 5); srv(history, 6); srv(aux, 7);
  uav(target, 8); uav(keep, 9); uav(aux, 10);
  D3D12_CONSTANT_BUFFER_VIEW_DESC cv { cb->GetGPUVirtualAddress(), 256 }; dev->CreateConstantBufferView(&cv, cpu(11));
  // meter table 12..23: t6 = keep (what the resolve wrote), t7 = aux, u0..u2 grids
  for (UINT i = 0; i < 6; i++) srv(proxy, 12 + i); srv(keep, 18); srv(aux, 19);
  // aux is also a UAV in the resolve -- in the meter it is read as t7
  uav(grid[0], 20); uav(grid[1], 21); uav(grid[2], 22);
  D3D12_CONSTANT_BUFFER_VIEW_DESC cv2 { cb->GetGPUVirtualAddress() + 256, 256 }; dev->CreateConstantBufferView(&cv2, cpu(23));

  // prepared resolve table 24..35 (t7 = prep), prep table 36..47 (u0 = prep)
  srv(proxy, 24); srv(model, 25); srv(input, 26); srv(proxy, 27); srv(proxy, 28); srv(depthTex, 29); srv(history, 30); srv(prep, 31);
  uav(target, 32); uav(keep, 33); uav(aux, 34); dev->CreateConstantBufferView(&cv, cpu(35));
  for (UINT i = 0; i < 8; i++) srv(i == 2 ? input : i == 5 ? depthTex : proxy, 36 + i);
  uav(prep, 44); uav(keep, 45); uav(aux, 46);
  for (UINT i = 0; i < 8; i++) srv(i == 7 ? prep : proxy, 48 + i); uav(prep, 56); uav(keep, 57); uav(aux, 58);
  D3D12_CONSTANT_BUFFER_VIEW_DESC cv3 { cb->GetGPUVirtualAddress() + 512 - 256 + 0, 256 };
  // queries
  D3D12_QUERY_HEAP_DESC qhd { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 8 }; ID3D12QueryHeap* qh; CHECK(dev->CreateQueryHeap(&qhd, IID_PPV_ARGS(&qh)));
  D3D12_HEAP_PROPERTIES rhp { D3D12_HEAP_TYPE_READBACK };
  D3D12_RESOURCE_DESC rbd = cbd; rbd.Width = 64; ID3D12Resource* rb; CHECK(dev->CreateCommittedResource(&rhp, D3D12_HEAP_FLAG_NONE, &rbd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)));
  UINT64 freq; CHECK(queue->GetTimestampFrequency(&freq));

  // put every resource in a usable state
  auto barrier = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) { D3D12_RESOURCE_BARRIER br {}; br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; br.Transition = { r, 0, a, b }; list->ResourceBarrier(1, &br); };
  for (auto* r : { proxy, model, input, history, depthTex }) barrier(r, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  for (auto* r : { target, keep, aux, prep, grid[0], grid[1], grid[2] }) barrier(r, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  Submit();

  Constants base {};
  base.Mode = 1; base.WhitePoint = F(sj, "whitePoint"); base.Width = W; base.Height = H; base.TransferStrength = F(sj, "transferStrength"); base.ColourStrength = F(sj, "colourStrength");
  base.MaxRatio = F(sj, "maxRatio"); base.Passthrough = U(sj, "passthrough"); base.MvScaleX = 1; base.MvScaleY = 1; base.GuideWidth = U(sj, "guideWidth"); base.GuideHeight = U(sj, "guideHeight");
  base.CompareZoom = 1; base.Transfer = U(sj, "transfer"); base.DebugScale = 1; base.ReversibleMode = U(sj, "reversibleMode"); base.ApplyModel = 1; base.UseGameExposure = 0;
  base.ExposurePreMul = F(sj, "exposurePreMul"); base.Brightness = F(sj, "brightness"); base.Contrast = F(sj, "contrast");
  base.CleanupStrength = F(sj, "cleanupStrength"); base.CleanupEdge = F(sj, "cleanupEdge"); base.CleanupBalance = F(sj, "cleanupBalance"); base.CleanupMotion = F(sj, "cleanupMotion");
  base.CleanupHaveMotion = 0; base.CleanupHaveDepth = 1; base.CleanupDepthInverted = U(sj, "cleanupDepthInverted"); base.CleanupHistory = 2; base.CleanupProfile = 0;

  for (int c = 4; c < argc; c++) {
    std::string spec = argv[c]; auto colon = spec.find(':'); std::string name = spec.substr(0, colon);
    Constants k = base; bool meter = true; bool usePrep = false; bool quietPass = false;
    std::stringstream ss(colon == std::string::npos ? "" : spec.substr(colon + 1)); std::string kv;
    while (std::getline(ss, kv, ',')) {
      auto eq = kv.find('='); std::string key = kv.substr(0, eq); float v = (float) atof(kv.substr(eq + 1).c_str());
      if (key == "strength") k.CleanupStrength = v; else if (key == "edge") k.CleanupEdge = v; else if (key == "balance") k.CleanupBalance = v;
      else if (key == "motion") k.CleanupMotion = v; else if (key == "history") k.CleanupHistory = (uint32_t) v; else if (key == "profile") k.CleanupProfile = (uint32_t) v;
      else if (key == "depth") k.CleanupHaveDepth = (uint32_t) v; else if (key == "meter") meter = v != 0; else if (key == "contrast") k.Contrast = v; else if (key == "colour") k.ColourStrength = v; else if (key == "detail") k.TransferStrength = v; else if (key == "brightness") k.Brightness = v; else if (key == "prep") usePrep = v != 0; else if (key == "quietpass") quietPass = v != 0;
    }
    if (k.CleanupStrength <= 0) meter = false;
    Constants m = k; m.Mode = 5; m.Width = 64; m.Height = 64;
    Constants pp = k; pp.Mode = 6; Constants pq = k; pq.Mode = 7;
    if (usePrep) k.CleanupPrepared = 1;
    memcpy(cbMap, &k, sizeof k); memcpy(cbMap + 256, &m, sizeof m); memcpy(cbMap + 512, &pp, sizeof pp); memcpy(cbMap + 768, &pq, sizeof pq);
    { D3D12_CONSTANT_BUFFER_VIEW_DESC c3 { cb->GetGPUVirtualAddress() + 512, 256 }; dev->CreateConstantBufferView(&c3, cpu(47)); D3D12_CONSTANT_BUFFER_VIEW_DESC c4 { cb->GetGPUVirtualAddress() + 768, 256 }; dev->CreateConstantBufferView(&c4, cpu(59)); }
    double best[2] = { 1e9, 1e9 }, sum[2] = { 0, 0 }, prepSum = 0; int n = 0;
    for (int it = 0; it < iters; it++) {
      ID3D12DescriptorHeap* hs[] = { heap }; list->SetDescriptorHeaps(1, hs); list->SetComputeRootSignature(root); list->SetPipelineState(pso);
      list->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, 0);
      if (usePrep && k.CleanupStrength > 0) {
        list->SetPipelineState(prepPso); list->SetComputeRootDescriptorTable(0, gpu(36)); list->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
        { D3D12_RESOURCE_BARRIER u2 {}; u2.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; u2.UAV.pResource = prep; list->ResourceBarrier(1, &u2); }
        if (quietPass) { list->SetComputeRootDescriptorTable(0, gpu(48)); list->Dispatch(((W + 7) / 8 + 7) / 8, ((H + 7) / 8 + 7) / 8, 1); }
        list->SetPipelineState(pso);
        barrier(prep, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, 3);
        list->SetComputeRootDescriptorTable(0, gpu(24)); list->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
        barrier(prep, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      } else {
      list->SetComputeRootDescriptorTable(0, gpu(0)); list->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
      }
      D3D12_RESOURCE_BARRIER ub {}; ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; list->ResourceBarrier(1, &ub);
      list->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, 1);
      if (meter) {
        barrier(keep, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); barrier(aux, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->SetComputeRootDescriptorTable(0, gpu(12)); list->Dispatch(8, 8, 1);
        barrier(keep, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); barrier(aux, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      }
      list->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, 2);
      if (!(usePrep && k.CleanupStrength > 0)) list->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, 3);
      list->ResolveQueryData(qh, D3D12_QUERY_TYPE_TIMESTAMP, 0, 4, rb, 0);
      Submit();
      UINT64* t; D3D12_RANGE r { 0, 32 }; CHECK(rb->Map(0, &r, (void**) &t));
      double a = (t[1] - t[0]) * 1000.0 / freq, b = (t[2] - t[1]) * 1000.0 / freq; double pr = usePrep && k.CleanupStrength > 0 ? (t[3] - t[0]) * 1000.0 / freq : 0; prepSum += pr; D3D12_RANGE w { 0, 0 }; rb->Unmap(0, &w);
      if (it < 5) continue; // warm up
      best[0] = std::min(best[0], a); best[1] = std::min(best[1], b); sum[0] += a; sum[1] += b; n++;
    }
    { // read the picture back: <name>.rgba (RGBA8, W*H)
      UINT pitch = (W * 4 + 255) & ~255u; D3D12_RESOURCE_DESC bd = cbd; bd.Width = (UINT64) pitch * H;
      ID3D12Resource* back; CHECK(dev->CreateCommittedResource(&rhp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&back)));
      barrier(target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION s { target, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; s.SubresourceIndex = 0;
      D3D12_TEXTURE_COPY_LOCATION d { back, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; d.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, pitch };
      list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
      barrier(target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); Submit();
      char* p; CHECK(back->Map(0, nullptr, (void**) &p)); std::ofstream o(name + ".rgba", std::ios::binary);
      for (UINT y = 0; y < H; y++) o.write(p + (size_t) y * pitch, W * 4); back->Unmap(0, nullptr); back->Release();
    }
    printf("%-22s resolve+prep mean %.3f ms (best %.3f; prep alone %.3f)  meter mean %.3f ms\n", name.c_str(), sum[0] / n, best[0], prepSum / (n + 5), sum[1] / n);
  }
  // write the last config's output for checking
  return 0;
}

