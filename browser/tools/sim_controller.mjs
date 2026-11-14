// sim_controller.mjs - headless replay of browser/quality.js against a synthetic machine.
//
// Why this exists: the two controller rules changed on 2026-11-13 (bidirectional price correction, and
// sustained evidence before a second cut) can only be judged on a long run of frames with noise in it,
// and a browser run cannot be made to produce a compositor spike on demand. This harness drives the
// controller frame by frame against a cost model of the *measured* Edge machine and prints the decision
// trace, so the before and after policies can be compared on the identical workload.
//
//     node browser/tools/sim_controller.mjs [--module <path-to-quality.js>] [--frames N] [--seed N]
//
// The workload is the machine from docs/web-port-notes.md section 12, not an invented one:
//   - backing store 1944x1240 => 2410.56 kilopixels (the run's own `window` line);
//   - `floor_empty_full_grid` 1.00 ms, the cost of a frame that traces nothing;
//   - trace 16.47 ms net at scale 1.00 and 1 spp => 0.006833 ms per kilopixel per sample;
//   - filter 1.8 ms per megapixel, inside the 1.84-4.56 ms/MP band README 9.4 records for this card;
//   - budget 9.8 ms: 1000/60 * (1 - 0.26) - 2.5, the same arithmetic the run reported.
// The frame time the controller sees is that truth plus a dispatch floor *and* a compositing/present
// cost that appear in the whole-frame wall clock but in neither split submission - which is exactly why
// the measured run's trace price drifted upward: the two-term model has nowhere else to charge them, and
// a model that cannot account for a constant 4 ms of frame will keep repricing the pixels to explain it.
// On top of that the machine has compositor noise: a mild ±3 % jitter and, every `spikeEvery` frames,
// one frame carrying an extra few milliseconds. That spike is the thing the old one-way price ratchet
// could not survive - it charged the whole excess to the trace term and never took it back.
//
// Two workloads are replayed, because the ratchet was written to stop flapping in the *other*
// direction and that has to still hold:
//   `spike`   - the measured machine with compositor spikes: does it settle, and does it stay?
//   `optimist`- a machine whose frames come in cheap against an over-priced seed: does it climb and then
//               flap? (the failure the ratchet was originally written against.)
//
// Pass --module on the committed copy of quality.js to see the old policy; with no argument it checks
// out the working tree, which is the new one.

import { pathToFileURL } from 'node:url';
import path from 'node:path';

const argv = process.argv.slice(2);
function opt(name, dflt) {
  const i = argv.indexOf('--' + name);
  return i < 0 ? dflt : argv[i + 1];
}
const modPath = pathToFileURL(path.resolve(opt('module', 'browser/quality.js'))).href;
const FRAMES = +opt('frames', 400);
const SEED = +opt('seed', 7);

// mulberry32: deterministic, so a reported trace is reproducible from its seed alone.
function rng(seed) {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6D2B79F5) >>> 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

const MACHINE = {
  kiloPx1: (1944 * 1240) / 1000,
  floorMs: 1.00,             // measured empty-grid dispatch floor
  presentMs: 3.2,            // compositing/present: in the whole-frame wall clock, in neither submission
  tracePerKiloPxSpp: 16.47 / ((1944 * 1240) / 1000),   // 0.006833
  filterPerMp: 1.8,
  roundTripMs: 0.9,          // one split submission's CPU-GPU round trip, priced into both probe terms
};

/**
 * A machine the controller renders on. `spikes` adds an occasional extra cost to the whole-frame
 * submission only - a compositor hiccup is in the wall clock, not in the GPU work, which is exactly the
 * kind of reading a price must not be rebuilt from.
 */
function makeMachine(o) {
  const rand = rng(o.seed);
  const traceRate = o.tracePerKiloPxSpp ?? MACHINE.tracePerKiloPxSpp;
  const filterRate = o.filterPerMp ?? MACHINE.filterPerMp;
  return (scale, spp, denoise, frameIndex, probe) => {
    const kilo = MACHINE.kiloPx1 * scale * scale;
    const mp = kilo / 1000;
    const trace = traceRate * kilo * spp;
    const filter = denoise ? filterRate * mp : 0;
    const jitter = 1 + (rand() - 0.5) * 2 * o.jitter;
    let frame = (MACHINE.floorMs + MACHINE.presentMs + trace + filter) * jitter;
    if (o.spikes > 0 && frameIndex % o.spikeEvery === o.spikeEvery - 1) {
      frame += o.spikes * (0.5 + rand());
    }
    if (probe) {
      // Two submissions: the split readings each carry a round trip, and so does the frame.
      return {
        frameMs: frame + MACHINE.roundTripMs,
        traceMs: (trace + MACHINE.floorMs) * jitter + MACHINE.roundTripMs,
        filterMs: (filter > 0 ? filter : MACHINE.floorMs) * jitter + MACHINE.roundTripMs,
      };
    }
    return { frameMs: frame, traceMs: 0, filterMs: 0 };
  };
}

async function run(label, cfg, machine) {
  const { Quality } = await import(modPath);
  const q = new Quality(cfg);
  const trace = [];
  let appliedScale = cfg.scale;
  for (let f = 0; f < FRAMES; ++f) {
    const probe = q.wantsProbe(f);
    // A probe frame encodes the filter even when the denoiser is off, as app.js does.
    const r = machine(q.scale, q.spp, probe ? true : q.denoise, f, probe);
    if (probe) {
      q.observe(r.frameMs, r.traceMs, r.filterMs, 1000 / cfg.targetFps, true);
    } else {
      const d = q.observe(r.frameMs, r.traceMs, r.filterMs, 1000 / cfg.targetFps, false);
      if (d) {
        trace.push(`  f${String(f).padStart(3)}  ${d.what} ${fmtFrom(d, q)} ` +
          `-> ${d.what === 'scale' ? (+d.to).toFixed(3) : d.to}` +
          `  [frame ${d.ms} ms / budget ${d.budget} ms, trace price ` +
          `${q.o.traceMsPerSampleKiloPx.toFixed(5)}, filter ${q.o.filterMsPerMegaPx.toFixed(2)}]`);
        appliedScale = q.scale;
      }
      if (f % 50 === 0) {
        trace.push(`  f${String(f).padStart(3)}  .. scale ${q.scale.toFixed(3)} ${q.spp} spp ` +
          `denoise ${q.denoise ? 'on ' : 'off'} avg ${q.avg.toFixed(1)} ms ` +
          `trace price ${q.o.traceMsPerSampleKiloPx.toFixed(5)}`);
      }
    }
  }
  const st = q.state();
  const scaleCuts = q.decisions.filter(d => d.what === 'scale' && d.to < d.scale).length;
  const scaleClimbs = q.decisions.filter(d => d.what === 'scale' && d.to > d.scale).length;
  // Flapping, counted the way a user would see it: direction reversals in the scale knob after the last
  // change, and denoiser give-and-take.
  const scales = q.decisions.filter(d => d.what === 'scale').map(d => d.to);
  let reversals = 0;
  for (let i = 2; i < scales.length; ++i) {
    if (Math.sign(scales[i] - scales[i - 1]) !== Math.sign(scales[i - 1] - scales[i - 2])) ++reversals;
  }
  const last20 = [];
  for (let f = 0; f < 20; ++f) last20.push(st.scale);
  console.log(`\n--- ${label} (${path.basename(modPath)}) seed ${SEED} frames ${FRAMES} ---`);
  console.log(trace.join('\n'));
  console.log(`  RESULT scale=${st.scale} spp=${st.spp} denoise=${st.denoise}` +
    ` avg=${st.avgMs}ms budget=${st.budgetMs}ms withinBudget=${st.withinBudget}` +
    ` exhausted=${st.exhausted} settled=${st.settled}`);
  console.log(`  changes=${q.decisions.length} (cuts ${scaleCuts}, climbs ${scaleClimbs},` +
    ` denoise flips ${st.flips}) scale reversals=${reversals}`);
  console.log(`  price corrections up=${st.correctionsUp ?? 'n/a'} down=${st.correctionsDown ?? 'n/a'}` +
    ` trace price ${st.tracePerKiloPxSpp} (the GPU truth for the slow workloads is ` +
    `${MACHINE.tracePerKiloPxSpp.toFixed(5)}; the dispatch and present cost is charged to this term)`);
  return { label, st, reversals, scaleCuts, scaleClimbs, q };
}

function fmtFrom(d, q) {
  if (d.what === 'scale') return d.scale.toFixed(3);
  if (d.what === 'spp') return String(d.spp);
  return d.denoise ? 'on' : 'off';
}

const base = {
  auto: true, targetFps: 60, headroom: 0.26, cpuReserveMs: 2.5,
  kiloPx1: MACHINE.kiloPx1, minScale: +opt('min-scale', 0.35), maxScale: 1.0, maxSpp: 4,
  denoise: true, priority: 'pixels',
  // The seeds the real run carried into play mode: the probe's NVIDIA number, roughly half the truth.
  traceMsPerSampleKiloPx: 0.0035, filterMsPerMegaPx: 1.0,
};

const results = [];
// 1. The measured machine, with compositor spikes. This is the run the brief's log came from.
results.push(await run('spike: measured Edge machine + compositor spikes', base,
  makeMachine({ seed: SEED, jitter: 0.03, spikes: 5.0, spikeEvery: 37 })));
// 2. A machine that FITS its budget at full size but is hit by compositor spikes. This is the workload
//    the one-way ratchet failed on: every spike charged its excess to the trace price and nothing ever
//    gave it back, so a card with room to spare was walked down to a small picture.
results.push(await run('cheap: fits the budget at scale 1.0, but the browser spikes its frames',
  base, makeMachine({
    seed: SEED + 2, jitter: 0.03, spikes: 8.0, spikeEvery: 14,
    tracePerKiloPxSpp: 0.0015, filterPerMp: 0.8,
  })));
// 3. The optimistic direction the ratchet was written against: frames cheaper than the model believes.
results.push(await run('optimist: over-priced seed, cheap frames',
  Object.assign({}, base, { traceMsPerSampleKiloPx: 0.02 }),
  makeMachine({ seed: SEED + 1, jitter: 0.03, spikes: 0, spikeEvery: 999 })));

let bad = 0;
for (const r of results) {
  if (r.reversals > 0) { console.log(`FLAP ${r.label}: scale reversed direction ${r.reversals}x`); ++bad; }
  if (r.st.flips > 0) { console.log(`FLAP ${r.label}: denoiser flipped ${r.st.flips}x`); ++bad; }
}
console.log(bad === 0 ? '\nno flapping: no scale reversal, no denoiser give-and-take' :
  `\n${bad} workload(s) flap`);
console.log('verdict inputs: settled scale, reversals, flips, corrections up/down');
