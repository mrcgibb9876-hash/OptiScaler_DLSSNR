// Independent check of Image Clean Up on a real capture (tools/cleanup-lab).
//
//   node tools/cleanup-lab/grade.js <capture> [--variant <after.rgba | after.f32>] [--crops <folder>]
//
// --variant grades another after picture instead of the capture's composed_after: an RGBA8 dump from
// bench.exe (the real shader run on the capture) or raw float32 RGBA from cleanup-harness.js --dump-after.
// --crops writes side-by-side PNGs (input | composed before | composed after | mask) of the four
// worst-glow regions at 2x.
//
// Original header: Shares nothing with the pass's mask, band or
// meter: silhouettes come from depth discontinuities (the depth buffer as captured in mask.g) and from
// the input's own luminance edges; the halo is measured as how much brighter the composed picture is than
// the input just outside a silhouette, relative to the same difference further out on the same surface
// (the model's regional tone change), in stops of linear light.
//
//   node indep.js <capture> [--crops <outdir>] [--variant <composed_after override .f32>]
'use strict';
const fs = require('fs');
const path = require('path');
const { loadCapture, writePng } = require('./cap.js');
const argv = process.argv.slice(2);
const dir = argv[0];
const cropsAt = argv.indexOf('--crops') >= 0 ? argv[argv.indexOf('--crops') + 1] : null;
const variantAt = argv.indexOf('--variant') >= 0 ? argv[argv.indexOf('--variant') + 1] : null;
const quiet = argv.includes('--quiet');
const { S, img } = loadCapture(dir);
const W = S.width, H = S.height, N = W * H;

const dec = (v) => Math.pow(Math.max(v, 0), 2.2);
function linLog(im) {
  const o = new Float32Array(N);
  for (let i = 0; i < N; i++) o[i] = Math.log2(0.2126 * dec(im.data[i * 4]) + 0.7152 * dec(im.data[i * 4 + 1]) + 0.0722 * dec(im.data[i * 4 + 2]) + 1 / 512);
  return o;
}
function chroma(im) { // YCoCg over Y on linear
  const co = new Float32Array(N), cg = new Float32Array(N);
  for (let i = 0; i < N; i++) {
    const r = dec(im.data[i * 4]), g = dec(im.data[i * 4 + 1]), b = dec(im.data[i * 4 + 2]);
    const y = 0.25 * r + 0.5 * g + 0.25 * b + 1e-3;
    co[i] = (0.5 * r - 0.5 * b) / y; cg[i] = (-0.25 * r + 0.5 * g - 0.25 * b) / y;
  }
  return { co, cg };
}
let afterImg = img.composed_after;
if (variantAt) { const raw = fs.readFileSync(variantAt); let f; if (variantAt.endsWith('.rgba')) { f = new Float32Array(N * 4); for (let i = 0; i < N * 4; i++) f[i] = raw[i] / 255; } else f = new Float32Array(raw.buffer.slice(raw.byteOffset, raw.byteOffset + raw.byteLength)); afterImg = { w: W, h: H, data: f }; }
const Lin = linLog(img.input), Lb = linLog(img.composed_before), La = linLog(afterImg), Lm = linLog(img.model);
const D = new Float32Array(N);
for (let i = 0; i < N; i++) D[i] = img.mask.data[i * 4 + 1];

// Box blur, clamped window average.
function box(src, r) {
  const t = new Float32Array(N), o = new Float32Array(N);
  for (let y = 0; y < H; y++) { const row = y * W; for (let x = 0; x < W; x++) { let s = 0, n = 0; for (let k = Math.max(0, x - r); k <= Math.min(W - 1, x + r); k++) { s += src[row + k]; n++; } t[row + x] = s / n; } }
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) { let s = 0, n = 0; for (let k = Math.max(0, y - r); k <= Math.min(H - 1, y + r); k++) { s += t[k * W + x]; n++; } o[y * W + x] = s / n; }
  return o;
}

// ---- silhouettes ---------------------------------------------------------------------------------------
// Depth: a 4-neighbour jump of more than 0.5 stops of reciprocal depth; the farther pixel (lower value,
// inverted depth) is the far side, distance 1. Luma: gradient of the input's 1-pixel-blurred log luminance
// over 1.5 stops (dither-proof), not on a depth edge; the darker side is the "far" side.
const inv = S.cleanupDepthInverted !== 0;
const nearer = (a, b) => (inv ? D[a] > D[b] : D[a] < D[b]);
const distD = new Int16Array(N).fill(-1), distL = new Int16Array(N).fill(-1), sideNear = new Uint8Array(N);
const LinB = box(Lin, 1);
let q = [];
for (let y = 1; y < H - 1; y++) for (let x = 1; x < W - 1; x++) {
  const i = y * W + x;
  for (const j of [i + 1, i - 1, i + W, i - W]) {
    if (Math.abs(D[j] - D[i]) > 0.5) { if (nearer(j, i)) { if (distD[i] !== 1) { distD[i] = 1; q.push(i); } } else sideNear[i] = 1; }
  }
}
function grow(dist, q, max, same) {
  for (let h = 0; h < q.length; h++) {
    const i = q[h], x = i % W, y = (i / W) | 0;
    if (dist[i] >= max) continue;
    for (const [dx, dy] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
      const xx = x + dx, yy = y + dy; if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
      const j = yy * W + xx;
      if (dist[j] < 0 && same(i, j)) { dist[j] = dist[i] + 1; q.push(j); }
    }
  }
}
grow(distD, q, 32, (i, j) => Math.abs(D[i] - D[j]) < 0.5 && !sideNear[j]);
q = [];
for (let y = 1; y < H - 1; y++) for (let x = 1; x < W - 1; x++) {
  const i = y * W + x;
  if (distD[i] >= 0 && distD[i] <= 3) continue;
  if (sideNear[i]) continue;
  const gx = LinB[i + 1] - LinB[i - 1], gy = LinB[i + W] - LinB[i - W];
  if (Math.hypot(gx, gy) > 1.5) {
    // mark the darker neighbour along the gradient as distance 1
    const j = Math.abs(gx) > Math.abs(gy) ? (gx > 0 ? i - 1 : i + 1) : (gy > 0 ? i - W : i + W);
    if (distL[j] < 0 && distD[j] < 0) { distL[j] = 1; q.push(j); }
  }
}
grow(distL, q, 24, (i, j) => Math.abs(LinB[i] - LinB[j]) < 0.35 && distD[j] < 0);

// Colour silhouettes, depth not consulted at all (the Present route's depth can sit several pixels off the
// picture): every strong edge of the input's blurred log luminance (> 1 stop across 2 px), its darker side
// at distance 1, grown through pixels of similar luminance.
const distC = new Int16Array(N).fill(-1);
q = [];
for (let y = 1; y < H - 1; y++) for (let x = 1; x < W - 1; x++) {
  const i = y * W + x;
  const gx = LinB[i + 1] - LinB[i - 1], gy = LinB[i + W] - LinB[i - W];
  if (Math.hypot(gx, gy) > 1.0) {
    const j = Math.abs(gx) > Math.abs(gy) ? (gx > 0 ? i - 1 : i + 1) : (gy > 0 ? i - W : i + W);
    if (distC[j] < 0) { distC[j] = 1; q.push(j); }
  }
}
grow(distC, q, 24, (i, j) => Math.abs(LinB[i] - LinB[j]) < 0.35);

// The same, only for colour edges with a depth jump somewhere within 12 px: object silhouettes found by
// colour, tolerant of the depth guide sitting a few pixels off.
const nearJump = new Uint8Array(N);
{
  const jump = new Uint8Array(N);
  for (let y = 0; y < H - 1; y++) for (let x = 0; x < W - 1; x++) { const i = y * W + x; if (Math.abs(D[i + 1] - D[i]) > 0.7 || Math.abs(D[i + W] - D[i]) > 0.7) jump[i] = 1; }
  const t = new Uint8Array(N);
  for (let y = 0; y < H; y++) { let run = -1e9; for (let x = 0; x < W; x++) { if (jump[y * W + x]) run = x; if (x - run <= 12) t[y * W + x] = 1; } run = 1e9; for (let x = W - 1; x >= 0; x--) { if (jump[y * W + x]) run = x; if (run - x <= 12) t[y * W + x] = 1; } }
  for (let x = 0; x < W; x++) { let run = -1e9; for (let y = 0; y < H; y++) { if (t[y * W + x]) run = y; if (y - run <= 12) nearJump[y * W + x] = 1; } run = 1e9; for (let y = H - 1; y >= 0; y--) { if (t[y * W + x]) run = y; if (run - y <= 12) nearJump[y * W + x] = 1; } }
}
const distS = new Int16Array(N).fill(-1);
q = [];
for (let i = 0; i < N; i++) if (distC[i] === 1 && nearJump[i]) { distS[i] = 1; q.push(i); }
grow(distS, q, 24, (i, j) => Math.abs(LinB[i] - LinB[j]) < 0.35);

// ---- measurements --------------------------------------------------------------------------------------
function profile(dist, Lx) {
  const s = new Float64Array(33), n = new Float64Array(33);
  for (let i = 0; i < N; i++) { const d = dist[i]; if (d >= 1 && d <= 32) { s[d] += Lx[i] - Lin[i]; n[d]++; } }
  return { s, n, at: (a, b) => { let ss = 0, nn = 0; for (let d = a; d <= b; d++) { ss += s[d]; nn += n[d]; } return nn ? ss / nn : 0; } };
}
// Halo per silhouette pixel neighbourhood: local, so a regional tone change elsewhere does not mask it.
// For each far-side pixel at distance 1..6, its excess over the mean (composed - input) of far-side pixels at
// distance 14..24 within a 49x49 window around it. Reported: mean excess (stops), and the fraction of those
// pixels with excess > 0.1 stop (a visible glow).
function localHalo(dist, Lx, near0, near1, far0, far1) {
  // coarse grid of baseline means, 16 px cells, from pixels at far distance
  const CW = Math.ceil(W / 16), CH = Math.ceil(H / 16), bs = new Float64Array(CW * CH), bn = new Float64Array(CW * CH);
  for (let i = 0; i < N; i++) { const d = dist[i]; if (d >= far0 && d <= far1) { const c = ((i / W / 16) | 0) * CW + ((i % W) / 16 | 0); bs[c] += Lx[i] - Lin[i]; bn[c]++; } }
  let s = 0, n = 0, vis = 0, pos = 0;
  for (let i = 0; i < N; i++) {
    const d = dist[i]; if (d < near0 || d > near1) continue;
    const cx = ((i % W) / 16) | 0, cy = ((i / W) / 16) | 0; let ss = 0, nn = 0;
    for (let j = -1; j <= 1; j++) for (let k = -1; k <= 1; k++) { const X = cx + k, Y = cy + j; if (X < 0 || Y < 0 || X >= CW || Y >= CH) continue; ss += bs[Y * CW + X]; nn += bn[Y * CW + X]; }
    if (nn < 20) continue;
    const e = (Lx[i] - Lin[i]) - ss / nn;
    s += e; n++; if (e > 0.1) vis++; if (e > 0) pos += e;
  }
  return { mean: n ? s / n : 0, glow: n ? pos / n : 0, visible: n ? vis / n : 0, n };
}
// Band-pass detail (dither-proof): 1-px box minus 4-px box, RMS, per region class.
function detail(Lx, cls) {
  const bp = box(Lx, 1), wide = box(Lx, 4), acc = {};
  for (let i = 0; i < N; i++) { const c = cls(i); if (!c) continue; const v = bp[i] - wide[i]; acc[c] = acc[c] || [0, 0]; acc[c][0] += v * v; acc[c][1]++; }
  const o = {}; for (const k in acc) o[k] = Math.sqrt(acc[k][0] / acc[k][1]); return o;
}
const cls = (i) => (sideNear[i] || (distD[i] < 0 && false) ? 'outline(near side)' : distD[i] >= 1 && distD[i] <= 8 ? 'far side 1-8px' : distD[i] < 0 && distL[i] < 0 ? 'away from edges' : null);
// "away": neither depth nor luma edge within reach -- approximate with both distances unset and not near side
const res = {};
for (const [name, Lx] of [['model', Lm], ['before', Lb], ['after', La]]) {
  res[name] = { depth: localHalo(distD, Lx, 1, 6, 14, 24), luma: localHalo(distL, Lx, 1, 4, 10, 20), colour: localHalo(distC, Lx, 1, 5, 12, 22), sil: localHalo(distS, Lx, 1, 5, 12, 22) };
}
const detIn = detail(Lin, cls), detB = detail(Lb, cls), detA = detail(La, cls);

// per-pixel local excess (composed - input minus the local far baseline), depth far side 1..12
function localExcess(dist, Lx, far0, far1, maxd) {
  const CW = Math.ceil(W / 16), CH = Math.ceil(H / 16), bs = new Float64Array(CW * CH), bn = new Float64Array(CW * CH), e = new Float32Array(N).fill(NaN);
  for (let i = 0; i < N; i++) { const d = dist[i]; if (d >= far0 && d <= far1) { const c = ((i / W / 16) | 0) * CW + ((i % W) / 16 | 0); bs[c] += Lx[i] - Lin[i]; bn[c]++; } }
  for (let i = 0; i < N; i++) { const d = dist[i]; if (d < 1 || d > maxd) continue; const cx = ((i % W) / 16) | 0, cy = ((i / W) / 16) | 0; let ss = 0, nn = 0; for (let j = -1; j <= 1; j++) for (let k = -1; k <= 1; k++) { const X = cx + k, Y = cy + j; if (X < 0 || Y < 0 || X >= CW || Y >= CH) continue; ss += bs[Y * CW + X]; nn += bn[Y * CW + X]; } if (nn >= 20) e[i] = (Lx[i] - Lin[i]) - ss / nn; }
  return e;
}
const eB = localExcess(distD, Lb, 14, 24, 12), eA = localExcess(distD, La, 14, 24, 12);
const xB = localExcess(distC, Lb, 12, 22, 8), cA2 = localExcess(distC, La, 12, 22, 8);
let cRim = 0, cRimN = 0;
for (let i = 0; i < N; i++) if (distC[i] >= 1 && distC[i] <= 8 && !isNaN(cA2[i])) { cRimN++; if (cA2[i] < -0.15 && cA2[i] < xB[i] - 0.05) cRim++; }
// artifacts
let darkRim = 0, rimN = 0, maxJump = [], chromaShift = 0, chromaN = 0, chromaShiftAway = 0, awayN = 0, lift = 0;
const cB = chroma(img.composed_before), cA = chroma(afterImg), cI = chroma(img.input);
const corr = new Float32Array(N); for (let i = 0; i < N; i++) corr[i] = La[i] - Lb[i];
for (let i = 0; i < N; i++) {
  const d = distD[i];
  if (d >= 1 && d <= 12 && !isNaN(eA[i])) { rimN++; if (eA[i] < -0.15 && eA[i] < eB[i] - 0.05) darkRim++; }
  const dc = Math.hypot(cA.co[i] - cB.co[i], cA.cg[i] - cB.cg[i]);
  if (corr[i] !== 0 || dc > 0) { chromaShift += dc; chromaN++; }
  if (distD[i] < 0 && distL[i] < 0 && !sideNear[i]) { chromaShiftAway += dc; awayN++; lift = Math.max(lift, Math.abs(corr[i])); }
}
const jumps = [];
for (let y = 0; y < H; y++) for (let x = 0; x < W - 1; x++) { const i = y * W + x; if ((corr[i] !== 0 || corr[i + 1] !== 0) && Math.abs(D[i] - D[i + 1]) < 0.2 && Math.abs(Lb[i] - Lb[i + 1]) < 0.3) jumps.push(Math.abs(corr[i + 1] - corr[i])); }
jumps.sort((a, b) => a - b);
const pct = (p) => (jumps.length ? jumps[Math.floor(p * (jumps.length - 1))] : 0);
let moved = 0; for (let i = 0; i < N; i++) if (Math.abs(corr[i]) > 0.01) moved++;

const f = (v) => v.toFixed(3);
const name = path.basename(dir);
if (!quiet) {
  console.log(`${name}: depth-silhouette far-side px (1-6) ${res.before.depth.n}, luma-edge dark-side px (1-4) ${res.before.luma.n}`);
  for (const k of ['model', 'before', 'after']) console.log(`  ${k.padEnd(6)} halo at depth silhouettes: mean ${f(res[k].depth.mean)} glow ${f(res[k].depth.glow)} stops, visible(>0.1) ${(100 * res[k].depth.visible).toFixed(1)}% | at luma edges: mean ${f(res[k].luma.mean)} glow ${f(res[k].luma.glow)}, visible ${(100 * res[k].luma.visible).toFixed(1)}%`);
  for (const k of ['model', 'before', 'after']) console.log(`  ${k.padEnd(6)} halo at COLOUR silhouettes (dark side 1-5px vs 12-22px): mean ${f(res[k].colour.mean)} glow ${f(res[k].colour.glow)} stops, visible(>0.1) ${(100 * res[k].colour.visible).toFixed(1)}% (n ${res[k].colour.n})`);
  for (const k of ['model', 'before', 'after']) console.log(`  ${k.padEnd(6)} halo at OBJECT silhouettes by colour (depth jump within 12px): mean ${f(res[k].sil.mean)} glow ${f(res[k].sil.glow)} stops, visible(>0.1) ${(100 * res[k].sil.visible).toFixed(1)}% (n ${res[k].sil.n})`);
  console.log(`  dark rims at colour silhouettes (dark side 1-8px, >0.15 under local tone and moved >0.05 darker): ${(100 * cRim / Math.max(cRimN, 1)).toFixed(2)}%`);
  console.log('  band-pass detail RMS (stops) input / before / after:');
  for (const k of Object.keys(detB)) console.log(`    ${k.padEnd(18)} ${f(detIn[k])} / ${f(detB[k])} / ${f(detA[k])}  (after keeps ${(100 * detA[k] / detB[k]).toFixed(1)}% of before)`);
  console.log(`  pixels moved >0.01 stop: ${(100 * moved / N).toFixed(1)}%; dark rims (far side 1-12px, after darker than its local tone by >0.15 and moved >0.05 darker): ${(100 * darkRim / Math.max(rimN, 1)).toFixed(2)}%`);
  console.log(`  correction seam on one surface: adjacent-pixel jump p99 ${f(pct(0.99))} p99.9 ${f(pct(0.999))} max ${f(pct(1))} stops; chroma shift where moved ${(chromaShift / Math.max(chromaN, 1)).toFixed(4)}, away from edges ${(chromaShiftAway / Math.max(awayN, 1)).toFixed(4)}, largest move away from edges ${f(lift)} stops`);
  const pr = (Lx) => { const p = profile(distD, Lx); return [1, 2, 3, 4, 6, 8, 12, 16, 24].map((d) => `${d}:${f(p.at(d, d))}`).join(' '); };
  console.log('  far-side profile (composed - input, stops) by distance: before ' + pr(Lb));
  console.log('                                                        after  ' + pr(La));
}
module.exports = { res };
if (argv.includes('--json')) console.log(JSON.stringify({ name, halo: { before: res.before, after: res.after, model: res.model }, detail: { input: detIn, before: detB, after: detA } }));

// ---- crops ------------------------------------------------------------------------------------------------
if (cropsAt) {
  fs.mkdirSync(cropsAt, { recursive: true });
  // worst glow: 96x96 windows scored by the summed positive local excess of the composed-before picture on
  // the dark side of object silhouettes found by colour (1-5 px, against 12-22 px)
  const T = 96, scores = [];
  const ex = new Float32Array(N);
  { // reuse localHalo's baseline, per pixel
    const CW = Math.ceil(W / 16), CH = Math.ceil(H / 16), bs = new Float64Array(CW * CH), bn = new Float64Array(CW * CH);
    for (let i = 0; i < N; i++) { const d = distS[i]; if (d >= 12 && d <= 22) { const c = ((i / W / 16) | 0) * CW + ((i % W) / 16 | 0); bs[c] += Lb[i] - Lin[i]; bn[c]++; } }
    for (let i = 0; i < N; i++) { const d = distS[i]; if (d < 1 || d > 5) continue; const cx = ((i % W) / 16) | 0, cy = ((i / W) / 16) | 0; let ss = 0, nn = 0; for (let j = -1; j <= 1; j++) for (let k = -1; k <= 1; k++) { const X = cx + k, Y = cy + j; if (X < 0 || Y < 0 || X >= CW || Y >= CH) continue; ss += bs[Y * CW + X]; nn += bn[Y * CW + X]; } if (nn >= 20) ex[i] = Math.max((Lb[i] - Lin[i]) - ss / nn, 0); }
  }
  for (let y = 0; y + T <= H; y += 16) for (let x = 0; x + T <= W; x += 16) { let s = 0; for (let j = 0; j < T; j += 2) for (let k = 0; k < T; k += 2) s += ex[(y + j) * W + x + k]; scores.push({ x, y, s }); }
  scores.sort((a, b) => b.s - a.s);
  const picked = [];
  for (const c of scores) { if (picked.length >= 4 || c.s <= 0) break; if (picked.every((p) => Math.abs(p.x - c.x) >= T || Math.abs(p.y - c.y) >= T)) picked.push(c); }
  const Z = 2, gap = 4, P = 4, OW = P * T * Z + (P - 1) * gap, OH = T * Z;
  picked.forEach((c, n) => {
    // one display gain for all panels: input's 98th percentile to 0.85
    const vals = []; for (let j = 0; j < T; j++) for (let k = 0; k < T; k++) { const i = ((c.y + j) * W + c.x + k) * 4; vals.push(Math.max(img.input.data[i], img.input.data[i + 1], img.input.data[i + 2])); }
    vals.sort((a, b) => a - b); const gain = Math.min(Math.max(0.85 / Math.max(vals[Math.floor(0.98 * vals.length)], 1e-3), 1), 6);
    const rgb = new Uint8Array(OW * OH * 3).fill(255);
    const srcs = [img.input.data, img.composed_before.data, afterImg.data, null];
    for (let p = 0; p < P; p++) for (let y = 0; y < OH; y++) for (let x = 0; x < T * Z; x++) {
      const sx = c.x + ((x / Z) | 0), sy = c.y + ((y / Z) | 0), i = (sy * W + sx) * 4; let col;
      if (srcs[p]) col = [srcs[p][i] * gain, srcs[p][i + 1] * gain, srcs[p][i + 2] * gain];
      else { const m = img.mask.data; col = [m[i], Math.min(Math.abs(La[sy * W + sx] - Lb[sy * W + sx]) * 4, 1), 0.25 * Math.min(Math.max(img.input.data[i + 1] * gain, 0), 1)]; }
      const o = (y * OW + p * (T * Z + gap) + x) * 3;
      for (let ch = 0; ch < 3; ch++) rgb[o + ch] = Math.max(0, Math.min(255, Math.round(col[ch] * 255)));
    }
    const file = path.join(cropsAt, `crop${n + 1}_x${c.x}_y${c.y}_gain${gain.toFixed(1)}.png`);
    writePng(file, OW, OH, rgb);
    console.log('  crop ' + file);
  });
}
