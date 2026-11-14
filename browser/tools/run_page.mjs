// tools/run_page.mjs - drive the page in a real headless browser and take the evidence from
// outside it.
//
// Why a driver at all: the page's own console lines are claims. "frames advanced", "the denoiser
// flipped", "the frame counter froze" are all things a page can print while drawing nothing, and
// this project has been burned by exactly that (docs/web-port-notes.md sections 3, 9.7, 10, 11). So
// this driver:
//
//   1. starts Edge headless with a throwaway profile and --remote-debugging-port,
//   2. talks DevTools protocol over a plain WebSocket (Node's built-in client - no npm, no npm
//      install, nothing to vendor),
//   3. captures the page's console.log with Runtime.consoleAPICalled into a file, so the log the
//      brief asks for is the log that was actually produced,
//   4. dispatches *browser-side* input events (Input.dispatchKeyEvent / dispatchMouseEvent), which
//      are trusted events - the same class of event a keyboard produces - rather than a JS-created
//      KeyboardEvent the page's own handlers would accept without the browser being involved,
//   5. reads state and pixel readbacks back through Runtime.evaluate, and prints a JSON verdict.
//
// Nothing is killed except processes carrying this run's profile directory, so a browser window the
// user already had open is never touched.
//
//   node tools/run_page.mjs --scenario smoke --logFile C:\...\console.log
//   node tools/run_page.mjs --scenario keys  --timeout 180
//   node tools/run_page.mjs --scenario resize --query '?dpr=1'
//   node tools/run_page.mjs --scenario still --query '?capture=1&accum=48&size=1280x720&cmp=1'
//   node tools/run_page.mjs --scenario features --evidence C:\...\browser\evidence
//   node tools/run_page.mjs --scenario pin --query '?targetfps=8&denoise=0&dpr=1'

import { spawn } from 'node:child_process';
import { writeFileSync, appendFileSync, existsSync, mkdirSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { decodePng, diffRegion, meanLuma } from './png_read.mjs';

const argv = process.argv.slice(2);
const opt = (k, d) => {
  const i = argv.indexOf(`--${k}`);
  return i >= 0 && i + 1 < argv.length ? argv[i + 1] : d;
};
const has = (k) => argv.includes(`--${k}`);

const EDGE = opt('browser', 'C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe');
const PORT = Number(opt('cdpPort', 9333));
const SCENARIO = opt('scenario', 'smoke');
const QUERY = opt('query', '');
const PAGE = opt('page', `http://127.0.0.1:${opt('serverPort', 8731)}/browser/index.html${QUERY}`);
const TIMEOUT = Number(opt('timeout', 240)) * 1000;
const LOG = opt('logFile', join(tmpdir(), `ray-page-console-${SCENARIO}.log`));
const OUT = opt('out', join(tmpdir(), `ray-page-result-${SCENARIO}.json`));
// The flag set web/run_demo.ps1 uses and that this project has measured: adding --use-angle=d3d11
// here changed behaviour (the canvas copy came back empty every time), so the defaults stay on the
// browser's own choice and any flag experiment is said out loud in the log.
const FLAGS = opt('flags', '--headless=new,--enable-unsafe-webgpu,--no-sandbox')
  .split(',').filter(Boolean);
const WIN = opt('window', '1280,760');

const profile = join(tmpdir(), `ray-html-profile-${Date.now().toString(36)}`);
mkdirSync(profile, { recursive: true });
writeFileSync(LOG, `=== run_page.mjs ${new Date().toISOString()} scenario=${SCENARIO}\n` +
  `url=${PAGE}\nflags=${FLAGS.join(' ')}\nprofile=${profile}\n\n`);

const say = (m) => {
  const line = `[driver ${new Date().toISOString().slice(11, 19)}] ${m}`;
  process.stdout.write(line + '\n');
  appendFileSync(LOG, line + '\n');
};

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const pids = [];

let browser = null;
let ws = null;
let nextId = 1;
const pending = new Map();
const consoleLines = [];

function send(method, params = {}) {
  const id = nextId++;
  return new Promise((resolve, reject) => {
    pending.set(id, { resolve, reject });
    ws.send(JSON.stringify({ id, method, params }));
  });
}

/**
 * Which page target is ours. Matching only on "index.html" appearing in the URL is not enough for a
 * site reached through a path prefix as a directory - http://host/strata_raytracer/, which is exactly
 * the shape GitHub Pages gives - because that URL names no file at all. So the URL the driver launched
 * is compared properly first, and the old loose match stays as the fallback it always was.
 */
const wantPage = (() => {
  try { const u = new URL(PAGE); return u.origin + u.pathname; } catch { return ''; }
})();
const isOurPage = (u) => {
  try { return wantPage !== '' && new URL(u).origin + new URL(u).pathname === wantPage; }
  catch { return false; }
};

async function attach() {
  for (let i = 0; i < 120; i++) {
    try {
      const res = await fetch(`http://127.0.0.1:${PORT}/json/list`);
      const list = await res.json();
      const page = list.find((t) => t.type === 'page' && isOurPage(t.url))
        || list.find((t) => t.type === 'page' && /index\.html/.test(t.url));
      if (page && page.webSocketDebuggerUrl) return page.webSocketDebuggerUrl;
    } catch { /* not up yet */ }
    await sleep(250);
  }
  throw new Error('DevTools endpoint never appeared');
}

function connect(url) {
  return new Promise((resolve, reject) => {
    const s = new WebSocket(url);
    s.onopen = () => { ws = s; resolve(); };
    s.onerror = (e) => reject(new Error(`websocket: ${e.message || 'error'}`));
    s.onmessage = (ev) => {
      let m;
      try { m = JSON.parse(ev.data); } catch { return; }
      if (m.id && pending.has(m.id)) {
        const p = pending.get(m.id);
        pending.delete(m.id);
        if (m.error) p.reject(new Error(`${m.error.message} (${m.error.code})`));
        else p.resolve(m.result);
        return;
      }
      if (m.method === 'Runtime.consoleAPICalled') {
        const p = m.params || {};
        const text = (p.args || []).map((a) => (a.value !== undefined ? String(a.value)
          : (a.description || a.type))).join(' ');
        const stamp = new Date().toISOString().slice(11, 23);
        const line = `${stamp} ${String(p.type || 'log').toUpperCase().padEnd(5)} ${text}`;
        consoleLines.push({ type: p.type || 'log', text });
        appendFileSync(LOG, line + '\n');
      } else if (m.method === 'Runtime.exceptionThrown') {
        const d = m.params.exceptionDetails;
        const text = d.exception?.description || d.text || 'exception';
        appendFileSync(LOG, `EXCEPTION ${text}\n`);
        consoleLines.push({ type: 'exception', text });
      } else if (m.method === 'Log.entryAdded') {
        const e = m.params.entry;
        appendFileSync(LOG, `BROWSER ${e.level.toUpperCase().padEnd(5)} ${e.text}\n`);
      }
    };
    s.onclose = () => { for (const p of pending.values()) p.reject(new Error('websocket closed')); };
  });
}

const evaluate = async (expr) => {
  const r = await send('Runtime.evaluate', { expression: expr, awaitPromise: true,
    returnByValue: true });
  if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description || 'eval threw');
  return r.result.value;
};

/**
 * A trusted key event, the way the browser sees one. `hold` keeps it down for that long, which
 * matters: the app polls the key set per frame exactly as the exe polls glfwGetKey, so a key pressed
 * and released inside one frame moves nothing, and a test that "WASD moves the camera" has to hold
 * the key across frames like a hand does.
 */
async function key(code, { key, vk, modifiers = 0, text, hold = 0 } = {}) {
  const base = { code, key: key || code, windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk, modifiers };
  await send('Input.dispatchKeyEvent', { type: 'rawKeyDown', ...base });
  if (text) await send('Input.dispatchKeyEvent', { type: 'char', ...base, text });
  if (hold) await sleep(hold);
  await send('Input.dispatchKeyEvent', { type: 'keyUp', ...base });
}

async function waitFor(expr, ms, what, interval = 300) {
  const t0 = Date.now();
  let last = null;
  while (Date.now() - t0 < ms) {
    try {
      last = await evaluate(expr);
      if (last) return last;
    } catch (e) { last = String(e.message); }
    await sleep(interval);
  }
  throw new Error(`timed out waiting for ${what} (last: ${JSON.stringify(last)})`);
}

const results = { scenario: SCENARIO, url: PAGE, checks: [], notes: [] };
const check = (name, ok, detail) => {
  results.checks.push({ name, ok: !!ok, detail });
  say(`${ok ? 'PASS' : 'FAIL'}  ${name}${detail === undefined ? '' : `  ${detail}`}`);
};

// --------------------------------------------------------------------------- scenarios
async function scenarioSmoke() {
  await waitFor('window.__ray && window.__ray.ready', 120000, 'window.__ray');
  const s = await evaluate('window.__ray.state()');
  results.state0 = s;
  say(`adapter=${JSON.stringify(s.adapter)} frames=${s.frame} title="${s.title}"`);
  const hashes = [];
  for (let i = 0; i < 4; i++) {
    await sleep(700);
    // The page already retries an empty canvas copy; the driver also repeats the whole call, and
    // records how many attempts the evidence needed instead of quietly accepting the first one.
    let h = null;
    let tries = 0;
    for (; tries < 3; tries++) {
      h = await evaluate('window.__ray.canvas(640, 360)');
      if (h && !h.error && h.meanLuma > 0) break;
      await sleep(500);
    }
    h.driverTries = tries + 1;
    hashes.push(h);
    say(`  canvas ${hashes.length}: ${JSON.stringify(h)}`);
  }
  results.canvasHashes = hashes;
  const present = await evaluate('window.__ray.present()');
  results.present = present;
  const distinct = new Set(hashes.map((h) => h.hash)).size;
  check('three successive canvas readbacks differ', distinct >= 3,
    `${distinct} distinct of ${hashes.length}`);
  check('canvas carries a lit pixel at (640,360)', hashes[0].px && hashes[0].px.some((v) => v > 12),
    JSON.stringify(hashes[0].px));
  const saved = await evaluate(`window.__ray.save('smoke')`);
  results.saved = saved;
  say('saved ' + JSON.stringify(saved));
  const st = await evaluate('window.__ray.state()');
  const pair = await evaluate('window.__ray.canvasStats()');
  results.pair = pair;
  results.stateEnd = st;
  check('no WebGPU errors', st.gpuErrors === 0, `gpuErrors=${st.gpuErrors}`);
  check('the adapter is named, so this machine really ran on a GPU',
    !!(st.adapter && st.adapter.vendor), JSON.stringify(st.adapter));
  check('no fatal console lines', st.consoleErrors === 0, `consoleErrors=${st.consoleErrors}`);
  check('the canvas and the presentation texture agree on brightness',
    pair.meanLuma !== null && Math.abs(pair.meanLuma - pair.presentMeanLuma) < 3,
    `canvas ${pair.meanLuma} vs presentation texture ${pair.presentMeanLuma} ` +
    `(empty canvas copies: ${pair.emptyCanvasCopies})`);
  check('the canvas copy worked at all', pair.canvasBytesAvailable === true,
    JSON.stringify(pair));
  check('the frame advanced between the first and last readback', st.frame > s.frame,
    `${s.frame} -> ${st.frame}`);
  check('title carries the exe format', /Vulkan Ray Tracer - \d+ fps - \d+x\d+ trace \d+x\d+ - \d+ spp - WebGPU pipeline - \(N\) Denoise (ON|OFF) - \(ESC\) Quit/.test(st.title), st.title);
}

/** Did the page actually print this sentence? Searched in the captured console, not in a flag. */
const sawText = (sub) => consoleLines.some((l) => l.text.includes(sub));

async function scenarioKeys() {
  await waitFor('window.__ray && window.__ray.ready', 120000, 'window.__ray');
  const before = await evaluate('window.__ray.state()');
  const beforeCanvas = await evaluate('window.__ray.canvas(700, 420)');
  results.before = { state: before, canvas: beforeCanvas };

  // --- N flips the denoiser, the title, and prints the exe's sentence
  await key('KeyN', { key: 'n', vk: 78, text: 'n' });
  await sleep(400);
  const afterN = await evaluate('window.__ray.state()');
  check('N flips the denoiser', afterN.denoise !== before.denoise,
    `${before.denoise} -> ${afterN.denoise}`);
  check('N makes the user own the switch', afterN.denoiseUserToggled === true,
    `denoiseUserToggled=${afterN.denoiseUserToggled}`);
  check('the title says which way the denoiser is',
    /Denoise ON/.test(afterN.title) === afterN.denoise, afterN.title);
  check('and the page printed the exe\'s line', sawText('denoise ON (pressed N)')
    || sawText('denoise OFF (pressed N)'), 'searched the console');
  await key('KeyN', { key: 'n', vk: 78, text: 'n' });      // back to where it started
  await sleep(400);

  // --- WASD moves the camera: held across frames, as a hand holds it
  await key('KeyW', { key: 'w', vk: 87, text: 'w', hold: 1500 });
  await sleep(600);
  const moved = await evaluate('window.__ray.state()');
  const movedCanvas = await evaluate('window.__ray.canvas(700, 420)');
  results.afterW = { state: moved, canvas: movedCanvas };
  check('W enters free look', moved.freeCam === true, `freeCam=${moved.freeCam} blend=${moved.blend}`);
  check('W changes the camera position',
    Math.hypot(moved.camPos[0] - before.camPos[0], moved.camPos[2] - before.camPos[2]) > 0.05,
    `${JSON.stringify(before.camPos)} -> ${JSON.stringify(moved.camPos)}`);
  check('the frame stops matching the previous one', movedCanvas.hash !== beforeCanvas.hash,
    `${beforeCanvas.hash.slice(0, 12)} -> ${movedCanvas.hash.slice(0, 12)}`);
  const pxChanged = movedCanvas.px && beforeCanvas.px
    && movedCanvas.px.some((v, i) => Math.abs(v - beforeCanvas.px[i]) > 6);
  check('a fixed pixel changes', pxChanged,
    `${JSON.stringify(beforeCanvas.px)} -> ${JSON.stringify(movedCanvas.px)}`);
  check('the page printed the exe\'s free-look line',
    sawText('camera: free look (WASD to move, mouse to look; idle 10 s hands control back)'),
    'searched the console');

  // --- the ten-second idle hand-back
  await waitFor('window.__ray.state().freeCam === false ? true : null', 20000,
    'the camera to be handed back after idling', 1000);
  const handed = await evaluate('window.__ray.state()');
  results.handedBack = handed;
  check('idling 10 s hands control back to the orbit', handed.freeCam === false
    && handed.blend < 0.5, `freeCam=${handed.freeCam} blend=${handed.blend}`);
  check('and the page printed the exe\'s line for it',
    sawText('camera: idle for 10 s, easing back to the orbit'), 'searched the console');

  // --- ESC stops rendering: the frame counter freezes and the title says quit. This runs before any
  // fullscreen test on purpose: while the browser itself is in fullscreen it consumes Escape to leave
  // fullscreen and does not deliver the key to the page, which is the browser's rule, not the app's.
  const pre = await evaluate('window.__ray.state()');
  await key('Escape', { key: 'Escape', vk: 27 });
  await sleep(1500);
  const post = await evaluate('window.__ray.state()');
  const again = await evaluate('window.__ray.state()');
  results.escape = { pre: pre.frame, post: post.frame, again: again.frame, title: post.title };
  check('ESC freezes the frame counter', post.frame === again.frame && post.quit === true,
    `frame ${pre.frame} -> ${post.frame} -> ${again.frame}, quit=${post.quit}`);
  check('the page titles itself as quit', /quit/.test(post.title), post.title);
  check('and the frame counter is still frozen a second later', again.frame === post.frame,
    `${again.frame} vs ${post.frame}`);
}

/**
 * Alt+Enter on its own, in a separate run: the browser's own Escape handling would otherwise be in
 * the way, and the answer this records is what the browser permitted rather than what the app asked
 * for. Headless Edge reports a virtual 800x600 screen, which is also why the size in the log line is
 * worth printing instead of inferring.
 */
async function scenarioFullscreen() {
  await waitFor('window.__ray && window.__ray.ready', 120000, 'window.__ray');
  const before = await evaluate('window.__ray.state()');
  await send('Input.dispatchKeyEvent', { type: 'rawKeyDown', code: 'Enter', key: 'Enter',
    windowsVirtualKeyCode: 13, nativeVirtualKeyCode: 13, modifiers: 1 });
  await sleep(1500);
  await send('Input.dispatchKeyEvent', { type: 'keyUp', code: 'Enter', key: 'Enter',
    windowsVirtualKeyCode: 13, nativeVirtualKeyCode: 13, modifiers: 1 });
  await sleep(1200);
  const mid = await evaluate('window.__ray.state()');
  const metric = await evaluate('(window.__ray.results().fullscreen || "absent")');
  results.fullscreen = { before: { w: before.winW, h: before.winH, frame: before.frame },
    metric, element: mid.fullscreen, after: { w: mid.winW, h: mid.winH, frame: mid.frame } };
  check('Alt+Enter reached the page and the browser answered', metric !== 'absent',
    `fullscreen=${metric} document.fullscreenElement=${mid.fullscreen}`);
  check('the page printed what the browser permitted',
    sawText('window: fullscreen') || sawText('Alt+Enter asked the browser'), 'searched the console');
  check('frames keep advancing through the transition', mid.frame > before.frame,
    `${before.frame} -> ${mid.frame}, window ${before.winW}x${before.winH} -> ${mid.winW}x${mid.winH}`);
}

async function scenarioResize() {
  await waitFor('window.__ray && window.__ray.ready', 120000, 'window.__ray');
  const a0 = await evaluate('window.__ray.canvas(640, 360)');
  results.before = a0;
  const targets = [[1024, 640], [700, 500], [1280, 720]];
  for (const [w, h] of targets) {
    await evaluate(`window.__ray.setWindow(${w}, ${h})`);
    await sleep(1600);
    const st = await evaluate('window.__ray.state()');
    const c = await evaluate(`window.__ray.canvas(${Math.round(w / 2)}, ${Math.round(h / 2)})`);
    results['resize_' + w + 'x' + h] = { state: st, canvas: c };
    check(`resize to ${w}x${h} changes the canvas`, c.w === w * st.dpr && c.h === h * st.dpr,
      `canvas ${c.w}x${c.h}, state ${st.winW}x${st.winH}, dpr ${st.dpr}`);
    check(`the frame after resizing ${w}x${h} is not the frame before it`, c.hash !== a0.hash,
      `${a0.hash.slice(0, 12)} -> ${c.hash.slice(0, 12)}`);
    check(`title names the new window size ${w * st.dpr}x${h * st.dpr}`,
      st.title.includes(`${w * st.dpr}x${h * st.dpr}`), st.title);
    const saved = await evaluate(`window.__ray.save('resize_${w}x${h}')`);
    results['saved_' + w + 'x' + h] = saved;
  }
}

async function scenarioStill() {
  await waitFor('window.__ray && window.__ray.ready', 240000, 'window.__ray');
  const still = await waitFor('window.__ray.still()', 240000, 'a captured still', 2000);
  results.still = still;
  const r = await evaluate('window.__ray.results()');
  results.results = r;
  const saved = await evaluate(`window.__ray.save('still')`);
  results.saved = saved;
  check('the still was written', !!still && !!still.name, JSON.stringify(still));
  const stillDone = Object.keys(r).some((k) => /^cmp_(still|depth|albedo|normal)_/.test(k));
  if (r.cmp_verdict === 'skipped') {
    // The reference image is not in the published site, so ?cmp=1 has to say that and move on. What
    // must NOT happen is this counting as a pass, or as a crash.
    check('the still comparison reported itself skipped (no reference on this host)', true,
      r.cmp_skipped || 'skipped');
  } else {
    check('the criteria comparison ran', stillDone,
      Object.keys(r).filter((k) => k.startsWith('cmp_') || k.startsWith('pass_')).join(','));
  }
  const chan = r.cmp_channel || 'still';
  const verdictKey = 'correctness_verdict_' + chan;
  if (r[verdictKey]) {
    check(`the ${chan} still meets the criteria fixed in README 9.3`,
      r[verdictKey] === 'pipeline_matches_native',
      `${r[verdictKey]} (failures=${r['correctness_failures_' + chan]})`);
  }
  check('the comparison left no correctness failures behind',
    r.cmp_verdict === 'skipped' || Number(r['correctness_failures_' + (r.cmp_channel || 'still')]) === 0,
    r.cmp_verdict === 'skipped' ? 'comparison skipped: nothing measured, nothing claimed'
      : `correctness_failures_${chan}=${r['correctness_failures_' + chan]}`);
}

async function scenarioAdaptive() {
  await waitFor('window.__ray && window.__ray.ready', 120000, 'window.__ray');
  await waitFor('window.__ray.state().frame > 72 ? true : null',
    Number(opt('settleSec', 120)) * 1000, '72 frames (six decision windows)');
  const s = await evaluate('window.__ray.state()');
  results.at72 = s;
  const trace = Object.entries(await evaluate('window.__ray.results()'))
    .filter(([k]) => k.startsWith('decision_'));
  say(`decision trace: ${trace.map(([, v]) => v).join(' | ') || 'none'}`);
  check('the controller made decisions', trace.length > 0, `${trace.length} decision(s)`);
  check('the run misses the target it was given',
    s.controller.exhausted === true || s.controller.withinBudget === false,
    `exhausted=${s.controller.exhausted} withinBudget=${s.controller.withinBudget} ` +
    `avg=${s.controller.avgMs} ms against a ${s.controller.budgetMs} ms budget`);

  // The resulting stable state: nothing moves over a window of frames afterwards.
  const a = await evaluate('window.__ray.state()');
  await sleep(6000);
  const b = await evaluate('window.__ray.state()');
  results.stable = { a: a.controller, b: b.controller };
  const same = a.controller.spp === b.controller.spp && a.controller.scale === b.controller.scale
    && a.controller.denoise === b.controller.denoise;
  check('and it settles: spp, scale and denoiser unchanged over 6 s',
    same && b.frame > a.frame, `${JSON.stringify(a.controller)} vs ${JSON.stringify(b.controller)}`);

  const saved = await evaluate(`window.__ray.save('adaptive')`);
  results.saved = saved;
  check('adapter named for this run', !!(s.adapter && s.adapter.vendor), JSON.stringify(s.adapter));
  check('and the calibration price is recorded with it', !!s.calib, JSON.stringify(s.calib));
}

/**
 * A readback pattern probe: take the canvas eight times with known gaps and report what each one
 * holds. Used to tell "the canvas is black" from "the canvas copy sometimes grabs a buffer that has
 * just been handed to the compositor" - which matters, because the evidence path has to be the
 * reliable kind.
 */
async function scenarioProbe() {
  await waitFor('window.__ray && window.__ray.ready', 120000, 'window.__ray');
  const rows = [];
  for (let i = 0; i < 10; i++) {
    const r = await evaluate(`(async () => {
      const c = await window.__ray.canvas(623, 308);
      const s = window.__ray.state();
      if (c.error) return { i: ${i}, error: c.error, frame: s.frame };
      const stats = await window.__ray.canvasStats();
      return { i: ${i}, frame: s.frame, w: c.w, h: c.h, hash: c.hash.slice(0, 12),
        mean: stats.meanLuma, distinct: stats.distinct, px: stats.px };
    })()`);
    rows.push(r);
    say(`  ${JSON.stringify(r)}`);
    await sleep(Number(opt('gap', 500)));
  }
  results.probe = rows;
  const ok = rows.filter((r) => !r.error && r.mean > 8).length;
  check('most readbacks show the rendered frame', ok >= rows.length - 2, `${ok}/${rows.length}`);
}

/**
 * Mouse look, measured against the desktop app's own gain rather than just "something changed".
 * frame.cpp:1281-1295 re-centres the cursor each frame and applies the offset at 0.0022 per pixel with
 * pitch clamped to +/-1.45; a page cannot warp the cursor, so it gets the same displacement as
 * movement deltas. Headless Edge refuses pointer lock, which is itself the answer worth recording:
 * the fallback is the route a page inside a cross-origin iframe takes too.
 */
async function scenarioMouse() {
  await waitFor('window.__ray && window.__ray.ready', 120000, 'window.__ray');
  await key('KeyW', { key: 'w', vk: 87, text: 'w', hold: 900 });
  const before = await evaluate('window.__ray.state()');
  const beforeCanvas = await evaluate('window.__ray.canvas(700, 420)');
  results.lockState = { pointerLocked: before.pointerLocked, refused: before.lockRefused,
    mode: before.mouseLookMode };

  const move = (x, y) => send('Input.dispatchMouseEvent',
    { type: 'mouseMoved', x, y, buttons: 0 });
  await move(600, 300);                       // priming move: establishes where the cursor is
  await sleep(150);
  const primed = await evaluate('window.__ray.state()');
  await move(700, 340);                       // 100 px right, 40 px down
  await sleep(300);
  await move(560, 250);                       // then 140 px left, 90 px up
  await sleep(400);
  const after = await evaluate('window.__ray.state()');
  const dyaw = after.yaw - primed.yaw, dpitch = after.pitch - primed.pitch;
  const wantYaw = -(100 - 140) * 0.0022, wantPitch = -(40 - 90) * 0.0022;
  results.mouse = { primed: { yaw: primed.yaw, pitch: primed.pitch },
    after: { yaw: after.yaw, pitch: after.pitch }, dyaw, dpitch, wantYaw, wantPitch };
  // What "correct" means depends on the route the page took, so the run states it and the checks follow.
  // strict with no lock is the one combination where the right answer is that the mouse does nothing.
  const shouldLook = primed.pointerLocked || (primed.mouseLookMode === 'auto' && primed.lockRefused)
    || primed.mouseLookMode === 'always';
  check('the mouse was in free look when it moved', primed.freeCam === true,
    `freeCam=${primed.freeCam}`);
  if (shouldLook) {
    check('moving the mouse turns the camera', Math.abs(dyaw) > 1e-4, `yaw ${dyaw.toFixed(5)} rad`);
    check('at the desktop app\'s gain of 0.0022 per pixel',
      Math.abs(dyaw - wantYaw) < Math.abs(wantYaw) * 0.25,
      `turned ${dyaw.toFixed(5)} rad for 40 px net, the exe would turn ${wantYaw.toFixed(5)}`);
    check('and the same gain vertically',
      Math.abs(dpitch - wantPitch) < Math.abs(wantPitch) * 0.25,
      `pitched ${dpitch.toFixed(5)} rad, the exe would pitch ${wantPitch.toFixed(5)}`);
    check('with the pitch clamp the exe applies', Math.abs(after.pitch) <= 1.45,
      `pitch ${after.pitch}`);
  } else {
    check('this route looks only with the lock, and the mouse did not turn the camera',
      Math.abs(dyaw) < 1e-9 && Math.abs(dpitch) < 1e-9,
      `mode=${primed.mouseLookMode} locked=${primed.pointerLocked} dyaw=${dyaw.toFixed(6)}, ` +
      'which is what ?mouselook=strict asks for when the lock is not held');
  }
  check('and the picture kept rendering through it', after.frame > primed.frame,
    `frame ${primed.frame} -> ${after.frame}, canvas pixel before ${JSON.stringify(beforeCanvas.px)}`);
  check('and the page recorded which pointer-lock route it took',
    typeof primed.mouseLookMode === 'string',
    `mode=${primed.mouseLookMode} locked=${primed.pointerLocked} refused=${primed.lockRefused} ` +
    `(a refusal is what a page in a cross-origin iframe sees)`);
}

/**
 * The three additions the brief asked for, asserted by what changes on screen rather than by what the
 * handler printed: P (scene clock frozen, accumulation climbing, then resumed), Up/Down (samples move,
 * clamp at both ends), and the status overlay (proved by pixel readback of two full-page screenshots
 * taken outside the browser, plus a plain click still taking pointer lock afterwards).
 */
async function scenarioFeatures() {
  await waitFor('window.__ray && window.__ray.ready', 120000, 'window.__ray');
  const dir = opt('evidence', join(process.cwd(), 'evidence'));
  mkdirSync(dir, { recursive: true });
  const shot = async (name) => {
    const r = await send('Page.captureScreenshot', { format: 'png' });
    const file = join(dir, name);
    writeFileSync(file, Buffer.from(r.data, 'base64'));
    return file;
  };

  // ---- the overlay: what it says, and whether the compositor is actually drawing it top-left.
  const style = await evaluate(`(() => { const e = document.getElementById('status');
    const c = getComputedStyle(e); const v = document.getElementById('view').getBoundingClientRect();
    return { text: e.textContent, position: c.position, pointerEvents: c.pointerEvents,
      fontFamily: c.fontFamily, shadow: c.textShadow !== 'none',
      background: c.backgroundColor, active: document.activeElement.tagName,
      canvas: [Math.round(v.width), Math.round(v.height)],
      scroll: [document.documentElement.scrollWidth, document.documentElement.scrollHeight,
               window.innerWidth, window.innerHeight] }; })()`);
  results.overlayStyle = style;
  // Both shots have to be of the SAME frame, or the diff measures the animation and not the overlay -
  // the first run of this check found exactly that: 500919 of 768536 pixels "differed", covering the
  // whole page, because the scene moved between them. So the frame loop is stopped first with the
  // driver's own knob (__ray.freeze, which is not the P key: P keeps rendering, README 2.1), and the
  // loop is started again below and shown to still be running.
  await evaluate('window.__ray.freeze(true)');
  await sleep(500);
  const frozen = await evaluate('window.__ray.state()');
  const fileOn = await shot('overlay_page.png');
  const hidden = await evaluate(`(() => { const e = document.getElementById('status');
    const v = document.getElementById('view').getBoundingClientRect();
    e.style.display = 'none'; return [Math.round(v.width), Math.round(v.height)]; })()`);
  await sleep(400);
  const fileOff = await shot('overlay_page_no_status.png');
  const shown = await evaluate(`(() => { const e = document.getElementById('status');
    e.style.display = ''; const v = document.getElementById('view').getBoundingClientRect();
    return [Math.round(v.width), Math.round(v.height)]; })()`);
  await evaluate('window.__ray.freeze(false)');
  await sleep(700);
  const resumed = await evaluate('window.__ray.state()');
  const on = decodePng(fileOn), off = decodePng(fileOff);
  const dOn = diffRegion(off, on);          // what the overlay adds to one and the same frame
  results.freeze = { at: frozen.frame, resumed: resumed.frame, lumaOn: meanLuma(on),
    lumaOff: meanLuma(off) };
  check('the two screenshots are of the same still frame, and both show the render',
    resumed.frame > frozen.frame && meanLuma(off) > 5 && meanLuma(on) > 5,
    `frame ${frozen.frame} frozen -> ${resumed.frame} after unfreezing; mean luma ${meanLuma(off)} ` +
    `(no overlay) vs ${meanLuma(on)} (overlay)`);
  results.overlay = { fileOn, fileOff, w: on.w, h: on.h, page: dOn,
    canvasRect: { hidden, shown, style: style.canvas } };
  say(`overlay: ${JSON.stringify(results.overlay)}`);
  check('the overlay is drawn, and only in the top-left',
    dOn.pixels > 300 && dOn.bbox.x0 <= 24 && dOn.bbox.y0 <= 24
    && dOn.bbox.x1 < on.w * 0.75 && dOn.bbox.y1 < on.h * 0.5 && dOn.meanDelta > 25,
    `${dOn.pixels} px differ, bbox ${JSON.stringify(dOn.bbox)} of ${on.w}x${on.h}, ` +
    `mean delta ${dOn.meanDelta} (the overlay is near-white text on translucent black)`);
  check('it disturbs no layout: the canvas is the same size with it shown and hidden',
    hidden[0] === shown[0] && hidden[1] === shown[1] && style.canvas[0] === shown[0],
    `canvas ${JSON.stringify(style.canvas)}, with overlay hidden ${JSON.stringify(hidden)}, ` +
    `shown ${JSON.stringify(shown)}; document ${JSON.stringify(style.scroll)}`);
  check('it is a fixed, monospace, translucent, click-through layer',
    style.position === 'fixed' && style.pointerEvents === 'none' && style.shadow
    && /mono/i.test(style.fontFamily) && /rgba?/.test(style.background), JSON.stringify(style));
  check('it holds the live line and the key hints',
    /fps/.test(style.text) && /trace/.test(style.text) && /spp/.test(style.text)
    && /WebGPU pipeline/.test(style.text) && /denoise/i.test(style.text) && /LIVE|PAUSED/.test(style.text)
    && /N denoise/.test(style.text) && /P pause/.test(style.text) && /Up\/Down samples/.test(style.text)
    && /WASD free camera/.test(style.text) && /Alt\+Enter fullscreen/.test(style.text)
    && /ESC quit/.test(style.text), JSON.stringify(style.text));
  check('and it has not taken focus', style.active === 'BODY' || style.active === 'HTML',
    `activeElement=${style.active}`);

  // ---- P: the scene clock stops, the accumulation counter does not.
  const a = await evaluate('window.__ray.state()');
  await key('KeyP', { key: 'p', vk: 80, text: 'p' });
  await sleep(1500);
  const b = await evaluate('window.__ray.state()');
  await sleep(2500);
  const c = await evaluate('window.__ray.state()');
  results.pause = { a, b, c };
  check('P says PAUSED in the title and the overlay',
    b.paused === true && /PAUSED/.test(b.title) && /PAUSED/.test(b.overlay),
    `title "${b.title}"`);
  // `a` was read before the key was delivered, so it may be a few frames ahead of the freeze; the
  // claim is that the clock stops, which is b === c with both at most a frame's worth past a.
  check('P freezes the scene clock', c.simTime === b.simTime && Math.abs(b.simTime - a.simTime) < 0.3
    && c.simTime > 0, `simTime ${a.simTime} -> ${b.simTime} -> ${c.simTime} over ${(c.frame - b.frame) / c.fps | 0} s of frames`);
  check('while accumulation keeps rising', c.accum > b.accum && b.accum > 1,
    `accumulated ${a.accum} -> ${b.accum} -> ${c.accum}`);
  check('and the renderer is still running (frames and fps move, so the still is converging)',
    c.frame > b.frame && c.fps > 5, `frame ${b.frame} -> ${c.frame}, fps ${c.fps}`);
  check('the title still names the pipeline and the denoiser while paused',
    /WebGPU pipeline/.test(b.title) && /Denoise (ON|OFF)/.test(b.title), b.title);
  const paused = await evaluate('window.__ray.state()');
  await key('KeyP', { key: 'p', vk: 80, text: 'p' });
  await sleep(1600);
  const r1 = await evaluate('window.__ray.state()');
  await sleep(1200);
  const r2 = await evaluate('window.__ray.state()');
  results.resumed = { paused, r1, r2 };
  check('P again resumes: the clock advances and the title stops saying PAUSED',
    r2.paused === false && r2.simTime > r1.simTime && !/PAUSED/.test(r2.title),
    `simTime ${r1.simTime} -> ${r2.simTime}, title "${r2.title}"`);
  check('resuming restarts accumulation, as a motion restart requires',
    r1.accum < paused.accum && r1.accum <= 1,
    `accum ${paused.accum} (paused) -> ${r1.accum} (resumed) -> ${r2.accum}`);
  check('and the page printed both lines',
    sawText('the scene clock is stopped and accumulation continues')
    && sawText('the scene clock runs again and accumulation restarts'), 'searched the console');

  // ---- Up/Down: samples move, and the clamp holds at both ends.
  const s0 = await evaluate('window.__ray.state()');
  await key('ArrowUp', { key: 'ArrowUp', vk: 38 });
  await key('ArrowUp', { key: 'ArrowUp', vk: 38 });
  await sleep(400);
  const s1 = await evaluate('window.__ray.state()');
  check('ArrowUp raises samples per pixel per frame', s1.spp === s0.spp + 2
    && s1.sppUserToggled === true, `${s0.spp} -> ${s1.spp}, user-pinned=${s1.sppUserToggled}`);
  check('the title follows the sample count', new RegExp(` - ${s1.spp} spp - `).test(s1.title), s1.title);
  check('and the page logged it as the user taking the knob',
    sawText(`quality decision: samples ${s0.spp} -> ${s0.spp + 1} (pressed ArrowUp) - samples are ` +
            'the user\'s from here on'), 'searched the console');
  for (let i = 0; i < 6; i++) await key('ArrowDown', { key: 'ArrowDown', vk: 40 });
  await sleep(400);
  const lo = await evaluate('window.__ray.state()');
  for (let i = 0; i < 22; i++) await key('ArrowUp', { key: 'ArrowUp', vk: 38 });
  await sleep(400);
  const hi = await evaluate('window.__ray.state()');
  results.samples = { s0, s1, lo, hi };
  check('ArrowDown clamps at 1', lo.spp === 1, `spp=${lo.spp}`);
  check('ArrowUp clamps at the app ceiling (16, the controller max in scene.h)',
    hi.spp === 16 && hi.sppMax === 16, `spp=${hi.spp} of max ${hi.sppMax}`);
  check('the clamp is logged, not swallowed', sawText('quality decision: samples 16 held'),
    'searched the console');
  check('the overlay names the sample count as the user\'s', /\d+ spp \(user\)/.test(hi.overlay),
    JSON.stringify(hi.overlay));
  await evaluate('window.__ray.samples(1)');       // leave the run cheap for the click test
  await sleep(600);

  // ---- a plain click still takes pointer lock, overlay or no overlay
  await key('KeyW', { key: 'w', vk: 87, text: 'w', hold: 900 });
  const free = await evaluate('window.__ray.state()');
  await evaluate('document.exitPointerLock && document.exitPointerLock()');
  await sleep(500);
  const unlocked = await evaluate('window.__ray.state()');
  const cx = Math.round(free.winW / 2 / free.dpr), cy = Math.round(free.winH / 2 / free.dpr);
  for (const type of ['mousePressed', 'mouseReleased']) {
    await send('Input.dispatchMouseEvent', { type, x: cx, y: cy, button: 'left', clickCount: 1,
      buttons: type === 'mousePressed' ? 1 : 0 });
  }
  await sleep(900);
  const clicked = await evaluate('window.__ray.state()');
  results.pointerLock = { free: { locked: free.pointerLocked, refused: free.lockRefused },
    unlocked: unlocked.pointerLocked, clicked: clicked.pointerLocked, at: [cx, cy] };
  check('a click on the canvas still takes pointer lock (the overlay does not eat it)',
    unlocked.pointerLocked === false && clicked.pointerLocked === true,
    `locked ${free.pointerLocked} -> ${unlocked.pointerLocked} after exitPointerLock -> ` +
    `${clicked.pointerLocked} after the click at ${cx},${cy}; refusal reported=${free.lockRefused}`);
  check('and the overlay is still not the focus target after the click',
    await evaluate('document.activeElement.tagName') !== 'DIV',
    `activeElement=${await evaluate('document.activeElement.tagName')}`);
  const end = await evaluate('window.__ray.state()');
  results.stateEnd = end;
  check('no WebGPU errors and no fatal lines through all of it',
    end.gpuErrors === 0 && end.consoleErrors === 0,
    `gpuErrors=${end.gpuErrors} consoleErrors=${end.consoleErrors}`);
}

/**
 * The other half of the sample-count rule: once the user has set it, the controller must stop choosing
 * it - and the budget this run is given is one that would otherwise force it to. `?targetfps=8` hands
 * the controller a budget ~15x what the frames cost with the filter off, which is the case where the
 * policy buys samples until it hits `maxSamplesPerFrame`. So the run shows the move happening while the
 * samples are still the controller's, pins them with one keypress, and then shows the same budget
 * leaving them alone.
 */
async function scenarioPin() {
  await waitFor('window.__ray && window.__ray.ready', 120000, 'window.__ray');
  const s0 = await evaluate('window.__ray.state()');
  results.start = s0;
  // First: without a keypress, this budget does move samples. That is the counterfactual the claim
  // needs; without it, "the controller left them alone" would be indistinguishable from a run that
  // never wanted a different count at all.
  let moved = null;
  const t0 = Date.now();
  while (Date.now() - t0 < 90000) {
    const s = await evaluate('window.__ray.state()');
    if (s.spp !== s0.spp || s.controller.spp !== s0.controller.spp) { moved = s; break; }
    await sleep(1000);
  }
  results.controllerMovedSpp = moved;
  check('this budget does make the controller choose the sample count', !!moved,
    moved ? `spp ${s0.controller.spp} -> ${moved.controller.spp} at ${moved.controller.avgMs} ms ` +
            `against a ${moved.controller.budgetMs} ms budget` : 'spp never moved within 90 s');
  if (!moved) return;

  // Now the user takes it, one step down from wherever the controller had got to.
  const before = await evaluate('window.__ray.state()');
  await key('ArrowDown', { key: 'ArrowDown', vk: 40 });
  await sleep(600);
  const pinned = await evaluate('window.__ray.state()');
  results.pinned = pinned;
  check('ArrowDown pins it one step down', pinned.spp === before.spp - 1
    && pinned.controller.sppUser === true, `${before.spp} -> ${pinned.spp}, sppUser=${pinned.controller.sppUser}`);
  check('and the log says samples are the user\'s from here on',
    sawText(`quality decision: samples ${before.spp} -> ${pinned.spp} (pressed ArrowDown) - samples ` +
            'are the user\'s from here on; the controller keeps the resolution and the filter'),
    'searched the console');

  const a = await evaluate('window.__ray.state()');
  await sleep(25000);
  const b = await evaluate('window.__ray.state()');
  results.afterPin = { a, b };
  check('the controller does not touch the sample count any more', a.spp === b.spp
    && a.controller.spp === b.controller.spp && a.spp === pinned.spp,
    `spp ${a.spp} -> ${b.spp} over 25 s at ${b.controller.avgMs} ms against a ` +
    `${b.controller.budgetMs} ms budget, cap ${b.sppMax}`);
  check('the budget it was given still wants more samples (so "left alone" is the rule, not luck)',
    b.controller.withinBudget === true && b.spp < b.sppMax,
    `withinBudget=${b.controller.withinBudget}, spp ${b.spp} < ceiling ${b.sppMax}`);
  check('the controller still owns the resolution and the filter',
    b.controller.sppUser === true && b.controller.denoiseUser === false
    && typeof b.controller.scale === 'number',
    `scale ${b.controller.scale}, denoise ${b.controller.denoise}, denoiseUser=${b.controller.denoiseUser}`);
  check('and frames kept advancing throughout', b.frame > a.frame, `${a.frame} -> ${b.frame}`);
  const st = await evaluate('window.__ray.state()');
  check('no WebGPU errors', st.gpuErrors === 0, `gpuErrors=${st.gpuErrors}`);
}

const SCENARIOS = { smoke: scenarioSmoke, keys: scenarioKeys, resize: scenarioResize,
  still: scenarioStill, adaptive: scenarioAdaptive, probe: scenarioProbe,
  fullscreen: scenarioFullscreen, mouse: scenarioMouse,
  features: scenarioFeatures, pin: scenarioPin };

// --------------------------------------------------------------------------- what the page fetched
/**
 * The page's own resource list, read out of the browser at the end of the run. This is the part that
 * tells "the site loaded" from "the site 404s one module at a time": a path-prefix mistake does not
 * throw, it just never answers, and the canvas stays black while the page prints its own health
 * claims. performance's resource timing covers everything the load fetched, including requests made
 * before this driver attached - which the DevTools Network domain would have missed.
 */
const RESOURCE_DUMP = `(() => {
  const clean = (list) => list.filter((e) => !/^(data|blob):/.test(e.name)).map((e) => ({
    url: e.name, status: (typeof e.responseStatus === 'number' ? e.responseStatus : null),
    bytes: Math.round(e.encodedBodySize), kind: e.initiatorType }));
  const nav = performance.getEntriesByType('navigation')[0];
  return JSON.stringify({
    page: location.href,
    nav: nav ? { url: nav.name, status: (typeof nav.responseStatus === 'number' ? nav.responseStatus : null) } : null,
    resources: clean(performance.getEntriesByType('resource')),
  });
})()`;

/**
 * Requests the page is built to make and live without: the ordered candidates for the pak, the loose
 * assets and the reference image (a deployed site does not carry the native PNGs), and the developer
 * server's /results and /upload sink (a static host has neither). Any *other* non-2xx answer is a
 * real failure.
 */
const PROBE_OK = /(\/assets\/scene\.pak$|\/assets\/scene\.json$|\/assets\/textures\/|(^|\/)refs\/|\/results$|\/upload\?)/;

async function reportNetwork() {
  let dump;
  try {
    dump = JSON.parse(await evaluate(RESOURCE_DUMP));
  } catch (e) {
    say('network: could not read the page resource list: ' + e.message);
    return;
  }
  results.network = dump;
  const seen = new Map();
  for (const r of dump.resources) {
    const k = `${r.status} ${r.url}`;
    const e = seen.get(k) || { ...r, count: 0 };
    e.count++;
    seen.set(k, e);
  }
  const rows = [...seen.values()];
  say(`network: navigation ${dump.nav && dump.nav.status} for ${dump.page}; ` +
    `${dump.resources.length} resource requests, ${rows.length} distinct`);
  for (const r of rows) say(`  RES ${String(r.status).padEnd(4)} x${r.count} ${r.bytes}B ${r.kind} ${r.url}`);
  const bad = rows.filter((r) => r.status !== null && !(r.status >= 200 && r.status < 300) && r.status !== 304);
  const probes = bad.filter((r) => PROBE_OK.test(r.url));
  const failures = bad.filter((r) => !PROBE_OK.test(r.url));
  for (const r of probes) say(`  PROBE ${String(r.status).padEnd(4)} ${r.url} (a candidate the page can live without)`);
  check('every resource the page needs came back', failures.length === 0,
    failures.length ? failures.map((r) => `${r.status} ${r.url}`).join(' , ')
      : `${rows.length} distinct requests, ${probes.length} allowed-to-miss probe(s), no other non-2xx`);
  check('the page was served from the prefix it was asked for',
    dump.nav && dump.nav.status >= 200 && dump.nav.status < 300,
    `nav=${dump.nav && dump.nav.status} ${dump.page}`);
}

// --------------------------------------------------------------------------- run
async function main() {
  if (!existsSync(EDGE)) throw new Error(`browser not found: ${EDGE}`);
  say(`launch ${EDGE} ${FLAGS.join(' ')} --window-size=${WIN}`);
  browser = spawn(EDGE, [`--user-data-dir=${profile}`, '--no-first-run', '--no-default-browser-check',
    `--remote-debugging-port=${PORT}`, `--window-size=${WIN}`, ...FLAGS, PAGE],
  { stdio: ['ignore', 'ignore', 'pipe'] });
  pids.push(browser.pid);
  browser.stderr.on('data', (b) => {
    const t = b.toString();
    if (/CONSOLE|ERROR/.test(t)) appendFileSync(LOG, `STDERR ${t.trim()}\n`);
  });

  const wsUrl = await attach();
  say(`devtools: ${wsUrl}`);
  await connect(wsUrl);
  await send('Runtime.enable');
  await send('Page.enable');
  await send('Log.enable').catch(() => { /* optional */ });

  const deadline = Date.now() + TIMEOUT;
  let outcome = 'ok';
  try {
    await Promise.race([
      SCENARIOS[SCENARIO](),
      new Promise((_, rej) => setTimeout(() => rej(new Error(`scenario timed out after ${TIMEOUT} ms`)), TIMEOUT)),
    ]);
  } catch (e) {
    outcome = 'error';
    results.error = String(e.message || e);
    say('ERROR ' + results.error);
  }
  results.consoleCounts = consoleLines.reduce((m, l) => { m[l.type] = (m[l.type] || 0) + 1; return m; }, {});
  await reportNetwork();
  results.outcome = outcome;
  writeFileSync(OUT, JSON.stringify(results, null, 2));
  say(`result -> ${OUT}; console -> ${LOG}`);
  say('VERDICT ' + (outcome === 'ok' && results.checks.every((c) => c.ok) ? 'PASS' : 'FAIL')
    + ` (${results.checks.filter((c) => !c.ok).length} failing check(s))`);
}

main().catch((e) => { say('FATAL ' + (e.stack || e.message)); results.error = String(e.message); writeFileSync(OUT, JSON.stringify(results, null, 2)); })
  .finally(async () => {
    try { if (ws) ws.close(); } catch { /* ignore */ }
    // Kill only processes whose command line names this throwaway profile.
    try {
      const ps = spawn('powershell', ['-NoProfile', '-Command',
        `Get-CimInstance Win32_Process -Filter "Name LIKE '%msedge%'" | Where-Object { $_.CommandLine -like '*${profile}*' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }`],
      { stdio: 'ignore' });
      await new Promise((r) => ps.on('exit', r));
    } catch { /* ignore */ }
    try { browser && browser.kill(); } catch { /* ignore */ }
    setTimeout(() => { try { rmSync(profile, { recursive: true, force: true }); } catch { /* ignore */ } }, 1500);
    process.exit(0);
  });
