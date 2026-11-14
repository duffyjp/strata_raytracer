// quality.js - the adaptive quality controller, driven by CPU frame times.
//
// Why CPU times: web content cannot use GPU timestamps honestly. `timestamp-query` is offered by this
// adapter (the probe measured that), but resolving a query needs another submit plus a mapAsync round
// trip, and that round trip measured 0.5 - 3.0 ms on this machine - the same size as the frame budget,
// and bimodal. So the controller reads wall-clock milliseconds between submissions, and every decision
// is made on a streak of readings rather than one of them.
//
// The policy is the native controller's, reimplemented rather than imported (docs/web-port-notes.md
// section 5): a two-term cost model with the filter priced as its own term, decisions on streaks
// inside a deadband, a settle period after any change, and an asymmetric step size so it cannot flap by
// one sample either side of the line. What is *not* reused is any native code or any constant that only
// makes sense on Vulkan.
//
// One thing IS deliberately diverged from the shipped native controller: the order in which things are
// given up. Native surrenders the denoiser first, before pixels or samples. On the web path that order
// is the wrong one, because the two terms are priced on different GPUs - see PRIORITY below.
//
// Two-term model, as in src/frame.cpp:550:
//     frame(scale, spp) = traceMsPerSampleKiloPx * kiloPx * scale^2 * spp + filterMsPerMegaPx * MP
// The second term does not care about spp, which is why it has to be separate: with one constant doing
// both jobs the controller buys samples that it thinks are free.
//
// --------------------------------------------------------------------------- what changed, and why
// The play-mode run in docs/web-port-notes.md section 10 sat at 95 ms against a 9.17 ms budget, at
// `scale 1`, having made one decision, because every mechanism that could have saved it was shut off by
// the same three things:
//
//   1. One submission per frame means there is no trace/filter split to learn from. The old code only
//      ever learned from `traceMs > 0` / `filterMs > 0`, so in play mode BOTH learned terms stayed at
//      their constructor seeds - and those seeds were measured on the NVIDIA adapter by the probe
//      (0.0035 ms per kilopixel per sample). On the adapter actually running the demo the truth is an
//      order of magnitude larger, so the model predicted 3.23 ms of a 95 ms frame.
//      Fix: `wantsProbe()` - the caller is told when to measure a frame as two submissions, on the
//      *same* workload (same resolution, same accumulation, same moving lights), early and often until
//      both terms are priced, then periodically. Plus a per-frame correction (below) so the model cannot
//      keep a price that its own workload contradicts.
//   2. The model said one sample at full size cost 3.23 ms, i.e. that the current configuration already
//      fitted the budget, so every "what do I want" computation answered "what I already have" and the
//      controller stopped moving while 10x over budget. Fix: an over-budget fallback that steps from the
//      *measured* frame time instead of from the model (pixels cost milliseconds ~linearly, so
//      scale *= sqrt(budget/measured)), used whenever the model offers no move. It obeys the same 20 %
//      minimum-move rule, so it cannot nibble.
//   3. The denoiser was surrendered at frame 12 without ever being priced, so the flip was a guess and
//      the filter's price was never learned - the mirror image of the native rule that the filter is not
//      *restored* without calibration. Fix: the surrender waits for the filter to be priced (with a
//      hard frame cap, so a browser that cannot probe is not locked out of ever shedding it), and the
//      restore requires the same calibration.
//
// --------------------------------------------------------------------------- PRIORITY
// The native controller gives up the denoiser before it gives up pixels, and on the machine it was tuned
// on that is nearly free: MEASURED 0.86 ms/MP (docs section 2.7), 0.79 ms of a 16.9 ms frame, 4.7 % of
// the cost. The same passes through the browser cost MEASURED 3.6 ms/MP on nvidia/blackwell and 37.8
// ms/MP on Intel UHD 770 (docs 9.4, 10.4) - 4x to 44x the native price, because the ported SVGF is
// itself 12-19x the proxy it was budgeted against. So on the web path the filter is a first-order term
// of the frame, not a rounding error, and the priority is reversed here:
//
//     pixels before filter. The denoiser is surrendered only once the resolution floor is reached and
//     one sample per pixel there still does not fit the budget.
//
// Why that is the right order for the picture and not only for the milliseconds, at the two measured
// prices: at 3.6 ms/MP (blackwell) the filter is 0.83 ms of a 9.17 ms budget at scale 0.5, so keeping it
// costs the resolution almost nothing; at 37.8 ms/MP (UHD 770) it is 8.7 ms at scale 0.5, i.e. the whole
// budget, and the choice is between a soft, smooth, stable frame at a reduced size and a 1-spp frame of
// raw estimator noise at full size. The desktop app never offers that second picture, and the browser
// page that did is what the user described as degrading into an ugly unstable image a few seconds in.
// Samples are still never raised while the filter is on (`cap` below), which is what kept the native
// order cheap: it never had to pay for samples *and* the filter at once.
//
// Set `priority: 'denoise'` to get the shipped native order back, and `maxChanges: 0` for the old
// behaviour of never stopping deciding.
//
// And when nothing is left to give - one sample, floor resolution, no filter, still late - the
// controller says so: `exhausted` and `withinBudget` are part of its state, so a run that missed its
// target cannot be read as a success.

export const CONTROLLER_DEFAULTS = {
  targetFps: 60,
  headroom: 0.3,           // fraction of the frame left unused, as scene.json's qualityHeadroom
  cpuReserveMs: 2.5,       // the loop's own stages, which a GPU measurement would not see
  deadband: 0.72,          // between 72% and 104% of budget, change nothing
  cutMargin: 0.05,         // a cut is worth making if it moves the scale by more than this
  climbMargin: 0.86,       // going up, aim below budget so the settle lands inside the deadband
  overStreak: 2,           // readings over budget before cutting
  underStreak: 3,          // readings under budget before spending more
  settleFrames: 3,         // readings thrown away after a change
  decideEvery: 12,         // weigh the evidence this many frames apart, so a streak is independent
  maxFlips: 3,             // times the denoiser may be given up and taken back before it is left alone
  priority: 'pixels',      // 'pixels' = cut resolution before surrendering the denoiser (see PRIORITY
                           // above); 'denoise' = the shipped native order, kept for comparison
  maxChanges: 8,           // decisions after which the controller stops moving and says so (0 = never)
  // Seeds. They are the probe's NVIDIA numbers, and they are *seeds only*: `calibrated` says whether
  // anything has replaced them, and the controller will not trust them against its own measurements.
  traceMsPerSampleKiloPx: 0.0035,   // seeded from the probe: ~1 ms for 1 spp at 0.92 MP
  filterMsPerMegaPx: 1.0,           // seeded from the probe's a-trous: ~0.75 ms/MP for five iterations
  // Calibration protocol (see wantsProbe).
  probeWarmupFrames: 3,    // first frames are warm-up: shader compile, cold accumulator, first refit
  calibrateMeasurements: 3,// readings needed per term before it counts as priced
  probeEvery: 40,          // re-check the prices this often once calibrated
  calibrateByFrame: 36,    // after this, stop waiting for a price that is not coming
  maxCorrectionPerStep: 3, // how far one *sustained* contradiction may move the trace price, either way
  // Price correction (see #correctPrice). Bidirectional since the Edge run in docs section 12: the
  // upward-only rule was supposed to stop optimistic shrink-flapping and did, while guaranteeing the
  // opposite failure - one slow sample permanently shrinks the picture.
  correctionWindow: 7,     // recent frame/model ratios the correction is decided from (median)
  correctionsNeeded: 3,    // ratios in that window before any correction may fire
  correctionBand: 0.25,    // median ratio must be past 1 +/- this to move a price at all
  priceWindow: 9,          // readings the split-probe median is taken over (was 7)
  // Sustained evidence before spending the resolution knob again (see decide).
  cutDeadband: 0.10,       // "missed" means worse than 110 % of budget, not merely outside the deadband
  cutsNeeded: 2,           // consecutive deciding points that must miss before a scale/spp cut
};

export class Quality {
  constructor(cfg = {}) {
    const d = CONTROLLER_DEFAULTS;
    this.o = Object.assign({}, d, cfg);
    this.seed = {
      traceMsPerSampleKiloPx: this.o.traceMsPerSampleKiloPx,
      filterMsPerMegaPx: this.o.filterMsPerMegaPx,
    };
    this.spp = 1;
    this.scale = 1;
    this.denoise = cfg.denoise !== false;
    this.denoiseUser = false;
    // Set by the Up/Down keys: the user owns the sample count for the rest of the run, exactly as
    // `denoiseUser` means for the filter. The controller keeps resolution and the filter.
    this.sppUser = false;
    this.flips = 0;
    this.overStreak = 0;
    this.underStreak = 0;
    this.settle = 0;
    this.decideFrames = 0;
    this.avg = 0;            // smoothed full-frame CPU time, dispatch floor included
    this.traceAvg = 0;       // smoothed time of the trace submission alone
    this.filterAvg = 0;      // smoothed time of the filter submission alone
    this.frameAvg = 0;       // smoothed time of the whole-frame submission
    this.calibrated = false;
    this.traceMeasures = 0;
    this.filterMeasures = 0;
    this.traceUnits = [];      // recent per-kilopixel-per-sample readings (median-priced, see #price)
    this.filterUnits = [];     // recent per-megapixel readings for the filter
    this.ratios = [];          // recent measured/model frame-time ratios (median-corrected, see #correctPrice)
    this.priceMoves = [];      // every price correction, with its direction, for the run's own log
    this.missStreak = 0;       // consecutive deciding points past the cut deadband (see cutsNeeded)
    this.lastCorrection = -1e9; // frame index of the last price correction (see #correctPrice)
    this.modelCorrections = 0; // frames where a measurement contradicted the model and repriced it
    this.probeFrames = 0;
    this.exhausted = false;
    this.exhaustedAt = 0;
    this.settled = false;      // the decision budget (o.maxChanges) is spent: no further moves
    this.pendingGive = false;  // over budget, denoiser on, waiting to be able to price it
    this.flipArmed = null;   // {atMs, atSpp, atScale, turningOn}
    this.decisions = [];
    this.lateFrames = 0;
    this.frames = 0;
  }

  budget() {
    const frameMs = 1000 / this.o.targetFps;
    return Math.max(1.5, frameMs * (1 - this.o.headroom) - this.o.cpuReserveMs);
  }

  kiloPxAt(scale) { return this.o.kiloPx1 * scale * scale; }
  mpAt(scale) { return this.kiloPxAt(scale) / 1000; }

  msAt(scale, spp, denoise = this.denoise) {
    let ms = this.o.traceMsPerSampleKiloPx * this.kiloPxAt(scale) * Math.max(1, spp);
    if (denoise) ms += this.o.filterMsPerMegaPx * this.mpAt(scale);
    return ms;
  }

  get traceCalibrated() { return this.traceMeasures >= this.o.calibrateMeasurements; }
  get filterCalibrated() { return this.filterMeasures >= this.o.calibrateMeasurements; }

  /**
   * Should the caller measure this frame as two submissions (trace, then filter+present) so the split
   * can be learned? The answer is the whole calibration protocol: nothing during warm-up, then every
   * frame until both terms have enough readings, then every `probeEvery` frames so a price that drifts
   * (thermal, contention, a different resolution) is caught. The caller must pass the resulting
   * traceMs/filterMs to observe() with `probe = true`.
   *
   * The filter submission is encoded on a probe frame even when the denoiser is currently off, because
   * that is the only way to price it without changing what the user sees - and the native controller
   * will not hand the filter back either until it knows what it costs.
   */
  wantsProbe(frameIndex) {
    if (!this.o.auto) return false;
    if (frameIndex < this.o.probeWarmupFrames) return false;
    const needsTrace = this.traceMeasures < this.o.calibrateMeasurements;
    const needsFilter = this.filterMeasures < this.o.calibrateMeasurements;
    if (needsTrace || needsFilter) return true;
    return (frameIndex - this.o.probeWarmupFrames) % this.o.probeEvery === 0;
  }

  /**
   * Feed one frame's measurements.
   * @param frameMs   wall clock for the whole-frame submission (what the user would feel)
   * @param traceMs   wall clock for the trace submission alone (0 when not measured this frame)
   * @param filterMs  wall clock for the denoiser submission alone (0 when not measured / not run)
   * @param targetMs  the deadline to hold, defaults to 1000/targetFps
   * @param probe     true when this frame was run as two submissions: it prices the terms but it is not
   *                  evidence about the frame budget, because the extra round trip is in its time.
   */
  observe(frameMs, traceMs, filterMs, targetMs, probe = false) {
    const target = targetMs || 1000 / this.o.targetFps;
    if (this.settle > 0) { --this.settle; return null; }
    // A frame rendered straight after a change carries a partly-warm accumulator and partly-cold
    // shader state, so the first settleFrames readings are thrown away rather than fed to the model.
    if (probe) {
      ++this.probeFrames;
      this.#learnTrace(traceMs);
      // The probe's second submission ran temporal + all five a-trous passes, so its cost per megapixel
      // is read directly rather than as a residual of two models.
      if (filterMs > 0) this.#learnFilter(filterMs);
      return null;
    }

    ++this.frames;
    if (frameMs > target) ++this.lateFrames;
    const a = 0.25;
    this.avg = this.avg <= 0 ? frameMs : a * frameMs + (1 - a) * this.avg;
    this.#learnTrace(traceMs);
    // Only a frame that really ran the filter can price it: with the denoiser off the caller's
    // "filter" submission is present-and-copy, and pricing that as the SVGF would be exactly the kind
    // of number that looks learned and is not. (A probe frame always runs the filter, so it is priced
    // on the probe path above regardless of the current state.)
    if (this.denoise && filterMs > 0) this.#learnFilter(filterMs);

    // The model may not keep a price its own workload contradicts - and the contradiction may point
    // either way. It used to be allowed to point only upward, on the theory that a price may only be
    // raised by a frame and lowered by a measurement, because an unusually fast frame is the submission
    // floor rather than a cheap trace. That stopped the optimistic shrink-flapping it was written for,
    // and made the opposite failure certain: one slow sample, from a compositor hiccup or anything else,
    // over-priced the trace term for the rest of the session and every decision after it cut harder than
    // the truth. So a contradiction is now evidence in both directions, but the evidence is the *median*
    // of the recent frame/model ratios rather than the latest one, and it has to be past a deadband -
    // which is what a single sample was never allowed to be, in either direction. See #correctPrice.
    const pred = this.msAt(this.scale, this.spp, this.denoise);
    // The ratio is taken on the part of the frame the model is meant to explain. `frameMs` is the whole
    // loop iteration and `budget()` already sets aside `cpuReserveMs` for the loop's own stages, so
    // comparing the raw wall clock against a GPU-work prediction charges that reserve to the trace term
    // as well - a standing ~30 % upward bias on a 9.8 ms budget, and the reason a price corrected from
    // one-way ratios could only ever climb.
    const submittable = Math.max(0.05, frameMs - this.o.cpuReserveMs);
    if (pred > 0 && frameMs > 0) {
      this.ratios.push(submittable / pred);
      if (this.ratios.length > this.o.correctionWindow) this.ratios.shift();
    }
    this.#correctPrice();
    return this.decide();
  }

  /**
   * Fold a sustained contradiction between the model and what the frames actually cost into the trace
   * term, in whichever direction the median points. A single-submission frame can charge its excess to
   * only one term, so the trace takes it - the same reasoning as before, minus the ratchet.
   *
   * Three things keep this from becoming a knob one noisy frame can turn: the evidence is the median of
   * the last `correctionWindow` frame/model ratios, that median has to sit outside 1 +/- `correctionBand`
   * to count as a contradiction at all, and the window is emptied once a correction is applied, so the
   * controller must collect fresh agreement before it can reprice again. The step is still capped at
   * `maxCorrectionPerStep` in either direction, so a repricing cannot teleport.
   */
  #correctPrice() {
    if (this.ratios.length < this.o.correctionsNeeded) return;
    // At most once per decision window. Without this a headless Edge run repriced the trace term 141
    // times in six seconds: the evidence was a median each time and the scale never moved, but a price
    // that churns every three frames is not a settled price, and it makes the run log unreadable.
    if (this.frames - this.lastCorrection < this.o.decideEvery) return;
    const sorted = this.ratios.slice().sort((x, y) => x - y);
    const med = sorted[sorted.length >> 1];
    const band = this.o.correctionBand;
    if (med <= 1 + band && med >= 1 - band) return;
    const units = this.kiloPxAt(this.scale) * Math.max(1, this.spp);
    const cur = this.o.traceMsPerSampleKiloPx;
    const excess = (med - 1) * this.msAt(this.scale, this.spp, this.denoise) / units;
    const cap = this.o.maxCorrectionPerStep;
    let want = Math.min(Math.max(cur + excess, cur / cap), cur * cap);
    // A whole-frame contradiction cannot say *which* term it is charging, and the two-term model has two
    // of them: the first Edge run of this section had the filter under-priced at its 1.0 ms/MP seed, so
    // the whole-frame excess was the filter's and the ratchet booked it to the trace, which is how the
    // price doubled to 0.01649. Corrected both ways, the mirror image appears - a headless Edge run on
    // 2026-11-13 let the filter price run up to 12 ms/MP and the trace price then fell through the floor
    // to its 1e-4 clamp, predicting a 0.05 ms frame. So the correction is bounded by what the split
    // probes measured *directly for this term*: outside that bracket a whole-frame reading is not
    // evidence about the trace, it is evidence about something else in the frame.
    if (this.traceUnits.length >= 2) {
      const probed = this.traceUnits.slice().sort((x, y) => x - y)[this.traceUnits.length >> 1];
      want = Math.min(Math.max(want, probed * 0.5), probed * 3);
    }
    this.o.traceMsPerSampleKiloPx = Math.min(Math.max(want, 1e-4), 500);
    this.modelCorrections++;
    this.priceMoves.push({
      frame: this.frames, dir: med > 1 ? 'up' : 'down', ratio: +med.toFixed(3),
      from: +cur.toFixed(5), to: +this.o.traceMsPerSampleKiloPx.toFixed(5),
    });
    this.calibrated = true;
    this.lastCorrection = this.frames;
    this.ratios.length = 0;   // spent: fresh evidence, not the same median repricing every frame
  }

  #learnTrace(traceMs) {
    if (!(traceMs > 0)) return;
    const a = 0.25;
    this.traceAvg = this.traceAvg <= 0 ? traceMs : a * traceMs + (1 - a) * this.traceAvg;
    ++this.traceMeasures;
    this.#price(this.traceUnits, traceMs / (this.kiloPxAt(this.scale) * Math.max(1, this.spp)),
      'traceMsPerSampleKiloPx');
    this.calibrated = true;
  }

  #learnFilter(filterMs) {
    const a = 0.25;
    this.frameAvg = this.frameAvg <= 0 ? filterMs : a * filterMs + (1 - a) * this.frameAvg;
    ++this.filterMeasures;
    this.#price(this.filterUnits, filterMs / Math.max(1e-4, this.mpAt(this.scale)), 'filterMsPerMegaPx');
  }

  /**
   * Fold one measured unit price into a term. The MEDIAN of the last few readings is the price, not the
   * average and not the first one, because a submission's CPU-GPU round trip on the open web is
   * bimodal - 0.17 - 1.0 ms measured in Chromium, and far worse in Firefox, where the first split probe
   * of a session measured the a-trous chain at several times the cost the same frames reach later. A
   * first sample therefore has to be allowed to be wrong in both directions: the old code clamped each
   * step to a factor of two from whatever it already believed and, combined with the one-way correction
   * below, that left one expensive probe over-pricing the trace term for the rest of the session.
   *
   * The measurement still includes the submission floor, so what is learned is an upper bound on the
   * GPU cost - the right direction to err in, because a controller that under-prices overshoots.
   */
  #price(samples, unit, key) {
    if (!(unit > 0)) return;
    samples.push(unit);
    if (samples.length > this.o.priceWindow) samples.shift();
    const sorted = samples.slice().sort((x, y) => x - y);
    let med = sorted[sorted.length >> 1];
    if (samples.length > 1) {
      med = Math.min(Math.max(med, this.o[key] / 2), this.o[key] * 2);
      med = 0.4 * med + 0.6 * this.o[key];
    }
    this.o[key] = Math.min(Math.max(med, 1e-4), 500);
  }

  /** Spread of the last few price readings, so a noisy term is visible instead of merely averaged away. */
  #spread(samples) {
    if (samples.length < 2) return 0;
    const lo = Math.min(...samples), hi = Math.max(...samples);
    return lo > 0 ? hi / lo - 1 : 0;
  }

  decide() {
    if (!this.o.auto) return null;
    if (++this.decideFrames < this.o.decideEvery) return null;
    this.decideFrames = 0;

    // Stop deciding, once and visibly. Every mechanism below is hysteresis, but hysteresis still leaves
    // a controller free to walk the scale knob across a long session if the frame time wanders, and a
    // picture that changes size every few seconds reads as instability no matter how reasoned each step
    // was. After `maxChanges` moves it holds whatever it has and says so once.
    if (this.o.maxChanges > 0 && this.decisions.length >= this.o.maxChanges) {
      if (this.settled) return null;
      this.settled = true;
      if (this.exhausted) return null;   // `exhausted` has already said the same thing, once
      const c = {
        what: 'settled', to: 'stopped_moving',
        why: `${this.o.maxChanges} changes made; holding scale ${this.scale.toFixed(2)}, ${this.spp} spp, `
          + `denoiser ${this.denoise ? 'on' : 'off'} at ${this.avg.toFixed(1)} ms against a `
          + `${this.budget().toFixed(1)} ms budget. The controller will not move again.`,
      };
      c.ms = +this.avg.toFixed(2); c.budget = +this.budget().toFixed(2);
      c.spp = this.spp; c.scale = +this.scale.toFixed(3); c.denoise = this.denoise;
      this.decisions.push(c);
      return c;
    }

    const budget = this.budget();
    const spend = budget;
    const over = this.avg > spend * 1.04;
    const under = this.avg < this.o.deadband * spend;
    if (over) { ++this.overStreak; this.underStreak = 0; } else if (under) { ++this.underStreak; this.overStreak = 0; }
    else { this.overStreak = this.underStreak = 0; }
    // A separate, stricter streak for spending the resolution knob: `over` is only 4 % past the budget,
    // which is inside what this browser's compositor does to one frame's wall-clock time. A cut is now
    // made on `cutsNeeded` consecutive deciding points each worse than `cutDeadband` past the budget, so
    // the controller can reach a scale and sit on it instead of walking down one deciding point at a
    // time. In the measured Edge run of docs section 12 the three cuts were decided at 21.1, 11.7 and
    // 11.6 ms against a 9.8 ms budget - the last two each decided on a single deciding point past 110 %
    // of budget, with no confirmation, while the price ratchet doubled the trace price underneath them.
    if (this.avg > spend * (1 + this.o.cutDeadband)) ++this.missStreak; else this.missStreak = 0;

    const lateFrac = this.frames >= 12 ? this.lateFrames / this.frames : 0;
    let change = null;

    // 1. Surrendering the denoiser. Gross lateness needs only two readings, because measuring a frame
    //    that costs three times the budget is not information, it is a bad-looking application.
    //    It is given up only once its price is known, or once waiting has stopped being productive:
    //    an unpriced surrender is the mistake this controller was rebuilt to stop making (it threw the
    //    filter away and then could never price it, restore it, or explain itself).
    const mayGive = this.denoise && !this.denoiseUser && this.flips < this.o.maxFlips;
    // ...and under `priority: 'pixels'` only once there is no resolution left to give up either, which
    // is the whole divergence from the native controller (see PRIORITY at the top of this file). Until
    // then `giveNow` is false, so the pixel path below is the one that runs, and the measured-step
    // fallback is allowed to fire with the filter still on.
    // `sppUser` means the sample count is not a knob this controller may touch, so for "is there
    // anything left to give" the samples are already at their floor: the pixels and the filter are all
    // that is left, which is exactly the authority the user left it with.
    const atSppFloor = this.spp <= 1 || this.sppUser;
    const atFloor = this.scale <= this.o.minScale + 1e-9 && atSppFloor;
    const giveNow = mayGive && (this.o.priority !== 'pixels' || atFloor);
    const priced = this.filterCalibrated;
    const tooLateToWait = this.frames >= this.o.calibrateByFrame;
    if (giveNow && over && (priced || tooLateToWait)) {
      const gross = this.avg > budget * 3 && this.overStreak >= 2;
      if (gross || lateFrac >= 0.25 || this.overStreak >= this.o.overStreak + 6) {
        change = {
          what: 'denoise', to: false,
          why: (gross ? 'grossly over budget' : 'late frames')
            + (priced ? `, filter priced at ${this.o.filterMsPerMegaPx.toFixed(1)} ms/MP`
              : ' (filter UNPRICED: calibration never arrived)'),
          priced,
        };
      }
    } else if (giveNow && over && !priced && !tooLateToWait) {
      // Not a silent stall: this is the one case where being over budget deliberately does nothing.
      this.pendingGive = true;
    }

    if (!change) {
      // Cutting costs resolution, so it asks for the stricter streak: two deciding points in a row past
      // the cut deadband, not merely two readings 4 % past the budget. Climbing keeps its own, longer
      // streak, which is what stops the optimistic direction of the same flap.
      const mayReduce = this.overStreak >= this.o.overStreak
        && this.missStreak >= this.o.cutsNeeded && !giveNow;
      const mayClimb = this.underStreak >= this.o.underStreak && !over;
      // What the model wants, in the order quality is worth it: samples first while the denoiser is
      // off (they are the whole quality story without a filter), then resolution.
      const cap = this.denoise ? 1 : this.o.maxSpp;
      const limit = mayReduce ? spend : budget * this.o.climbMargin;
      let wantSpp = Math.max(1, Math.min(cap, Math.floor(
        (limit - (this.denoise && priced ? this.o.filterMsPerMegaPx * this.mpAt(this.scale) : 0)) /
        (this.o.traceMsPerSampleKiloPx * this.kiloPxAt(this.scale)))));
      let wantScale = this.scale;
      if (this.msAt(this.scale, 1) > limit) {
        // Not even one sample at this size fits, so shrink to where one does - never below the floor.
        const perUnit = this.msAt(1, 1, this.denoise);
        wantScale = perUnit > 0 ? Math.sqrt(limit / perUnit) : this.o.minScale;
      }
      wantScale = Math.min(Math.max(wantScale, this.o.minScale), this.o.maxScale);

      // Asymmetric steps - and the asymmetry has to be in both directions of the code, not only in the
      // comment above: a frame that misses its deadline every time is not a borderline case, so a cut is
      // worth making at `cutMargin` (5 %), while climbing has to be worth a real change (two samples, or
      // a fifth of the current scale) or the controller flaps either side of the line. Before this, one
      // 20 % threshold governed both, and a run that measured 10.2 ms against a 9.17 ms budget sat there
      // all session because the fix it wanted was a 12 % cut.
      const minMove = mayReduce ? this.o.cutMargin : 0.2;
      const scaleDiffers = Math.abs(wantScale - this.scale) > minMove * this.scale;
      // Once the user has set the sample count, the controller cannot move it in either direction
      // (app.js, the Up/Down keys) - the same rule the filter has after one press of N. It still owns
      // the resolution and the filter, so the scale path above and the flip path below are unaffected.
      const sppMove = !this.sppUser && (mayReduce ? wantSpp < this.spp
        : (mayClimb && wantSpp - this.spp >= 2));
      const scaleMove = scaleDiffers && (mayReduce ? wantScale < this.scale : (mayClimb && wantScale > this.scale));
      if (scaleMove) {
        change = { what: 'scale', to: wantScale, why: mayReduce ? 'over budget' : 'budget to spare' };
      } else if (sppMove) {
        change = { what: 'spp', to: wantSpp, why: mayReduce ? 'over budget' : 'budget to spare' };
      } else if (this.denoiseUser === false && !this.denoise && this.flips < this.o.maxFlips &&
                 this.underStreak >= this.o.underStreak + 3 && this.avg < budget * 0.5 && priced) {
        // Give the filter back only when there is clearly room for it *and* its price has been measured
        // (the native rule), and count the flip: a controller that flaps the denoiser on and off is
        // worse than one that never gave it up.
        change = { what: 'denoise', to: true, why: 'well under budget, filter priced' };
      }
    }

    // 2. Nothing the model asked for was available while still over budget. Historically that is where
    //    this controller stopped: it believed its own prediction, so "want" equalled "have" at `scale 1`
    //    and the resolution knob was never touched. Step from the measurement instead - pixels cost
    //    milliseconds close to linearly, so the scale that would fit the budget is the current scale
    //    times sqrt(budget / measured). Same 20 % minimum-move rule as the model path, so it cannot
    //    nibble its way down one percent a frame.
    if (!change && over && this.overStreak >= this.o.overStreak && !giveNow) {
      const want = this.scale * Math.sqrt(Math.max(1e-4, (budget * 0.85) / this.avg));
      const to = Math.min(Math.max(want, this.o.minScale), this.scale);
      const sustained = this.missStreak >= this.o.cutsNeeded;
      if (sustained && to <= this.scale * (1 - this.o.cutMargin) - 1e-9) {
        change = {
          what: 'scale', to,
          why: `measured step: model offered no move at ${this.avg.toFixed(1)} ms against ` +
               `${budget.toFixed(1)} ms (model predicted ${this.msAt(this.scale, this.spp).toFixed(1)} ms)`,
          measured: true,
        };
      } else if (sustained && !this.sppUser && this.spp > 1) {
        change = { what: 'spp', to: Math.max(1, this.spp - 1), why: 'measured step: samples are the only knob left', measured: true };
      } else if (this.scale <= this.o.minScale + 1e-9 && atSppFloor) {
        // Nothing left to give. Say so, once, instead of going quiet.
        if (!this.exhausted) {
          this.exhausted = true;
          this.exhaustedAt = this.frames;
          change = {
            what: 'exhausted', to: 'nothing_left',
            why: `cannot reach ${this.o.targetFps} fps: ${budget.toFixed(1)} ms budget against ` +
                 `${this.avg.toFixed(1)} ms at ` +
                 (this.sppUser ? `${this.spp} spp (the user's, pinned)` : '1 spp') +
                 `, scale ${this.scale.toFixed(2)} (the floor)` +
                 (this.denoise ? '' : ', no denoiser') +
                 `. Needs a cheaper trace or a lower resolution floor than this demo allows.`,
          };
          change.ms = +this.avg.toFixed(2);
          change.budget = +budget.toFixed(2);
          change.spp = this.spp;
          change.scale = +this.scale.toFixed(3);
          change.denoise = this.denoise;
          this.decisions.push(change);
          return change;
        }
      }
    }

    if (!change) return null;
    change.ms = +this.avg.toFixed(2);
    change.budget = +budget.toFixed(2);
    change.spp = this.spp;
    change.scale = +this.scale.toFixed(3);
    change.denoise = this.denoise;
    if (change.what !== 'exhausted') {
      this.apply(change);
      this.decisions.push(change);
      if (change.what === 'denoise') this.pendingGive = false;
    }
    return change;
  }

  apply(change) {
    if (change.what === 'spp') this.spp = change.to;
    else if (change.what === 'scale') this.scale = change.to;
    else if (change.what === 'denoise') {
      this.denoise = change.to;
      ++this.flips;
    }
    this.settle = this.o.settleFrames;   // readings right after a change are not representative
    this.overStreak = this.underStreak = 0;
    this.missStreak = 0;   // the next cut starts from zero evidence, whatever this one was
    this.ratios.length = 0;  // and so does the price evidence: this config's ratios are gone
    // A change of scale or samples invalidates both prices' context: keep the numbers (they are
    // per-unit, so they transfer) but require the terms to be re-confirmed by probes, which is what
    // wantsProbe() will now ask for if the counts fall below calibrateMeasurements again.
  }

  /** Whether the last measured average is inside the budget it was given. */
  withinBudget() { return this.avg <= this.budget() * 1.04; }

  /** Public accessor for the spread of recent price readings for a term (`hi/lo - 1`). */
  spread(samples) { return this.#spread(samples || []); }

  state() {
    return {
      spp: this.spp, scale: +this.scale.toFixed(3), denoise: this.denoise,
      sppUser: this.sppUser, denoiseUser: this.denoiseUser,
      avgMs: +this.avg.toFixed(2), budgetMs: +this.budget().toFixed(2),
      tracePerKiloPxSpp: +this.o.traceMsPerSampleKiloPx.toFixed(5),
      filterPerMegaPx: +this.o.filterMsPerMegaPx.toFixed(3),
      flips: this.flips, lateFrac: this.frames ? +(this.lateFrames / this.frames).toFixed(3) : 0,
      predictedMs: +this.msAt(this.scale, this.spp).toFixed(2),
      tracePriced: this.traceCalibrated, filterPriced: this.filterCalibrated,
      probes: this.probeFrames, corrections: this.modelCorrections,
      correctionsUp: this.priceMoves.filter(m => m.dir === 'up').length,
      correctionsDown: this.priceMoves.filter(m => m.dir === 'down').length,
      withinBudget: this.frames > 0 && this.withinBudget(), exhausted: this.exhausted,
      settled: this.settled,
    };
  }
}
