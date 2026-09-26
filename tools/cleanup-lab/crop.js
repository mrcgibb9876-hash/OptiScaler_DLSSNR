// crop.js <capture> <out.png> x y w h zoom gain names...   (side by side, display values * gain)
'use strict';
const { loadCapture, writePng } = require('./cap.js');
const [dir, out, xs, ys, ws, hs, zs, gs, ...names] = process.argv.slice(2);
const x0 = +xs, y0 = +ys, w = +ws, h = +hs, z = +zs, gain = +gs;
const { S, img } = loadCapture(dir);
const W = S.width;
const gap = 4, n = names.length, OW = n * w * z + (n - 1) * gap, OH = h * z;
const rgb = new Uint8Array(OW * OH * 3).fill(255);
const q = (v) => Math.max(0, Math.min(255, Math.round(v * 255)));
names.forEach((name, k) => {
  let src = name, mode = 'rgb';
  if (name === 'mask') { src = 'mask'; mode = 'mask'; }
  if (name.startsWith('diff:')) { mode = 'diff'; }
  for (let y = 0; y < OH; y++) for (let x = 0; x < w * z; x++) {
    const sx = x0 + Math.floor(x / z), sy = y0 + Math.floor(y / z), i = (sy * W + sx) * 4;
    let c;
    if (mode === 'mask') { const m = img.mask.data; c = [m[i], Math.min(m[i + 3] * 4, 1), Math.min(m[i + 2] * 4, 1)]; }
    else if (mode === 'diff') { const [a, b] = name.slice(5).split('-'); c = [0, 1, 2].map((ch) => 0.5 + 4 * (img[a].data[i + ch] - img[b].data[i + ch])); }
    else { const d = img[src].data; c = [d[i] * gain, d[i + 1] * gain, d[i + 2] * gain]; }
    const o = (y * OW + k * (w * z + gap) + x) * 3;
    rgb[o] = q(c[0]); rgb[o + 1] = q(c[1]); rgb[o + 2] = q(c[2]);
  }
});
writePng(out, OW, OH, rgb);
