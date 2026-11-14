// tools/check_favicon.mjs - ask a real headless Edge what it fetched, and look for the icon request.
//
// The claim being tested is narrow: with `<link rel="icon">` in index.html the browser should ask for
// browser/favicon.ico and never for the server root's /favicon.ico, which is the 404 that showed up in
// the run log. The page's own resource timing list is the evidence - it records every fetch the page made,
// including the one the browser makes for the icon on its own initiative.
//
//   node browser/tools/check_favicon.mjs [--serverPort 8731]

import { spawn } from 'node:child_process';
import { rmSync, mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const opt = (k, d) => { const i = process.argv.indexOf('--' + k); return i < 0 ? d : process.argv[i + 1]; };
const EDGE = opt('browser', 'C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe');
const PORT = Number(opt('cdpPort', 9345));
const SERVER = Number(opt('serverPort', 8731));
const PAGE = `http://127.0.0.1:${SERVER}/browser/index.html?capture=1&size=320x240`;
const profile = mkdtempSync(join(tmpdir(), 'favcheck-'));
let edge = null;

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const get = async (path) => (await fetch(`http://127.0.0.1:${PORT}${path}`)).json();

try {
  edge = spawn(EDGE, [`--user-data-dir=${profile}`, '--no-first-run', '--no-default-browser-check',
    `--remote-debugging-port=${PORT}`, '--window-size=800,600', '--headless=new',
    '--enable-unsafe-webgpu', '--no-sandbox', PAGE], { stdio: 'ignore' });

  let target = null;
  for (let i = 0; i < 60 && !target; ++i) {
    await sleep(500);
    try {
      target = (await get('/json/list')).find((t) => t.type === 'page' && t.webSocketDebuggerUrl);
    } catch { /* not up yet */ }
  }
  if (!target) throw new Error('no devtools page target');

  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
  let id = 0;
  const pending = new Map();
  ws.onmessage = (ev) => {
    const m = JSON.parse(ev.data);
    if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
  };
  const send = (method, params = {}) => new Promise((res) => {
    const n = ++id; pending.set(n, res); ws.send(JSON.stringify({ id: n, method, params }));
  });
  const evaluate = async (expression) => {
    const r = await send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true });
    return r.result && r.result.result ? r.result.result.value : undefined;
  };

  await send('Runtime.enable');
  // Give the page a couple of seconds of real loading: the icon fetch happens at load, not later.
  await sleep(6000);
  const names = await evaluate(`JSON.stringify(performance.getEntriesByType('resource')
      .map((e) => e.name).filter((n) => /favicon/i.test(n)))`);
  const total = await evaluate(`performance.getEntriesByType('resource').length`);
  const found = JSON.parse(names || '[]');
  const status = await evaluate(`(async () => {
    const r = await fetch('/favicon.ico', { cache: 'no-store' });
    return r.status;
  })()`);

  console.log(`resources fetched by the page: ${total}`);
  console.log(`favicon fetches: ${JSON.stringify(found)}`);
  console.log(`server root /favicon.ico, if asked, answers: ${status}`);
  const askedRoot = found.some((n) => new URL(n).pathname === '/favicon.ico');
  const askedOurs = found.some((n) => /\/build\/html\/favicon\.ico$/.test(new URL(n).pathname));
  console.log(`asked for our icon: ${askedOurs}; asked the server root: ${askedRoot}`);
  console.log(askedOurs && !askedRoot ? 'PASS  the icon comes from browser/ and the root 404 is not requested'
    : `FAIL  expected our icon and no root request, got ${JSON.stringify(found)}`);
  ws.close();
} finally {
  if (edge) edge.kill();
  await sleep(400);
  try { rmSync(profile, { recursive: true, force: true }); } catch { /* left behind is fine */ }
}
