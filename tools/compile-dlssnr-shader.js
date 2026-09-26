// Rebuilds the DLSS-NR pass shader (OptiScaler/shaders/dlssnr/precompile/dlssnr.hlsl) for both APIs:
//
//   DlssNr_Shader.cso / DlssNr_Shader.h        D3D12, DXIL (symbol DlssNr_cso)
//   DlssNr_ShaderPrep.cso / DlssNr_ShaderPrep.h D3D12, DXIL, entry CSPrep: Image Clean Up's prep (symbol
//                                              DlssNr_prep_cso)
//   DlssNr_Shader_Vk.spv / DlssNr_Shader_Vk.h  Vulkan, SPIR-V with VK_MODE defined (symbol dlssnr_spv)
//
//   node tools/compile-dlssnr-shader.js [vulkan-dxc.exe] [d3d12-dxc.exe]
//
// Two compilers, on purpose, because that is how the shipped binaries were made (checked 2026-09-23 by
// rebuilding the unchanged shader: each comes out byte-identical only with its own compiler):
//   Vulkan  Microsoft's release dxc -- the Windows SDK's cannot emit SPIR-V (same as
//           tools/compile-upsamplers-vk.js).
//   D3D12   the Windows SDK's dxc, the one every D3D12 game has run so far.
// Re-run after touching dlssnr.hlsl -- the binaries are what the engine loads; the .hlsl alone changes
// nothing.
'use strict';
const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');

const DXC_VK = process.argv[2] || 'C:/Users/mrcgi/dev/tools/dxc/bin/x64/dxc.exe';
const DXC_DX12 = process.argv[3] || 'C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/dxc.exe';
const DIR = path.join(__dirname, '..', 'OptiScaler/shaders/dlssnr/precompile');
const SRC = path.join(DIR, 'dlssnr.hlsl');

const header = (bytes, symbol) => {
  const lines = ['#pragma once', '', `inline static const unsigned char ${symbol}[] = {`];
  for (let i = 0; i < bytes.length; i += 12) {
    const row = [...bytes.slice(i, i + 12)].map((b) => `0x${b.toString(16).padStart(2, '0')}`);
    lines.push('    ' + row.join(', ') + (i + 12 >= bytes.length ? '' : ', '));
  }
  lines.push('};', '');
  return lines.join('\r\n');
};

const builds = [
  { out: 'DlssNr_Shader', symbol: 'DlssNr_cso', ext: '.cso', dxc: DXC_DX12, args: [] },
  { out: 'DlssNr_ShaderPrep', symbol: 'DlssNr_prep_cso', ext: '.cso', dxc: DXC_DX12, args: [], entry: 'CSPrep' },
  { out: 'DlssNr_Shader_Vk', symbol: 'dlssnr_spv', ext: '.spv', dxc: DXC_VK, args: ['-spirv', '-DVK_MODE=1'] },
];

for (const b of builds) {
  const bin = path.join(DIR, b.out + b.ext);
  execFileSync(b.dxc, ['-T', 'cs_6_0', '-E', b.entry || 'CSMain', ...b.args, '-Fo', bin, SRC], { stdio: 'pipe' });
  const bytes = fs.readFileSync(bin);
  fs.writeFileSync(path.join(DIR, `${b.out}.h`), header(bytes, b.symbol));
  console.log(`${b.out}${b.ext}: ${bytes.length} bytes`);
}
