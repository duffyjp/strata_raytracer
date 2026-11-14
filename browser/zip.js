// zip.js - minimal ZIP reader for scene.pak, and a pak-level scene loader.
//
// Why hand-rolled: the brief forbids npm packages and third-party binaries, and a .pak is a plain
// ZIP. src/pak.cpp reads the same container and *does* carry its own DEFLATE decoder, so a pak may
// legitimately be compressed even though the one this repo builds is stored. Methods supported here:
//   0 (stored)  - decoded directly from the file data
//   8 (deflate) - decoded with the browser's built-in DecompressionStream('deflate-raw')
// Anything else is reported as unsupported rather than silently producing garbage.
//
// The central directory is parsed (not the local headers) because that is where the authoritative
// compressed size and offsets live; scanning local headers would need inflate to find each next
// header when entries are compressed.

const EOCD_SIG = 0x06054b50;
const CD_SIG = 0x02014b50;

/** Accept an ArrayBuffer or any typed view of one, and return them both. */
function asBytes(src) {
  if (src instanceof ArrayBuffer) return { buf: src, bytes: new Uint8Array(src) };
  if (ArrayBuffer.isView(src)) {
    return { buf: src.buffer, bytes: new Uint8Array(src.buffer, src.byteOffset, src.byteLength) };
  }
  throw new Error('zip: expected an ArrayBuffer or a typed array');
}

/** Locate the End Of Central Directory record (it may carry a trailing comment). */
function findEocd(view) {
  const n = view.byteLength;
  const scanFrom = Math.max(0, n - 22 - 0xffff);
  for (let i = n - 22; i >= scanFrom; i--) {
    if (view.getUint32(i, true) === EOCD_SIG) return i;
  }
  return -1;
}

/**
 * Parse a ZIP archive into a list of central-directory entries.
 * Throws with a clear message on anything that is not a ZIP.
 */
export function readZip(src) {
  const { bytes } = asBytes(src);
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const eocd = findEocd(view);
  if (eocd < 0) throw new Error('zip: no end-of-central-directory record (not a ZIP?)');
  const count = view.getUint16(eocd + 10, true);
  let off = view.getUint32(eocd + 16, true);
  const list = [];
  for (let i = 0; i < count; i++) {
    if (off + 46 > view.byteLength || view.getUint32(off, true) !== CD_SIG) {
      throw new Error(`zip: central directory entry ${i} has a bad signature at ${off}`);
    }
    const method = view.getUint16(off + 10, true);
    const compSize = view.getUint32(off + 20, true);
    const uncompSize = view.getUint32(off + 24, true);
    const nameLen = view.getUint16(off + 28, true);
    const extraLen = view.getUint16(off + 30, true);
    const commentLen = view.getUint16(off + 32, true);
    const localOff = view.getUint32(off + 42, true);
    const name = new TextDecoder().decode(bytes.subarray(off + 46, off + 46 + nameLen));
    off += 46 + nameLen + extraLen + commentLen;
    list.push({ name, method, compSize, uncompSize, localOff });
  }
  return list;
}

/**
 * Skip a local header and return a view of that entry's stored bytes.
 * The length comes from the central directory, not from the local header: a streaming writer is allowed
 * to leave the local sizes zero and fill them in only at the end, and reading them from offset 18 of the
 * local header instead of 20 of the central one is the kind of off-by-two that silently hands back the
 * next entry's bytes as part of this one.
 */
function localData(bytes, view, localOff, compSize) {
  if (view.getUint32(localOff, true) !== 0x04034b50) {
    throw new Error(`zip: bad local header at ${localOff}`);
  }
  const nameLen = view.getUint16(localOff + 26, true);
  const extraLen = view.getUint16(localOff + 28, true);
  const start = localOff + 30 + nameLen + extraLen;
  return bytes.subarray(start, start + compSize);
}

async function inflateRaw(bytes, expected) {
  if (typeof DecompressionStream !== 'function') {
    throw new Error('zip: entry is deflated and this browser has no DecompressionStream');
  }
  const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
  const out = new Uint8Array(await new Response(stream).arrayBuffer());
  if (expected && out.byteLength !== expected) {
    throw new Error(`zip: inflated to ${out.byteLength} bytes, central directory says ${expected}`);
  }
  return out;
}

/**
 * Full read: Map<name, Uint8Array>, inflated as needed.
 * Also returns {compressed:n} so a caller can say out loud which case it handled.
 */
export async function unzip(src) {
  const { bytes } = asBytes(src);
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const list = readZip(bytes);
  const map = new Map();
  let compressed = 0;
  for (const e of list) {
    const stored = localData(bytes, view, e.localOff, e.compSize);
    if (e.method === 0) {
      map.set(e.name, stored);
    } else if (e.method === 8) {
      map.set(e.name, await inflateRaw(stored, e.uncompSize));
      compressed++;
    } else {
      throw new Error(`zip: unsupported compression method ${e.method} for ${e.name}`);
    }
  }
  return { files: map, count: list.length, compressed };
}

/**
 * Load a scene.pak over HTTP and hand back its decoded entries.
 * Returns null on any failure so the caller can fall back to loose files under /assets/.
 */
export async function loadPak(url) {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`fetch ${url} -> HTTP ${res.status}`);
  const buf = await res.arrayBuffer();
  const { files, count, compressed } = await unzip(buf);
  return { files, count, compressed, bytes: buf.byteLength, url };
}
