// app.js - the web build: one canvas, the exe's title bar, the exe's log, the exe's controls.
//
// Everything that had already been proven in web/ is imported, not rewritten: the uncompressed-ZIP
// pak loader (zip.js), the procedural geometry and scene tables (scene.js, geometry.js), the
// CPU-built two-level acceleration structure (bvh.js), the path tracer with Lambert/GGX and
// next-event estimation, the G-buffer prepass, the SVGF temporal pass with five a-trous iterations,
// ACES, the vignette and the canvas blit (renderer.js + shaders/*.wgsl), and the adaptive controller
// (quality.js). What is new here is the shell the brief asks for: no report page, no results panel,
// no buttons - the render window, document.title, console.log, URL parameters for the exe's flags,
// and the keyboard/pointer controls of the desktop app.
//
// The frame loop follows src/frame.cpp's runWindowed() step for step, in its order:
// read input -> advance the clock and the camera blend -> decide reset/accumulation -> upload the
// frame's instances -> submit -> present -> feed the controller -> once a second, the title bar.

import { unzip } from './zip.js';
import { defaultScene, applyJson, buildMeshes, cameraBasis, computeInstances } from './scene.js';
import { BVH } from './bvh.js';
import { buildWorldTriangles, packTrianglesInto, packInstances, packMaterials, packLights,
  makeLightRegen, updateInstanceRows } from './geometry.js';
import { Renderer } from './renderer.js';
import { Quality, CONTROLLER_DEFAULTS } from './quality.js';
import { encodePng, decodePng, compareImages } from './imageio.js';
import { L, pf, adapterName, exeTitle, QUIT_TITLE, sha256Hex, postResults, uploadPng,
  logCounts } from './exe.js';

// --------------------------------------------------------------------------- where things live
//
// GitHub Pages serves this site at https://<user>.github.io/<repo>/ - under a path prefix - while the
// developer's server (web/demo_server.py, and `python -m http.server` at the repo root) serves the
// same files at /browser/. A root-absolute URL like /build/bin/scene.pak can only name one of
// those two, so it names neither: under a prefix it 404s, and a page whose modules and assets 404
// does not say so, it just never finishes starting up.
//
// So every URL the page asks for is written relative to *this module* and resolved against it
// (`import.meta.url` survives both a directory URL like /strata_raytracer/ and a file URL with a
// query string on it, which document.baseURI does not always do). Where two layouts are possible -
// a deployed site that carries its own assets/, and a developer's server whose repo has them one
// level up - the page keeps an ordered candidate list and takes the first that answers. Nothing is
// hardcoded to a prefix, so the same build serves from any.
// The paths below are relative to browser/, because that is where this module lives, and they say
// so by their shape: `../../build/bin/` is the exe's pak one directory above the site, `../assets/`
// is the repository's loose assets, `../web/refs/` is the first attempt's references. Under GitHub
// Pages, where browser/ *is* the published tree, these same strings walk out of the site and 404 -
// which is exactly why each one is a candidate to fall back on, not the primary answer.
const siteUrl = (p) => new URL(p, import.meta.url).href;

/** A deployed site keeps scene.pak in assets/, beside index.html. */
const PAK_CANDIDATES = ['assets/scene.pak', '../../build/bin/scene.pak'];
/** ... and, as a fallback for a site built without a pak, the loose files the desktop app packs. */
const ASSET_DIRS = ['assets', '../assets'];
/**
 * The reference image for ?cmp=1 is opt-in, and it is the one input that a deployed site does not
 * necessarily carry (the native PNGs live in the repository's web/refs/ and browser/refs/, and are
 * not part of the published site). Both are tried, and if neither answers the comparison says so.
 */
const REF_CANDIDATES = ['../web/refs/native_still48_t0.55.png', 'refs/native_still48_t0.55.png'];

/**
 * Fetch the first URL in `list` that answers with 2xx. Returns {res, url} - the resolved url is kept
 * because saying which candidate won is the difference between a prefix that works and a prefix that
 * silently fell back. Returns null when none of them do, which is never fatal on its own.
 */
async function firstOk(list) {
  const misses = [];
  for (const u of list) {
    try {
      const res = await fetch(u, { cache: 'no-store' });
      if (res.ok) return { res, url: u };
      misses.push(`${u} -> HTTP ${res.status}`);
    } catch (e) {
      misses.push(`${u} -> ${e.message}`);
    }
  }
  return { res: null, url: null, misses };
}

// --------------------------------------------------------------------------- command line -> URL
const qs = new URLSearchParams(location.search);
const num = (k, d) => (qs.has(k) ? Number(qs.get(k)) : d);
const flag = (k, d) => (qs.has(k) ? qs.get(k) !== '0' : d);
const clamp = (v, lo, hi) => Math.min(Math.max(v, lo), hi);
const sizeOf = (s, d) => {
  const m = /^(\d+)\s*[xX]\s*(\d+)$/.exec(String(s || ''));
  return m ? [Number(m[1]), Number(m[2])] : d;
};

const HELP = [
  'WebGPU ray tracer - the browser build of the desktop app. The interface is the render window.',
  '',
  '  ?window=WxH        canvas size in CSS pixels (default: fill the viewport; a page cannot resize',
  '                     its browser window, so this sizes the canvas and says so in the log)',
  '  ?capture=N         render N stills at the capture camera, upload them, render no animation',
  '  ?accum=S           frames accumulated per captured still                       [48]',
  '  ?size=WxH          capture resolution                                           [1280x720]',
  '  ?t=T               camera time for the stills (the exe uses 0.55 + img * 2.35)  [0.55]',
  '  ?pak=URL           asset archive to load (default: assets/scene.pak beside the page, then',
  '                     ../../build/bin/scene.pak, then the loose assets/ scene.json + textures)',
  '  ?out=DIR           where the server writes uploaded PNGs                        [browser/evidence]',
  '  ?scale=F           trace resolution ceiling (0.4 .. 1.0)                        [1.0]',
  '  ?autoq=0           pin samples and scale instead of adapting to the GPU',
  '  ?quality=MODE      high | balanced | performance | off                          [high]',
  '  ?spp=N             samples per pixel per frame (start value when ?autoq=1)      [1]',
  '  ?maxspp=N          most samples per pixel per frame it may spend                [16]',
  '  ?minscale=F        smallest trace resolution it may fall back to                [0.40]',
  '  ?maxscale=F        largest trace resolution scale (1.0: the blit magnifies, it does not',
  '                     box-filter supersampled pixels)                              [1.0]',
  '  ?denoise=0         turn the SVGF denoiser off at start-up (press N to toggle)',
  '  ?gbuf=CH           albedo | normal | depth (denoised | diff | variance are unported)',
  '  ?filterz=F ?filtern=F ?filterl=F   denoiser edge stopping                       [0.02 128 24]',
  '  ?refresh=N         treat the panel as running at N Hz (default: estimated from rAF)',
  '  ?targetfps=N       frame rate the adaptive controller aims for                   [60]',
  '  ?priority=MODE     what the controller gives up first: pixels | denoise          [pixels]',
  '  ?frames=N          stop after N frames (for scripted runs)',
  '  ?autoexit=S        stop after S seconds and print the frame summary the exe prints',
  '  ?testtoggle=N      toggle the denoiser after N frames (same path as the N key)',
  '  ?testgiveup=N      force "the frame rate is being missed" from frame N on',
  '  ?testbudget=MS     replace the controller frame budget',
  '  ?testcamera=1      drive free look from the clock: free at 2 s, released at 6 s',
  '  ?dumpaccum=N       also upload the raw accumulated float target after still frame N',
  '  ?cmp=1             compare the captured still against a native reference PNG',
  '  ?ref=URL           ...against that reference (default: the native PNG in the repository\'s',
  '                     web/refs/, then refs/ beside the page; if neither answers the comparison is',
  '                     reported as SKIPPED, which is what a deployed site does)',
  '  ?verbose=1         the exe\'s extra logging (per-frame phases, camera, resize)',
  '  ?dpr=F             pin devicePixelRatio for reproducible measurements',
  '  ?mouselook=MODE    auto (default: pointer lock, falling back to mouse movement if refused),',
  '                     strict (only while locked) or always (ignore the lock)',
  '  ?upload=0          do not POST results or PNGs',
  '  ?calib=0           skip the fixed calibration kernel',
  '  ?help=1            this text',
  '',
  '  keys: ESC quit (a page cannot close its tab: it stops rendering and says quit)',
  '        Alt+Enter fullscreen, N denoiser, P pause the scene clock, Up/Down samples per pixel,',
  '        WASD free camera, Shift faster, mouse look once the canvas has pointer lock,',
  '        10 s idle hands the camera back',
  '',
  '        P freezes the orbit and the lights and keeps accumulating, so a pause converges to a clean',
  '        still instead of freezing as one noisy frame; P again resumes and restarts accumulation.',
  '        Up/Down set the samples per pixel per frame (1..16, the range the desktop app uses) and,',
  '        like N on the denoiser, a manual change is the user\'s for the rest of the run: the',
  '        adaptive controller keeps the resolution and the filter and stops choosing samples.',
];

const args = {
  help: flag('help', false),
  windowSize: sizeOf(qs.get('window'), null),
  capture: Math.max(0, num('capture', 0) | 0),
  accum: Math.max(1, num('accum', 48) | 0),
  size: sizeOf(qs.get('size'), [1280, 720]),
  t: parseFloat(qs.get('t') !== null ? qs.get('t') : '0.55'),
  // Empty (the default) means "try the candidate list above". A value here is resolved the same way,
  // so ?pak=assets/other.pak and ?pak=https://elsewhere/x.pak both work; neither is rewritten.
  pak: qs.get('pak') || '',
  pakUrl: (qs.get('pak') || '').trim() ? [siteUrl(qs.get('pak').trim())] : PAK_CANDIDATES.map(siteUrl),
  out: qs.get('out') === null ? 'browser/evidence' : (qs.get('out') === '0' ? '' : qs.get('out')),
  scale: clamp(num('scale', 1.0), 0.4, 1.0),
  autoq: flag('autoq', true),
  quality: qs.get('quality') || 'high',
  spp: Math.max(1, num('spp', 1) | 0),
  maxSpp: Math.max(1, num('maxspp', 16) | 0),
  minScale: clamp(num('minscale', 0.4), 0.25, 1.0),
  maxScale: clamp(num('maxscale', 1.0), 1.0, 3.0),
  denoise: flag('denoise', true),
  gbuf: (qs.get('gbuf') || '').toLowerCase(),
  filterZ: num('filterz', 0.02), filterN: num('filtern', 128), filterL: num('filterl', 24),
  refresh: num('refresh', 0),
  targetFps: num('targetfps', 60),
  priority: (qs.get('priority') || 'pixels').toLowerCase() === 'denoise' ? 'denoise' : 'pixels',
  frames: num('frames', 0) | 0,
  autoexit: num('autoexit', 0),
  testToggle: num('testtoggle', -1) | 0,
  testGiveUp: num('testgiveup', -1) | 0,
  testBudget: num('testbudget', 0),
  testCamera: flag('testcamera', false),
  dumpAccum: num('dumpaccum', -1) | 0,
  cmp: flag('cmp', false),
  // An explicit ?ref= is used as given (resolved against the page if it is relative); with none, the
  // candidate list decides, and a reference that is not there skips the comparison rather than
  // failing the run.
  ref: (qs.get('ref') || '').trim() ? siteUrl(qs.get('ref').trim()) : '',
  refUrls: (qs.get('ref') || '').trim() ? [siteUrl(qs.get('ref').trim())] : REF_CANDIDATES.map(siteUrl),
  verbose: flag('verbose', false),
  dpr: num('dpr', 0),
  // auto: look with pointer lock, and fall back to the mouse's own movement if the browser refuses the
  // lock (a page inside a cross-origin iframe without allow="pointer-lock" is the real case). strict:
  // only while locked, which is the closest thing to the exe's behaviour. always: ignore the lock.
  mouseLook: (qs.get('mouselook') || 'auto').toLowerCase(),
  // Rehearses the browser refusing pointer lock, so the fallback path can be measured at all: some
  // browsers (the headless one here among them) grant it every time.
  testNoLock: flag('testnolock', false),
  upload: flag('upload', true),
  calib: flag('calib', true),
};

const GBUF = { albedo: 1, normal: 2, depth: 3, denoised: 4, diff: 5, variance: 6 };
const debugView = GBUF[args.gbuf] || 0;

// The criteria for the still comparison were fixed in README.md section 9.3 before this page was
// written; the numbers below are that table, not something chosen after seeing a measurement.
const CRITERIA = {
  g1DepthWithin: 0.99, g1DepthTol: 4, g1SkyAgree: 0.98, g1SkyCode: 2,
  g2AlbedoWithin: 0.95, g2AlbedoTol: 12, g2AlbedoMean: 6,
  g3NormalWithin: 0.95, g3NormalTol: 12,
  s1FloorRel: 0.06, s2Within: 0.70, s2Tol: 16, s3FloorWithin: 0.85, s4FrameRel: 0.08,
};
const FLOOR_BAND = { x0: 0.30, x1: 0.70, y0: 0.84, y1: 0.96 };

// --------------------------------------------------------------------------- state
const canvas = document.getElementById('view');
const results = new Map();
const metric = (k, v) => { results.set(k, String(v)); };
const RUN_ID = `${Date.now().toString(36)}-${Math.floor(Math.random() * 1e6).toString(36)}`;

let device = null;
let ctx = null;
let renderer = null;
let cfg = null;
let quality = null;

const gpuErrors = { n: 0, list: [] };
function noteGpuError(msg) {
  ++gpuErrors.n;
  const key = String(msg).replace(/\d+/g, 'N').slice(0, 200);
  if (!gpuErrors.list.includes(key)) {
    gpuErrors.list.push(key);
    metric('uncaptured_error_' + gpuErrors.list.length, key);
  }
  L.warn('web: WebGPU error #%d: %s\n', gpuErrors.n, key);
}

// runWindowed()'s locals: the clock, the fps window, the accumulation counter, the camera.
const run = {
  frameIndex: 0, simTime: 0, samplesAccumulated: 0, quit: false, startedAt: performance.now(),
  last: 0, fpsClock: 0, fpsFrames: 0, fps: 0,
  winW: 0, winH: 0, traceW: 0, traceH: 0, dpr: 1,
  dtMin: 1e9, dtMax: 0, dtSum: 0, dtLate: 0, cpuMsAvg: 0, lastFrameLate: false,
  denoiseUserToggled: false,
  // P: the scene clock is stopped, the renderer is not. `frozen` is the driver's "stop the loop
  // entirely" switch (the evidence path needs it; README 10.1 says why pausing the loop is the wrong
  // way to take a readback) and has nothing to do with the key.
  paused: false, frozen: false,
  sppUserToggled: false,
  freeCam: false, camBlend: 0, yaw: 0, pitch: 0, camPos: [0, 0, 0], lastWasd: -1e9,
  orbitO: [0, 0, 0], orbitF: [0, 0, -1], pointerLocked: false, lockAsked: false, lockRefused: false,
  lateFramesWindow: 0, framesWindow: 0, testLateForced: false, denoiseLateStreak: 0,
};

// --------------------------------------------------------------------------- small helpers
const nowS = () => performance.now() / 1000;
const f16 = (x) => (Number.isFinite(x) ? x.toFixed(1) : String(x));

function invalidateQuality() {
  // frame.cpp:460 invalidateQuality(): throw away the smoothed total, keep the per-unit prices,
  // because those are per kilopixel per sample and survive a resize or a sample-count change.
  if (!quality) return;
  quality.avg = 0;
  quality.settle = quality.o.settleFrames;
  quality.overStreak = 0;
  quality.underStreak = 0;
  quality.decideFrames = 0;
  quality.lateFrames = 0;
  quality.frames = 0;
  run.lateFramesWindow = 0;
  run.framesWindow = 0;
}

/** The camera the exe would have this frame: the orbit, blended toward the free camera. */
function frameBasis(t, w, h) {
  const orbit = cameraBasis(cfg, t, w, h);
  run.orbitO = orbit.origin.slice();
  run.orbitF = orbit.forward.slice();
  const blend = run.camBlend;
  if (blend <= 0.0005) return orbit;
  // core2.cpp:354-375: the ORBIT basis is the starting point and the free camera is what it is
  // blended TOWARD, so blend 0 is "the orbit, entirely" and blend 1 "the free camera, entirely".
  const cp = Math.cos(run.pitch);
  let ff = [Math.sin(run.yaw) * cp, Math.sin(run.pitch), Math.cos(run.yaw) * cp];
  const fl = Math.hypot(...ff);
  ff = fl > 1e-6 ? ff.map((v) => v / fl) : ff;
  // The origin and the forward have to be blended in the same direction. Reading the exe's
  // `f[i] += (ff[i] - f[i]) * blend` the other way round - starting at the free forward and easing
  // toward orbit.forward - leaves the orbit's own direction as the rendered view once the blend is
  // full, so the camera translated where you steered it while still looking along a line that went
  // on orbiting: measured at 2.206 deg of turn per frame with W held (tools/run_page.mjs
  // --scenario orbitHold), easing to nothing the moment the two directions agree. The origin was
  // already blended the right way, which is why the position moved as asked and only the view lied.
  const o = orbit.origin.map((v, i) => v + (run.camPos[i] - v) * blend);
  let f = orbit.forward.map((v, i) => v + (ff[i] - v) * blend);
  // core2.cpp:366-374: RE-DERIVE right and up from the blended forward, rather than interpolating
  // them. Lerping a basis shears the picture halfway through the handover.
  const fl2 = Math.hypot(...f);
  if (fl2 > 1e-6) f = f.map((v) => v / fl2);
  let r = [-f[2], 0, f[0]];
  const rl = Math.hypot(r[0], r[2]);
  r = rl < 1e-5 ? [1, 0, 0] : [r[0] / rl, 0, r[2] / rl];
  const u = [r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]];
  return Object.assign({}, orbit, { origin: o, right: r, up: u, forward: f });
}

function enterFreeCam() {
  // frame.cpp:533-544 - taking over starts exactly where the automated camera is, or the first free
  // frame reads as a jump.
  const f = run.orbitF;
  run.pitch = Math.asin(clamp(f[1], -0.999, 0.999));
  run.yaw = Math.atan2(f[0], f[2]);
  run.camPos = run.orbitO.slice();
  run.freeCam = true;
  L.log('camera: free look (WASD to move, mouse to look; idle 10 s hands control back)\n');
  if (args.testNoLock) {
    // A labelled rehearsal of the refusal, because some browsers - including the headless one this is
    // tested with - grant the lock so readily that the fallback never runs on its own. The log says the
    // refusal was forced, so nothing in the record pretends the browser did it.
    markLockRefused('forced by ?testnolock=1');
  } else {
    askPointerLock();
  }
}

function askPointerLock() {
  if (run.lockAsked) return;
  run.lockAsked = true;
  // GLFW disables the cursor in the same call; a page may only take pointer lock inside a user
  // gesture, and a browser may still say no - a page in a cross-origin iframe without
  // allow="pointer-lock" does, and the message below is the honest version of what happens then:
  // looking carries on from the mouse's own movement, and the thing that is lost is the warp back to
  // the centre that stops the exe's cursor from ever reaching an edge.
  const refused = (why) => markLockRefused(why);
  try {
    const p = canvas.requestPointerLock ? canvas.requestPointerLock() : null;
    if (p && typeof p.catch === 'function') p.catch((e) => refused(String((e && e.name) || e)));
    else if (!p) refused('no requestPointerLock on this element');
  } catch (e) {
    refused(`requestPointerLock threw ${String((e && e.name) || e)}`);
  }
}

function markLockRefused(why) {
  if (run.lockRefused) return;
  run.lockRefused = true;
  L.warn('web: pointer lock refused (%s) - %s\n', why, args.mouseLook === 'strict'
    ? 'and ?mouselook=strict means the mouse will not look at all; WASD still moves'
    : 'looking continues from the mouse\'s own movement instead, but the cursor stays visible and can '
      + 'reach the edge of the window, which the desktop app cannot (it warps the cursor back to the '
      + 'centre each frame, frame.cpp:1294)');
}

// --------------------------------------------------------------------------- input, as the exe polls it
const keysDown = new Set();
const keyEdge = { n: false, altEnter: false };

window.addEventListener('keydown', (ev) => {
  if (ev.code === 'Escape') { ev.preventDefault(); quit('ESC'); return; }
  if (ev.code === 'Enter' || ev.code === 'NumpadEnter') {
    if (ev.altKey) { ev.preventDefault(); toggleFullscreen(); }
  }
  if (ev.code === 'KeyN' && !ev.repeat) toggleDenoise();
  if (ev.code === 'KeyP' && !ev.repeat) setPaused(!run.paused, 'pressed P');
  if ((ev.code === 'ArrowUp' || ev.code === 'ArrowDown') && !ev.repeat) {
    ev.preventDefault();                       // arrows must not scroll the page
    stepSamples(ev.code === 'ArrowUp' ? 1 : -1, ev.code === 'ArrowUp' ? 'ArrowUp' : 'ArrowDown');
  }
  if (ev.code === 'Tab') ev.preventDefault();
  keysDown.add(ev.code);
});
window.addEventListener('keyup', (ev) => keysDown.delete(ev.code));
window.addEventListener('blur', () => keysDown.clear());

document.addEventListener('pointerlockchange', () => {
  run.pointerLocked = document.pointerLockElement === canvas;
});
document.addEventListener('pointerlockerror', () => {
  // The other route to the same fact: some browsers resolve the request and then fire this. Guarded so
  // the page says it once rather than twice.
  markLockRefused('pointerlockerror');
});
canvas.addEventListener('click', () => {
  // A click is the gesture the browser will accept pointer lock from, so it re-arms the request.
  if (run.freeCam && !run.pointerLocked) { run.lockAsked = false; askPointerLock(); }
});
window.addEventListener('mousemove', (ev) => {
  if (!run.freeCam) return;
  // frame.cpp:1281-1295: read the cursor's offset from the window centre each frame, apply it at
  // 0.0022 per pixel with pitch clamped to +/-1.45, then warp the cursor back to the centre. Pointer
  // lock hands that same offset over as movementX. Without the lock movementX is still the offset the
  // cursor travelled, so the look keeps working; what the browser cannot do is move the cursor back,
  // which is the difference recorded in README section 7.
  const dx = ev.movementX || 0, dy = ev.movementY || 0;
  if (!dx && !dy) return;
  const allowed = run.pointerLocked
    || (args.mouseLook === 'auto' && run.lockRefused) || args.mouseLook === 'always';
  if (!allowed) return;
  run.yaw -= dx * 0.0022;
  run.pitch = clamp(run.pitch - dy * 0.0022, -1.45, 1.45);
});

function fwdSideKeys() {
  const down = (c) => (keysDown.has(c) ? 1 : 0);
  return {
    fwd: down('KeyW') - down('KeyS'),
    side: down('KeyD') - down('KeyA'),
    shift: down('ShiftLeft') || down('ShiftRight'),
  };
}

/** frame.cpp:1225-1320, in the exe's order. Returns false when the run should stop. */
function pollInput(dt) {
  const { fwd, side, shift } = fwdSideKeys();
  if ((fwd || side) && !run.freeCam) enterFreeCam();
  if (fwd || side) run.lastWasd = nowS();

  if (args.testCamera) {
    // The same state machine driven from the clock, so the handover can be read in the log instead
    // of a human holding keys (frame.cpp:1270-1280).
    const el = (performance.now() - run.startedAt) / 1000;
    if (el > 2.0 && el < 6.0) {
      if (!run.freeCam) enterFreeCam();
      run.lastWasd = nowS();
      run.yaw += 0.5 * dt;
      run.camPos[2] += 2.0 * dt;
    }
  }
  if (run.freeCam) {
    const speed = shift ? 12 : 4;
    const fx = Math.sin(run.yaw), fz = Math.cos(run.yaw);
    run.camPos[0] += (fx * fwd + -fz * side) * speed * dt;
    run.camPos[2] += (fz * fwd + fx * side) * speed * dt;
    // Movement stays in the horizontal plane - no vertical control, exactly as the exe.
    if (!run.paused && nowS() - run.lastWasd > 10.0) {
      run.freeCam = false;
      L.log('camera: idle for 10 s, easing back to the orbit\n');
      if (document.exitPointerLock && run.pointerLocked) document.exitPointerLock();
    }
  }
  // The handover eases on the scene clock, so a pause stops it as well: a camera drifting back toward
  // the orbit is scene motion, and motion is exactly what P stops. Pushing WASD or moving the mouse
  // while paused still updates the free-camera state, so a view can be lined up; it takes effect when
  // the clock starts again.
  if (run.paused) return true;
  // Taking control must feel immediate, handing it back must not be noticeable (frame.cpp:1306-1313).
  const want = run.freeCam ? 1 : 0;
  const rate = run.freeCam ? 8 : 0.7;
  if (run.camBlend < want) run.camBlend = Math.min(want, run.camBlend + rate * dt);
  else if (run.camBlend > want) run.camBlend = Math.max(want, run.camBlend - rate * dt);
  return true;
}

function toggleDenoise() {
  if (!renderer) return;
  // frame.cpp:374-388: a press of N is a decision, not a suggestion. The controller is told to stop
  // second-guessing it, in both directions, for the rest of the run.
  run.denoiseUserToggled = true;
  quality.denoiseUser = true;
  quality.flips = quality.o.maxFlips;
  quality.pendingGive = false;
  setDenoiseEnabled(!quality.denoise, 'pressed N');
}

function setDenoiseEnabled(on, why) {
  if (quality.denoise === on) return;
  quality.denoise = on;
  // setDenoiseEnabled() in the exe rebuilds the feature images and the filter history, so what comes
  // back has no history and no accumulated samples - the right answer when the pipeline underneath
  // the image just changed, since the alternative is showing a blend of two filters as one image.
  run.samplesAccumulated = 0;
  renderer.historyValid = false;
  renderer.prevBasis = null;
  invalidateQuality();
  L.log('denoise %s (%s)\n', on ? 'ON' : 'OFF', why);
  updateTitle(true);
}

// --------------------------------------------------------------------------- P: pause the scene, not the renderer
/**
 * What P means here, decided rather than guessed at (README section 2).
 *
 * The exe has no pause, so there is no sentence to copy; what it does have is the way its stills work -
 * a still is the same camera averaged over many frames. So pause freezes **scene time** and keeps the
 * renderer running: the orbit stops, the two light orbits stop, any free camera keeps moving only if
 * the user is still holding WASD, and every frame keeps tracing new samples into the accumulation
 * target, which is what lets the picture converge to a clean still instead of stopping as one noisy
 * frame. Freezing the frame loop instead would show exactly the frame you did not want to look at, and
 * README 10.1 measured that a paused loop also cannot be read back.
 *
 * Resuming restarts accumulation: the first frame after the scene starts moving again has to, because
 * samples from a stationary scene and samples from a moving one are not the same image. The exe has the
 * same rule for the things that do restart its accumulation - a resize, a denoiser flip, a scale change.
 */
function setPaused(on, why) {
  if (run.paused === !!on) return;
  run.paused = !!on;
  if (run.paused) {
    // The accumulation window opens: while the scene holds still, `cfg.accumulationFrames` no longer
    // describes what is worth averaging, so frames keep piling into the same target (see step()).
    L.log('web: %s - the scene clock is stopped and accumulation continues: the lights and the orbit ' +
          'hold their place while the frames keep averaging into a still. P again resumes.\n', why);
  } else {
    // A motion restart: the samples gathered at the frozen camera are stale from here on.
    run.samplesAccumulated = 0;
    L.log('web: %s - the scene clock runs again and accumulation restarts, because the frames before ' +
          'the pause belong to a still that no longer exists\n', why);
  }
  metric('pause_' + run.frameIndex, run.paused ? 'paused' : 'resumed');
  updateTitle(true);
  updateStatus();
}

// --------------------------------------------------------------------------- Up/Down: the sample count is the user's
const SPP_MIN = 1;
const sppMax = () => Math.max(SPP_MIN, args.maxSpp);
/** The sample count in force, on the exe's own rule (`core2.cpp:384`): live value or pinned value. */
const liveSpp = () => Math.max(SPP_MIN, quality.o.auto ? quality.spp : cfg.samplesPerFrame);

/**
 * One step of samples-per-pixel-per-frame, clamped to the range the exe uses: 1 up to
 * `cfg.maxSamplesPerFrame`, which `scene.h:133` sets to 16 and which is the controller's own ceiling
 * (`--max-spp`, `?maxspp=`). Like `N` on the denoiser, a manual change is authoritative for the rest of
 * the run: `quality.sppUser` takes samples out of the controller's hands and leaves it resolution and
 * the filter (`frame.cpp:374-388` is the rule this copies).
 */
function stepSamples(dir, why) {
  if (!renderer) return;
  const from = liveSpp();
  const to = clamp(from + dir, SPP_MIN, sppMax());
  quality.sppUser = true;
  run.sppUserToggled = true;
  quality.spp = to;
  cfg.samplesPerFrame = to;          // the pinned-quality path reads this, as the exe's does
  if (to === from) {
    L.log('quality decision: samples %d held (pressed %s: %d is the %s this app allows, %d..%d) - ' +
          'samples are the user\'s from here on\n', from, why, to, dir > 0 ? 'most' : 'fewest',
      SPP_MIN, sppMax());
  } else {
    L.log('quality decision: samples %d -> %d (pressed %s) - samples are the user\'s from here on; ' +
          'the controller keeps the resolution and the filter\n', from, to, why);
  }
  metric('spp_' + run.frameIndex, `${from}->${to}:${why}:user-pinned`);
  updateTitle(true);
  updateStatus();
}

// --------------------------------------------------------------------------- the status overlay
// A deliberate deviation from "the canvas is the only interface", asked for explicitly and recorded as
// one in README section 4.1: the exe's live numbers are in its window decoration, which a page has no
// equivalent for, so they go in a fixed-position layer that is not part of the layout, takes no
// pointer events, takes no focus, and cannot interfere with pointer lock.
const statusEl = document.getElementById('status');

function updateStatus() {
  if (!statusEl || !quality || run.quit) return;
  // The same values document.title carries, on one line, plus the pause state - so "is this picture
  // live?" is never a guess - and then the keyboard, because a window decoration has no room for it.
  const line = [
    `${Math.round(run.fps)} fps`,
    `${run.winW}x${run.winH} trace ${run.traceW}x${run.traceH}`,
    `${liveSpp()} spp${run.sppUserToggled ? ' (user)' : ''}`,
    'WebGPU pipeline',
    quality.denoise ? 'denoise ON' : 'denoise OFF',
    run.paused ? 'PAUSED' : 'LIVE',
  ].join('  |  ');
  const keys = 'N denoise  |  P pause  |  Up/Down samples  |  WASD free camera (10 s idle returns to ' +
    'the orbit)  |  Alt+Enter fullscreen  |  ESC quit';
  statusEl.textContent = `${line}\n${keys}`;
}

let fullscreenPending = false;
function toggleFullscreen() {
  // Alt+Enter, as Windows users expect: the exe puts the window on the monitor's own mode and
  // restores the rect it came from. A page can only ask the browser for fullscreen, and cannot pick
  // a display mode, so what the browser permits is logged instead of assumed (README section 2).
  const already = !!document.fullscreenElement;
  const screenW = (window.screen && window.screen.width) || 0;
  const screenH = (window.screen && window.screen.height) || 0;
  if (already) {
    const r = document.exitFullscreen ? document.exitFullscreen() : null;
    finishFullscreen(r, false, screenW, screenH);
  } else {
    const el = document.documentElement;
    const r = el.requestFullscreen ? el.requestFullscreen() : null;
    finishFullscreen(r, true, screenW, screenH);
  }
}

function finishFullscreen(promise, entering, screenW, screenH) {
  const report = (ok, err) => {
    if (!ok) {
      L.warn('web: Alt+Enter asked the browser for fullscreen and it refused (%s). A page cannot ' +
        'choose a display mode; the exe uses the monitor\'s native mode.\n', String(err || 'no reason given'));
      metric('fullscreen', 'refused:' + String(err || 'unknown').slice(0, 60));
      return;
    }
    metric('fullscreen', entering ? 'entered' : 'exited');
    L.log('window: %s (%dx%d)\n', entering ? 'fullscreen' : 'windowed', screenW, screenH);
    run.resizePending = true;
  };
  if (promise && typeof promise.then === 'function') promise.then(() => report(true), (e) => report(false, e && e.name));
  else report(!!(document.fullscreenElement || entering), entering ? 'requestFullscreen unavailable' : 'exitFullscreen unavailable');
}

// --------------------------------------------------------------------------- window size and resize
run.resizePending = false;

function desiredExtent() {
  const dpr = args.dpr > 0 ? args.dpr : (window.devicePixelRatio || 1);
  const css = args.windowSize || [window.innerWidth || 1280, window.innerHeight || 720];
  return { cssW: Math.max(16, css[0]), cssH: Math.max(16, css[1]), dpr };
}

function applyExtent(force) {
  const { cssW, cssH, dpr } = desiredExtent();
  const w = Math.max(16, Math.round(cssW * dpr));
  const h = Math.max(16, Math.round(cssH * dpr));
  canvas.style.width = cssW + 'px';
  canvas.style.height = cssH + 'px';
  const changed = force || canvas.width !== w || canvas.height !== h || run.dpr !== dpr;
  if (!changed) return false;
  const asked = args.windowSize;
  run.dpr = dpr;
  run.winW = w; run.winH = h;
  canvas.width = w; canvas.height = h;
  if (ctx && !run.quit) ctx.configure({ device, format: renderer.ctxformat, alphaMode: 'opaque' });
  applyHdrSize();
  if (asked && (canvas.width !== Math.round(asked[0] * dpr) || canvas.height !== Math.round(asked[1] * dpr))) {
    L.log('window: asked for %dx%d, given %dx%d\n', asked[0], asked[1],
      Math.round(canvas.width / dpr), Math.round(canvas.height / dpr));
  }
  if (run.frameIndex > 0) {
    // frame.cpp:956 - the resize line is --verbose only in the exe, and the cost model is discarded
    // either way because the pixel count changed.
    L.log('resize -> %ux%u (trace %ux%u)\n', run.winW, run.winH, run.traceW, run.traceH);
  }
  return true;
}

/** applyHdrSize(): the trace target is the window at the live scale, and accumulation restarts. */
function applyHdrSize() {
  const s = quality && quality.o.auto ? quality.scale : args.scale;
  // Not rounded up to a whole number of 8x8 workgroups, as the first attempt did: every kernel here
  // bounds-checks its thread against P.frame (trace.wgsl:650, svgf_temporal.wgsl:65,
  // svgf_atrous.wgsl:85, present.wgsl:68), so a window that is not a multiple of 8 traces at exactly
  // the window's size - which is what `trace %ux%u` in the title bar has to agree with.
  run.traceW = Math.max(16, Math.round(run.winW * s));
  run.traceH = Math.max(16, Math.round(run.winH * s));
  const capPx = 8500000;                            // cfg.maxTracePixels
  if (run.traceW * run.traceH > capPx) {
    const k = Math.sqrt(capPx / (run.traceW * run.traceH));
    run.traceW = Math.max(16, Math.round(run.traceW * k));
    run.traceH = Math.max(16, Math.round(run.traceH * k));
  }
  renderer.resize(run.traceW, run.traceH);
  run.samplesAccumulated = 0;
}

window.addEventListener('resize', () => { run.resizePending = true; });
window.addEventListener('orientationchange', () => { run.resizePending = true; });
// Worth a line, because it explains a frozen frame counter that is nobody's bug: requestAnimationFrame
// is the loop's clock, and the browser stops calling it for a page it considers not visible - after a
// fullscreen exit in a headless run, for instance. The desktop app keeps rendering an unfocused
// window; a backgrounded tab does not get that choice (README section 7).
document.addEventListener('visibilitychange', () => {
  metric('visibility_' + (document.hidden ? 'hidden' : 'visible') + '_' + run.frameIndex,
    document.hidden ? 'hidden' : 'visible');
  L.log(document.hidden
    ? 'web: the page is now hidden - the browser stops calling requestAnimationFrame, so the frame ' +
      'counter stops here and resumes when it is visible again\n'
    : 'web: the page is visible again - resuming the frame loop\n');
});

// --------------------------------------------------------------------------- title bar
let lastTitleAt = 0;
function updateTitle(force) {
  if (run.quit) return;
  const t = performance.now();
  if (!force && t - lastTitleAt < 1000) return;
  lastTitleAt = t;
  const spp = quality.o.auto ? quality.spp : Math.max(1, cfg.samplesPerFrame);
  document.title = exeTitle({
    fps: run.fps, winW: run.winW, winH: run.winH, traceW: run.traceW, traceH: run.traceH,
    spp, denoise: quality.denoise, freeLook: run.camBlend > 0.5, autoQuality: quality.o.auto,
    paused: run.paused,
  });
  metric('title', document.title);
  updateStatus();
}

// --------------------------------------------------------------------------- one frame
/** Write this frame's lights, instances, BVH refit and params. Mirrors uploadFrameInstances. */
function setFrame({ t, frameIndex, reset, spp, denoise, samplesForDisplay, debugView = 0,
  moving = false, presentMode = 0, dither = false }) {
  const lights = packLights(cfg, t);
  renderer.device.queue.writeBuffer(renderer.lightBuf, 0, lights);
  const cam = frameBasis(t, renderer.W, renderer.H);
  const prev = moving && renderer.prevBasis ? renderer.prevBasis : cam;
  renderer.writeParams({
    cam, prev, maxBounces: cfg.maxBounces, time: t, exposure: cfg.exposure,
    radianceClamp: cfg.radianceClamp, skyMode: cfg.skyMode, skyColor: cfg.skyColor,
    frameIndex, spp, samplesForDisplay, reset, denoise,
    featureWrites: denoise || debugView > 0, debugView, historyCap: cfg.denoiseHistoryCap,
    sigmaZ: args.filterZ, sigmaN: args.filterN, sigmaL: args.filterL,
    prevValid: moving && !!renderer.prevBasis && !reset, checkPixels: 0, dither, presentMode,
    kiloPx1: (renderer.W * renderer.H) / 1000,
  });
  if (reset) renderer.hdrCur = 0;
  if (moving) {
    updateInstanceRows(renderer.instRows, cfg, t);
    renderer.device.queue.writeBuffer(renderer.instBuf, 0, renderer.instRows);
    renderer.bvh.refit(renderer.regen);
    packTrianglesInto(renderer.bvh.tris, renderer.bvh.meta, renderer.triF32, renderer.triU32,
      renderer.bvh.totalTris);
    renderer.device.queue.writeBuffer(renderer.nodeBuf, 0, renderer.bvh._rawNodes, 0,
      renderer.bvh.nodeCount * 8);
    renderer.device.queue.writeBuffer(renderer.triBuf, 0, renderer.triF32);
  }
  renderer.prevBasis = cam;
  renderer.prevBasis.valid = true;
}

async function renderOnce(opts) {
  const d = renderer.device;
  const enc = d.createCommandEncoder({ label: 'enc_frame' });
  renderer.encodeFullFrame(enc, { denoise: opts.denoise, present: true, accum: true });
  d.queue.submit([enc.finish()]);
  await d.queue.onSubmittedWorkDone();
  renderer.drawToCanvas();
}

/** A probe frame: the same work, split into two submissions so both cost terms get priced. */
async function renderProbeFrame(opts) {
  const d = renderer.device;
  const t0 = performance.now();
  const e1 = d.createCommandEncoder({ label: 'enc_probe_trace' });
  renderer.encodeTrace(e1, renderer.trace.full, { accum: true });
  d.queue.submit([e1.finish()]);
  await d.queue.onSubmittedWorkDone();
  const mid = performance.now();
  const e2 = d.createCommandEncoder({ label: 'enc_probe_filter' });
  renderer.encodeTemporal(e2);
  renderer.encodeAtrous(e2);
  renderer.encodePresent(e2);
  d.queue.submit([e2.finish()]);
  await d.queue.onSubmittedWorkDone();
  renderer.drawToCanvas();
  const end = performance.now();
  return { frameMs: end - t0, traceMs: mid - t0, filterMs: end - mid };
}

async function step(now) {
  if (run.frozen) { run.last = now; run.prevNow = now; return false; }
  const t0 = performance.now();
  if (run.last === 0) { run.last = now; run.fpsClock = now; }
  let dt = (now - run.last) / 1000;
  run.last = now;
  if (dt > 0.1) dt = 0.1;                       // frame.cpp:1216
  // P: the clock is the scene's, so this is the one line pause touches in the loop's timekeeping.
  // Everything downstream of it - the orbit angle, the light orbits, the orb refit - reads run.simTime
  // and therefore holds still, while dt, the fps window and the frame counter keep running normally.
  if (!run.paused) run.simTime += dt;
  const target = args.targetFps > 0 ? args.targetFps : 60;
  run.lastFrameLate = dt > 1.25 / Math.max(10, target);
  if (dt < run.dtMin) run.dtMin = dt;
  if (dt > run.dtMax) run.dtMax = dt;
  if (dt > 0.020) ++run.dtLate;
  run.dtSum += dt;

  const itv = now - (run.prevNow || now);
  run.prevNow = now;
  if (itv > 0.5 && itv < 100) run.cpuMsAvg = run.cpuMsAvg <= 0 ? itv : 0.1 * itv + 0.9 * run.cpuMsAvg;

  pollInput(dt);
  if (run.testGiveUp >= 0 && run.frameIndex === run.testGiveUp) {
    run.denoiseLateStreak = 8;
    run.testLateForced = true;
    quality.pendingGive = true;
    L.log('test: forced a frame rate that is missing its target\n');
  }
  if (run.testLateForced && !quality.denoise) {
    run.testLateForced = false;
    L.log('test: forcing stopped, the give-up it was asking for has happened\n');
  }
  if (args.testToggle >= 0 && run.frameIndex === args.testToggle) toggleDenoise();

  if (run.resizePending) { run.resizePending = false; applyExtent(false); invalidateQuality(); }

  // A scale change is a reallocation (the exe's setLiveScale, frame.cpp:484): the trace target is
  // rebuilt and accumulation restarts. It is applied before the accumulation decision below so the
  // frame that pays for the reallocation is not also the frame that restarts the count.
  if (quality.scale !== (run.appliedScale || -1)) {
    // The screen keeps the last complete frame while this happens, because the blit is only redrawn
    // once the new target has been traced - the same reason the exe moves the old images aside
    // instead of destroying them (app.h's `hold`).
    run.appliedScale = quality.scale;
    applyHdrSize();
    invalidateQuality();
  }

  // The exe's accumulation rule: the camera orbits, so every presented frame is a new image and the
  // target is only kept when the scene asks for several frames per image.
  // While paused there is no new image: the scene holds one place, which is precisely when averaging
  // frames is the point (it is how the exe's stills get their clean look), so the window is unbounded
  // and the count climbs until P is pressed again.
  const cap = run.paused ? Infinity : Math.max(1, cfg.accumulationFrames || 1);
  const reset = run.samplesAccumulated <= 0 || run.samplesAccumulated >= cap;
  run.samplesAccumulated = reset ? 1 : run.samplesAccumulated + 1;

  const spp = liveSpp();
  const denoise = quality.denoise;
  if (denoise !== run.appliedDenoise) run.appliedDenoise = denoise;

  setFrame({
    t: run.simTime, frameIndex: run.frameIndex + 1, reset, spp, denoise,
    samplesForDisplay: run.samplesAccumulated, debugView, moving: run.frameIndex > 0,
    presentMode: 1, dither: true,
  });

  let frameMs, traceMs = 0, filterMs = 0, probe = false;
  if (quality.wantsProbe(run.frameIndex)) {
    probe = true;
    const r = await renderProbeFrame({ denoise });
    frameMs = r.frameMs; traceMs = r.traceMs; filterMs = r.filterMs;
  } else {
    await renderOnce({ denoise });
    frameMs = performance.now() - t0;
  }
  ++run.frameIndex;
  ++run.fpsFrames;
  if (run.lastFrameLate) ++run.lateFramesWindow;
  ++run.framesWindow;

  // `--test-give-up` in the exe says "treat this run as missing its target whatever the numbers".
  // The controller's own definition of missing it is frame time against the deadline it was handed,
  // so forcing it from the caller's side is the same statement rather than a new knob: hand it a
  // 1 ms deadline and every frame is late. Pair it with ?testbudget=, which is the exe's other
  // documented way to reach that state on a GPU that comfortably clears 60.
  const targetMs = run.testLateForced ? 1 : 1000 / target;
  const decision = probe ? null : quality.observe(frameMs, traceMs, filterMs, targetMs, false);
  if (probe) quality.observe(frameMs, traceMs, filterMs, targetMs, true);
  if (decision) reportDecision(decision);
  if (quality.settled && !run.settledSaid) {
    run.settledSaid = true;
    L.log('web: controller settled at %d spp, scale %.2f, denoiser %s - it will not move again\n',
      quality.spp, quality.scale, quality.denoise ? 'on' : 'off');
  }

  const el = performance.now() - run.startedAt;
  if (now - run.fpsClock > 1000) {
    run.fps = 1000 * run.fpsFrames / (now - run.fpsClock);
    run.fpsClock = now;
    run.fpsFrames = 0;
    updateTitle(true);
  }
  if (args.verbose && run.frameIndex % 60 === 0) {
    L.log('  per-frame ms: cpu-side %.1f  gpu+submit %.1f  | fb %dx%d hdr %ux%u swap %ux%u - ' +
          '%d spp @ scale %.2f (frame avg %.1f ms, budget %.1f)\n',
      t0 === 0 ? 0 : 0, frameMs, run.winW, run.winH, run.traceW, run.traceH, run.winW, run.winH,
      spp, quality.scale, quality.avg, quality.budget());
    L.log('  cam: %s blend %.2f pos %.1f %.1f %.1f yaw %.2f pitch %.2f\n',
      run.freeCam ? 'free  ' : 'handing', run.camBlend, run.camPos[0], run.camPos[1], run.camPos[2],
      run.yaw, run.pitch);
  }

  // No pixel readback in the ordinary path: a readback costs a GPU round trip and would land inside
  // the very frame time the controller is measuring. The driver asks for one when it wants it,
  // through window.__ray.canvas(), or asks for the pair through window.__ray.save(), which is
  // carried out *inside* the next frame - see wantSave below.

  if (run.wantSave && !run.saveBusy) {
    // Taken here, immediately after the blit of this frame, because that is the one moment when the
    // presentation texture and the canvas are known to be the same frame. Reading them from outside
    // the loop lets a frame boundary fall between the two reads, and pausing the loop stops the
    // compositor presenting at all - measured here: a paused canvas copy came back cleared while the
    // same frame copied from inside the loop matched the presentation texture at 100 %.
    run.saveBusy = true;
    try {
      const bytes = await renderer.readPresent();
      evidence.lastPresent = bytes;
      const c = await canvasBytes();
      evidence.lastCanvas = c;
      const out = [];
      const dir = args.out || 'browser/evidence';
      if (c) out.push([`${run.wantSave}_canvas.png`, await uploadPng(`${run.wantSave}_canvas.png`,
        await encodePng(c.rgba, c.w, c.h), dir)]);
      out.push([`${run.wantSave}_present.png`, await uploadPng(`${run.wantSave}_present.png`,
        await encodePng(bytes, renderer.W, renderer.H), dir)]);
      if (run.still && run.still.pngBytes) {
        out.push([run.still.name, await uploadPng(run.still.name, run.still.pngBytes, dir)]);
      }
      metric('canvas_mean_luma', c ? meanLuma(c.rgba).toFixed(2) : 'unavailable');
      metric('present_mean_luma', meanLuma(bytes).toFixed(2));
      metric('pair_canvas_size', c ? `${c.w}x${c.h}` : 'none');
      metric('pair_present_size', `${renderer.W}x${renderer.H}`);
      metric('canvas_empty_copies', canvasCopy.emptyCopies);
      metric('canvas_copy_errors', canvasCopy.errors.length);
      run.saveResult = { at: run.frameIndex, files: out };
    } catch (e) {
      run.saveResult = { at: run.frameIndex, error: String(e.message || e) };
    }
    run.wantSave = null;
    run.saveBusy = false;
  }

  if (args.frames > 0 && run.frameIndex >= args.frames) return finish(`reached ${args.frames} frames`);
  if (args.autoexit > 0 && el / 1000 > args.autoexit) {
    L.log('auto-exit after %.1f s: %d frames (%.1f fps average) | frame interval %.1f..%.1f ms, ' +
          'mean %.1f, %d over 20 ms\n', el / 1000, run.frameIndex, run.frameIndex / (el / 1000),
      run.dtMin * 1000, run.dtMax * 1000, 1000 * run.dtSum / Math.max(1, run.frameIndex), run.dtLate);
    return finish(`auto-exit after ${args.autoexit} s`);
  }
  return false;
}

/** The exe's decision sentences (frame.cpp:718, 860, 872, 913), with the honest measurement word. */
function reportDecision(d) {
  const late = run.framesWindow >= 12 ? run.lateFramesWindow / run.framesWindow : 0;
  if (d.what === 'denoise' && d.to === false) {
    L.log('quality decision: denoise off (late fraction %.2f, scale %.2f, spp %d)\n',
      late, quality.scale, quality.spp);
    setDenoiseEnabled(false, d.why);
  } else if (d.what === 'denoise') {
    L.log('quality decision: denoise on (headroom %.1f ms, scale %.2f, spp %d)\n',
      Math.max(0, quality.budget() - quality.msAt(quality.scale, quality.spp)), quality.scale,
      quality.spp);
    setDenoiseEnabled(true, d.why);
  } else if (d.what === 'scale') {
    L.log('quality decision: scale %.2f -> %.2f (frame %.1f ms measured, budget %.1f ms)\n',
      d.scale, d.to, d.ms, d.budget);
  } else if (d.what === 'spp') {
    L.log('quality decision: samples %d -> %d (frame %.1f ms measured, budget %.1f ms)\n',
      d.spp, d.to, d.ms, d.budget);
  } else if (d.what === 'exhausted') {
    L.warn('web: CONTROLLER EXHAUSTED: %s\n', d.why);
  } else if (d.what === 'settled') {
    L.log('web: %s\n', d.why);
  }
  const st = quality.state();
  L.log('  quality: %.1f ms measured -> %d spp at scale %.2f (budget %.1f ms of a %.1f ms period, ' +
        'trace %.5f ms per kilopixel per sample, filter %.2f ms per megapixel)\n',
    d.ms, st.spp, st.scale, st.budgetMs, 1000 / args.targetFps, st.tracePerKiloPxSpp,
    st.filterPerMegaPx);
  metric('decision_' + quality.decisions.length,
    `frame${run.frameIndex}:${d.what}=${d.to}:budget${d.budget}:avg${d.ms}`);
  if (d.what === 'scale') run.appliedScale = -1;   // applied by the next frame's applyHdrSize
  updateTitle(true);
}

// --------------------------------------------------------------------------- stills (--capture)
async function capture() {
  const [W, H] = args.size;
  let denoise = args.denoise;
  if (!qs.has('denoise') && debugView === 0) {
    denoise = false;
    L.log('stills: denoiser left off (a still averages frames; it does not filter them)\n');
  }
  L.log('capturing %d image(s) x %d samples into %s\\\n', args.capture, args.accum, args.out || '(no upload)');
  const written = [];
  for (let img = 0; img < args.capture; ++img) {
    const t = args.t + img * 2.35;              // frame.cpp:1489, the fixed still schedule
    renderer.resize(W, H);
    run.winW = W; run.winH = H; run.traceW = W; run.traceH = H;
    canvas.width = W; canvas.height = H;
    ctx.configure({ device, format: renderer.ctxformat, alphaMode: 'opaque' });
    let samples = 0;
    for (let f = 0; f < args.accum; ++f) {
      samples += 1;
      if (args.verbose) {
        L.log('  accum frame %d reset=%d frameIndex=%d samples=%d\n', f, f === 0 ? 1 : 0, f + 1, samples);
      }
      setFrame({
        t, frameIndex: f + 1, reset: f === 0, spp: Math.max(1, cfg.samplesPerFrame), denoise,
        samplesForDisplay: debugView ? 1 : samples, debugView, moving: false,
        presentMode: 0, dither: false,
      });
      const enc = device.createCommandEncoder({ label: 'enc_still' });
      renderer.encodeFullFrame(enc, { denoise, present: false, accum: true });
      device.queue.submit([enc.finish()]);
      await device.queue.onSubmittedWorkDone();
      if (args.dumpAccum === f) {
        const where = await uploadFloatDump(`accum_${String(f).padStart(3, '0')}`, W, H);
        L.log('  dumped raw target after frame %d (%s)\n', f, where);
      }
    }
    const enc2 = device.createCommandEncoder({ label: 'enc_still_present' });
    setFrame({
      t, frameIndex: samples, reset: false, spp: Math.max(1, cfg.samplesPerFrame), denoise,
      samplesForDisplay: debugView ? 1 : samples, debugView, presentMode: 0, dither: false,
    });
    renderer.encodePresent(enc2);
    device.queue.submit([enc2.finish()]);
    await device.queue.onSubmittedWorkDone();
    const bytes = await renderer.readPresent();
    const chanName = debugView === 1 ? '_albedo' : debugView === 2 ? '_normal'
      : debugView === 3 ? '_depth' : '';
    const name = `frame_${String(img).padStart(3, '0')}_t${t.toFixed(2)}${chanName}.png`;
    const png = await encodePng(bytes, W, H);
    const where = args.out ? await uploadPng(name, png, args.out) : '(uploading off)';
    L.log('  wrote %s (%d spp)\n', name, samples);
    L.log('  -> %s\n', where);
    written.push({ name, w: W, h: H, rgba: bytes, t });
    run.still = { name, w: W, h: H, bytes, pngBytes: png, t, denoise };
    metric('still_name', name);
    metric('still_frames', samples);
    metric('still_denoise', denoise ? 'on' : 'off (a still averages frames, it does not filter them)');
    metric('still_t', t.toFixed(2));
  }
  if (args.cmp && written.length) await compareAgainstNative(written[0]);
  return written;
}

/**
 * The float accumulation dump. The exe writes accum_NNN.bin; a page cannot write a file and
 * demo_server.py's /upload takes PNG bytes or a data: URL only, and web/ is not to be edited - so
 * the raw float32 buffer goes out as a PNG whose pixels are the bytes, which is lossless and says
 * so in its name.
 */
async function uploadFloatDump(base, W, H) {
  const half = await renderer.readHalf(renderer.hdr);
  const bytes = new Uint8Array(half.buffer.slice(0));
  const side = Math.max(1, Math.ceil(Math.sqrt(bytes.length / 4)));
  const rgba = new Uint8Array(side * side * 4);
  rgba.set(bytes.subarray(0, Math.min(bytes.length, rgba.length)));
  const png = await encodePng(rgba, side, side);
  return uploadPng(`${base}_floats.png`, png, (args.out || 'browser/evidence') + '');
}

async function compareAgainstNative(mine) {
  L.log('web: still criteria were fixed in README section 9.3 before measuring: %s\n',
    JSON.stringify(CRITERIA));
  // Opt-in (?cmp=1) and skippable: the reference is a native-rendered PNG that lives in the
  // repository, not in the published site, so on GitHub Pages the honest outcome is "skipped, no
  // reference here" - not a failure, and above all not a silent pass.
  const refTry = await firstOk(args.refUrls);
  if (!refTry.res) {
    const misses = (refTry.misses || []).join('; ');
    L.warn('web: still comparison SKIPPED - no reference image (tried %s)\n', misses.slice(0, 200));
    metric('cmp_verdict', 'skipped');
    metric('cmp_skipped', `no reference: ${misses}`.slice(0, 200));
    return;
  }
  L.log('web: reference %s\n', refTry.url);
  let ref = null;
  try {
    // The candidate probe already has the bytes in hand; decoding them beats asking again for a
    // 1.3 MB PNG.
    ref = await decodePng(new Uint8Array(await refTry.res.arrayBuffer()));
  } catch (e) {
    L.warn('web: cannot decode the native reference %s: %s\n', refTry.url, e.message);
    metric('cmp_verdict', 'skipped');
    metric('cmp_skipped', `undecodable reference: ${e.message}`.slice(0, 200));
    return;
  }
  // Which criteria the comparison is judged by follows the channel that was rendered, so a depth dump
  // is held to the depth criterion rather than to the picture's.
  const chan = debugView === 1 ? 'albedo' : debugView === 2 ? 'normal'
    : debugView === 3 ? 'depth' : 'still';
  const tol = { depth: CRITERIA.g1DepthTol, albedo: CRITERIA.g2AlbedoTol,
    normal: CRITERIA.g3NormalTol, still: CRITERIA.s2Tol }[chan];
  metric('cmp_channel', chan);
  const put = (k, v) => { metric(k, String(v)); L.log('web: %s = %s\n', k, v); };
  let compareFailures = 0;
  const chk = (name, ok, val) => {
    if (!ok) ++compareFailures;
    metric('pass_' + name, ok ? 'PASS' : 'FAIL');
    if (ok) L.log('web: %s: %s PASS\n', name, val); else L.warn('web: %s: %s FAIL\n', name, val);
  };
  const s = compareImages(mine, ref, FLOOR_BAND, tol);
  put(`cmp_${chan}_mean_abs_diff`, s.meanAbsDiff.toFixed(2));
  put(`cmp_${chan}_within_${tol}`, s.withinTol.toFixed(4));
  put(`cmp_${chan}_worst`, s.worst);
  put(`cmp_${chan}_worst_at`, `${s.worstX},${s.worstY}`);
  put(`cmp_${chan}_band_luma_web`, s.bandMeanLumaA.toFixed(2));
  put(`cmp_${chan}_band_luma_native`, s.bandMeanLumaB.toFixed(2));
  put(`cmp_${chan}_band_rel_luma`, s.bandRelLumaDiff.toFixed(4));
  put(`cmp_${chan}_band_within`, s.bandWithinTol.toFixed(4));
  const rel = wholeLumaRel(mine, ref);
  put(`cmp_${chan}_frame_rel_luma`, rel.toFixed(4));

  if (chan === 'still') {
    chk('S1_floor_band_luminance_within_6', s.bandRelLumaDiff <= CRITERIA.s1FloorRel,
      (s.bandRelLumaDiff * 100).toFixed(2) + ' %');
    chk('S2_whole_frame_within_16_255_70', s.withinTol >= CRITERIA.s2Within,
      (s.withinTol * 100).toFixed(2) + ' %');
    chk('S3_floor_band_within_16_255_85', s.bandWithinTol >= CRITERIA.s3FloorWithin,
      (s.bandWithinTol * 100).toFixed(2) + ' %');
    chk('S4_whole_frame_luminance_within_8', rel <= CRITERIA.s4FrameRel, (rel * 100).toFixed(2) + ' %');
  } else if (chan === 'depth') {
    chk('G1_depth_within_4_255_99', s.withinTol >= CRITERIA.g1DepthWithin,
      (s.withinTol * 100).toFixed(2) + ' %');
    chk('G1_sky_feature_mask_98', skyAgreement(mine, ref) >= CRITERIA.g1SkyAgree,
      (skyAgreement(mine, ref) * 100).toFixed(2) + ' %');
  } else if (chan === 'albedo') {
    chk('G2_albedo_within_12_255_95', s.withinTol >= CRITERIA.g2AlbedoWithin,
      (s.withinTol * 100).toFixed(2) + ' %');
    chk('G2_albedo_mean_diff_6', s.meanAbsDiff <= CRITERIA.g2AlbedoMean, s.meanAbsDiff.toFixed(2));
    put('cmp_sky_mask_agree', skyAgreement(mine, ref).toFixed(4));
  } else if (chan === 'normal') {
    chk('G3_normal_within_12_255_95', s.withinTol >= CRITERIA.g3NormalWithin,
      (s.withinTol * 100).toFixed(2) + ' %');
  }
  metric('correctness_failures_' + chan, compareFailures);
  metric('correctness_verdict_' + chan,
    compareFailures === 0 ? 'pipeline_matches_native' : 'DOES_NOT_MEET_CRITERIA');
}

/**
 * The silhouette test: which pixels report no feature surface at all (sky, or a path that never
 * reaches a diffuse hit). It does not care about shading, only about geometry, so it is the one
 * number in the comparison that cannot be explained away by a shading difference.
 */
function skyAgreement(a, b) {
  if (!a || !b || a.w !== b.w || a.h !== b.h) return 0;
  const code = (img, p) => img.rgba[p * 4] <= CRITERIA.g1SkyCode
    && img.rgba[p * 4 + 1] <= CRITERIA.g1SkyCode && img.rgba[p * 4 + 2] <= CRITERIA.g1SkyCode;
  const n = a.w * a.h;
  let same = 0;
  for (let p = 0; p < n; ++p) if (code(a, p) === code(b, p)) ++same;
  return same / n;
}

function wholeLumaRel(a, b) {
  const lum = (img) => {
    let s = 0;
    for (let i = 0; i < img.rgba.length; i += 4) s += 0.2126 * img.rgba[i] + 0.7152 * img.rgba[i + 1] + 0.0722 * img.rgba[i + 2];
    return s / (img.rgba.length / 4);
  };
  const la = lum(a), lb = lum(b);
  return lb > 0 ? Math.abs(la - lb) / lb : 0;
}

// --------------------------------------------------------------------------- frame evidence
// Three successive canvas readbacks whose hashes differ is the proof that frames advance; the last
// of them has to be the same bytes the presentation texture holds, because a blit that samples its
// source with coordinates outside 0..1 presents a picture that was never rendered
// (docs/web-port-notes.md section 11). The readbacks are taken on demand - by the driver, through
// window.__ray.canvas() - and never from inside the frame loop, because a readback costs a GPU
// round trip and would be measured as part of the frame it is checking.
const evidence = { hashes: [], errors: [] };
const canvasCopy = { attempts: 1, emptyCopies: 0, retries: 0, errors: [] };

async function canvasBytes() {
  // copyExternalImageToTexture from a WebGPU <canvas> is the least reliable call in this page. Taken
  // between a present and the next acquire it can hand back a buffer the compositor has just cleared,
  // and on some browsers it raises "[Invalid Texture] is invalid due to a previous error" instead
  // (docs/web-port-notes.md 11.6 lists this as a known refusal: it is a fact about the evidence path,
  // not a validation error of the run, so it is taken inside an error scope and counted apart).
  // The rendered frame is in renderer.presentTex either way, so an empty copy is retried after the
  // next compositor turn, the retry count is reported rather than hidden, and a canvas that is
  // genuinely black is still reported as black once the retries are spent.
  const d = renderer.device;
  const scoped = await pushScope(d);
  let last = null;
  try {
    for (let attempt = 0; attempt < 6; ++attempt) {
      await nextFrame();
      try {
        last = await renderer.readCanvasPixels(canvas);
      } catch (e) {
        canvasCopy.errors.push(String(e.message || e).slice(0, 140));
        return null;
      }
      if (last && !allZero(last.rgba)) {
        canvasCopy.attempts = attempt + 1;
        canvasCopy.retries += attempt;
        return last;
      }
      canvasCopy.emptyCopies++;
    }
  } finally {
    await popScope(d, scoped);
  }
  return last;
}

const nextFrame = () => new Promise((r) => requestAnimationFrame(() => r()));

async function pushScope(d) {
  try { await d.pushErrorScope('validation'); return true; } catch (e) { return false; }
}

async function popScope(d, scoped) {
  if (!scoped) return;
  try {
    const e = await d.popErrorScope();
    if (e) canvasCopy.errors.push(String(e.message).replace(/\s+/g, ' ').slice(0, 160));
  } catch (err) { /* pop unsupported: the copy already told us what it knows */ }
}

function allZero(bytes) {
  for (let i = 0; i < bytes.length; i += 97) if (bytes[i] !== 0) return false;
  return true;
}

function meanLuma(bytes) {
  let s = 0, n = 0;
  for (let i = 0; i < bytes.length; i += 4) {
    s += 0.2126 * bytes[i] + 0.7152 * bytes[i + 1] + 0.0722 * bytes[i + 2];
    ++n;
  }
  return n ? s / n : 0;
}

// --------------------------------------------------------------------------- quit / teardown
let finishing = false;
async function finish(reason) {
  if (finishing) return true;
  finishing = true;
  metric('finished', reason);
  metric('frames', run.frameIndex);
  metric('gpu_errors', gpuErrors.n);
  metric('console_errors', logCounts.error);
  metric('run_verdict', gpuErrors.n === 0 && logCounts.error === 0 ? 'clean' : 'NOT_CLEAN');
  metric('final_state', JSON.stringify(quality ? quality.state() : {}));
  await deliver(true);
  return true;
}

function quit(via) {
  if (run.quit) return;
  run.quit = true;
  // The exe leaves its loop and destroys everything. A page cannot close its own tab - browsers
  // forbid window.close() for a tab the script did not open - so parity is the rest of that:
  // rendering stops, the device is torn down, and the title says quit. README section 2.
  L.log('web: %s - rendering stopped, device released, frame counter frozen at %d ' +
        '(a page cannot close its own tab, so this is what quit means here)\n', via, run.frameIndex);
  metric('quit_via', via);
  metric('quit_frame', run.frameIndex);
  (async () => {
    try { await device.queue.onSubmittedWorkDone(); } catch (e) { /* already gone */ }
    if (renderer) renderer.destroy();
    if (device.destroy) device.destroy();
    document.title = QUIT_TITLE;
    metric('quit_title', QUIT_TITLE);
    await deliver(true);
  })();
}

// --------------------------------------------------------------------------- results
let delivered = 0;
async function deliver(end) {
  if (!args.upload) return;
  const stamp = new Date().toISOString();
  const lines = [`run_started=${stamp}`, `run_id=${RUN_ID}`,
    `run_query=${location.search.slice(1)}`, `adapter=${adapterName(adapterInfo)}`,
    ...[...results].map(([k, v]) => `${k}=${v}`)];
  if (end) lines.push('RESULTS_END');
  const where = await postResults(lines.join('\n') + '\n');
  if (end) { delivered++; metric('results_posted', where); }
}
setInterval(() => { if (!finishing) deliver(false); }, 4000);

// --------------------------------------------------------------------------- adapter identity
let adapterInfo = {};
const ADAPTER_FIELDS = ['vendor', 'architecture', 'device', 'description', 'isFallbackAdapter',
  'subgroupMinSize', 'subgroupMaxSize'];

/**
 * Which GPU is this, read the way the API actually allows. The fields are attributes on the
 * prototype, so Object.keys() on the object is empty even when info.vendor reads 'nvidia'; and the
 * accessor has moved three times, so all three routes are tried and the empty ones are reported as
 * empty rather than as 'none'.
 */
async function readAdapter(adapter) {
  const read = (o) => {
    const r = {};
    if (!o || (typeof o !== 'object' && typeof o !== 'function')) return r;
    for (const k of ADAPTER_FIELDS) {
      try { if (o[k] !== undefined && o[k] !== null && o[k] !== '') r[k] = o[k]; } catch (e) { /* threw */ }
    }
    return r;
  };
  const proto = (o) => {
    try { const p = Object.getPrototypeOf(o); return p ? Object.getOwnPropertyNames(p).join(',') : ''; }
    catch (e) { return 'threw'; }
  };
  const routes = [];
  let got = null, route = 'none';
  const tryRoute = async (name, fn) => {
    if (got) return;
    try {
      const o = await fn();
      if (!o) { routes.push(`${name}->${o === null ? 'null' : 'undefined'}`); return; }
      const r = read(o);
      routes.push(`${name}: read=[${Object.keys(r).join(',')}] own=[${Object.keys(o).join(',')}]`);
      if (Object.keys(r).length) { got = r; route = name; }
    } catch (e) { routes.push(`${name}: threw ${String(e && e.message || e).slice(0, 60)}`); }
  };
  await tryRoute('navigator.gpu.requestAdapterInfo',
    () => (typeof navigator.gpu.requestAdapterInfo === 'function' ? navigator.gpu.requestAdapterInfo() : null));
  await tryRoute('adapter.requestAdapterInfo',
    () => (typeof adapter.requestAdapterInfo === 'function' ? adapter.requestAdapterInfo() : null));
  await tryRoute('adapter.info', () => (typeof adapter.info === 'object' ? adapter.info : null));
  adapterInfo = got || {};
  metric('adapter_info_source', route);
  metric('adapter_info_routes', routes.join(' ; ').slice(0, 300));
  metric('adapter_info_raw', JSON.stringify(adapterInfo).slice(0, 300));
  metric('adapter_vendor', String(adapterInfo.vendor || 'unknown'));
  metric('adapter_architecture', String(adapterInfo.architecture || 'unknown'));
  metric('adapter_device', String(adapterInfo.device || 'unknown'));
  metric('adapter_description', String(adapterInfo.description || 'unknown').slice(0, 120));
  const fb = adapterInfo.isFallbackAdapter !== undefined ? adapterInfo.isFallbackAdapter
    : adapter.isFallbackAdapter;
  metric('is_fallback_adapter', String(fb));
  const desc = `${adapterInfo.vendor || ''} ${adapterInfo.device || ''} ${adapterInfo.description || ''}`.toLowerCase();
  const software = /llvmpipe|swrast|software|basic render|softwar|swiftshader|mesa offscreen/.test(desc);
  metric('adapter_software_suspect', String(software));
  if (fb === true || software) {
    L.warn('web: the adapter looks like a SOFTWARE adapter (isFallbackAdapter=%s, description="%s"): ' +
      'every millisecond below is a CPU timing\n', String(fb), String(adapterInfo.description || '').slice(0, 60));
  }
  return { route, fallback: fb, software };
}

/**
 * The fixed calibration kernel: a constant 2147483648 dependent FMAs, so the number is comparable
 * between runs and between machines. It is logged next to the adapter on every run, and nothing is
 * ever attributed to a card from it alone - the adapter fields are the identity, this is the
 * cross-check.
 */
async function runCalibration() {
  const CALIB_FLOPS = 1048576 * 1024 * 2;
  let code;
  try {
    const res = await fetch('./shaders/calib.wgsl');
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    code = await res.text();
  } catch (e) {
    metric('calib_status', 'kernel could not be fetched: ' + e.message.slice(0, 80));
    L.warn('web: calibration kernel unavailable (%s), so this run has no cross-checkable price\n', e.message);
    return null;
  }
  const mod = device.createShaderModule({ code, label: 'calib' });
  const info = await mod.getCompilationInfo();
  const errs = info.messages.filter((m) => m.type === 'error');
  if (errs.length) {
    metric('calib_status', 'compile error: ' + errs[0].message.slice(0, 100));
    return null;
  }
  const buf = device.createBuffer({ label: 'calib_out', size: 256, usage: GPUBufferUsage.STORAGE });
  const pipe = device.createComputePipeline({
    label: 'pipe_calib', layout: 'auto',
    compute: { module: mod, entryPoint: 'main' },
  });
  const bind = device.createBindGroup({
    label: 'bind_calib', layout: pipe.getBindGroupLayout(0), entries: [{ binding: 0, resource: { buffer: buf } }],
  });
  const once = async () => {
    const enc = device.createCommandEncoder({ label: 'enc_calib' });
    const pass = enc.beginComputePass({ label: 'pass_calib' });
    pass.setPipeline(pipe);
    pass.setBindGroup(0, bind);
    pass.dispatchWorkgroups(4096);
    pass.end();
    const t0 = performance.now();
    device.queue.submit([enc.finish()]);
    await device.queue.onSubmittedWorkDone();
    return performance.now() - t0;
  };
  await once();                                     // warm-up: the first dispatch pays for module upload
  const samples = [];
  let capped = false;
  for (let r = 0; r < 3; ++r) {
    samples.push(await once());
    if (samples[samples.length - 1] > 2000) { capped = true; break; }
  }
  const med = samples.slice().sort((a, b) => a - b)[samples.length >> 1];
  buf.destroy();
  metric('calib_flops', CALIB_FLOPS);
  metric('calib_ms', med.toFixed(3));
  metric('calib_gflops', (CALIB_FLOPS / (med / 1000) / 1e9).toFixed(2));
  metric('calib_rounds', samples.map((x) => x.toFixed(2)).join('/'));
  metric('calib_status', capped ? 'capped after one slow round' : 'ok');
  L.log('web: calibration kernel %d FLOP: %.2f ms median of %d round(s) = %.2f GFLOP/s ' +
        '(logged with the adapter so a run is never attributed to a GPU from timing alone%s)\n',
  CALIB_FLOPS, med, samples.length, CALIB_FLOPS / (med / 1000) / 1e9,
    capped ? ', capped: this adapter is far slower than the rounds assumed' : '');
  return { ms: med, gflops: CALIB_FLOPS / (med / 1000) / 1e9, capped };
}

/**
 * Per-pass prices for this machine, measured once at start-up and printed next to the empty-dispatch
 * floor they have to be read against. Web content has no trustworthy GPU timestamps, so every number
 * here is a submit plus onSubmittedWorkDone round trip - which is why the floor is measured in the
 * same run at the same dispatch counts, and why a number near the floor is a bound, not a price.
 */
async function pricePasses() {
  const W = run.traceW, H = run.traceH;
  const denoise = true;
  renderer.resize(W, H);
  setFrame({ t: args.t, frameIndex: 1, reset: true, spp: 1, denoise, samplesForDisplay: 1,
    debugView: 0, featureWrites: true });
  const rows = [];
  const time = async (label, encode, iters) => {
    const r = await renderer.timePass(encode, iters, 3, 400);
    rows.push({ label, ms: r.med, min: r.min, capped: r.capped });
    return r;
  };
  await time('floor_empty_full_grid', (e) => renderer.emptyDispatch(e, renderer.empty.grid, renderer.grid), 3);
  await time('trace', (e) => renderer.encodeTrace(e, renderer.trace.full), 3);
  await time('denoise_temporal', (e) => renderer.encodeTemporal(e), 3);
  await time('denoise_atrous_x5', (e) => renderer.encodeAtrous(e), 3);
  await time('present', (e) => renderer.encodePresent(e), 3);
  await time('accum_copy', (e) => renderer.encodeAccumCopy(e), 3);
  const mp = (W * H) / 1e6;
  const by = Object.fromEntries(rows.map((r) => [r.label, r]));
  const net = (v) => Math.max(0, v - by.floor_empty_full_grid.ms);
  L.log('web: pass prices at %dx%d with the filter on (median of 3 rounds, next to the %.2f ms ' +
        'empty-grid dispatch floor):\n', W, H, by.floor_empty_full_grid.ms);
  for (const r of rows) {
    L.log('  %-22s %6.2f ms  (min %5.2f%s)%s\n', r.label, r.ms, r.min, r.capped ? ' capped' : '',
      r.label === 'floor_empty_full_grid' ? '' : `  net ${net(r.ms).toFixed(2)} ms`);
  }
  // Saying which of these are measurements and which are only bounds. Web content has no GPU clock, so
  // a pass costs its own work plus one CPU-GPU round trip; a pass whose net comes out near the floor of
  // an empty dispatch has no measurable work at all, and quoting it as a price - or dividing it by the
  // megapixel count - would be quoting the round trip.
  const floor = by.floor_empty_full_grid.ms;
  const bounded = rows.filter((r) => r.label !== 'floor_empty_full_grid' && net(r.ms) < 2 * floor);
  if (bounded.length) {
    L.log('  BOUND, NOT A MEASUREMENT: %s each came out under twice the %.2f ms dispatch floor, so all ' +
          'that is known about any of them is that it costs less than about %.2f ms; the controller ' +
          'learns their real prices from split-probe frames while it runs\n',
      bounded.map((r) => r.label).join(', '), floor, 2 * floor);
  }
  const filterNet = net(by.denoise_temporal.ms) + net(by.denoise_atrous_x5.ms);
  const traceMeasured = net(by.trace.ms) >= 2 * floor;
  const filterMeasured = filterNet >= 2 * floor;
  const per = {
    trace_ms_per_megapixel: +(net(by.trace.ms) / mp).toFixed(3),
    filter_ms_per_megapixel: +(filterNet / mp).toFixed(3),
    present_ms_per_megapixel: +(net(by.present.ms) / mp).toFixed(3),
    floor_ms: +floor.toFixed(3),
  };
  L.log('  filter (temporal + 5 a-trous) %.2f ms = %.2f ms per megapixel traced%s\n', filterNet,
    per.filter_ms_per_megapixel,
    filterMeasured ? '' : ' - which is the floor showing through, not a price: this machine filters ' +
      'inside one round trip at this size');
  for (const [k, v] of Object.entries(per)) metric('price_' + k, v);
  metric('price_trace_measurable', traceMeasured ? 'yes' : 'no, inside the dispatch floor');
  metric('price_filter_measurable', filterMeasured ? 'yes' : 'no, inside the dispatch floor');
  // Seed the controller with what this machine actually charges rather than the probe's NVIDIA seed:
  // a wrong seed is how a controller sat 10x over budget while believing it was inside it. Where the
  // measurement could not see the pass at all, the value stays where the probe left it - and the line
  // below says which is which, rather than printing a default as though it had been measured here.
  if (traceMeasured) {
    quality.o.traceMsPerSampleKiloPx = per.trace_ms_per_megapixel / 1000;
    quality.seed.traceMsPerSampleKiloPx = CONTROLLER_DEFAULTS.traceMsPerSampleKiloPx;
  }
  if (filterMeasured) {
    quality.o.filterMsPerMegaPx = per.filter_ms_per_megapixel;
    quality.seed.filterMsPerMegaPx = CONTROLLER_DEFAULTS.filterMsPerMegaPx;
  }
  L.log('web: controller seeded with trace %.5f ms per kilopixel per sample (%s) and filter ' +
        '%.2f ms per megapixel (%s); the probe seed was %.5f / %.2f\n',
    quality.o.traceMsPerSampleKiloPx,
    traceMeasured ? 'measured on this machine' : 'kept from the probe',
    quality.o.filterMsPerMegaPx,
    filterMeasured ? 'measured on this machine' : 'kept from the probe, unmeasurable at this size',
    CONTROLLER_DEFAULTS.traceMsPerSampleKiloPx, CONTROLLER_DEFAULTS.filterMsPerMegaPx);
  return per;
}

// --------------------------------------------------------------------------- panel-rate estimate
/**
 * The exe reads the mode off the monitor (frame.cpp:1129-1135). A page cannot: there is no display
 * API, so the rate is estimated from requestAnimationFrame intervals and marked `~` wherever it is
 * printed, and ?refresh= pins it - which is what a measurement run should use.
 */
function estimateRefresh() {
  return new Promise((resolve) => {
    if (args.refresh > 0) return resolve({ hz: args.refresh, estimated: false });
    const t = [];
    let last = 0;
    const step = (now) => {
      if (last) { const d = now - last; if (d > 0.5 && d < 100) t.push(d); }
      last = now;
      if (t.length < 60) requestAnimationFrame(step);
      else {
        const s = t.slice().sort((a, b) => a - b);
        resolve({ hz: 1000 / s[s.length >> 1], estimated: true });
      }
    };
    requestAnimationFrame(step);
    setTimeout(() => resolve({ hz: 60, estimated: true, timedOut: true }), 4000);
  });
}

// --------------------------------------------------------------------------- startup
async function boot() {
  metric('run_started', new Date().toISOString());
  metric('run_id', RUN_ID);
  metric('run_query', location.search.slice(1));
  metric('ua', navigator.userAgent.slice(0, 120));
  metric('args', JSON.stringify(args));
  if (args.help) { HELP.forEach((l) => L.log('%s\n', l)); metric('help', 'printed'); return; }
  if (typeof navigator === 'undefined' || !navigator.gpu) {
    L.fatal('navigator.gpu is absent - this browser cannot run the port');
    document.title = 'Vulkan Ray Tracer - WebGPU is unavailable';
    await deliver(true);
    return;
  }

  const adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
  if (!adapter) { L.fatal('requestAdapter() returned null'); await deliver(true); return; }
  const who = await readAdapter(adapter);
  metric('adapter_route', who.route);
  metric('features_present', ['timestamp-query', 'shader-f16', 'float32-filterable',
    'bgra8unorm-storage', 'float16', 'indirect-first-instance']
    .filter((f) => adapter.features.has(f)).join('+') || 'none');
  metric('adapter_feature_count', adapter.features.size);

  device = await adapter.requestDevice();
  device.lost.then((i) => { metric('device_lost', String(i.message).slice(0, 120)); L.fatal('device lost: %s', i.message); });
  device.addEventListener('uncapturederror', (ev) => noteGpuError((ev.error && ev.error.message)
    || (ev.reason && ev.reason.message) || String(ev)));
  if (typeof device.pushErrorHandler === 'function') {
    device.pushErrorHandler('warning', (type, msg) => L.warn('web: WebGPU warning (%s): %s\n', type, String(msg).slice(0, 200)));
  }
  L.log('rendering on: %s  [%s]\n', adapterName(adapterInfo), 'WebGPU pipeline');
  L.log('pipeline chosen by the browser: %s (a page gets one adapter and no ray-tracing pipeline: ' +
        'the trace is a compute kernel over a CPU-built BVH)\n', 'WebGPU pipeline');
  // Say it before the browser does, so the run log does not read as a fault report: on Windows Chromium
  // ignores the power preference handed to requestAdapter() (crbug.com/369219127), so this page gets
  // whatever adapter the browser picked and the 'high-performance' hint above is documentation. A run
  // on a laptop that has two GPUs therefore cannot be attributed from the flag alone, which is what the
  // adapter identity and the calibration kernel printed below are for.
  L.log('note: Chromium warns that "powerPreference is currently ignored when calling requestAdapter()" ' +
        'on Windows (crbug.com/369219127). That warning is expected on this page and is not a fault: ' +
        'the port targets Edge or Chrome, and Firefox is unsupported here for reasons measured in ' +
        'browser/README.md.\n');
  L.log('device: %d optional features enabled on it, the adapter offers %d; ' +
        'maxTextureDimension2d=%d maxComputeInvocationsPerWorkgroup=%d maxStorageBufferBindingSize=%d ' +
        'maxBufferSize=%d\n',
    device.features.size, adapter.features.size, device.limits.maxTextureDimension2D,
    device.limits.maxComputeInvocationsPerWorkgroup, device.limits.maxStorageBufferBindingSize,
    device.limits.maxBufferSize);
  // `device.limits` reads empty through Object.keys() for the same reason GPUAdapterInfo does - the
  // members are prototype getters - so the named properties above are what gets logged, never a key
  // count.
  metric('device_limits_named', JSON.stringify({
    tex2d: device.limits.maxTextureDimension2D,
    wg: device.limits.maxComputeInvocationsPerWorkgroup,
    storage: device.limits.maxStorageBufferBindingSize, buf: device.limits.maxBufferSize,
    keys: Object.keys(device.limits).length,
  }));
  L.log('step: device + queues created\n');

  // The canvas is sized before the context is taken, because the drawing buffer cannot change after
  // the context is configured without reconfiguring it, and the first frame must already be at the
  // size the title bar is going to name.
  run.dpr = args.dpr > 0 ? args.dpr : (window.devicePixelRatio || 1);
  const ext = desiredExtent();
  canvas.width = run.winW = Math.max(16, Math.round(ext.cssW * run.dpr));
  canvas.height = run.winH = Math.max(16, Math.round(ext.cssH * run.dpr));
  canvas.style.width = ext.cssW + 'px';
  canvas.style.height = ext.cssH + 'px';
  ctx = canvas.getContext('webgpu');
  renderer = await Renderer.load(device, ctx);
  renderer.ctxformat = navigator.gpu.getPreferredCanvasFormat();
  ctx.configure({ device, format: renderer.ctxformat, alphaMode: 'opaque' });
  // The exe's next log line names the sampler, so the sampler has to exist before it is printed -
  // which is why renderer.js exposes ensureSampler().
  renderer.ensureSampler();
  L.log('step: command pool, sync objects, sampler\n');

  // --- assets, in the exe's order and words
  let files = null, root = null, used = '';
  const trimJson = (s) => { const end = s.lastIndexOf('}'); return end >= 0 ? s.slice(0, end + 1) : s.trim(); };
  const applyScene = (text) => { root = JSON.parse(trimJson(text)); L.log('scene.json applied\n'); };
  const pakTry = await firstOk(args.pakUrl);
  if (pakTry.res) {
    try {
      const bytes = await pakTry.res.arrayBuffer();
      const pak = await unzip(bytes);
      files = pak.files;
      used = pakTry.url;
      L.log('pak: %s (%d entries)\n', pakTry.url, pak.count);
    } catch (e) {
      L.warn('web: pak at %s could not be read (%s)\n', pakTry.url, String(e.message).slice(0, 80));
      files = null;
    }
  }
  if (files) {
    if (files.get('scene.json')) applyScene(new TextDecoder().decode(files.get('scene.json')));
  } else {
    L.warn('web: pak unavailable (%s), trying loose assets\n',
      (pakTry.misses || ['no candidate']).join('; ').slice(0, 160));
    // The loose files, again from both layouts: the site's own assets/, or the repository's one up.
    const dirs = ASSET_DIRS.map((d) => siteUrl(d + '/'));
    const sceneTry = await firstOk(dirs.map((d) => d + 'scene.json'));
    if (!sceneTry.res) {
      L.fatal('neither %s nor %s could be read', args.pakUrl.join(', '), dirs.join(' or '));
      await deliver(true);
      return;
    }
    used = sceneTry.url;
    applyScene(await sceneTry.res.text());
    files = new Map();
    for (const f of ['textures/checker.png', 'textures/emissive_ramp.png']) {
      const t = await firstOk(dirs.map((d) => d + f));
      if (!t.res) { L.fatal('missing %s (%s)', f, (t.misses || []).join('; ')); await deliver(true); return; }
      files.set(f, new Uint8Array(await t.res.arrayBuffer()));
    }
  }
  metric('scene_source', used);
  cfg = applyJson(defaultScene(), root);
  metric('materials', cfg.materials.length);
  metric('lights', cfg.lights.length);
  metric('objects', cfg.objects.length);
  metric('max_bounces', cfg.maxBounces);
  metric('exposure', cfg.exposure);

  const bitmaps = [];
  const texLines = [];
  for (const td of cfg.textures) {
    const bytes = files.get(td.file);
    if (!bytes) { texLines.push(`  texture absent from pak (shader uses its procedural fallback): ${td.file}\n`); bitmaps.push(null); continue; }
    bitmaps.push(await createImageBitmap(new Blob([bytes], { type: 'image/png' })));
  }

  // --- geometry: the exe's mesh line, then the acceleration structure
  const meshes = buildMeshes();
  for (const [name, m] of Object.entries(meshes)) {
    L.log('  mesh %s: uploading %d verts / %d indices\n', name, m.pos.length, m.idx.length);
  }
  const world = buildWorldTriangles(cfg, meshes, args.t);
  const bvh = new BVH(world.tris, world.meta, world.count, world.lightRanges);
  renderer.bvh = bvh;
  renderer.regen = makeLightRegen(meshes, cfg);
  renderer.instRows = packInstances(computeInstances(cfg, args.t));
  const packed = new ArrayBuffer(bvh.totalTris * 64);
  renderer.triF32 = new Float32Array(packed);
  renderer.triU32 = new Uint32Array(packed);
  packTrianglesInto(bvh.tris, bvh.meta, renderer.triF32, renderer.triU32, bvh.totalTris);
  renderer.setGeometry({
    materials: packMaterials(cfg), lights: packLights(cfg, args.t), instances: renderer.instRows,
    nodes: bvh._rawNodes.subarray(0, bvh.nodeCount * 8), tris: renderer.triF32, root: bvh.root,
    occlRoot: bvh.subtreeRanges[0][0], triCount: bvh.totalTris,
  });
  metric('world_triangles', world.count);
  metric('bvh_nodes', bvh.stats.nodes);
  L.log('step: geometry and acceleration structures\n');
  let slot = 0;
  for (let i = 0; i < cfg.textures.length; ++i) {
    if (bitmaps[i]) L.log('  texture from pak: %s (%dx%d) -> slot %d\n', cfg.textures[i].file,
      bitmaps[i].width, bitmaps[i].height, slot++);
  }
  for (const l of texLines) L.log(l.replace(/\n$/, '\n'));
  renderer.setTextures(bitmaps);
  L.log('step: textures\n');
  // The exe's line is `step: %s pipeline ready` with the path name in the slot (main.cpp:287); the
  // web path's name is "WebGPU", so the sentence reads "step: WebGPU pipeline ready".
  L.log('step: %s pipeline ready\n', 'WebGPU');

  // The trace target exists from here on: renderer.resize() builds bind groups over the geometry
  // buffers, so it cannot run before setGeometry(), and the window line below names the size it made.
  applyHdrSize();
  if (args.windowSize && (Math.round(args.windowSize[0] * run.dpr) !== run.winW ||
      Math.round(args.windowSize[1] * run.dpr) !== run.winH)) {
    L.log('window: asked for %dx%d, given %dx%d\n', args.windowSize[0], args.windowSize[1],
      Math.round(run.winW / run.dpr), Math.round(run.winH / run.dpr));
  }
  L.log('window %ux%u - drag the border to resize, ESC to quit\n', run.winW, run.winH);

  // --- the controller, then the first-run banner the brief asks for
  quality = new Quality({
    auto: args.autoq && args.quality !== 'off', targetFps: args.targetFps,
    kiloPx1: (run.winW * run.winH) / 1000, minScale: args.minScale, maxScale: args.maxScale,
    maxSpp: args.maxSpp, denoise: args.denoise || debugView > 0, priority: args.priority,
    headroom: args.quality === 'balanced' ? 0.34 : args.quality === 'performance' ? 0.40 : 0.26,
  });
  quality.spp = args.spp;
  quality.scale = args.scale;
  if (args.testBudget > 0) {
    // --test-budget in the exe replaces the budget outright; budget() is
    // frameMs*(1-headroom) - cpuReserve, so headroom 0 and a reserve of frameMs - budget reproduces
    // it exactly (still above the same 1.5 ms floor).
    quality.o.headroom = 0;
    quality.o.cpuReserveMs = Math.max(0, 1000 / args.targetFps - args.testBudget);
    metric('test_budget_ms', args.testBudget);
  }
  metric('quality_mode', args.quality);
  metric('priority', args.priority);

  if (args.capture === 0) {
    const panel = await estimateRefresh();
    metric('panel_hz', panel.hz.toFixed(2));
    metric('panel_estimated', String(panel.estimated));
    const phrase = panel.estimated
      ? `display: ~${Math.round(panel.hz)} Hz panel (estimated from requestAnimationFrame: the ` +
        'browser exposes no display mode)'
      : `display: ${Math.round(panel.hz)} Hz panel (?refresh=, pinned by the caller)`;
    L.log('%s, targeting %.0f fps, vsync %s, quality mode %s\n', phrase, args.targetFps, 'on',
      args.quality);
    if (panel.estimated && panel.hz + 0.5 < args.targetFps) {
      L.log('  panel rate ~%.0f Hz is not the frame target: this run is held to %.0f fps, the rate ' +
            'that was asked for (see effectiveFps)\n', panel.hz, args.targetFps);
    }
    await pricePasses();
    run.appliedScale = quality.scale;
  }
  if (args.calib) await runCalibration();
  L.log('web: adapter identity for this run: %s (source %s, isFallbackAdapter=%s)\n',
    JSON.stringify(adapterInfo).slice(0, 220), who.route, String(who.fallback));

  L.log('quality start: denoise %s, scale %.2f, spp %d, filter %.2f ms per megapixel\n',
    quality.denoise ? 'on' : 'off', quality.scale, quality.spp, quality.o.filterMsPerMegaPx);
  document.title = exeTitle({
    fps: 0, winW: run.winW, winH: run.winH, traceW: run.traceW, traceH: run.traceH,
    spp: quality.spp, denoise: quality.denoise, freeLook: false, autoQuality: quality.o.auto,
  });
  updateStatus();
  await deliver(false);

  api();
  if (args.capture > 0) {
    run.winW = args.size[0]; run.winH = args.size[1];
    await capture();
    renderer.drawToCanvas();
    await finish('capture complete');
    return;
  }
  L.log('web: the window is live - the canvas is the whole interface, and this console is the log\n');
  requestAnimationFrame(function tick() {
    if (run.quit) return;
    step(performance.now()).then((done) => { if (!done && !run.quit) requestAnimationFrame(tick); },
      (e) => { L.fatal('frame loop: %s', String(e && e.stack || e)); finish('frame loop threw'); });
  });
}

// --------------------------------------------------------------------------- the driver's handle
// Not UI: nothing here is visible. It is how a scripted run proves things instead of being told
// them - readback hashes, state, a resize, an evidence pair for png_check.py.
function api() {
  window.__ray = {
    RUN_ID,
    args,
    get ready() { return true; },
    state: () => ({
      frame: run.frameIndex, quit: run.quit, title: document.title, fps: +run.fps.toFixed(1),
      winW: run.winW, winH: run.winH, traceW: run.traceW, traceH: run.traceH, dpr: run.dpr,
      simTime: +run.simTime.toFixed(3), freeCam: run.freeCam, blend: +run.camBlend.toFixed(3),
      paused: run.paused, frozen: run.frozen, accum: run.samplesAccumulated,
      camPos: run.camPos.map((v) => +v.toFixed(3)), yaw: +run.yaw.toFixed(4),
      pitch: +run.pitch.toFixed(4), pointerLocked: run.pointerLocked,
      // What the eye actually sees, as opposed to the free camera's own numbers above: the basis
      // writeParams handed to the GPU for the frame just drawn, after the orbit/free blend (app.js
      // setFrame stores it in renderer.prevBasis). `orbitPhase` is the orbit angle that produced it.
      view: renderer && renderer.prevBasis ? {
        origin: renderer.prevBasis.origin.map((v) => +v.toFixed(5)),
        forward: renderer.prevBasis.forward.map((v) => +v.toFixed(5)),
      } : null,
      orbitPhase: +((run.simTime * cfg.camera.speed) % (Math.PI * 2)).toFixed(5),
    lockRefused: run.lockRefused, mouseLookMode: args.mouseLook,
      denoise: quality.denoise, denoiseUserToggled: run.denoiseUserToggled,
      spp: liveSpp(), sppUserToggled: run.sppUserToggled, sppMax: sppMax(),
      overlay: statusEl ? statusEl.textContent : '', scale: +quality.scale.toFixed(3), controller: quality.state(),
      gpuErrors: gpuErrors.n, consoleErrors: logCounts.error,
      canvasCopy: { emptyCopies: canvasCopy.emptyCopies, retries: canvasCopy.retries,
        errors: canvasCopy.errors.length, last: canvasCopy.errors.slice(-2) },
      canvasReadbacks: evidence.hashes.length,
      hashes: evidence.hashes.slice(-8),
      fullscreen: !!document.fullscreenElement,
      calib: results.get('calib_ms') ? { ms: +results.get('calib_ms'),
        gflops: +results.get('calib_gflops') } : null,
      adapter: adapterInfo,
      verdict: gpuErrors.n === 0 && logCounts.error === 0 ? 'clean' : 'NOT_CLEAN',
    }),
    /**
     * SHA-256 of the <canvas> as displayed, plus a probe pixel. Each call is recorded in
     * state().hashes, so "three successive frames differ" is a property of the readback history the
     * driver built, not of anything the page chose to mention.
     */
    canvas: async (px = 640, py = 360) => {
      const c = await canvasBytes();
      if (!c) return { error: canvasCopy.errors.join(' | ') || evidence.errors.join(' | ')
        || 'unavailable' };
      const i = (py * c.w + px) * 4;
      const hash = await sha256Hex(c.rgba);
      evidence.hashes.push({ frame: run.frameIndex, canvas: hash, w: c.w, h: c.h,
        at: [px, py], ts: Math.round(performance.now()) });
      return { w: c.w, h: c.h, hash, at: [px, py],
        px: [c.rgba[i], c.rgba[i + 1], c.rgba[i + 2]],
        meanLuma: +meanLuma(c.rgba).toFixed(2),
        emptyCopiesBeforeThisOne: canvasCopy.emptyCopies };
    },
    /** Mean luma and a probe pixel for the canvas and the presentation texture, in one call. */
    canvasStats: async (px, py) => {
      const c = await canvasBytes();
      const p = await renderer.readPresent();
      const x = px === undefined ? (c ? c.w >> 1 : 0) : px;
      const y = py === undefined ? (c ? c.h >> 1 : 0) : py;
      const i = (y * c.w + x) * 4;
      return {
        meanLuma: c ? +meanLuma(c.rgba).toFixed(2) : null,
        distinct: c ? new Set(Array.from({ length: Math.min(4000, c.rgba.length / 4) },
          (_, k) => (c.rgba[k * 4] << 16) | (c.rgba[k * 4 + 1] << 8) | c.rgba[k * 4 + 2])).size : null,
        px: c ? [c.rgba[i], c.rgba[i + 1], c.rgba[i + 2]] : null,
        presentMeanLuma: +meanLuma(p).toFixed(2),
        canvasBytesAvailable: !!c,
        emptyCanvasCopies: canvasCopy.emptyCopies,
      };
    },
    /** SHA-256 of the presentation texture the blit samples, at the same instant. */
    present: async () => {
      const b = await renderer.readPresent();
      return { w: renderer.W, h: renderer.H, hash: await sha256Hex(b) };
    },
    /**
     * Upload the pair web/png_check.py --pair checks, plus the still if one was rendered. The reads
     * happen inside the next rendered frame (see wantSave in step()), so the two files describe one
     * frame rather than two moments.
     */
    save: async (name = 'evidence') => {
      run.saveResult = null;
      run.wantSave = name;
      for (let i = 0; i < 200 && !run.saveResult && !run.quit; ++i) {
        await new Promise((r) => setTimeout(r, 50));
      }
      return run.saveResult || { error: 'no frame completed within 10 s to take the pair in' };
    },
    /** Force a size, the way dragging a border would; the resize path is tested, not assumed. */
    setWindow: async (w, h) => {
      args.windowSize = [Math.max(16, w | 0), Math.max(16, h | 0)];
      run.resizePending = true;
      return args.windowSize;
    },
    toggleDenoise, quit: () => quit('api'), fullscreen: toggleFullscreen,
    /** The P key: scene clock stopped, accumulation continuing. */
    pause: (on) => setPaused(!!on, 'api'),
    /** Stop the frame loop outright - the driver's own knob, not the key. README 10.1. */
    freeze: (on) => { run.frozen = !!on; },
    samples: (n) => {
      if (n === undefined) return liveSpp();
      // Same authority as the keys: setting it here is the user setting it.
      quality.sppUser = run.sppUserToggled = true;
      quality.spp = cfg.samplesPerFrame = clamp(Math.round(n), SPP_MIN, sppMax());
      updateTitle(true);
      return liveSpp();
    },
    statusText: () => (statusEl ? statusEl.textContent : ''),
    overlayVisible: () => (statusEl
      ? getComputedStyle(statusEl).display !== 'none' && getComputedStyle(statusEl).opacity !== '0'
      : false),
    still: () => (run.still ? { name: run.still.name, w: run.still.w, h: run.still.h,
      t: run.still.t } : null),
    results: () => Object.fromEntries(results),
  };
}

// --------------------------------------------------------------------------- go
boot().catch((e) => {
  L.fatal('start-up: %s', String(e && e.stack || e));
  document.title = 'Vulkan Ray Tracer - failed to start';
  metric('boot_error', String(e && e.message || e).slice(0, 200));
  metric('run_verdict', 'NOT_CLEAN');
  deliver(true);
});
