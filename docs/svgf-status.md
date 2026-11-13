# SVGF implementation status

Working notes for the denoiser build. `docs/svgf-spec.md` is the algorithm reference (derived from
the KIT preprint of Schied et al. 2017, equations checked verbatim); this file is what has actually
been built, what is decided, and what is left.

Baseline: the user committed before starting, so the pre-denoise renderer is recoverable from git.
`--denoise` must stay **off by default** until it is proven, and with it off the rendered image must
be identical to the committed build.

## Decisions taken (deviating from the spec where noted)

1. **No position image.** World position is reconstructed from stored reciprocal depth plus the
   camera basis, so the reprojection needs no extra buffer. Depth is stored as `1/(1+t)` because a
   far sentinel overflows a half float and because the filter compares depths relatively.
2. **Three feature images, not five.** `gbufImage[0]` albedo (rgb + w = feature found),
   `gbufImage[1]` normal (rgb + w = roughness), `gbufImage[2]` (x = reciprocal depth, y = primary hit
   exists, z = material index for the validity test). All RGBA16F, same format as the HDR target so
   the existing format-capability probe covers them.
3. **Features come from the first sample of the frame, and from the first non-delta surface.**
   `pathTrace` reports through module-scope globals (`gbAlbedo`, `gbNormal`, `gbDepth`, `gbPrimary`,
   `gbHave`, `gbRough`) instead of out-parameters, so `fallback.comp` keeps tracing identical paths.
4. **The ray-query fallback does not write feature images.** If `usingComputeFallback` is set,
   denoising must be forced off with a log line rather than silently producing garbage.
5. **v1 filters one combined channel.** The spec's correction is right: the paper separates direct
   from indirect, not diffuse from specular by roughness, and its own evaluation used a single
   channel. Known cost: sharp reflections of the emissive orbs blur, and specular can ghost because a
   reflection is not anchored to the surface showing it. A two-channel split is a v2 change.
6. **Feature images are always allocated** (even with denoising off) so the trace descriptor cannot
   hold a null view. The history and wavelet images are allocated only when denoising is on.
7. Accumulation and temporal filtering are mutually exclusive: with denoising on, the per-frame
   sample count is 1, the raygen stores instead of adds, and the present pass must not divide by an
   accumulated frame count.

## Done

- `shaders/sceneblock.glsl`: `camera[16]`; new defines `CAM_PREV_ORIGIN/RIGHT/UP/FWD` (rows 8-11,
  row 11.w = previous frame usable), `CAM_FILTER` (row 12: x = feature writes on, y = channel view,
  z = history cap), `GBUF_ALBEDO/NORMAL/DEPTH` (rows 13-15) for a future CPU-side copy.
- `shaders/pt_common.glsl`: feature globals + `gbBegin()`, and the capture inside the loop. Depth is
  the primary hit; albedo/normal come from the first surface that is not delta (glass, or metal with
  roughness below 0.03 are followed through); emissive surfaces report nothing.
- `shaders/main.rgen`: `gbufImage[3]` at set 0 binding 6, per-pixel feature capture at `s == 0`,
  writes when `CAM_FILTER.x > 0.5`, and a channel view selected by `CAM_FILTER.y`
  (1 albedo, 2 normal, 3 depth) that writes straight to the HDR target for inspection.

## To do, in order

1. **Phase A plumbing** (in progress): `SceneGpuData::camera` to 16 rows; three feature images
   created and destroyed beside the HDR target in `recreateHdrTargets`; descriptor layout binding 6
   with count 3, pool size, and `writeTraceDescriptors` writing the three views in `GENERAL`;
   `fillSceneData` publishing the previous frame's basis (which must be the *blended* camera, so the
   free camera and the orbit both work) and the filter row; `--denoise`, `--gbuf CHANNEL` flags.
   Verify by capturing each channel view and looking at them: mirror and glass must show the albedo
   of what the reflection lands on, sky must be black in albedo, depth must ramp cleanly.
2. **Phase B**: temporal filter compute pass — reprojection by projecting the reconstructed world
   point with `CAM_PREV_*`, validity (material match, normal dot above 0.9, depth-gradient test),
   EMA with the per-pixel history length, moments for variance, history ping-pong. Verify noise
   reduction numerically at 1 spp in a flat floor band.
3. **Phase C**: five wavelet iterations, dilation 1,2,4,8,16, kernel {1/16,1/4,3/8,1/4,1/16},
   weights `wz*wn*wl` with `sigma_l_eff = sigma_l * sqrt(4 s^2 V)`, variance propagated with
   `sum(h^2 w^2 V)/(sum h w)^2`, remodulation by albedo folded into the last iteration.
4. **Phase D**: retarget the present pass at the filtered image (a `displayImage[slot]` pointer so
   the hold-frame machinery keeps working), clear history wherever `CAM_FRAME.z` resets, let the
   quality controller spend its floor on 1 spp plus scale, measure the pass cost with the existing
   GPU timestamps, and compare stills at equal sample counts.

## Traps in this codebase, already known

- Recreating the HDR target happens on resize *and* on every quality change; anything with temporal
  state must be cleared there, and the `holdPending`/`holdShown` pair exists precisely because a
  freshly allocated target would otherwise be displayed empty.
- Uploads are submitted with a queue drain on purpose; a deferred variant crashed the driver.
- The present pass reads a *sampled* view of the HDR target; a storage image cannot be sampled
  without the layout transition, and the code currently transitions GENERAL -> SHADER_READ_ONLY
  before present.
- The image format is chosen by probing storage + sampled + transfer + blit; reuse it rather than
  assuming a format is supported.

## Phase A: built and verified

Descriptor binding 6 (three storage images) wired, feature images allocated beside the HDR target and retired with it, previous-frame basis published in camera rows 8-11 with the validity flag in row 11.w, filter control in row 12. Flags: --denoise, --gbuf albedo|normal|depth. Captures in gb_albedo, gb_normal, gb_depth, gb_default.


## Phase B: shader written, not yet dispatched

`shaders/svgf_temporal.comp` compiles (the shader step globs `*.comp`) and is currently dead code -
nothing creates a pipeline for it. Interface is descriptor set 1, bindings 0..9: 0 noisy radiance
(the HDR target), 1 albedo, 2 normal, 3 depth, 4/5/6 history in (colour, moments, depth+material),
7/8/9 the matching outputs. It reads the scene block from set 0 binding 2 through
`sceneblock.glsl`, so its pipeline layout must include the existing `traceSetLayout` as set 0 and a
new filter layout as set 1.

Left to do, in order:

1. Six more images (three history, three output), RGBA16F, GENERAL at creation, allocated only when
   `cfg.denoise` is on. History must ping-pong: outputs become next frame's inputs.
2. Compute pipeline, descriptor set layout, pipeline layout, pool; write the sets wherever the HDR
   target is recreated.
3. Record the dispatch in `core2.cpp` after the trace dispatch, with barriers from RAYGEN writes to
   COMPUTE reads, and a write->read barrier before anything that consumes the output.
4. Confirm `CAM_FRAME.z` is set on the first frame and on every quality change - that is what stops
   history from a previous resolution being read.
5. Phase C: `svgf_atrous.comp`, five iterations at dilation 1,2,4,8,16, kernel
   {1/16,1/4,3/8,1/4,1/16}, weights wz*wn*wl, sigma_l_eff = 4*sqrt(4*s*s*V), variance propagated as
   sum(h*h*w*w*V)/pow(sum(h*w),2), remodulation by albedo in the last iteration.
6. Phase D: present reads the remodulated output through a `displayImage[slot]` pointer so the
   hold-frame path survives, and the present divisor is bypassed while denoising. Then measure GPU
   cost with the existing timestamps and a numeric noise figure (mean absolute deviation from a 5x5
   box blur over a flat floor band) at 1 spp, filter off and on.

Deviation recorded in the shader: no normal-history rejection (needs another history image) and a
plain relative depth tolerance instead of the paper's local-plane gradient form, which costs history
at grazing angles - noisier there, not wrong.

## Phase B: dispatching, measured, one false result corrected

The temporal pass is created, bound (set 1, ten storage images) and dispatched after the trace, with
the filtered state copied back into the slot's history. Windowed with --denoise: 58.7 fps, exit 0.

A first noise measurement (mean absolute deviation from a 5x5 box blur over a floor band) gave raw
1 spp = 30.0, progressive accumulation over 32 frames = 22.6, filtered over the same 32 frames =
2.5, which looked like a twelve-fold win. It was wrong: the capture path read the accumulation
target, and while filtering that target holds only the last frame, so the image measured was mostly
black. The metric fell because the signal went missing, not because noise was removed. The readback
now takes the filtered image. Lesson recorded: a filter that reduces energy is not a filter that
reduces noise, and the number must be paired with looking at the image.

### Still broken at the end of this round - do not trust the numbers above

After pointing the readback at the filtered image, the capture is 26 KB with variance exactly 0.000:
a single flat value, almost certainly black, which means the compute pass wrote nothing the readback
can see. The three figures (30.0 / 22.6 / 2.5) are therefore not a comparison of anything; only the
raw and progressive ones are real, and 2.5 was a black image.

Order to check, cheapest first:

1. Was the pass recorded at all? Add a one-shot log at the dispatch: cfg.denoise, hdrWidth/Height,
   whether denOut[slot].image is non-null. A flat target that was never cleared (it is created
   undefined and never zeroed) reads back as exactly this.
2. Do the cleared state and the reset path agree? denOut is never cleared, so if every invocation
   returns early on the bounds test nothing is written. CAM_FRAME.xy is the trace extent, which
   should equal the dispatch extent - print both.
3. Is the descriptor set actually written for capture? writeTraceDescriptors is called at target
   creation; capture mode creates its targets through the same path, but confirm the denoiser set
   there points at the capture-size images and not at ones retired by a later resize.
4. Only after the image is non-flat, re-run the comparison. A denoiser's quality claim needs the
   image looked at as well as the number: this metric falls when signal disappears, which is exactly
   how the false result slipped through once.

The windowed run at 58.7 fps proves nothing about correctness, only that it did not crash.

## Phase B verified working (round 2)

Two bugs, both found by reading rather than guessing:

1. The compute pipeline layout reuses the trace set as set 0, but the dispatch bound only the filter
   set - which installed it in slot 0. The shader then read a garbage camera, failed its bounds test
   for every pixel and wrote nothing, and the never-cleared target read back as a flat black image.
   Binding both sets fixed it.
2. The capture path divides the read-back radiance by the accumulated frame count. The filtered
   image is already a mean, so that divisor made it 32x too dark and turned the sky black. The
   divisor is now skipped for the filtered view.

Equal-cost comparison, 1280x720, 32 frames at 1 sample per pixel, floor band between y=560 and 640,
noise = mean absolute deviation from a 5x5 box blur, luminance = image mean after tone mapping:

| path | noise | mean luminance |
| --- | --- | --- |
| single frame, 1 spp, unfiltered | 30.03 | 142.0 |
| 32 frames, progressive accumulation (same cost) | 22.64 | 144.4 |
| 32 frames, temporal filter (this phase) | 18.89 | 187.4 |

So the temporal stage already beats an accumulator costing the same. The brightness is 30% high and
the checkerboard on the floor is gone in the filtered view; both are expected, not defects - what is
displayed at this stage is the *demodulated* field (radiance divided by albedo), which by design
removes the albedo's own pattern, and the multiply back happens at the end of the wavelet chain that
does not exist yet. The vanished tiles are therefore evidence the demodulation is working.

## Remaining

- Phase C: svgf_atrous.comp - five iterations, dilation 1,2,4,8,16, kernel {1/16,1/4,3/8,1/4,1/16},
  weights wz*wn*wl, sigma_l_eff = sigma_l*sqrt(4*s*s*V), variance propagated as
  sum(h*h*w*w*V)/pow(sum(h*w),2), remodulation by albedo on the last iteration. Expect the mean
  luminance to return to roughly the unfiltered value and the noise to fall well below 18.9.
- Phase D: quality controller (1 spp floor while filtering), the windowed present path reading the
  filtered image through a displayImage pointer so the hold-frame logic survives, and the fallback
  pipeline forcing denoise off with a log line.
- Add svgf_atrous.comp to STAGES in tools/build_shaders.py - the list is explicit and a shader that
  is not in it is silently never compiled.

## Phase C built; two faults found, one of them breaking the default path

The wavelet pass (five iterations, dilation 1,2,4,8,16, kernel {1/16,1/4,3/8,1/4,1/16}, weights
wz*wn*wl with the variance-scaled luminance threshold, variance propagated as sum(h^2 w^2 V) over
(sum h w)^2, albedo multiplied back on the last iteration) is written, dispatched, and runs.

Two faults, both mine, both worth remembering:

1. Bulk text replacement put the pipeline-creation block in front of every occurrence of an anchor
   line, so four copies landed in four different functions. The copies that ran early built a
   pipeline around a layout that did not exist yet and killed the process with an access fault
   before the first frame. Deleted down to one copy, placed where the layouts exist.
2. Allocating three descriptor sets needs an array of three layout handles. Passing the address of a
   single member made the driver read three handles from it and walk into the rest of the struct -
   which faulted during init and broke the default, non-denoising path as well. Fixed with a local
   array. This was the crash: after it, windowed with --denoise runs clean at 53 fps, GPU 6.9 ms.

Lesson for this codebase: edit with targeted replacements verified by a count, never a global
replace, and after any change confirm the path where the new feature is OFF.

## Measurement that does not add up - investigate first

After Phase C the filtered capture reads noise 18.885, mean luminance 145.3. The luminance is now
right, which is the remodulation working (it was 187.4 when the demodulated image was displayed).
But 18.885 is *identical to three decimal places* to the temporal-only measurement taken before the
wavelet pass existed, while the image it was measured on is a different brightness. Two different
images cannot produce the same figure to that precision by chance, so one of these is true:

- the capture is not reading the wavelet output (the readback chooses denPP[slot][1][0] - check the
  slot index actually used by capture, and that it is the slot the dispatch wrote);
- the wavelet pass is a no-op on this content (all non-centre weights suppressed, so every tap sum
  reduces to the centre); sigmaZ of 0.02 in reciprocal-depth units is the obvious candidate;
- the metric is insensitive to the thing that changed.

md5 of the current capture is recorded in the round notes. Re-run with a variance/weight debug view,
or force the wavelet to a single iteration with sigmaZ large, and see whether the number moves at
all. Until then the claim is: temporal filtering works and is measured; the wavelet stage runs
without crashing and restores correct brightness, and its filtering effect is unverified.

## The identical number explained, and what the wavelet actually does

Ruled out by experiment, not by reading the code:

- Not a plumbing failure. Three values of the depth threshold (0.02, 0.5, 5.0) via the new
  --filter-z flag produce three different images (distinct md5s), so the wavelet output really is
  what the capture reads, and push constants really do reach the shader.
- Not the luminance threshold. --filter-l 1000 makes that weight fully permissive and the floor band
  moves only 18.895 -> 18.584.

So the earlier coincidence was coincidence, and the real finding is narrower than I implied: the
wavelet stage runs and changes the image, but contributes almost nothing to smoothness. The
improvement that is measured and reproducible comes from the temporal pass:

| path, floor band, 1280x720 | noise | mean luminance |
| --- | --- | --- |
| one frame at 1 spp | 30.03 | 142.0 |
| 32 frames, progressive accumulation, same cost | 22.64 | 144.4 |
| 32 frames, temporal + wavelet | 18.6-18.9 | 145.3 |

That is 38 percent below the raw frame and 18 percent below an accumulator spending the same, which
is a real result at equal cost - but it is a temporal-filter result, not yet a denoiser result. With
every weight permissive the five iterations should flatten that band to near zero, so one of them is
still collapsing and it is not the two I varied.

Next diagnostic, in order of value:

1. A difference view - the last wavelet iteration writes |out - in| instead of out, so a black image
   means the pass is the identity and a lit image localises where it acts. Add as CAM_FILTER.y 5 and
   a --gbuf diff flag.
2. A variance view of the temporal moments. If E[Y^2] - E[Y]^2 is near zero the wavelet's
   denominator is tiny and its luminance weight rejects every tap regardless of sigmaL, because
   sigmaLEff multiplies the variance rather than adding to it. This is the leading hypothesis now:
   the temporal pass stores the moments of the *smoothed* signal, whose variance really is small.
3. If variance is the cause, feed the wavelet the temporal filter's raw per-sample variance rather
   than the variance of its own output.

Also added this round: --filter-z and --filter-l, so the wavelet thresholds can be swept without a
rebuild.

## Ground truth says the denoiser is not yet earning its keep

Variance view: bright, saturating - so variance is NOT near zero and my leading hypothesis was wrong.
Difference view (|wavelet out - in| on the last iteration): also bright, so the wavelet is changing
pixels substantially, not idling.

RMSE against a converged 512-frame reference, 1280x720, lower is closer to truth:

| path | RMSE |
| --- | --- |
| one frame, 1 spp | 22.55 |
| 32 frames, progressive accumulation | 10.23 |
| 32 frames, temporal + wavelet | 10.22 |

Statistically identical. The earlier claim of an eighteen percent improvement came from a
local-smoothness metric (deviation from a 5x5 blur), which rewards blurring whether or not the image
gets closer to the truth - the same class of mistake as the black-image measurement two rounds ago.
Against ground truth the temporal filter merely matches accumulation, which is exactly what a capped
20-frame exponential average should do on a slow orbit: it is the same idea with a shorter memory.

Conclusion so far: the pipeline is correct and running, but the denoiser has no demonstrated
advantage. Where it should win is where accumulation cannot reach - very few frames per image, and
fast camera motion - which is the experiment to run next, followed by the two known weaknesses that
would cap it anyway: the missing normal-history rejection, and the plain depth tolerance that drops
history at grazing angles.

## Bit-identical with the flag off: confirmed

Comparing the committed build (git worktree at HEAD) against the current build, same pak, same
flags, --capture 1 --accum 16 --size 1280x720: 0 of 921,600 pixels differ; mean luminance 144.47 in
both. The unfiltered path is unchanged.

An earlier run of the same comparison reported both frames differing. That was a measurement race,
not a regression: the worktree checkout was still writing files while the reference build ran. When
comparing against a worktree, wait for it to finish materialising and copy the pak before running.

## Value demonstrated at last, against ground truth

RMSE to a converged 512-frame reference, 1280x720, one sample per pixel per frame:

| frames | progressive accumulation | temporal + wavelet |
| --- | --- | --- |
| 2 | 22.52 | 13.18 |
| 4 | 16.35 | 10.19 |
| 32 | 10.23 | 10.22 |

At four frames the filter matches what thirty-two accumulated frames produce, an eight-fold gain in
effective samples; at two frames it is 41 percent closer to truth. At thirty-two frames it merely
ties, which is expected - a capped exponential average and progressive accumulation are the same idea,
and the win is exactly where accumulation cannot reach. That is the case for letting the quality
controller sit at one sample per frame while filtering.

## Left before this can be called done

1. Windowed present must show the filtered image whenever --denoise is on, not only when --gbuf
   denoised was also passed; right now a plain --denoise run still presents the unfiltered target.
   Route it through a displayImage pointer so the hold-frame path keeps working.
2. Quality controller: allow the sample floor to drop to 1 while filtering, and check the cost model
   still holds (the wavelet's cost is per-frame and independent of samples, which the current model,
   cost proportional to pixels x samples, does not know about).
3. Ray-query fallback: force denoising off with a log line rather than filtering unwritten inputs.
4. README and DESIGN entries; the two debug views (--gbuf diff / variance) and --filter-z / --filter-l
   are undocumented.
5. Known quality limits to state plainly: no normal-history rejection, plain depth tolerance instead
   of the paper's gradient form, one combined channel rather than direct/indirect.

## Captures are not deterministic unless quality is pinned

Two runs of the same build with the same flags produced different pixels at the same camera time.
Cause: the adaptive controller's decisions depend on measured timings, so liveSpp at capture varies,
and the frame index that seeds the RNG varies with how many frames ran before the capture. Any
before/after pixel comparison must therefore pin quality (--no-auto-quality) and match frames by
filename, which carries the camera time. An earlier bit-identity result (0 of 921,600 pixels) was
obtained with adaptation left on and cannot be trusted as the controlled test it appeared to be.

## Bit-identity with the flag off: NOT met, and the shape of the failure is the clue

Three runs of each build, same flags, flag off, --capture 1 --accum 32:

- committed build (HEAD): all three runs identical to each other, RMSE 0.000. So the renderer *is*
  bit-reproducible across processes; the earlier guess that acceleration-structure build order made
  captures nondeterministic is wrong - it was my own change that broke reproducibility.
- current build: run1 != run2, run2 != run3, but run1 == run3. Two alternating outputs.
- run2 of the current build is identical to all three committed runs (RMSE 0.000); run1 and run3
  differ from committed by RMSE 1.409 on a 0-255 scale, about half a percent.

So the unfiltered path produces the committed image on some runs and a slightly different one on
others, alternating. The leading hypothesis is the allocation set, not the arithmetic: the current
build always allocates three feature images per slot, whether denoising is on or not, which the
committed build does not. Extra allocations shift memory addresses, and if anything in the pipeline
is sensitive to address-dependent ordering (BVH build internals, descriptor packing), the float
summation order can change without any code on the shading path changing. RMSE of half a percent is
the size of signature-rounding noise, not of a logic error.

Fix to test that hypothesis, and the right shape anyway: allocate the feature images only when
denoising or a feature view is requested, and bind the HDR view into those descriptor slots as the
placeholder - legal, because the shader's writes are gated on the same flag, so a never-read binding
never touches them. Then the default path allocates exactly what the committed build allocates, and
either bit-identity returns (confirming the cause) or it does not (and something on the shading path
really did change, which is then a bug to find rather than a hypothesis to keep).

Correction to an earlier note in this file: the claim that the unfiltered path was confirmed
bit-identical (0 of 921,600 pixels differing) was taken from one of the runs that happened to land on
the committed image. It was real evidence that the shading did not change, but it was not the
controlled test it was recorded as.

## Bit-identity cannot be tested here, because the renderer was never run-to-run reproducible

Three runs of each build, flag off, --accum 32, once with adaptation on and once with
--no-auto-quality. RMSE between runs of the *committed* build alone, before any of my changes:

| pair | adaptive | pinned quality |
| --- | --- | --- |
| run1 vs run2 | 2.40 | 2.34 |
| run2 vs run3 | 1.40 | 2.72 |
| run1 vs run3 | 2.77 | 1.40 |

Between the two builds: 1.45 to 3.58, mean 2.48 - inside the spread each build shows against itself.
So the correct statement is not that the unfiltered path is bit-identical, and not that my changes
broke reproducibility. It is that captures are not reproducible across processes in this build of the
renderer, with or without my work, and therefore bit-identity is not a property that can be
demonstrated by comparing two runs at all. Last round's table, which showed the committed build at
0.000 across three runs and concluded my allocations had broken determinism, was a lucky sample: the
committed build does vary run to run, and the parity pattern I read into the current build's results
was that same noise.

What can be claimed, with evidence:

- No detectable regression in the unfiltered path: between-build differences are statistically
  indistinguishable from run-to-run variation within either build.
- Nothing on the shading path changed. Feature writes are gated on the same flag that allocates the
  feature images, and the present branch is gated on it too, so with --denoise absent the new code is
  not executed.
- Leading cause of the run-to-run variation, still untested: acceleration-structure build order, which
  is allowed to differ per process and changes floating-point summation order. It would move pixels by
  roughly the observed fraction of a percent and would be independent of my changes, which fits.

The objective's bit-identity clause should be restated as reproducible-capture work: pin the seed
source, build the acceleration structure deterministically or reuse a cached one, and then re-run the
comparison. Until then the honest wording is no measurable change, not bit-identical.

## The wavelet does work - my earlier verdict was a measurement artifact

Isolation test: one frame, so the temporal filter has no history and the wavelet is everything.
RMSE to the converged reference, 1 sample per pixel, 1280x720:

| | RMSE |
| --- | --- |
| unfiltered, 1 spp | 22.46 |
| filtered, default weights (sigmaZ 0.02, sigmaN 128, sigmaL 4) | 12.91 |
| filtered, looser (sigmaZ 2.0, sigmaN 8, sigmaL 12) | 11.08 |

So the spatial filter alone halves the error at one sample per pixel. Two things I had wrong: I had
only ever compared at thirty-two frames, where every method converges and the wavelet genuinely adds
nothing, and I had read a threshold sweep that produced different images as evidence that the
thresholds did not matter - producing a different image is not the same as producing a better one.
Loosening the edge stopping is worth 1.8 RMSE, and the paper's constants are tuned for scenes where
depth and normal disagree strongly; a synthetic checkerboard with high-frequency albedo gives those
tests far less to work with, so a permissive setting is not surprising.

Still open: at two frames the filtered result was identical to one frame (12.91 vs 12.90), which says
the temporal pass contributed nothing in that specific case and deserves a look before the numbers at
higher frame counts are taken at face value.

## Ladder with the looser weights, and what it says about both systems

RMSE to the 512-frame reference, 1 sample per pixel per frame, 1280x720:

| frames | accumulated | temporal + wavelet |
| --- | --- | --- |
| 1 | 22.46 | 11.08 |
| 2 | 22.42 | 11.11 |
| 4 | 16.17 | 11.06 |
| 16 | 11.23 | 11.14 |

Two independent findings in that table.

**The denoiser is limited by bias, not by noise.** Adding frames to the filtered path changes nothing
at all - one frame and sixteen frames land on 11.08 and 11.14. Once a method stops improving with more
samples, the remaining error is systematic, so the work that would improve it is better edge
preservation, the missing normal-history rejection, and separating direct from indirect light - not a
larger sample budget or a longer history.

**The capture path accumulates roughly half the frames it is told to.** Averaging independent samples
should cut error with the square root of the count, and the unfiltered column does not: 1 frame 22.46,
2 frames 22.42 (no gain at all), 4 frames 16.17 (the gain expected from 2), 16 frames 11.23 (the gain
expected from 4). The effective sample count is frames divided by two. This is a bug in the still
capture, not in the denoiser - it shows in the unfiltered column, and it predates this work - and it
also means every baseline in the earlier tables of this file was measured against a weaker accumulator
than advertised. On the corrected reading, one filtered frame matches about eight accumulated
effective samples, so the honest claim is eight-fold, not sixteen.

To do next: find where capture drops every other frame - the accumulation target is per slot while
capture writes and reads slot 0, which is the obvious place to look - then re-measure the ladder, then
attack the bias floor with the normal-history test and a direct/indirect split.

## Staircase measurement: consecutive frames come in correlated pairs

RMSE to a 512-frame reference, unfiltered, one sample per frame, all in one run so the reference is
common:

| frames | 1 | 2 | 3 | 4 | 5 | 6 | 8 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| RMSE | 22.45 | 22.41 | 16.85 | 16.14 | 14.41 | 14.07 | 12.23 |

Independent samples should give 22.46 over sqrt(n): 22.5, 15.9, 13.0, 11.2, 10.0, 9.2, 7.9. The
measurements instead sit in levels of two: frames 1-2 share a level, 3-4 share one, 5-6 share one. So
each pair of consecutive frames contributes about one independent sample, and within a pair the two
frames are similar but not identical - if the second frame were a duplicate, frames 1 and 2 would have
produced exactly the same file, and they differ slightly.

Ruled out: the per-frame seed. The shader seed includes the frame index (core2.cpp line 380 passes
frameIndex into CAM_POST.y) and the capture loop sets it to a distinct value per frame, so frames do
not restart the same random stream. The mechanism is therefore somewhere else - the leading remaining
candidate is a per-frame jitter or stratification that only has two phases, which would make even and
odd frames each repeat one of two patterns.

Why it matters, and how far: it is a property of the still-capture accumulation path and predates the
denoiser work, so it affects image quality in --capture output generally, and it means every
convergence claim in this file was measured against an accumulator that gains at half the advertised
rate. On the corrected reading one filtered frame (RMSE 11.08) is worth about eight accumulated
effective samples, which is the number the README should carry.

Next diagnostic, cheap and decisive: log the frame index the GPU actually sees for each capture frame,
and dump a single frame mid-sequence with the reset flag forced on. If a forced reset at frame five
reproduces frame four's image rather than new noise, the pair effect lives in the seed path after all;
if it produces different noise, the pair effect is in the jitter, and the fix is to advance the jitter
phase with the frame index rather than with a two-phase pattern.

## Halved convergence: the CPU side is proven correct, so the cause is in the samples themselves

Verbose logging was added to the still path (one line per accumulation frame, plus one when the HDR
targets are recreated). Running --capture 1 --accum 4 --verbose:

    hdr targets recreated (1280x720)
    hdr targets recreated (1280x720)
    accum frame 0 reset=1 frameIndex=1 samples=1
    accum frame 1 reset=0 frameIndex=2 samples=2
    accum frame 2 reset=0 frameIndex=3 samples=3
    accum frame 3 reset=0 frameIndex=4 samples=4
    wrote dbgcap\frame_000_t0.55.png (4 spp)

So the tracer is told the truth: one reset, four distinct frame indices, the divisor matching the
number of frames, and no target recreation mid-sequence that would wipe the accumulator. The shader
side is equally plain: it loads the previous value and adds this frame's mean, and the readback divides
by the frame count.

Three measurements then constrain what is left:

- Brightness is right at every frame count (floor-band mean 100.94, 100.96, 103.42, 103.82 against a
  reference at 104.05). If a frame were being thrown away after its value had been added, or the divisor
  were wrong, brightness would be off by a factor - it is not. The sums are therefore correct.
- Convergence proceeds at half rate, in pairs (one and two frames both 22.45; three and four both about
  16.5; five and six about 14.2).
- The first and second images differ by RMSE 29.5. Under correct averaging of two independent frames the
  difference would be nu / sqrt(2), where nu is the per-frame noise, and nu cannot exceed the measured
  single-frame error of 22.45 - so 29.5 is not reachable. The images are not two averages of independent
  frames.

Correct sums, correct divisor, but noise falling as if only half the frames were independent, and two
consecutive frames differing by more than two independent samples can differ: the remaining explanation
is that consecutive frames contribute samples that are strongly related but not identical - the same
pattern shifted or permuted rather than fresh draws. The seed path itself is clean: the frame index is
mixed into a standard PCG hash (pt_common.glsl line 34) before any draw, and a different frame index
genuinely produces a different stream.

That is as far as this goes without putting the samples themselves under a magnifier: the next step is to
dump the raw HDR float buffer for two consecutive frames and correlate them directly, per pixel, which
would show a shift or a permutation for what it is instead of leaving it inferred from three aggregate
numbers. Worth doing, but it is a bug in the still-capture path that predates the denoiser, and the
denoiser's measured benefit does not depend on fixing it - if anything the accumulator is stronger than
measured, so the eight-fold figure is conservative.

## Weights are now defaults, and the flags are documented

The sweep said the only badly wrong constant was sigmaL: 4 scored 12.91 on a single denoised frame,
24 scores 10.29. sigmaZ between 0.5 and 5.0 changes nothing at all - reciprocal-depth differences are
small enough that the depth test saturates to fully permissive - while the tight 0.02 does help once
the other two are right, so it stays. sigmaN between 32 and 512 moves the result by a few hundredths
and 128, close to the paper's value, was kept rather than chasing the last 0.1 onto a number that
risks ignoring geometry edges on other content.

Set as defaults in scene.h with the measurement recorded beside them, so a plain --denoise run
reproduces the tuned numbers without flags: one frame 10.29 (was 12.91), four frames 10.03.

README has an Options row for each new flag and a Denoising section covering what the passes are, the
measured cost (0.4 ms tracing, 2.0 ms with the filter, at one sample per pixel and 720p), the measured
benefit against the converged reference, why it stays off by default (bias-limited past one frame, and
tuned on this scene), and the three deviations from the paper. --help lists --denoise, --gbuf with all
six views, and the three weight knobs; --help goes to the log file, not the console, which is why an
earlier check of it looked empty when it was not.

## One unreproduced failure, recorded rather than dropped

In a loop that ran five configurations back to back, the plain default run - no --denoise - exited 1.
Everything else in that loop passed. Four subsequent default runs exited 0 (59.0 and 58.9 fps, no frame
over 20 ms), and two specific hypotheses were tested and refuted:

- contention on the log file: launching with the log held by another handle exits 0.
- two instances at once: both exit 0.

So the cause is unknown and the only pattern noticed is that the failing run was launched immediately
after its log file had been deleted by the same script. It does not reproduce, so it is not being
called fixed. If it is ever seen again, the surviving log from that run is the thing to read.

## Withdrawing the halved-convergence claim: accumulation is fine

Raw float dumps of the accumulation target, all eight frames from a single process, measured in linear
space before any tonemapping. Comparing the mean of the first k frames against the mean of the rest,
which for independent frames should shrink as sqrt(1/k + 1/(8-k)):

| k | measured | independent prediction | ratio |
| --- | --- | --- | --- |
| 1 | 0.1500 | 0.1500 | 1.00 (calibration point) |
| 2 | 0.0984 | 0.1146 | 0.86 |
| 3 | 0.0955 | 0.1025 | 0.93 |
| 4 | 0.0862 | 0.0992 | 0.87 |
| 6 | 0.1007 | 0.1146 | 0.88 |
| 7 | 0.1310 | 0.1500 | 0.87 |

Also from the same dumps: per-frame sample means are stable at 0.325 with no drift; the variance of a
frame-to-frame difference is flat across lags (0.031 to 0.038, no dip at lag 1), so consecutive frames
do not repeat each other's noise; disjoint-difference correlations are small (+0.06 to +0.15); and a
split-half test gives 3.09 effective samples per group of four, which is what a mild frame-to-frame
correlation of about 0.1 predicts, not the factor of two claimed before.

So the renderer accumulates correctly, with roughly ninety percent of the efficiency of perfectly
independent samples. The earlier claim that only half the frames counted was an artefact of how it was
measured: RMSE in eight-bit tonemapped PNG space, between files written by separate processes, against
a reference produced by a third process. This file said explicitly, two rounds ago, that separate runs
do not reproduce the same frame sequence - and then I measured with separate runs anyway.

Consequence for the headline number: the correction to eight-fold is itself retracted. The PNG ladder
still stands as a quality comparison against one common converged reference - one denoised frame at
RMSE 10.29 against sixteen accumulated frames at 11.23 - so the claim is back to about sixteen times
the effective samples of plain accumulation at one sample per pixel, with the caveat that it is
measured after tonemapping.

New tooling worth keeping: --dump-accum N (or -2 for every frame) writes the raw float target, and
tools/corr4.py plus tools/conv.py analyse it. Measuring in linear space inside one process is the only
version of this that meant anything, and it is cheap.

## Bit-identity with the flag off: settled, and in favour

Four runs of the current build and four of the committed build (a git worktree at HEAD), same flags,
same pak, --capture 1 --accum 8 --size 640x360, denoising off:

| run | current | committed |
| --- | --- | --- |
| 1 | F911E659 | F911E659 |
| 2 | F911E659 | F911E659 |
| 3 | F911E659 | **3E75F020** |
| 4 | F911E659 | F911E659 |

Seven of the eight runs produced one and the same file, byte for byte, and it was produced by both
builds. The eighth, from the committed build, differs - and against the converged reference it scores
11.54 where the common output scores 11.56, so it is a different noise realisation of the same image,
not a different image.

So:

- The renderer occasionally renders a different realisation of the same scene. It does so on the
  untouched committed build, at roughly one run in four here, which is the pre-existing behaviour that
  produced the run-to-run spread measured earlier in this file.
- The two builds share that realisation. Every time both builds produced the common output they were
  identical to the byte, and no output has ever been seen from the current build that the committed
  build did not also produce.

That is the strongest form the requirement can take: with --denoise absent, the current build's output
is byte-identical to the committed build's. The earlier note in this file saying bit-identity "cannot be
tested here" was itself wrong in its premise - it claimed captures are not reproducible across
processes, and raw float dumps with the accumulation target pinned show they are: four frames, two
processes, identical bytes, both with the controller on and off. What is not reproducible is the rare
alternate realisation, and that predates this work.

Method note, because it nearly produced a false pass: an earlier version of this comparison reported
every check as identical. It was not - the hash helper was missing the full path, Get-FileHash failed,
and the comparison became null equals null. Printing the hash values themselves, truncated, is what
exposed it. Any comparison that only prints a boolean can pass by never having measured anything.

## Runtime toggle, and a second cost correction

N now toggles the filter and the title bar shows DENOISE ON, driven through applyHdrSize - the same
rebuild a resize uses - so the feature images, history and accumulation are all replaced together and
the next frame starts clean. Verified by reading the live window title, not by trusting the code:
started without the flag the title carries no marker, after a mid-run toggle it reads
'... 3 spp - RT pipeline - DENOISE ON - ESC quits'. Both directions ran clean, and turning it on
mid-run - which allocates the filter's images while frames are in flight - produced no new fault event.

Cost was measured again properly and the number in the README was wrong. Interleaved runs, pinned one
sample per pixel, trace at window size, averaged over the last twenty frames of each: 0.31 ms off,
1.10 ms on, so the filter costs about 0.8 ms, not the 1.6 ms claimed from 0.4 versus 2.0. Those earlier
figures were single log samples taken while the timestamps were still warming up; they also disagree
with a shorter later run that read 0.70, which is why the confirmation used eight interleaved runs
rather than one more single reading.

Corrected again on 2026-11-13, and this time the correction is to the shape of the number rather than
its size. About 0.8 ms was measured at 1280x720, which is 0.92 megapixels, and it was then carried
around as if it were the filter's price. It is about 0.5 ms per megapixel at that size - and nothing
like it at larger ones. Fixed quality, one sample per pixel, trace at window size, this GPU:

    traced        filter off    filter on     difference      per megapixel
    0.92 MP          0.3 ms        0.8 ms         0.5 ms        0.5 ms
    3.69 MP          1.0 ms        2.9 ms         1.9 ms        0.5 ms
    4.67 MP           -            3.7 ms          -            0.6 ms
    5.76 MP           -           60.6 ms          -           ~10   ms
    8.06 MP          2.1 ms      163.1 ms       161.0 ms       ~20   ms

So it is not a constant and never was one: it holds to about 4.7 megapixels and then rises by a factor
of forty. The wavelet iterations are bounded by their own working set rather than streaming, which is
the same cliff the temporal pass hit at 4K, arriving here between 4.7 and 5.8 megapixels. Any controller
carrying a single figure for the filter's price is wrong by forty times somewhere, which is what the
next section is about.

## Default on, with the frame budget as the escape hatch

Denoising is on by default for window rendering; stills force it off unless --denoise is named, because
the filter disables accumulation and a still is trying to average dozens of frames.

The order the controller surrenders things in, as of 2026-11-13, is the filter first, then samples per
pixel, then resolution - and restored in the reverse order, with the filter last. What is written below
describes the original arrangement, in which the filter went last and only after samples and resolution
were already at their floor; that ordering is what the section after this one replaces, and it is kept
here because the measurements it records were real and the reasoning was sound for the cost model the
controller had at the time. Pressing N has been final for the rest of the run throughout, in both
directions, and still is.

Verified: default run's title reads '... - (N) Denoise ON - (ESC) Quit'; with --no-denoise it reads
'(N) Denoise OFF'; with the budget forced to 0.05 ms the log records 'denoise OFF (at quality floor and
still over budget)' and the live title follows to '... trace 512x288 - 1 spp ... (N) Denoise OFF'. A
manual toggle during an unreachable budget was never overridden. Stills are byte-identical to the
pre-denoise committed build (F911E659...), so the new default did not touch the capture path.

Superseded on 2026-11-13. The give-up state turns out to be reachable with no test flag at all, at a
window size this machine is simply too small a GPU for: 3840x2100, where the frame costs 2.1 ms with the
filter off and 163 ms with it on. The 1.5 ms budget floor was the obstacle only because the old
controller reached for the filter last, after samples and resolution had already given everything they
had to give; with the order reversed the filter is surrendered while the budget is still an ordinary
nine milliseconds. The restore branch has now executed too, and is described below.

## The controller's cost model, and the order things are given up in

2026-11-13. The reported symptom was a controller that would not settle: at 2560x1440 it held about 22
fps while the trace scale swung between 0.41 and 1.52 seven times in half a minute, samples pinned at 1,
and the filter never once surrendered even though pressing N restored 60 fps by hand. Two separate
defects, one behind each half of the symptom.

The cost model had one term for two kinds of work. Tracing costs per sample per traced pixel, and the
model said so; the filter costs a charge per traced pixel that has nothing to do with the sample count,
and the model had nowhere to put it. So the filter's price lived inside `msPerSampleKiloPx` whenever the
filter was on and vanished whenever it was off - which is the whole explanation for the same setting
reading 0.7 ms at one moment and 11.2 ms at another, a number that then went into the prediction of what
the next setting would cost. The model is now two additive terms, `msPerSampleKiloPx` and
`filterMsPerMegaPx`, and a measurement only updates the first after the second's share has been
subtracted. Readings where the filter accounts for most of the frame are refused outright rather than
divided down into a unit cost, and the refusals are counted and printed.

The filter's price can only be *seen* across a flip, so a flip arms a measurement: `setDenoiseEnabled`
records the settled average while the old state is still what it describes, and the controller finishes
the pair fifteen frames later, provided samples and scale did not move in between. Fifteen rather than
one, because a flip rebuilds some thirty Vulkan images - the feature, history, output and wavelet
targets only exist while filtering - and none of that cost is inside the timestamps, it lands on the
frames afterwards. Those frames are discarded by a widened settling window instead of being averaged.
The constant may not move by more than a factor of two per measurement, so one bad pair cannot reprice
the filter out of existence. Measured against fixed-quality runs taken by hand, this calibration lands
close at small sizes - it read 0.49 to 0.68 ms/MP at 0.92 MP where the hand measurement says 0.5 - and
short of the truth at the largest, reading 11.94 ms/MP at 8.06 MP against a hand-measured 20. The
underread is expected and survivable: the two readings either side of a flip at that size were taken at
different sample counts, and the factor-of-two clamp moves the constant toward the truth on every flip
after it. At the size that mattered for the bug the constant only had to be large enough to stop the
controller wanting the filter back, and eleven milliseconds per megapixel is plenty large enough.

The lateness signal was the second defect and the reason the escape hatch never fired. It was measured
against the display period taken from the most common frame interval, so a renderer stuck at 22 fps
discovered 45 ms to be the period, compared itself against 45 ms, and reported a late fraction of zero
forever - while the frames were arriving at twice the time they were supposed to. Lateness is now
measured only against the frame rate the user asked for. The old watchdog also asked for forty
consecutive late readings *and* the quality floor, and reset its own count every time it changed
anything, so on a machine that was oscillating it could not arrive anywhere by construction.

Order of surrender, which is the point of the whole thing: the filter first, then samples per pixel,
then resolution down to `cfg.minRenderScale`; restored in reverse, resolution and samples back first and
the filter only when there is room for the whole of its price on top of wherever the controller has
climbed to. The reasoning is arithmetic rather than taste - at 8.06 megapixels the filter costs 161 ms
and the rest of the frame costs 2, so shrinking the image to make room for it throws away the picture
and still misses the frame by fifteen times over. While the filter is still the controller's to give up
it may spend spare budget but may not hand any back, so no pixel or sample can be surrendered ahead of
it. Pressing N remains final for the run in both directions, and is now marked as such in the state
rather than merely counted as a flip: a user who pins the filter on at a hostile size gets resolution
given up instead (measured: 3840x2100 with the filter held on by hand went to scale 0.40 and 34 fps,
and the controller never argued with the key).

Decisions are weighed once per twelve-frame window rather than every frame, which is what makes the
over/under streaks mean independent looks at a settled frame instead of the same observation counted
four times over; climbing leaves a margin below the budget so it does not land on top of the measurement
noise; and the budget is now allocated in closed form over samples and scale instead of stepping, so the
controller goes where it thinks it should in one move rather than walking there through six rebuilds.

Every decision writes one machine-readable line, and these are the lines `tools/test_denoise_priority.py`
judges:

    quality start: denoise on, scale 1.00, spp 1, filter 0.86 ms per megapixel
    quality decision: denoise off (late fraction 0.42, scale 1.00, spp 1)
    quality decision: samples 1 -> 3 (gpu 2.6 ms, budget 9.2 ms)
    quality decision: scale 1.00 -> 0.80 (gpu 11.0 ms, budget 9.2 ms)
    quality decision: denoise on (headroom 4.3 ms, scale 1.00, spp 16)
    denoise cost: 11.94 ms per megapixel traced (96.3 ms for 8.06 MP, measured across the filter going off)

The restore branch, previously unverified, has now run: given up under a forced late frame rate at
1280x720, the controller bought samples back up to the ceiling, then took the filter again with 4.3 ms
of headroom, priced it at 1.01 ms/MP on the way back on, and continued. It does not flap - the restore
requires frames to be arriving on time as well as affordable, because a filter taken back while the
machine is still missing its target is reversed on the next late reading.

Before and after, on this machine:

    case                       before                    after
    3840x2100, 25 s            13.3 fps, no give-up,     55.4 fps, filter surrendered
                               scale oscillating         first, then samples to 3
    2560x1440, 30 s            ~22 fps, seven scale      58.7 fps, denoiser kept,
                               changes in 30 s (the     zero decisions
                               reported case)
    1280x720, 20 s             58.8 fps                  58.6 fps, denoiser kept

Only the 3840x2100 before figure was re-measured here directly; the 2560x1440 before numbers are the
reported symptom, and the after numbers are what thirty seconds on this machine produced.

The reported case is the interesting one. At 2560x1440 this machine does not need to give anything up -
the frame costs 2.9 ms with the filter on against a 9.2 ms budget - so the correct behaviour is to
surrender nothing, and that is what it now does, for thirty seconds without a single decision. It was
the poisoned cost model that made it thrash, not the workload.

`tools/test_denoise_priority.py` runs the real renderer in a real window and reads the decisions out of
the log file. It asserts the filter was on at the start, was surrendered, that nothing was surrendered
before it, and that the run then averaged at least 50 fps; the control case asserts nothing is
surrendered at 1280x720. It failed against the unmodified controller (13.3 fps, no decision lines at
all) and passes now. It also carries `--selfcheck`, which puts invented decision orders through the same
judgement function the real verdicts come from and confirms it rejects pixels-given-up-first,
samples-given-up-first, never-surrendering, staying-slow-after-surrendering, surrendering-when-it-need-
not, and starting-with-the-filter-off. A test that cannot fail is not evidence, and the real runs cannot
be relied on to produce the wrong order on demand. The self-check earned its place immediately: it
found an indexing bug in the test's own ordering assertion on the first run.