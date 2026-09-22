// Builds the SPIR-V for the Output Scaling upsamplers, which Vulkan had none of.
//
// On D3D the extra upsamplers (EWA Lanczos, xBR-lv2, Sharp bilinear, Integer scale, Nearest) are HLSL
// compiled at runtime; Vulkan's Shader_Vk takes SPIR-V and nothing else, so it fell back to bicubic
// and said so in the log. The sources already carry the `#ifdef VK_MODE` register spaces, so they
// compile as they are -- they just need compiling ahead of time, by a compiler that emits SPIR-V.
//
// The shader text is READ OUT OF OS_Upsamplers.cpp rather than copied, and assembled exactly as
// Assemble() does there (preamble, sigmoid for EWA only, then the body), so a change to a filter
// cannot leave the committed blob describing a different shader. Re-run after any such change:
//
//   node tools/compile-upsamplers-vk.js [path-to-dxc.exe]
//
// dxc must be one that emits SPIR-V (the Windows SDK's does not; Microsoft's own release does).
'use strict';
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { execFileSync } = require('node:child_process');

const REPO = path.resolve(__dirname, '..');
const SRC = path.join(REPO, 'OptiScaler/shaders/output_scaling/OS_Upsamplers.cpp');
const OUT = path.join(REPO, 'OptiScaler/shaders/output_scaling/precompile');
const DXC = process.argv[2] || 'C:/Users/mrcgi/dev/tools/dxc/bin/x64/dxc.exe';

// Each filter: the body literal in OS_Upsamplers.cpp, whether Assemble() gives it the sigmoid helper,
// and the names the committed headers use (matching the bicubic pair already there).
const FILTERS = [
  { literal: 'kEwaLanczos', sigmoid: true, file: 'BCUS_ewa', symbol: 'bcus_ewa_spv' },
  { literal: 'kXbr', sigmoid: false, file: 'BCUS_xbr', symbol: 'bcus_xbr_spv' },
  { literal: 'kSharpBilinear', sigmoid: false, file: 'BCUS_sharpbilinear', symbol: 'bcus_sharpbilinear_spv' },
  { literal: 'kIntegerScale', sigmoid: false, file: 'BCUS_integer', symbol: 'bcus_integer_spv' },
  { literal: 'kNearest', sigmoid: false, file: 'BCUS_nearest', symbol: 'bcus_nearest_spv' },
];

const cpp = fs.readFileSync(SRC, 'utf8');
const literal = (name) => {
  const start = cpp.indexOf(`const char* ${name} = R"(`);
  if (start < 0) throw new Error(`string literal not found: ${name}`);
  const from = cpp.indexOf('R"(', start) + 3;
  const to = cpp.indexOf(')";', from);
  if (to < 0) throw new Error(`unterminated literal: ${name}`);
  return cpp.slice(from, to);
};

const preamble = literal('kPreamble');
const sigmoid = literal('kSigmoid');
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'upsampler-spv-'));

// 12 bytes a line, CRLF, last line without its comma -- the format every committed blob header uses.
const header = (bytes, symbol) => {
  const lines = ['#pragma once', '', `inline static const unsigned char ${symbol}[] = {`];
  for (let i = 0; i < bytes.length; i += 12) {
    const row = [...bytes.slice(i, i + 12)].map((b) => `0x${b.toString(16).padStart(2, '0')}`);
    lines.push('    ' + row.join(', ') + (i + 12 >= bytes.length ? '' : ', '));
  }
  lines.push('};', '');
  return lines.join('\r\n');
};

for (const f of FILTERS) {
  const source = preamble + (f.sigmoid ? sigmoid : '') + literal(f.literal);
  const hlsl = path.join(tmp, `${f.file}.hlsl`);
  const spv = path.join(OUT, `${f.file}_Shader_Vk.spv`);
  fs.writeFileSync(hlsl, source);

  execFileSync(DXC, ['-T', 'cs_6_0', '-E', 'CSMain', '-spirv', '-DVK_MODE=1', '-Fo', spv, hlsl], { stdio: 'pipe' });
  const bytes = fs.readFileSync(spv);
  fs.writeFileSync(path.join(OUT, `${f.file}_Shader_Vk.h`), header(bytes, f.symbol));
  console.log(`${f.file}: ${bytes.length} bytes`);
}

fs.rmSync(tmp, { recursive: true, force: true });
