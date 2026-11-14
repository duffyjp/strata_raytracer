// imageio.js - PNG in and out, plus the pixel statistics the correctness comparison needs.
//
// Why an encoder instead of canvas.toDataURL: the export has to be the *same bytes* the GPU wrote, and
// routing them through a 2D canvas invites colour management and premultiplication to change values on
// the way. So the presentation texture's RGBA bytes are written straight into a PNG here (stdlib-style:
// raw chunks, filter 0, IDAT compressed with the browser's own CompressionStream('deflate'), which is
// zlib - the container PNG wants). For reading the native reference stills, createImageBitmap is used
// with colour conversion and premultiplication both turned off, so what comes back is the file's bytes.

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; ++n) {
    let c = n;
    for (let k = 0; k < 8; ++k) c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
    t[n] = c >>> 0;
  }
  return t;
})();

function crc32(bytes) {
  let c = 0xffffffff;
  for (let i = 0; i < bytes.length; ++i) c = CRC_TABLE[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

function chunk(type, data) {
  const out = new Uint8Array(12 + data.length);
  const v = new DataView(out.buffer);
  v.setUint32(0, data.length);
  for (let i = 0; i < 4; ++i) out[4 + i] = type.charCodeAt(i);
  out.set(data, 8);
  v.setUint32(8 + data.length, crc32(out.subarray(4, 8 + data.length)));
  return out;
}

/**
 * Encode RGBA8 bytes (row 0 first, top of image) as a PNG.
 * Synchronous-looking API but async because CompressionStream is stream-based.
 */
export async function encodePng(rgba, w, h) {
  const raw = new Uint8Array(h * (1 + w * 4));
  for (let y = 0; y < h; ++y) {
    raw[y * (1 + w * 4)] = 0;                       // filter type 0 (None): a PNG encoder is allowed to
    raw.set(rgba.subarray(y * w * 4, (y + 1) * w * 4), y * (1 + w * 4) + 1);
  }
  let compressed;
  if (typeof CompressionStream === 'function') {
    const stream = new Blob([raw]).stream().pipeThrough(new CompressionStream('deflate'));
    compressed = new Uint8Array(await new Response(stream).arrayBuffer());
  } else {
    compressed = raw;                               // stored: valid only if the decoder accepts it
  }
  const ihdr = new Uint8Array(13);
  const v = new DataView(ihdr.buffer);
  v.setUint32(0, w); v.setUint32(4, h);
  ihdr[8] = 8; ihdr[9] = 6;                         // 8-bit, truecolour with alpha
  const parts = [
    new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', ihdr), chunk('IDAT', compressed), chunk('IEND', new Uint8Array(0)),
  ];
  const total = parts.reduce((a, p) => a + p.length, 0);
  const out = new Uint8Array(total);
  let o = 0;
  for (const p of parts) { out.set(p, o); o += p.length; }
  return out;
}

/** Decode PNG bytes to {w, h, rgba} with no colour conversion and no premultiplication. */
export async function decodePng(bytes) {
  const bmp = await createImageBitmap(new Blob([bytes], { type: 'image/png' }), {
    colorSpaceConversion: 'none', premultiplyAlpha: 'none', resizeMode: 'none',
  });
  const c = new OffscreenCanvas(bmp.width, bmp.height);
  const g = c.getContext('2d', { willReadFrequently: true });
  g.clearRect(0, 0, bmp.width, bmp.height);
  g.drawImage(bmp, 0, 0);
  const d = g.getImageData(0, 0, bmp.width, bmp.height);
  const w = bmp.width, h = bmp.height;
  bmp.close();
  return { w, h, rgba: new Uint8Array(d.data.buffer) };
}

export async function fetchDecodedPng(url) {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`fetch ${url} -> HTTP ${res.status}`);
  return await decodePng(new Uint8Array(await res.arrayBuffer()));
}

// --------------------------------------------------------------------------- statistics
const LUM = [0.2126, 0.7152, 0.0722];
export const luma = (r, g, b) => r * LUM[0] + g * LUM[1] + b * LUM[2];

/**
 * Compare two same-size RGBA8 images.
 *  - mean luminance / mean per-channel value over an optional band (fractions of the frame)
 *  - fraction of pixels whose worst channel difference is within `tol`
 *  - the worst difference seen, and where
 * Returned numbers are all integers-as-floats of the 0..255 code, which is what the criteria are in.
 */
export function compareImages(a, b, band, tol) {
  if (a.w !== b.w || a.h !== b.h) {
    throw new Error(`size mismatch ${a.w}x${a.h} vs ${b.w}x${b.h}`);
  }
  const { w, h } = a;
  const x0 = Math.floor((band ? band.x0 : 0) * w), x1 = Math.ceil((band ? band.x1 : 1) * w);
  const y0 = Math.floor((band ? band.y0 : 0) * h), y1 = Math.ceil((band ? band.y1 : 1) * h);
  let n = 0, within = 0, withinBand = 0, bandN = 0, lumA = 0, lumB = 0, sumDiff = 0, worst = 0;
  let worstX = -1, worstY = -1;
  let sumDiffBand = 0;
  for (let y = 0; y < h; ++y) {
    const inBandRow = y >= y0 && y < y1;
    for (let x = 0; x < w; ++x) {
      const i = (y * w + x) * 4;
      const ra = a.rgba[i], ga = a.rgba[i + 1], ba = a.rgba[i + 2];
      const rb = b.rgba[i], gb = b.rgba[i + 1], bb = b.rgba[i + 2];
      const d = Math.max(Math.abs(ra - rb), Math.abs(ga - gb), Math.abs(ba - bb));
      ++n;
      sumDiff += d;
      if (d <= tol) ++within;
      if (d > worst) { worst = d; worstX = x; worstY = y; }
      if (inBandRow && x >= x0 && x < x1) {
        ++bandN;
        lumA += luma(ra, ga, ba);
        lumB += luma(rb, gb, bb);
        sumDiffBand += d;
        if (d <= tol) ++withinBand;
      }
    }
  }
  const mean = (v, k) => (k ? v / k : 0);
  return {
    pixels: n,
    tol,
    withinTol: within / n,
    meanAbsDiff: mean(sumDiff, n),
    worst, worstX, worstY,
    band: band || { x0: 0, y0: 0, x1: 1, y1: 1 },
    bandPixels: bandN,
    bandMeanLumaA: mean(lumA, bandN),
    bandMeanLumaB: mean(lumB, bandN),
    bandRelLumaDiff: mean(lumB, bandN) > 0
      ? Math.abs(mean(lumA, bandN) - mean(lumB, bandN)) / mean(lumB, bandN) : 0,
    bandWithinTol: mean(withinBand, bandN),
    bandMeanAbsDiff: mean(sumDiffBand, bandN),
  };
}

/** |a - b| amplified, with a floor so near-matches stay visible in the saved image. */
export function diffImage(a, b, gain = 4) {
  const n = a.rgba.length;
  const out = new Uint8Array(n);
  for (let i = 0; i < n; i += 4) {
    for (let c = 0; c < 3; ++c) {
      out[i + c] = Math.min(255, Math.abs(a.rgba[i + c] - b.rgba[i + c]) * gain);
    }
    out[i + 3] = 255;
  }
  return { w: a.w, h: a.h, rgba: out };
}

/**
 * A per-pixel classification of the disagreement, so "not within tolerance" can be read as a picture.
 * black = within tol, red = only the browser has structure there (or is much brighter), blue = the
 * opposite. This is the image to look at before believing or dismissing any single number.
 */
export function verdictImage(a, b, tol) {
  const n = a.rgba.length;
  const out = new Uint8Array(n);
  for (let i = 0; i < n; i += 4) {
    const la = luma(a.rgba[i], a.rgba[i + 1], a.rgba[i + 2]);
    const lb = luma(b.rgba[i], b.rgba[i + 1], b.rgba[i + 2]);
    const d = Math.max(Math.abs(a.rgba[i] - b.rgba[i]), Math.abs(a.rgba[i + 1] - b.rgba[i + 1]),
      Math.abs(a.rgba[i + 2] - b.rgba[i + 2]));
    out[i] = d > tol ? 220 : 0;
    out[i + 1] = 0;
    out[i + 2] = d > tol ? 0 : 0;
    if (d > tol) {
      if (la > lb) { out[i] = 220; out[i + 1] = 40; out[i + 2] = 40; }      // browser brighter
      else { out[i] = 40; out[i + 1] = 80; out[i + 2] = 220; }              // native brighter
    }
    out[i + 3] = 255;
  }
  return { w: a.w, h: a.h, rgba: out };
}
