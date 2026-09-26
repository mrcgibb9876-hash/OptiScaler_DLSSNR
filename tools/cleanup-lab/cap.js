// Shared capture loader + helpers for the offline analysis.
'use strict';
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

function half(h) {
  const s = (h & 0x8000) ? -1 : 1, e = (h >> 10) & 0x1f, m = h & 0x3ff;
  if (e === 0) return s * m * Math.pow(2, -24);
  if (e === 31) return m ? NaN : s * Infinity;
  return s * (1 + m / 1024) * Math.pow(2, e - 15);
}

function loadCapture(dir) {
  const manifest = JSON.parse(fs.readFileSync(path.join(dir, 'manifest.json'), 'utf8'));
  const img = {};
  for (const f of manifest.files) {
    const buf = fs.readFileSync(path.join(dir, f.file));
    const w = f.width, h = f.height, pitch = f.rowPitch, base = f.offset || 0;
    const out = new Float32Array(w * h * 4);
    for (let y = 0; y < h; y++) {
      const row = base + y * pitch;
      for (let x = 0; x < w; x++) {
        const o = (y * w + x) * 4;
        if (f.format === 'R8G8B8A8_UNORM') {
          const p = row + x * 4;
          out[o] = buf[p] / 255; out[o + 1] = buf[p + 1] / 255; out[o + 2] = buf[p + 2] / 255; out[o + 3] = buf[p + 3] / 255;
        } else if (f.format === 'R16G16B16A16_FLOAT') {
          const p = row + x * 8;
          out[o] = half(buf.readUInt16LE(p)); out[o + 1] = half(buf.readUInt16LE(p + 2));
          out[o + 2] = half(buf.readUInt16LE(p + 4)); out[o + 3] = half(buf.readUInt16LE(p + 6));
        } else if (f.format === 'R16_FLOAT') {
          out[o] = half(buf.readUInt16LE(row + x * 2));
        } else if (f.format === 'R32_FLOAT') {
          out[o] = buf.readFloatLE(row + x * 4);
        } else if (f.format === 'R32_FLOAT_X8X24') {
          out[o] = buf.readFloatLE(row + x * 8);
        } else if (f.format === 'R24_UNORM_X8') {
          out[o] = (buf.readUInt32LE(row + x * 4) & 0xffffff) / 16777215;
        } else if (f.format === 'R16G16_FLOAT') {
          out[o] = half(buf.readUInt16LE(row + x * 4)); out[o + 1] = half(buf.readUInt16LE(row + x * 4 + 2));
        } else throw new Error(f.format);
      }
    }
    img[f.name] = { w, h, data: out };
  }
  return { manifest, S: manifest.settings, img };
}

// display-encoded luma (values are already display-encoded on the passthrough route)
function luma(im) {
  const n = im.w * im.h, L = new Float32Array(n);
  for (let i = 0; i < n; i++) L[i] = 0.2126 * im.data[i * 4] + 0.7152 * im.data[i * 4 + 1] + 0.0722 * im.data[i * 4 + 2];
  return L;
}
// log2 light of a display-encoded luma, as the shader does (2.2 * log2(y + floor))
const logl = (y) => 2.2 * Math.log2(Math.max(y, 0) + 0.0587);

function crcTable() { const t = new Uint32Array(256); for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; } return t; }
const CRC = crcTable();
function crc32(buf) { let c = 0xffffffff; for (let i = 0; i < buf.length; i++) c = CRC[(c ^ buf[i]) & 0xff] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; }
function chunk(type, data) {
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
  const td = Buffer.concat([Buffer.from(type, 'ascii'), data]);
  const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td));
  return Buffer.concat([len, td, crc]);
}
// rgb: Uint8Array w*h*3
function writePng(file, w, h, rgb) {
  const raw = Buffer.alloc((w * 3 + 1) * h);
  for (let y = 0; y < h; y++) { raw[y * (w * 3 + 1)] = 0; Buffer.from(rgb.buffer, rgb.byteOffset + y * w * 3, w * 3).copy(raw, y * (w * 3 + 1) + 1); }
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  fs.writeFileSync(file, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw, { level: 6 })), chunk('IEND', Buffer.alloc(0))]));
}

// Log reciprocal depth per pixel, as the pass computes it: the captured depth guide when present, else the
// mask's green channel (captures made before depth was written).
function logDepth(cap) {
  const S = cap.S, n = S.width * S.height, D = new Float32Array(n);
  if (cap.img.depth) { const d = cap.img.depth.data; for (let i = 0; i < n; i++) { const v = d[i * 4]; D[i] = Math.log2(Math.max(S.cleanupDepthInverted !== 0 ? v : 1 - v, 1e-7)); } }
  else if (cap.img.mask) { for (let i = 0; i < n; i++) D[i] = cap.img.mask.data[i * 4 + 1]; }
  return D;
}

module.exports = { logDepth, loadCapture, luma, logl, writePng };
