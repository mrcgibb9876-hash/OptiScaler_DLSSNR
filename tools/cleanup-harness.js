// Offline harness for DLSS-NR Image Clean Up, on frames captured in game.
//
//   node tools/cleanup-harness.js <capture folder> [--strength 1] [--reach 0.75] [--edge 1.5]
//        [--tol-full 0] [--tol-zero 0.15] [--depth-tol 0.35] [--shift-r 48] [--no-shift] [--no-seg]
//        [--out <folder>] [--images]
//
// The capture comes from the engine (Ctrl+Shift+F12, the panel's "Capture frame for Image Clean Up",
// [DlssNr] CleanUpCapture=true, or a dlssnr-cleanup-capture.trigger file): a folder under
// dlssnr-cleanup-capture\ beside OptiScaler with manifest.json and raw surfaces (see
// OptiScaler/dlssnr/DlssNr_CleanCapture.h).
//
// This is a CPU port of the clean up in OptiScaler/shaders/dlssnr/precompile/dlssnr.hlsl (the resolve's
// Image Clean Up block and the functions it calls), run on the captured composed-before picture, with the
// variant settings from the command line. It prints the same halo readings the engine logs (75th
// percentile over 64x64 tiles of the mask-weighted excess past the loosest band, in stops), for the
// engine's own after picture and for the variant, plus how much fine detail survives inside the mask,
// what changed outside it, and the glow's profile with distance from the nearest silhouette -- the
// numbers that say whether a change to the shader helps before it is built. Keep it in step with the
// shader: when the shader changes, change this the same way.
//
// Single frame, so no mask history; motion is not modelled (the captured vectors are written but not
// read here). --images writes BMPs of the mask, the glow before and after, and the pictures.
'use strict';
const fs = require('fs');
const path = require('path');

// ---- arguments ----------------------------------------------------------------------------------------
const argv = process.argv.slice(2);
if (argv.length < 1) {
  console.error('usage: node tools/cleanup-harness.js <capture folder> [options]');
  process.exit(1);
}
const dir = argv[0];
const opt = (name, def) => {
  const i = argv.indexOf('--' + name);
  return i >= 0 && i + 1 < argv.length && !argv[i + 1].startsWith('--') ? parseFloat(argv[i + 1]) : def;
};
const flag = (name) => argv.includes('--' + name);
const outDir = (() => {
  const i = argv.indexOf('--out');
  return i >= 0 ? argv[i + 1] : path.join(dir, 'harness');
})();

const manifest = JSON.parse(fs.readFileSync(path.join(dir, 'manifest.json'), 'utf8'));
const S = manifest.settings;

const V = {
  strength: opt('strength', S.cleanupStrength > 0 ? S.cleanupStrength : 1.0),
  reach: opt('reach', S.cleanupBalance),
  edge: opt('edge', S.cleanupEdge),
  tolZero: opt('tol-zero', 0.15),
  tolFull: opt('tol-full', 0.0),
  depthTol: opt('depth-tol', 0.35),
  shiftR: opt('shift-r', 48),
  noShift: flag('no-shift'),
  shiftMedian: flag('shift-median'), // the plain median of the same-side taps instead of the lower one
  noSeg: flag('no-seg'),
};

// ---- decoding -----------------------------------------------------------------------------------------
function half(h) {
  const s = (h & 0x8000) ? -1 : 1, e = (h >> 10) & 0x1f, m = h & 0x3ff;
  if (e === 0) return s * m * Math.pow(2, -24);
  if (e === 31) return m ? NaN : s * Infinity;
  return s * (1 + m / 1024) * Math.pow(2, e - 15);
}
function uf(bits, mBits) { // unsigned small float (R11G11B10)
  const e = bits >> mBits, m = bits & ((1 << mBits) - 1);
  if (e === 0) return m * Math.pow(2, -14) / (1 << mBits);
  if (e === 31) return m ? NaN : Infinity;
  return (1 + m / (1 << mBits)) * Math.pow(2, e - 15);
}
const srgb = (v) => (v <= 0.04045 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4));

// Returns { w, h, c, data: Float32Array(w*h*4) } with the shader's view of the texels (an _SRGB format
// decoded to linear, as its view would).
function load(name) {
  const f = manifest.files.find((x) => x.name === name);
  if (!f) return null;
  const buf = fs.readFileSync(path.join(dir, f.file));
  const w = f.width, h = f.height, pitch = f.rowPitch, base = f.offset || 0;
  const out = new Float32Array(w * h * 4);
  const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  for (let y = 0; y < h; y++) {
    const row = base + y * pitch;
    for (let x = 0; x < w; x++) {
      const o = (y * w + x) * 4;
      let r = 0, g = 0, b = 0, a = 1;
      switch (f.format) {
        case 'R32G32B32A32_FLOAT': { const p = row + x * 16; r = dv.getFloat32(p, true); g = dv.getFloat32(p + 4, true); b = dv.getFloat32(p + 8, true); a = dv.getFloat32(p + 12, true); break; }
        case 'R16G16B16A16_FLOAT': { const p = row + x * 8; r = half(dv.getUint16(p, true)); g = half(dv.getUint16(p + 2, true)); b = half(dv.getUint16(p + 4, true)); a = half(dv.getUint16(p + 6, true)); break; }
        case 'R16G16B16A16_UNORM': { const p = row + x * 8; r = dv.getUint16(p, true) / 65535; g = dv.getUint16(p + 2, true) / 65535; b = dv.getUint16(p + 4, true) / 65535; a = dv.getUint16(p + 6, true) / 65535; break; }
        case 'R10G10B10A2_UNORM': { const v = dv.getUint32(row + x * 4, true); r = (v & 1023) / 1023; g = ((v >>> 10) & 1023) / 1023; b = ((v >>> 20) & 1023) / 1023; a = (v >>> 30) / 3; break; }
        case 'R11G11B10_FLOAT': { const v = dv.getUint32(row + x * 4, true); r = uf(v & 0x7ff, 6); g = uf((v >>> 11) & 0x7ff, 6); b = uf((v >>> 22) & 0x3ff, 5); break; }
        case 'R8G8B8A8_UNORM': case 'R8G8B8A8_UNORM_SRGB': { const p = row + x * 4; r = buf[p] / 255; g = buf[p + 1] / 255; b = buf[p + 2] / 255; a = buf[p + 3] / 255; if (f.format.endsWith('SRGB')) { r = srgb(r); g = srgb(g); b = srgb(b); } break; }
        case 'B8G8R8A8_UNORM': case 'B8G8R8A8_UNORM_SRGB': { const p = row + x * 4; b = buf[p] / 255; g = buf[p + 1] / 255; r = buf[p + 2] / 255; a = buf[p + 3] / 255; if (f.format.endsWith('SRGB')) { r = srgb(r); g = srgb(g); b = srgb(b); } break; }
        case 'R32_FLOAT': r = dv.getFloat32(row + x * 4, true); break;
        // Depth-stencil families (the depth guide): the depth in the first 4 bytes of 8, or the low 24 bits of 4.
        case 'R32_FLOAT_X8X24': r = dv.getFloat32(row + x * 8, true); break;
        case 'R24_UNORM_X8': r = (dv.getUint32(row + x * 4, true) & 0xffffff) / 16777215; break;
        case 'R16_FLOAT': r = half(dv.getUint16(row + x * 2, true)); break;
        case 'R16_UNORM': r = dv.getUint16(row + x * 2, true) / 65535; break;
        case 'R32G32_FLOAT': r = dv.getFloat32(row + x * 8, true); g = dv.getFloat32(row + x * 8 + 4, true); break;
        case 'R16G16_FLOAT': r = half(dv.getUint16(row + x * 4, true)); g = half(dv.getUint16(row + x * 4 + 2, true)); break;
        case 'R16G16_SNORM': r = Math.max(dv.getInt16(row + x * 4, true) / 32767, -1); g = Math.max(dv.getInt16(row + x * 4 + 2, true) / 32767, -1); break;
        default: throw new Error('format ' + f.format);
      }
      out[o] = r; out[o + 1] = g; out[o + 2] = b; out[o + 3] = a;
    }
  }
  return { w, h, data: out };
}

const img = {};
for (const n of ['input', 'proxy', 'model', 'composed_before', 'composed_after', 'depth', 'mask', 'model_reading'])
  img[n] = load(n);
for (const n of ['input', 'proxy', 'model', 'composed_before']) if (!img[n]) { console.error('missing ' + n + ' in the capture'); process.exit(1); }

const W = S.width, H = S.height;
const passthrough = S.passthrough !== 0;
const normScale = passthrough ? 1 : Math.max(S.whitePoint, 1e-4);
// Depth: the captured depth guide, or -- for a capture made before depth was written (format 21, the
// D32S8 SRV) -- the log reciprocal depth the resolve itself stored in the mask's green channel, which is
// exactly what the shader's tile holds (as a half).
const depthFromMask = !img.depth && !!img.mask && S.cleanupHaveDepth !== 0;
const haveDepth = (!!img.depth || depthFromMask) && S.cleanupHaveDepth !== 0;

// ---- the shader's functions -----------------------------------------------------------------------------
const kLuma = [0.2126, 0.7152, 0.0722];
const FLOOR = 1 / 512, FLOOR_E = 0.0587;
const clog = (y) => (passthrough ? 2.2 * Math.log2(Math.max(y, 0) + FLOOR_E) : Math.log2(Math.max(y, 0) + FLOOR));
const cunlog = (l) => (passthrough ? Math.max(Math.pow(2, l / 2.2) - FLOOR_E, 0) : Math.max(Math.pow(2, l) - FLOOR, 0));
const srgbToLinear = (v) => { v = Math.min(Math.max(v, 0), 1); return v < 0.04045 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4); };
const displayLuma = (r, g, b) => (passthrough ? kLuma[0] * Math.max(r, 0) + kLuma[1] * Math.max(g, 0) + kLuma[2] * Math.max(b, 0)
  : kLuma[0] * srgbToLinear(r) + kLuma[1] * srgbToLinear(g) + kLuma[2] * srgbToLinear(b));
const ss = (a, b, x) => { const t = Math.min(Math.max((x - a) / (b - a), 0), 1); return t * t * (3 - 2 * t); };
const sat = (v) => Math.min(Math.max(v, 0), 1);
const clampi = (v, a, b) => (v < a ? a : v > b ? b : v);

function sampleBilinear(im, u, v, ch) { // u,v normalised; clamp addressing
  const x = u * im.w - 0.5, y = v * im.h - 0.5;
  const x0 = Math.floor(x), y0 = Math.floor(y), tx = x - x0, ty = y - y0;
  const at = (xx, yy) => im.data[(clampi(yy, 0, im.h - 1) * im.w + clampi(xx, 0, im.w - 1)) * 4 + ch];
  return (at(x0, y0) * (1 - tx) + at(x0 + 1, y0) * tx) * (1 - ty) + (at(x0, y0 + 1) * (1 - tx) + at(x0 + 1, y0 + 1) * tx) * ty;
}
const lumaOf = (im, i) => kLuma[0] * Math.max(im.data[i * 4], 0) + kLuma[1] * Math.max(im.data[i * 4 + 1], 0) + kLuma[2] * Math.max(im.data[i * 4 + 2], 0);

// Per-pixel log luminance of the input and depth under each pixel (what the shader's tile holds).
const Lin = new Float32Array(W * H), Din = new Float32Array(W * H);
{
  const iw = img.input.w;
  const gW = Math.max(S.guideWidth || 1, 1), gH = Math.max(S.guideHeight || 1, 1);
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    const ix = Math.min(x, iw - 1), iy = Math.min(y, img.input.h - 1);
    Lin[y * W + x] = clog(lumaOf(img.input, iy * iw + ix) / normScale);
    if (depthFromMask) {
      Din[y * W + x] = img.mask.data[(y * img.mask.w + x) * 4 + 1];
    } else if (haveDepth) {
      const gx = clampi(Math.floor(((x + 0.5) / W) * gW), 0, gW - 1), gy = clampi(Math.floor(((y + 0.5) / H) * gH), 0, gH - 1);
      const d = img.depth.data[(Math.min(gy, img.depth.h - 1) * img.depth.w + Math.min(gx, img.depth.w - 1)) * 4];
      Din[y * W + x] = Math.log2(Math.max(S.cleanupDepthInverted ? d : 1 - d, 1e-7));
    }
  }
}
const Lat = (x, y) => Lin[clampi(y, 0, H - 1) * W + clampi(x, 0, W - 1)];
const Dat = (x, y) => Din[clampi(y, 0, H - 1) * W + clampi(x, 0, W - 1)];

const MESO = [[4, 0], [-4, 0], [0, 4], [0, -4], [3, 3], [-3, 3], [3, -3], [-3, -3]];
const WIDE = [[9, 0], [-9, 0], [0, 9], [0, -9], [6, 6], [-6, 6], [6, -6], [-6, -6]];
const DNEAR = [[3, 0], [-3, 0], [0, 3], [0, -3], [6, 0], [-6, 0], [0, 6], [0, -6], [4, 4], [-4, 4], [4, -4], [-4, -4]];
const DFAR = [[9, 0], [-9, 0], [0, 9], [0, -9], [12, 0], [-12, 0], [0, 12], [0, -12], [8, 8], [-8, 8], [8, -8], [-8, -8]];

function stats(x, y, p) {
  const xc = Lat(x, y), dc = Dat(x, y);
  const wM = sat(2 * p.reach), wW = sat(2 * p.reach - 1);
  let fmin = xc, fmax = xc, mmin = xc, mmax = xc, wmin = xc, wmax = xc;
  let aS = 0, aQ = 0, aN = 0, smin = xc, smax = xc, sS = 0, sQ = 0, sN = 0;
  const same = (tx, ty) => p.noSeg || !haveDepth || Math.abs(Dat(tx, ty) - dc) < p.depthTol;
  for (let j = -1; j <= 1; j++) for (let i = -1; i <= 1; i++) {
    const v = Lat(x + i, y + j); fmin = Math.min(fmin, v); fmax = Math.max(fmax, v); aS += v; aQ += v * v; aN++;
    if (same(x + i, y + j)) { smin = Math.min(smin, v); smax = Math.max(smax, v); sS += v; sQ += v * v; sN++; }
  }
  for (let k = 0; k < 8; k++) {
    const m = Lat(x + MESO[k][0], y + MESO[k][1]), w = Lat(x + WIDE[k][0], y + WIDE[k][1]);
    mmin = Math.min(mmin, m); mmax = Math.max(mmax, m); wmin = Math.min(wmin, w); wmax = Math.max(wmax, w);
    aS += m + w; aQ += m * m + w * w; aN += 2;
    if (wM > 0 && same(x + MESO[k][0], y + MESO[k][1])) { smin = Math.min(smin, m); smax = Math.max(smax, m); sS += m; sQ += m * m; sN++; }
    if (wW > 0 && same(x + WIDE[k][0], y + WIDE[k][1])) { smin = Math.min(smin, w); smax = Math.max(smax, w); sS += w; sQ += w * w; sN++; }
  }
  const thr = Math.max(p.edge, 0.05);
  const lumaEdge = Math.max(ss(thr, 2 * thr, fmax - fmin), wM * ss(thr, 2 * thr, mmax - mmin), wW * ss(thr, 2 * thr, wmax - wmin));
  if (sN >= 5) {
    const mean = sS / sN;
    return { x: xc, lo: smin, hi: smax, mean, sigma: Math.sqrt(Math.max(sQ / sN - mean * mean, 0)), lumaEdge, seg: true };
  }
  const lo = (1 - wW) * ((1 - wM) * fmin + wM * mmin) + wW * Math.min(mmin, wmin);
  const hi = (1 - wW) * ((1 - wM) * fmax + wM * mmax) + wW * Math.max(mmax, wmax);
  const mean = aS / aN;
  return { x: xc, lo, hi, mean, sigma: Math.sqrt(Math.max(aQ / aN - mean * mean, 0)), lumaEdge, seg: false };
}

function depthMask(x, y, p) {
  if (!haveDepth) return [0, 0, 0];
  const c = Dat(x, y);
  let lo = c, hi = c;
  for (let j = -1; j <= 1; j++) for (let i = -1; i <= 1; i++) { const v = Dat(x + i, y + j); lo = Math.min(lo, v); hi = Math.max(hi, v); }
  for (const [a, b] of DNEAR) { const v = Dat(x + a, y + b); lo = Math.min(lo, v); hi = Math.max(hi, v); }
  const nearRange = hi - lo;
  const wW = sat(2 * sat(p.reach) - 1);
  if (wW > 0) {
    let flo = lo, fhi = hi;
    for (const [a, b] of DFAR) { const v = Dat(x + a, y + b); flo = Math.min(flo, v); fhi = Math.max(fhi, v); }
    lo = lo + (flo - lo) * wW; hi = hi + (fhi - hi) * wW;
  }
  const range = hi - lo;
  const edge = Math.max(ss(0.35, 1, nearRange), ss(0.35, 1, range) * (1 - 0.2 * wW));
  const near = sat((c - lo) / Math.max(range, 1e-4));
  return [edge, 1 - near, c];
}

function edgeMask(lumaEdge, dm) {
  if (!haveDepth) return lumaEdge;
  const far = ss(0.25, 0.75, dm[1]);
  return sat(dm[0] * far * (0.6 + 0.4 * lumaEdge) + (1 - dm[0]) * 0.3 * lumaEdge);
}

function band(s, strength, p) {
  const gamma = 2.5 - 1.5 * sat(strength);
  const tol = p.tolZero + (p.tolFull - p.tolZero) * sat(strength);
  const t = sat((s.x - s.lo) / Math.max(s.hi - s.lo, 1e-4));
  const room = tol + (s.seg ? 0 : ss(0.15, 0.5, Math.min(t, 1 - t)) * 0.5 * (s.hi - s.lo));
  const boxLo = Math.max(s.lo, s.mean - gamma * s.sigma), boxHi = Math.min(s.hi, s.mean + gamma * s.sigma);
  return [Math.max(s.x - room, Math.min(boxLo, s.x) - tol), Math.min(s.x + room, Math.max(boxHi, s.x) + tol)];
}

// The group's shift taps: per 8x8 group, four taps shiftR out from its middle.
function shiftFor(x, y, dc, p) {
  if (p.noShift) return 0;
  const gx = Math.floor(x / 8) * 8 + 4, gy = Math.floor(y / 8) * 8 + 4;
  const taps = [];
  for (const [a, b] of [[p.shiftR, 0], [-p.shiftR, 0], [0, p.shiftR], [0, -p.shiftR]]) {
    const u = (gx + a) / W, v = (gy + b) / H;
    const m = displayLuma(sampleBilinear(img.model, u, v, 0), sampleBilinear(img.model, u, v, 1), sampleBilinear(img.model, u, v, 2));
    const q = displayLuma(sampleBilinear(img.proxy, u, v, 0), sampleBilinear(img.proxy, u, v, 1), sampleBilinear(img.proxy, u, v, 2));
    const tx = clampi(Math.floor(gx + a), 0, W - 1), ty = clampi(Math.floor(gy + b), 0, H - 1);
    taps.push({ s: clog(m) - clog(q), d: haveDepth ? Dat(tx, ty) : 0 });
  }
  const same = taps.filter((t) => !haveDepth || Math.abs(t.d - dc) < 1).map((t) => t.s).sort((a, b) => a - b);
  if (same.length === 0) { const all = taps.map((t) => t.s).sort((a, b) => a - b); return 0.5 * (all[1] + all[2]); }
  if (p.shiftMedian) return same.length % 2 ? same[(same.length - 1) / 2] : 0.5 * (same[same.length / 2 - 1] + same[same.length / 2]);
  return same[Math.floor((same.length - 1) / 2)];
}

// ---- run -------------------------------------------------------------------------------------------------
function resultLog(im, i) { return clog(lumaOf(im, i) / normScale); }

const N = W * H;
const edgeA = new Float32Array(N), exB = new Float32Array(N), exA = new Float32Array(N), exE = new Float32Array(N), exM = new Float32Array(N);
const outLog = new Float32Array(N), beforeLog = new Float32Array(N);
let segCount = 0, maskCount = 0;
const t0 = Date.now();
for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
  const i = y * W + x;
  const rb = resultLog(img.composed_before, i);
  beforeLog[i] = rb;
  outLog[i] = rb;
  const s = stats(x, y, V), dm = depthMask(x, y, V);
  const e = edgeMask(s.lumaEdge, dm);
  edgeA[i] = e;
  if (e <= 0) continue;
  maskCount++; if (s.seg) segCount++;
  const shift = shiftFor(x, y, dm[2], V);
  const b = band(s, V.strength, V).map((v) => v + shift), loose = band(s, 0, V).map((v) => v + shift);
  const amount = Math.min(1.5 * V.strength, 1) * sat(1.5 * e);
  const moved = amount > 1e-4 ? rb + (Math.min(Math.max(rb, b[0]), b[1]) - rb) * amount : rb;
  outLog[i] = moved;
  const ex = (v) => Math.max(v - loose[1], 0) + Math.max(loose[0] - v, 0);
  exB[i] = e * ex(rb);
  exA[i] = e * ex(moved);
  if (img.composed_after) exE[i] = e * ex(resultLog(img.composed_after, i));
  const u = (x + 0.5) / W, v = (y + 0.5) / H;
  const ml = clog(displayLuma(sampleBilinear(img.model, u, v, 0), sampleBilinear(img.model, u, v, 1), sampleBilinear(img.model, u, v, 2)))
    - clog(displayLuma(sampleBilinear(img.proxy, u, v, 0), sampleBilinear(img.proxy, u, v, 1), sampleBilinear(img.proxy, u, v, 2)));
  exM[i] = e * ex(s.x + ml);
}

// The meter's reduction: 64x64 tiles, 8x8 lattice each, mask-weighted means, 75th percentile over tiles.
function meter(ex) {
  const vals = [];
  for (let ty = 0; ty < 64; ty++) for (let tx = 0; tx < 64; tx++) {
    const x0 = Math.floor((tx * W) / 64), x1 = Math.floor(((tx + 1) * W) / 64), y0 = Math.floor((ty * H) / 64), y1 = Math.floor(((ty + 1) * H) / 64);
    const sx = Math.max(Math.floor((x1 - x0) / 8), 1), sy = Math.max(Math.floor((y1 - y0) / 8), 1);
    let w = 0, e = 0;
    for (let y = y0 + (sy >> 1); y < Math.max(y1, y0 + 1); y += sy) for (let x = x0 + (sx >> 1); x < Math.max(x1, x0 + 1); x += sx) {
      const i = Math.min(y, H - 1) * W + Math.min(x, W - 1);
      if (edgeA[i] > 0.05) { w += edgeA[i]; e += ex[i]; }
    }
    if (w > 0.5) vals.push(Math.min(e / w, 8));
  }
  if (vals.length < 24) return null;
  vals.sort((a, b) => a - b);
  return vals[Math.floor((vals.length * 3) / 4)];
}

// Fine detail: log luminance minus its 3x3 mean, RMS over the masked pixels.
function detail(logs) {
  let s = 0, n = 0;
  for (let y = 1; y < H - 1; y++) for (let x = 1; x < W - 1; x++) {
    const i = y * W + x;
    if (edgeA[i] <= 0.3) continue;
    let m = 0;
    for (let j = -1; j <= 1; j++) for (let k = -1; k <= 1; k++) m += logs[(y + j) * W + x + k];
    const d = logs[i] - m / 9;
    s += d * d; n++;
  }
  return n ? Math.sqrt(s / n) : 0;
}

let offMax = 0;
for (let i = 0; i < N; i++) if (edgeA[i] <= 0) offMax = Math.max(offMax, Math.abs(outLog[i] - beforeLog[i]));

const f3 = (v) => (v == null ? 'n/a' : v.toFixed(3));
console.log(`capture ${path.basename(dir)}: ${W}x${H}, ${passthrough ? 'tone-mapped (passthrough)' : 'linear HDR'}, depth ${haveDepth ? 'yes' : 'no'}, engine logged composed ${f3(S.haloBefore)} / after ${f3(S.haloAfter)} / model ${f3(S.haloModel)}`);
console.log(`variant ${JSON.stringify(V)}  (${((Date.now() - t0) / 1000).toFixed(1)} s)`);
console.log(`  masked pixels ${(100 * maskCount / N).toFixed(1)}%, of them same-side statistics ${(100 * segCount / Math.max(maskCount, 1)).toFixed(0)}%`);
console.log(`  halo (stops, as the meter): model ${f3(meter(exM))}, composed before ${f3(meter(exB))}, engine after ${img.composed_after ? f3(meter(exE)) : 'n/a'}, variant after ${f3(meter(exA))}`);
console.log(`  fine detail in the mask (RMS stops): before ${detail(beforeLog).toFixed(4)}, variant after ${detail(outLog).toFixed(4)}`);
console.log(`  largest change off the mask: ${offMax.toExponential(2)} stops (should be 0)`);

// Glow profile: distance from the nearest depth edge (far side), mean (composed - input) and
// (model - proxy) per pixel of distance, so its width and whether the regional shift took it are visible.
if (haveDepth) {
  const dist = new Int16Array(N).fill(-1);
  const queue = [];
  for (let y = 1; y < H - 1; y++) for (let x = 1; x < W - 1; x++) {
    const i = y * W + x, c = Din[i];
    let nearer = false;
    for (const [a, b] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) if (Din[(y + b) * W + x + a] - c > V.depthTol) nearer = true;
    if (nearer) { dist[i] = 0; queue.push(i); }
  }
  for (let q = 0; q < queue.length; q++) {
    const i = queue[q], x = i % W, y = (i / W) | 0;
    if (dist[i] >= 24) continue;
    for (const [a, b] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
      const xx = x + a, yy = y + b;
      if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
      const j = yy * W + xx;
      if (dist[j] < 0 && Math.abs(Din[j] - Din[i]) < V.depthTol) { dist[j] = dist[i] + 1; queue.push(j); }
    }
  }
  const sumC = new Float64Array(25), sumA = new Float64Array(25), sumM = new Float64Array(25), cnt = new Float64Array(25);
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    const i = y * W + x, d = dist[i];
    if (d < 0 || d > 24) continue;
    const u = (x + 0.5) / W, v = (y + 0.5) / H;
    const ml = clog(displayLuma(sampleBilinear(img.model, u, v, 0), sampleBilinear(img.model, u, v, 1), sampleBilinear(img.model, u, v, 2)))
      - clog(displayLuma(sampleBilinear(img.proxy, u, v, 0), sampleBilinear(img.proxy, u, v, 1), sampleBilinear(img.proxy, u, v, 2)));
    sumC[d] += beforeLog[i] - Lin[i]; sumA[d] += outLog[i] - Lin[i]; sumM[d] += ml; cnt[d]++;
  }
  console.log('  far side of silhouettes, by distance (px): composed-input / variant-input / model-proxy, stops');
  const rows = [];
  for (let d = 0; d <= 24; d += 2) if (cnt[d]) rows.push(`${d}: ${(sumC[d] / cnt[d]).toFixed(3)} / ${(sumA[d] / cnt[d]).toFixed(3)} / ${(sumM[d] / cnt[d]).toFixed(3)}`);
  console.log('    ' + rows.join('  |  '));
}

// ---- images ---------------------------------------------------------------------------------------------
function bmp(file, w, h, px) { // px(x, y) -> [r,g,b] 0..1
  const rowBytes = (w * 3 + 3) & ~3, size = 54 + rowBytes * h, b = Buffer.alloc(size);
  b.write('BM', 0); b.writeUInt32LE(size, 2); b.writeUInt32LE(54, 10); b.writeUInt32LE(40, 14);
  b.writeInt32LE(w, 18); b.writeInt32LE(h, 22); b.writeUInt16LE(1, 26); b.writeUInt16LE(24, 28); b.writeUInt32LE(rowBytes * h, 34);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    const c = px(x, h - 1 - y), o = 54 + y * rowBytes + x * 3;
    b[o] = Math.round(sat(c[2]) * 255); b[o + 1] = Math.round(sat(c[1]) * 255); b[o + 2] = Math.round(sat(c[0]) * 255);
  }
  fs.writeFileSync(file, b);
}
// --dump-after <file>: the variant's after picture as raw float32 RGBA, the capture's size and encoding (the
// composed-before colour scaled by the luminance move), for an independent metric to read.
{
  const i = argv.indexOf('--dump-after');
  if (i >= 0) {
    const out = new Float32Array(N * 4), cb = img.composed_before.data;
    for (let k = 0; k < N; k++) {
      const y = lumaOf(img.composed_before, k) / normScale;
      const ratio = outLog[k] !== beforeLog[k] && y > 1e-6 ? cunlog(outLog[k]) / y : 1;
      out[k * 4] = cb[k * 4] * ratio; out[k * 4 + 1] = cb[k * 4 + 1] * ratio; out[k * 4 + 2] = cb[k * 4 + 2] * ratio; out[k * 4 + 3] = 1;
    }
    fs.writeFileSync(argv[i + 1], Buffer.from(out.buffer));
    console.log('  after picture written to ' + argv[i + 1]);
  }
}

if (flag('images')) {
  fs.mkdirSync(outDir, { recursive: true });
  const disp = (l) => { const y = cunlog(l); return Math.pow(sat(passthrough ? y : y / (1 + y)), passthrough ? 1 : 1 / 2.2); };
  bmp(path.join(outDir, 'mask.bmp'), W, H, (x, y) => { const i = y * W + x, g = disp(Lin[i]) * 0.35; return [g + 0.65 * edgeA[i], g + 0.65 * sat(Math.abs(outLog[i] - beforeLog[i]) * 4), g]; });
  bmp(path.join(outDir, 'glow-before.bmp'), W, H, (x, y) => { const v = sat(exB[y * W + x] * 4); return [v, v, v]; });
  bmp(path.join(outDir, 'glow-after.bmp'), W, H, (x, y) => { const v = sat(exA[y * W + x] * 4); return [v, v, v]; });
  bmp(path.join(outDir, 'before.bmp'), W, H, (x, y) => { const v = disp(beforeLog[y * W + x]); return [v, v, v]; });
  bmp(path.join(outDir, 'after.bmp'), W, H, (x, y) => { const v = disp(outLog[y * W + x]); return [v, v, v]; });
  console.log('  images in ' + outDir);
}
