// exe.js - the layer that makes this page say the desktop app's sentences.
//
// The exe has no per-line level prefix: `logMsg()` in src/core.cpp:24 writes plain text to stdout and
// to raytracer.log, and the conventions are inside the messages themselves (`step: `, `quality
// decision: `, `  timing: `, `fatal: ` for fail()). So this is not a logger with invented severities
// bolted onto the exe's wording - it is the exe's wording with no changes, on console.log, with
// console.warn reserved for the web-only warnings and console.error for the exe's `fatal:` line.
//
// The format strings are copied out of src/*.cpp character for character (they are quoted in the
// comments next to each helper) so a diff of `raytracer.log` against the browser console shows only
// the lines that cannot be the same, and those are listed in README.md section 5.

/**
 * The subset of C printf the exe's format strings use: %s %d %u %llu %zu %.Nf and a left-justify
 * width (%-40s). Anything else is passed through unchanged, which is how a stray `%%` stays visible
 * instead of silently swallowing an argument.
 */
export function pf(spec, ...args) {
  let i = 0;
  return String(spec).replace(/%(-)?(\d*)?(?:\.(\d+))?(?:ll|l|z)?([sduxf])/g,
    (_m, left, width, prec, conv) => {
      const a = args[i++];
      let s;
      switch (conv) {
        case 'd': case 'u': case 'x':
          s = (conv === 'x' ? Number(a).toString(16) : String(Math.trunc(Number(a) || 0)));
          break;
        case 'f': {
          const n = Number(a);
          s = Number.isFinite(n) ? n.toFixed(prec === undefined ? 6 : Number(prec)) : String(a);
          break;
        }
        default: s = a === undefined ? '(none)' : String(a);
      }
      if (width && s.length < Number(width)) {
        s = left ? s.padEnd(Number(width), ' ') : s.padStart(Number(width), ' ');
      }
      return s;
    });
}

/** Counts of what went where, so a run can be failed by its own error lines. */
export const logCounts = { info: 0, warn: 0, error: 0 };

export const L = {
  /** logMsg(): stdout and raytracer.log. */
  log(spec, ...args) { ++logCounts.info; console.log(pf(spec, ...args)); },
  /** Web-only advisories: nothing in src/*.cpp prints these, so they are marked as ours. */
  warn(spec, ...args) { ++logCounts.warn; console.warn(pf(spec, ...args)); },
  /** fail(): "fatal: %s" on stderr and in the log file. */
  fatal(spec, ...args) {
    ++logCounts.error;
    console.error(`fatal: ${pf(spec, ...args)}`);
  },
};

/** The exe's adapter line (core.cpp:489) has one %-40s name; the web equivalent names one adapter. */
export function adapterName(info) {
  const parts = [info.vendor, info.architecture, info.device, info.description]
    .filter((s) => typeof s === 'string' && s.length);
  return parts.length ? parts.join(' ') : 'unknown adapter';
}

/**
 * Title bar, src/frame.cpp:1447-1464, in `document.title`. Two variants because the exe has two:
 * the adaptive one carries no ` - free look` clause and the pinned one does. The prefix stays
 * `Vulkan Ray Tracer` verbatim (README section 4) and only the pipeline-name slot changes, to
 * `WebGPU pipeline`.
 */
export function exeTitle(s) {
  const pipe = 'WebGPU pipeline';
  const den = s.denoise ? ' - (N) Denoise ON' : ' - (N) Denoise OFF';
  // The exe has no pause, so this clause is the port's own (README section 2 and 4): the picture is
  // still being rendered while the scene clock is stopped, and the title has to say which of the two
  // the viewer is looking at.
  const held = s.paused ? ' - PAUSED' : '';
  if (s.autoQuality) {
    return pf('Vulkan Ray Tracer - %.0f fps - %ux%u trace %ux%u - %d spp - %s%s%s - (ESC) Quit',
      s.fps, s.winW, s.winH, s.traceW, s.traceH, s.spp, pipe, den, held);
  }
  const free = s.freeLook ? ' - free look' : '';
  return pf('Vulkan Ray Tracer - %.0f fps - %ux%u trace %ux%u - %d spp - %s%s%s%s - (ESC) Quit',
    s.fps, s.winW, s.winH, s.traceW, s.traceH, s.spp, pipe, free, den, held);
}

/** What the title says once ESC has been pressed (see README section 2 for why not a closed tab). */
export const QUIT_TITLE = 'Vulkan Ray Tracer - quit (ESC)';

/** SHA-256 of a byte range as hex. localhost is a secure context, so crypto.subtle is available. */
export async function sha256Hex(buf) {
  const h = await crypto.subtle.digest('SHA-256', buf.buffer ?? buf);
  return [...new Uint8Array(h)].map((b) => b.toString(16).padStart(2, '0')).join('');
}

/**
 * The upload sink is the developer's own demo_server.py (web/demo_server.py), which answers /results
 * and /upload at the root of the server it is started on. A static host - GitHub Pages among them -
 * has no such endpoint, and POSTing to one there only puts a 404 in the console next to the
 * measurements. So the sink is addressed at that server's root when the page is being served by one
 * (loopback), and is simply absent otherwise. It is root-absolute on purpose: that is where the dev
 * server keeps it, whatever subdirectory the page itself is served from.
 */
const UPLOAD_SINK = /^(localhost|127\.0\.0\.1|\[::1\]|.*\.localhost)$/i.test(location.hostname)
  ? new URL('/', location.origin).href : null;

/** True when results and PNGs have somewhere to go. Logged once at start-up so it is never a guess. */
export const canUpload = UPLOAD_SINK !== null;

/** POST metrics to demo_server.py's /results, the same sink the previous attempt used. */
export async function postResults(text) {
  if (!UPLOAD_SINK) return 'not posted (static host: no /results sink here)';
  try {
    await fetch(UPLOAD_SINK + 'results', { method: 'POST', body: text, cache: 'no-store' });
    return 'posted';
  } catch (e) {
    return `not posted (${e.message})`;
  }
}

/** POST a PNG to /upload. The server writes it under its root; a page can write nothing itself. */
export async function uploadPng(name, pngBytes, dir = 'browser') {
  if (!UPLOAD_SINK) return `not uploaded (static host: no /upload here) -> ${dir}/${name}`;
  try {
    const res = await fetch(`${UPLOAD_SINK}upload?name=${encodeURIComponent(name)}&dir=${encodeURIComponent(dir)}`,
      { method: 'POST', body: pngBytes, cache: 'no-store' });
    const t = (await res.text()).trim();
    return res.ok ? t : `rejected: ${t}`;
  } catch (e) {
    return `not uploaded (${e.message})`;
  }
}
