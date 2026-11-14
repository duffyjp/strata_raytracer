// tools/png_read.mjs - read a PNG's pixels outside the browser, with nothing but Node's zlib.
//
// Why it exists: the overlay check compares two screenshots of the page, and the comparison cannot be
// done by the page that drew them - the thing being tested is whether a DOM layer is on the screen, and
// a page cannot read back its own compositor. So the driver hands the bytes to this decoder, which is
// the same arrangement README 9.4 uses for the stills: `web/png_check.py` decodes independently of
// Edge, and no part of the verdict comes from the renderer being judged.
//
// Supports what `Page.captureScreenshot` produces: 8-bit, non-interlaced, colour type 6 (RGBA) or 2
// (RGB). Anything else is an error, not a guess.

import { inflateSync } from 'node:zlib';
import { readFileSync } from 'node:fs';

const SIG = [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a];

export function decodePngBytes(buf) {
  for (let i = 0; i < 8; ++i) if (buf[i] !== SIG[i]) throw new Error('not a PNG');
  let p = 8, ihdr = null;
  const idat = [];
  while (p + 8 <= buf.length) {
    const len = buf.readUInt32BE(p);
    const type = buf.toString('ascii', p + 4, p + 8);
    const data = buf.subarray(p + 8, p + 8 + len);
    if (type === 'IHDR') {
      ihdr = { w: data.readUInt32BE(0), h: data.readUInt32BE(4), depth: data[8], color: data[9],
        compress: data[10], filter: data[11], interlace: data[12] };
    } else if (type === 'IDAT') idat.push(data);
    else if (type === 'IEND') break;
    p += 12 + len;
  }
  if (!ihdr) throw new Error('no IHDR');
  if (ihdr.depth !== 8 || ihdr.interlace !== 0 || ihdr.compress !== 0) {
    throw new Error(`unsupported PNG: depth=${ihdr.depth} interlace=${ihdr.interlace} ` +
      `compress=${ihdr.compress}`);
  }
  const channels = ihdr.color === 6 ? 4 : ihdr.color === 2 ? 3 : null;
  if (!channels) throw new Error(`unsupported PNG colour type ${ihdr.color}`);
  const raw = inflateSync(Buffer.concat(idat));
  const stride = ihdr.w * channels;
  const out = Buffer.alloc(ihdr.w * ihdr.h * 4);      // always RGBA out, whatever came in
  // The filters work in the source layout, so the row is rebuilt in that layout and then widened to
  // RGBA - otherwise a type-2 (RGB) image would have its Sub/Paeth predictors reading the wrong byte.
  let q = 0;
  let prev = new Uint8Array(stride);
  for (let y = 0; y < ihdr.h; ++y) {
    const ft = raw[q++];
    const line = raw.subarray(q, q + stride);
    q += stride;
    const cur = new Uint8Array(stride);
    const base = y * ihdr.w * 4;
    for (let x = 0; x < stride; ++x) {
      const a = x >= channels ? cur[x - channels] : 0;
      const b = prev[x];
      const c = x >= channels ? prev[x - channels] : 0;
      let v = line[x];
      switch (ft) {
        case 0: break;
        case 1: v = (v + a) & 255; break;
        case 2: v = (v + b) & 255; break;
        case 3: v = (v + ((a + b) >> 1)) & 255; break;
        case 4: {
          const pa = Math.abs(b - c), pb = Math.abs(a - c), pc = Math.abs(a + b - 2 * c);
          v = (v + (pa <= pb && pa <= pc ? a : pb <= pc ? b : c)) & 255;
          break;
        }
        default: throw new Error(`bad filter type ${ft} on row ${y}`);
      }
      cur[x] = v;
      const k = Math.floor(x / channels), ch = x % channels;
      out[base + k * 4 + ch] = v;
      if (channels === 3 && ch === 2) out[base + k * 4 + 3] = 255;
    }
    prev = cur;
  }
  return { w: ihdr.w, h: ihdr.h, rgba: out };
}

export const decodePng = (path) => decodePngBytes(readFileSync(path));

/** Where the two images differ, as a bounding box plus a count - the shape of "is it drawn there". */
export function diffRegion(a, b) {
  if (a.w !== b.w || a.h !== b.h) throw new Error(`size mismatch ${a.w}x${a.h} vs ${b.w}x${b.h}`);
  let n = 0, minx = a.w, miny = a.h, maxx = -1, maxy = -1, sumDelta = 0;
  for (let y = 0; y < a.h; ++y) {
    for (let x = 0; x < a.w; ++x) {
      const i = (y * a.w + x) * 4;
      const d = Math.max(Math.abs(a.rgba[i] - b.rgba[i]), Math.abs(a.rgba[i + 1] - b.rgba[i + 1]),
        Math.abs(a.rgba[i + 2] - b.rgba[i + 2]));
      if (d <= 2) continue;
      ++n; sumDelta += d;
      if (x < minx) minx = x;
      if (y < miny) miny = y;
      if (x > maxx) maxx = x;
      if (y > maxy) maxy = y;
    }
  }
  return { pixels: n, bbox: n ? { x0: minx, y0: miny, x1: maxx, y1: maxy } : null,
    meanDelta: n ? +(sumDelta / n).toFixed(2) : 0 };
}

export const meanLuma = (img) => {
  let s = 0;
  for (let i = 0; i < img.rgba.length; i += 4) {
    s += 0.2126 * img.rgba[i] + 0.7152 * img.rgba[i + 1] + 0.0722 * img.rgba[i + 2];
  }
  return +(s / (img.rgba.length / 4)).toFixed(2);
};
