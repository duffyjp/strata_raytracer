# SVGF Denoiser — Implementation Specification

Target: Schied, Kaplanyan, Chaitanya, Burgess, Dachsbacher, Lefohn, *Spatiotemporal
Variance-Guided Filtering: Real-Time Reconstruction for Path-Traced Global Illumination*,
HPG 2017. Everything below is written to be implementable without reading the paper.
All passes are GLSL 450 compute (`#version 450`, `local_size_x = local_size_y = 8`),
no external libraries. Where this spec departs from the paper it says so and why.

Notation: image coordinates are integer texel indices `p = (x, y)`, `W x H` is the trace
resolution. `s` is the à-trous dilation step. `L(c) = dot(c, vec3(0.2126, 0.7152, 0.0722))`
(Rec. 709) is the luminance of an RGB colour.

---

## 1. Inputs the filter needs per pixel

The ray tracer must expose, in addition to radiance, these full-resolution per-pixel
attributes. They are written by the existing raygen pass (`shaders/main.rgen`) into new
storage images; the ray-query fallback (`shaders/fallback.comp`) must write the same
images with the same encoding, otherwise the denoiser silently misbehaves in fallback mode.

| Name | Format | Contents | How obtained |
|---|---|---|---|
| `radianceIn` | RGBA16F | current-frame 1-spp radiance (pre-tonemap) | already exists (`hdrImage`), now written *without* accumulation (see §9) |
| `demodAlbedo` | RGBA16F | the colour the primary bounce multiplied the incoming radiance by | raygen: for lambert, `materials[m].albedo.rgb` (textured); for metal, the tint; for glass and emissive pixels, `vec3(1.0)` (no demodulation — see §3) |
| `normal` | RGBA16F | **world-space** shading normal, xyz, normalized | already computed in the hit shader (`hit.normal`); currently only used to shade |
| `depth` | R32F | **linear view depth** = `dot(hitPos - camOrigin, CAM_FWD.xyz)`. `0.0` means sky/miss | raygen, from the primary hit distance |
| `matId` | R32F | integer material index stored as a float; `4294967295.0` for sky. The light spheres get their own indices, which is what rejects light↔surface reprojections | raygen, from `instanceCustomIndex` |
| `prevPos` | RG32F | the previous-frame pixel coordinates of the same surface point | see 1.1 |

`R16F`/`RG16F` storage images are *not* core-required format features, so every image uses
`R32F`, `RG32F`, `RGBA16F` or `RGBA32F` (the set `src/core2.cpp` already probes for
storage+sampled+transfer). All passes read with `imageLoad` on integer coordinates and write
with `imageStore`; no hardware filtering is needed anywhere, so every image can stay in
`VK_IMAGE_LAYOUT_GENERAL` and the only synchronization between passes is
`vkCmdPipelineBarrier` (compute→compute, `ALL_COMMANDS_BIT`/`SHADER_WRITE` →
`SHADER_READ`).

### 1.1 Motion vectors

Geometry is static and only the camera (and the two emissive spheres) moves, so motion is
analytic — no motion-vector ray pass is needed. In raygen, after finding `hitPos`:

```
// current and previous frame pinhole cameras: origin O, right R (x aspect), up U, forward F,
// tanHalfFov = CAM_ORIGIN_FOV.w, frame size (W,H).  Upload the PREVIOUS frame's values as a
// second camera block in SceneBlock (camera[8..15]); scene.h/sceneblock.glsl must be
// extended in lockstep (std140, vec4-only members).
vec3  d  = normalize(CAM_FWD.xyz + CAM_RIGHT.w * ndc.x * tanHalf * CAM_RIGHT.xyz
                                        + ndc.y * tanHalf * CAM_UP.xyz);      // as today
vec3  dp = normalize(prevF + ndc.x * prevTan * prevR + ndc.y * prevTan * prevU);
vec2  prevPos = projectPrev(hitPos);
vec2  projectPrev(vec3 P) {
    vec3 v = P - prevO;
    float zz = dot(v, prevF);
    float tx = dot(v, prevR) / (zz * prevTan * prevAspect);   // -> [-1,1]
    float ty = dot(v, prevU) / (zz * prevTan);                // -> [-1,1]
    return vec2(tx * 0.5 + 0.5, 0.5 - ty * 0.5) * vec2(W, H); // pixel coords, top-left origin
}
```

For a pixel whose primary hit is an emissive sphere, reprojecting the world hit point with
the *old camera* is wrong because the sphere itself moved; the light's own displacement
dominates. Since the lights only translate, subtract that displacement before projecting:

```
if (primaryHitIsLight) prevPos = projectPrev(hitPos - (lightPosNow - lightPosPrev));
```

Upload `lightPosPrev` per light (4 lights in `SceneBlock` already). This matters even for
the sphere's interior pixels, because those are exactly the pixels that must not drag their
history into the following frame; `matId` rejection (§2.2) catches the silhouette ring,
this catches the interior smear.

Sky pixels: `prevPos = vec2(-1)`, `depth = 0`.

### 1.2 Where luminance variance comes from

Variance is **not** measured from the current frame's samples (1 spp gives no estimate).
It is estimated from temporally accumulated raw moments of luminance (§2.4), with a spatial
7×7 bilateral fallback wherever history is too short (§2.5). That estimate then drives the
filter, and the filter's own variance-propagation equation carries it through the à-trous
iterations (§4.4). There is no separate "variance pass" in the ray tracer.

---

## 2. Temporal filter (pass D1)

One dispatch, full resolution. Reads `radianceIn`, `demodAlbedo`, `normal`, `depth`,
`matId`, `prevPos` and the previous frame's history (`histColor`, `histMoments`,
`histDepth`, `histNormal`, `histMatId`), writes the current frame's history slots.
History is ping-ponged (two slots per buffer, swapped every frame) because this pass reads
history at reprojected coordinates while writing at `p`: reading and writing one image in one
dispatch is a genuine data race.

### 2.1 Demodulate first

```
vec3 A  = demodAlbedo.rgb;
vec3 Cd = (L(A) < 1e-3) ? radianceIn.rgb : radianceIn.rgb / A;   // filtered signal, §3
```

Everything the temporal pass accumulates — colour, moments, variance — is in this demodulated
domain. Tonemapped or modulated data must never enter the history.

### 2.2 Reprojection and validity

`prevPos` is resampled with an explicit **2×2** tap bilinear filter (four taps, weights
computed by hand from the fractional part, *not* `texture()` — each tap must be validity-tested
individually, which hardware filtering cannot do). For tap `t` with bilinear weight `b_t`
and current-frame pixel `p`:

```
bool valid(ivec2 t) {
    if (any(lessThan(t, ivec2(0))) || any(greaterThanEqual(t, ivec2(W,H)))) return false;
    float zh = histDepth[t], zc = depth[p];
    if (zh <= 0.0 || zc <= 0.0) return false;                    // sky on either side
    if (histMatId[t] != matId[p]) return false;                  // different surface
    if (dot(histNormal[t].xyz, normal[p].xyz) < 0.9) return false;      // > 25.8 deg apart
    float disagree = abs(zc - zh);
    float grad     = abs(dot(gradient(depth)[p], vec2(p) - vec2(t)));   // slope allowance
    return disagree <= max(1.0 * grad, 2e-3 * zc + 1e-4);
}
```

What each test protects against:

* **`matId`** — replaces the paper's mesh ID. It is the only test that fires at a
  light-sphere silhouette and at any two surfaces that happen to share screen position, where
  depth and normal agree by accident. Without it the moving lights bleed their
  (much brighter) history into whatever the camera pans onto.
* **Normal angle** (`cos > 0.9`, i.e. reject facets more than ~26° apart) — rejects
  reprojecting onto a different facet of a crease or a thin object seen from a new angle.
  Those are the cases where depth is nearly identical but the lighting is completely
  different, so the depth test alone passes and you get a halo of stale colour along the
  crease.
* **Depth-gradient test** — the depth disagreement is allowed up to whatever the surface's
  own slope can explain across the reprojection offset. This is what makes the test correct
  at grazing angles: on a surface nearly parallel to the view direction, `depth` changes by
  large amounts across a few pixels, so a plain relative threshold (`|zc-zh|/z > 1e-3`)
  rejects every pixel and the history collapses to 1 spp on exactly the surfaces that need
  it most (visible as sparkly over-sharpened floors/walls, and as the "over-blurring at
  grazing angles" family of artifacts in §8). The `max(..., τ_rel·z + τ_abs)` floor keeps
  the test from rejecting on floating-point noise when the camera is still and the offset is
  ~0.
  `gradient(f)` = central differences, `(f(x+1)-f(x-1))/2` per axis, clamped to the frame
  edge. Compute it from the current-frame `depth` image; do **not** use `dFdx`/`dFdy` in
  compute (derivatives are per 2×2 invocation quad and are meaningless for `imageLoad`ed
  values you gather yourself).
  Note: the paper measures the deviation from a local plane in **clip-space** depth, for
  which a plane's screen-space depth is exactly affine. With linear view depth the affine
  model is only approximate, and the error grows with distance from the view axis. Judgement
  call: use linear view depth everywhere (the task's input list, and simpler to keep one
  representation) and absorb the inaccuracy in `τ_grad = 1.0`. If far-field segmentation
  misbehaves, store `1/depth` in an extra channel and use that for the plane test instead —
  `1/z` is affine in screen space for a plane, i.e. the correct clip-space model.

Weighting (per the paper):

```
float wsum = 0, acc = 0;
for each of the 4 taps t: if (valid(t)) { wsum += b_t; acc += b_t * histColor[t]; }
if (wsum < 1e-6) -> try a 3x3 neighbourhood (9 taps, same test, uniform weights)
if (still wsum < 1e-6) -> disocclusion: n = 0, and the history sample is dropped entirely
else histColorSample = acc / wsum            // resampled, valid-tap-weighted history
```

The 3×3 retry is how thin geometry (foliage-like slivers, in the paper's words) is found when
the 2×2 footprint straddles an edge. Keep it; it is a few lines and it is the difference
between "history drops along every silhouette" and not.

### 2.3 Exponential moving average with an explicit history length

Use a per-pixel history length `n` so the first frames average properly and the filter
forgets old lighting at a bounded rate:

```
n_i     = min(nMax, alpha_i * n_{i-1} + 1.0);      // n_{i-1} = 0 on disocclusion
alpha_i = max(1.0 / (n_i + 1.0), 1.0 / nMax);      // 1/(n+1) while warming up
Cbar_i  = alpha_i * Cd + (1.0 - alpha_i) * Cbar_{i-1};
```

While `n` is small this is exactly an arithmetic mean of the frames seen so far (no early
bias toward a dim first frame); once `n` reaches `nMax` it becomes a plain EMA with
`alpha = 1/nMax`. When the history was invalid, use `alpha_i = 1` (reset).

The paper quotes `alpha = 0.2` as the weight on the incoming sample (its formula is
`Cbar_i = alpha*C_i + (1-alpha)*Cbar_{i-1}`, i.e. effective sample count ~5); many public
reimplementations instead apply their `alpha` to the *history*, which is the same number
read as `1-alpha`. This spec always writes the weight on the **new** sample, so `alpha` is
comparable to the paper's 0.2. Do not let a reviewer confuse the two: it is a factor-of-4
difference in effective history.

### 2.4 History-length cap and the moments

`nMax = 20` (§6). Rationale specific to this renderer: geometry is static and the camera is
the main motion source, so once a pixel has reprojection it *keeps* passing the validity
tests forever; nothing in a geometry-based test can tell you that the light that used to
graze a wall has orbited away. An uncapped EMA (`alpha -> 0`) therefore freezes slowly
changing lighting into the image for as long as the camera looks at the wall, and the
variance estimate collapses to ~0, which in turn tells the spatial filter to stop filtering
and preserves the wrong value. `nMax` bounds the memory of stale illumination to ~20 frames
(≈1/3 s at 60 Hz) and bounds the variance estimator's decay, which is what keeps the
filter responsive to orbiting lights.

Moments of luminance, accumulated with the same `alpha_i` and the same validity decision:

```
float Lsamp = L(Cd);                       // current sample, demodulated domain
mu1_i = alpha_i * Lsamp           + (1.0 - alpha_i) * mu1_{i-1};      // raw first moment
mu2_i = alpha_i * Lsamp * Lsamp   + (1.0 - alpha_i) * mu2_{i-1};      // raw second moment
sigma2_temp = max(0.0, mu2_i - mu1_i * mu1_i);                        // Var = E[L^2]-E[L]^2
```

Store `(mu1, mu2, n)` as `RGBA32F` (`.b` = n) in the moments history. `RGBA16F` is not
enough for `mu2` of an HDR image lit by emissive spheres: `L^2` for `L = 400` is `1.6e5`,
which is representable in half but with ~64 units of quantisation — enough to make the
variance of a low-noise region come out negative-looking and randomly large.

The moments history is updated in *this* pass, from unfiltered data. Only the **colour**
history is overwritten later by the first à-trous iteration (§4.5). That asymmetry is
deliberate and is what the paper does: variance must keep measuring sample-to-sample noise,
not the variance of the already-smoothed signal, or the filter stops seeing noise after
frame 1.

### 2.5 Spatial variance fallback (short history)

```
if (n_i < 4.0)  sigma2 = spatial variance (below);
else            sigma2 = sigma2_temp;
```

7×7 window, edge-stopped by depth and world-space normal only (no luminance term — using the
noisy signal to decide how to estimate the noise of the signal is circular):

```
w(p,q) = exp(-|depth[p]-depth[q]| / (sigma_z_spatial * abs(dot(gradient(depth)[p], vec2(p)-vec2(q))) + 1e-4))
         * pow(max(0.0, dot(normal[p].xyz, normal[q].xyz)), sigma_n_spatial);
sumW  = sum(w);      sumW2 = sum(w*w);
m1    = sum(w*L) / sumW;
var   = sum(w * (L-m1)*(L-m1)) / max(1e-6, sumW - sumW2/sumW);   // unbiased weighted variance
sigma2 = max(1e-6, var);
```

Write both `sigma2` and the moments back to the current history slots. The Bessel correction
matters here: with 49 edge-stopped taps whose effective count is often < 10, an uncorrected
weighted variance underestimates by 10–40 %, and the filter then refuses to blur exactly the
newly exposed surfaces it is supposed to rescue.

---

## 3. Diffuse demodulation

Before filtering, divide the radiance by the primary-surface albedo (1.1 / 2.1). After the
last à-trous iteration, multiply the filtered radiance by the same `demodAlbedo`. Nothing
else touches albedo.

```
filter domain:  Cd = radiance / demodAlbedo
display domain: radianceOut = C_filtered * demodAlbedo
```

Why: the spatial filter's edge-stopping functions look at depth, normal and luminance
*variance* only. A checkerboard texture is high-frequency *signal*, but it arrives at the
filter as high variance, indistinguishable from noise. Filtering raw radiance therefore
forces a choice between (a) a variance term so tolerant that the texture is blurred away and
neighbour pixels' albedo colours bleed across the surface, or (b) a variance term so tight
that the filter refuses to blur actual noise. Demodulating moves the texture out of the
filtered signal entirely: the filter sees smooth, untextured illumination, can blur
aggressively, and the texture is re-applied per-pixel afterwards with its original
sharpness. The paper notes it also increases legitimate spatial reuse between neighbouring
samples, which is what makes 1 spp reconstruct.

Guard rails:
* `L(demodAlbedo) < 1e-3` (black texture, or glass): skip the division and the remodulation
  for that pixel — writing `1e-3` in the denominator amplifies the noise by 1000× and the
  filter has no way to know it.
* Emissive pixels: `demodAlbedo = 1`, so demodulation is a no-op. Do not demodulate a light's
  emission by its "albedo".
* Demodulation is what makes the albedo-vs-geometry disagreement artifact of §8 much rarer,
  but it does not remove it: pixels with the *same* geometry and *different* effective BRDF
  (a metal next to a lambert, same normal and depth) still get averaged together in the
  combined-channel filter.

---

## 4. À-trous wavelet filter (passes D2…D6)

### 4.1 Kernel and dilation

5-tap binomial kernel, applied as an undecimated 2-D transform with separable weights:

```
h[5] = { 1/16, 4/16, 6/16, 4/16, 1/16 };      // sums to 1
iteration i (1-based):  step s = 2^(i-1)      -> 1, 2, 4, 8, 16
tap offsets: q = p + s * (dx, dy),  dx, dy in [-2,-1,0,1,2]   (25 taps, all used)
weight of tap:  hw = h[dx+2] * h[dy+2]
```

5 iterations (the paper's "five-level wavelet transform"). Total support radius =
`2*(1+2+4+8+16) = 62` px, so the true support is 125×125; the paper states "65×65". Sources
disagree — implementations that quote 65 count the step sum `1+2+4+8+16 = 31` as the radius.
It changes nothing about the code; use 5 iterations with the step sequence above and do not
try to "fix" the footprint number.

No shared-memory tiling for `s ≥ 4`: a tile that covers 25 taps at `s=16` would be 128×128
texels. Just gather with `imageLoad` and let the texture cache do its job; if you tile, tile
only for `s ∈ {1,2}`.

### 4.2 Edge-stopping weights

```
w(p,q) = wz * wn * wl                                              (paper Eq. 2)

zbar   = depth[p] + dot(grad, vec2(q) - vec2(p));      // local linear depth model
wz     = exp(-abs(depth[p] - depth[q]) / (sigma_z * abs(depth[p] - zbar) + 1e-4));   // Eq. 3
wn     = pow(max(0.0, dot(normal[p].xyz, normal[q].xyz)), sigma_n);                   // Eq. 4
wl     = exp(-abs(Lp - Lq) / (sigma_l_eff(p) + 1e-6));                                // Eq. 5
```

`zbar` is the depth that the tangent plane at `p` predicts for `q`. Dividing by the predicted
change instead of by a fixed sigma is what makes the depth weight independent of scene scale
and of view angle: on a plane, `|depth[p]-depth[q]| ≈ |depth[p]-zbar|`, so `wz = exp(-1/sigma_z)`
everywhere on the plane, and only a real discontinuity (where the plane model fails) drops `wz`
toward 0. `sigma_z = 1`. This is the one place where using linear view depth instead of
clip-space depth introduces model error (§2.2).

`Lp`, `Lq` are the luminances of the *current à-trous iteration's input* (`C_0` for `i=1`,
`C_{i-1}` afterwards), i.e. of the progressively smoother signal, never of `radianceIn`.

### 4.3 Variance scaling of `sigma_l` — the important part

```
Vhat   = max(0.0, varianceIn[p]);                       // this iteration's variance image
sigma_l_eff(p) = sigma_l * sqrt(max(1e-6, 4.0 * s * s * Vhat)) + 1e-6;
```

The paper's Eq. 5 reads `wl = exp(-|L(p)-L(q)| / (sigma_l * sigma_3x3(Var(L(p))) + eps))`,
i.e. the denominator is `sigma_l` times the *standard deviation* obtained from the
Gaussian-prefiltered variance image — hence `sqrt`, and hence the `3×3` prefilter below.
The paper does not print a factor of `4 s²`; it argues instead that "the luminance variance
tends to reduce with subsequent filter iterations, the influence of `wl` grows with each
iteration, preventing overblurring". The reference/most-copied implementations add the
`4 s²` scaling ("scale sigma with the square of the current filter scale to counteract
previous filtering"). This spec keeps the `4 s²`: the two statements are not the same thing,
and without the scaling `wl` gets monotonically stricter as `V` shrinks, so the wide
iterations (`s = 8, 16`) stop doing their job and residual low-frequency blotches survive —
the most visible failure mode when you drop it. Concretely: `sqrt(4 s² V) = 2 s sqrt(V)`
re-expresses the per-pixel standard deviation at the *tap spacing* of the current iteration,
because at `s = 16` the quantity being thresholded, `|L(p)-L(q)|`, is a difference of two
already-heavily-smoothed pixels 32 texels apart, whose legitimate signal difference is much
larger than a neighbouring-pixel difference. **Flagging this as the main judgement call in
this document** (§10). If you want the paper's literal behaviour, set the scaling factor to
1 and expect less low-frequency blur.

### 4.4 Variance propagation

The variance image is filtered too, so that later iterations judge against the variance of
what they are actually looking at:

```
varOut(p) = sum( hw^2 * w^2 * varIn[q] ) / max(1e-6, sum(hw * w * varIn[q])) ... 
```

Use the paper's formula (independence assumption), which is *not* the same filter as the
colour:

```
varOut(p) = ( Σ_q hw_q² * w(p,q)² * varIn[q] ) / ( Σ_q hw_q * w(p,q) )²
sigma2Out = max(0.0, varOut)
```

Write it to the variance ping-pong slot for the next iteration.

### 4.5 Colour update

```
num = Σ_q hw_q * w(p,q) * Cin[q];      den = Σ_q hw_q * w(p,q);
Cout[p] = (den < 1e-6) ? Cin[p] : num / den;
```

All taps of one iteration share `w`'s denominator form, so no per-channel renormalisation is
needed beyond this division. If `den < 1e-6` (isolated pixel with no valid neighbours) copy
the input; never write 0.

**Iteration 1 is special**: its colour output is *also* the next frame's colour history
(`histColor`), because a raw 1-spp sample is a poor thing to accumulate for 20 frames and a
once-filtered sample is much better, while iterating further would feed spatially biased data
back into time and progressively dissolve detail. So D2 writes `histColor[cur]` as well as the
working buffer; D3…D6 do not touch history.

### 4.6 Prefiltered / halved-resolution guidance vs full-resolution weights

Two different things get conflated here; both are decided for us:

1. **3×3 Gaussian prefilter of the variance (the paper does this, and only for `wl`).**
   The reference approach runs a separate pass that computes
   `Vpref = (Σ g_k V_k)/Σ g_k` with the 3×3 binomial `g = {1 2 1; 2 4 2; 1 2 1}/16`,
   and feeds `Vpref` into `sigma_l_eff`. The paper is explicit that this prefiltered image is
   *only* used to drive the luminance edge-stopping function and is **not** the image
   propagated to the next iteration — iteration `i+1` propagates `varOut` from `varIn`, and
   each iteration re-prefilters the variance it currently holds. Practical reading, adopted
   here: every à-trous pass computes the 3×3 Gaussian of its input variance inside its own
   gather (it is already reading 25 taps; add 9 reads, or read `V` from the same shared tile)
   and uses that as `Vhat`. So no separate prefilter pass is needed: it belongs inside
   D2…D6, which avoids one extra full-res image and is what §7's dispatch list assumes.
   The optional pass D7 exists only to visualise variance while debugging.
2. **Halved-resolution guidance (a reference-implementation bandwidth optimisation, not in
   the paper text).** For iterations with `s ≥ 2` the taps are ≥ 2 texels apart, so the
   variance/normal/depth lookups can be done from a half-resolution buffer (index `p/2`) and
   the iteration's variance output written at half resolution for the next iteration. Effect
   on the image: edge-stopping decisions become smoother and slightly late (a 2×2 box on
   `V`, `n`, `z`), which softens the filter's reaction at thin features and along
   silhouettes, and it makes the `4 s²` scaling partially redundant.
   **Decision for v1: compute all weights at full resolution.** Fewer images, no half-res
   format/alignment bookkeeping, sharper edge stopping at `s = 1, 2`, and the cost at our
   resolutions is bandwidth we can afford. Revisit only if profiling shows the à-trous
   passes dominating (§10).

---

## 5. The specular channel

**Correction to the premise.** The paper does *not* split diffuse from specular by roughness.
It splits **direct from indirect** illumination ("We reconstruct direct and indirect lighting
separately... Separation appears to double cost, but since many steps use the rasterized
G-buffer, significant work is shared"), filters the two with independent temporal states and
variance images, and sums them after the last iteration; in its own results it states that
separating the two is *not* the main source of its quality win, and its evaluation section
says the runs shown use "temporally accumulated, demodulated samples containing both direct
and indirect illumination" — i.e. a single combined channel. A **roughness-based
diffuse/specular split** is the later community/engine variant (it is what Falcor-style
passes do), and it is the useful one for this renderer because our materials are mostly
metal/glass.

The split, if/when it is wanted (v2):

```
ws = smoothstep(0.0, 0.3, roughness);       // 0 = mirror-like, 1 = fully diffuse
Cs = Cd * ws;   Cdiff = Cd * (1.0 - ws);    // roughness = materials[m].params.y
```

Run §2 and §4 twice, with **separate** `histColor`/`histMoments`/`variance` per channel
(they must not share `n` or variance), and add the two filtered channels before remodulating
albedo. `wl` is then computed per channel, so a mirror pixel whose specular signal is noisy
does not get its low-variance diffuse neighbour's opinion.

What degrades with one combined channel (v1 — accept these, they are the cost):

* **Sharp reflections blur.** The filter has no idea that a mirror's lobe support is
  infinitesimal, so a bright, sharp reflection of an emissive sphere is treated as noise on
  a smooth surface and spread out. The paper lists this as its own limitation
  ("Over-Blurred Specular Reflections"). With two orbiting emissive spheres this is the most
  visible artifact we will get.
* **Coloured/chroma over-blur.** Variance is luminance-only (the paper's own limitation), so
  a colourful reflection gets averaged with desaturated neighbours.
* **Wrong variance guidance at material boundaries.** A pixel that is 100 % mirror sitting
  next to lambert pixels gets its `V` from a neighbourhood whose dominant sample count and
  noise level belong to a different transport type, so `wl` is mis-scaled in both directions:
  over-blurring the mirror, under-blurring the diffuse.
* **Specular ghosting is worse than diffuse ghosting.** A reflection is not anchored to the
  surface that shows it: reprojection by the *surface* depth and normal carries a highlight's
  history to the wrong place as the camera orbits. Nothing in §2.2 can detect this; only the
  separate channel plus a shorter `nMax` for it can limit the damage.
* Fireflies from specular bounces spread into streaks along the à-trous support instead of
  being averaged out (§8).

---

## 6. Default parameters

| Parameter | Default | Why |
|---|---|---|
| `alphaMin` (= `1/nMax`) | `0.05` | Long enough to converge in ~20 frames, short enough that orbiting lights stop ghosting in ~1/3 s. Paper uses 0.2 (≈5 frames) with moving geometry; we have static geometry, so history never self-clears and the cap is the only forgetfulness we get. |
| `nMax` | `20.0` | The forgetfulness bound above; also bounds how wrong the variance estimate can get from stale lighting. Raise to 32 for slow camera drift, drop to 8 for fast orbit. |
| warm-up | `alpha_i = 1/(n_i+1)` | Prevents the dim first frame from being weighted 50 %. |
| `cosThetaNormal` (temporal) | `0.9` | Rejects facets > ~26° apart: crease/ thin-object history bleed, while surviving shaded normal noise on curved surfaces. |
| `tau_grad` (temporal depth) | `1.0` | Allows the full depth change the local slope predicts; >1 starts accepting across silhouettes (ghosting), <1 starts rejecting on slanted surfaces (grazing-angle sparkle). |
| `tau_rel`, `tau_abs` (temporal depth) | `2e-3`, `1e-4` | Accepts float noise when the reprojection offset is ~0 (static camera). |
| `nMinTemporalVar` | `4.0` | Below this the moment estimate is meaningless; use the spatial estimate (paper value). |
| spatial-variance window | 7×7 | Wider than the 3×3 prefilter because it must estimate variance from unfiltered signal. |
| `sigma_z` (à-trous depth) | `1.0` | Paper value; with the plane-model denominator this is scale-free, so it needs no scene-specific tuning. |
| `sigma_n` (à-trous normal) | `128` | Paper value; makes the cosine test nearly binary — only real facet changes stop the blur. |
| `sigma_l` (à-trous luminance) | `4.0` | Paper value; with the `4 s²` scaling (§4.3) it is the single most sensitive knob in the filter — start here before touching anything else. |
| `4 s²` variance step scaling | on | Keeps the wide iterations filtering; see §4.3. |
| variance prefilter | 3×3 binomial /16 | Paper: stabilises the sigma estimate at 1 spp; without it single fireflies inflate local sigma and freeze the filter around them. |
| `epsilon` guards | `1e-4` depth denominator, `1e-6` luminance sigma/denominator and variance floor, `1e-3` albedo luminance, `1e-6` `L` in `encodeSrgb`-style paths | Every one of these denominators can legitimately hit 0 (sky, black albedo, perfectly smooth flat regions). |
| à-trous iterations | 5 (`s = 1,2,4,8,16`) | Paper's footprint; 4 halves the wide-radius blur and leaves blotches at 1 spp. |
| `luminance` weights | Rec. 709 | Matches ACES-ish tonemap ordering and gives no free boost to green. |
| optional sample soft-clamp `k` | `3.0` | Before the EMA: `Lsamp = clamp(Lsamp, mu1 ± k*sqrt(V))` scaled back to the sample; not in the paper, cheap firefly suppression (§8). Ship off, evaluate on. |

---

## 7. Compute dispatches per frame

`W x H` = trace resolution, grid = `ceil(W/8) x ceil(H/8)`. Reads/writes below are images;
`prev`/`cur` mark the ping-pong slot.

| # | Pass | Reads | Writes |
|---|---|---|---|
| D0 | `main.rgen` (existing graphics pass, extended) | BLAS/TLAS, `SceneBlock` incl. prev camera + prev light positions | `radianceIn` (1 spp, no accumulation), `demodAlbedo`, `normal`, `depth`, `matId`, `prevPos` |
| D1 | temporal accumulate + moments | `radianceIn`, `demodAlbedo`, `normal`, `depth`, `matId`, `prevPos`, `histColor[prev]`, `histMoments[prev]`, `histDepth[prev]`, `histNormal[prev]`, `histMatId[prev]` | `histMoments[cur]` (`mu1,mu2,n`), `histDepth[cur]`, `histNormal[cur]`, `histMatId[cur]`, `sigma2` (working R32F), `C0` (working RGBA16F, = temporally integrated demodulated colour) |
| D2 | à-trous iteration 1 (`s=1`) | `C0`, `sigma2`, `depth`, `normal` | `C1`, `sigma2b`, **`histColor[cur]`** (= `C1`) |
| D3 | à-trous iteration 2 (`s=2`) | `C1`, `sigma2b`, `depth`, `normal` | `C0`, `sigma2` |
| D4 | à-trous iteration 3 (`s=4`) | `C0`, `sigma2`, `depth`, `normal` | `C1`, `sigma2b` |
| D5 | à-trous iteration 4 (`s=8`) | `C1`, `sigma2b`, `depth`, `normal` | `C0`, `sigma2` |
| D6 | à-trous iteration 5 (`s=16`) + remodulate albedo | `C0`, `sigma2`, `depth`, `normal`, `demodAlbedo` | `filtered` (RGBA16F storage image; written as `C1 * demodAlbedo`) |
| (D7) | optional: variance visualisation / 3×3 prefilter for debugging | `sigma2` | `debugRGBA16F` |
| — | `vkCmdCopyImage` `filtered` → `hdrSampled` | `filtered` | `hdrSampled` (present's sampler) |
| P0 | `present.frag` (unchanged apart from §9) | `hdrSampled` | back buffer |

Six compute dispatches (D1…D6). Fusing remodulation into D6 saves a dispatch and a full-res
read+write; keep it separate if you want to visualise the filtered illumination by itself.

Images that must be allocated (each full-res unless noted):

* persistent, ×2 slots (ping-pong): `histColor` RGBA16F, `histMoments` RGBA32F,
  `histDepth` R32F, `histNormal` RGBA16F, `histMatId` R32F — **10 images**
* persistent, ×1: `demodAlbedo` RGBA16F, `prevPos` RG32F, `depth` RGBA16F-normal/current is
  ×2 (it is read as history in D1 and written in D0 → `depth` also needs two slots, and
  likewise `normal` and `matId`: **6 more images**, `depth` R32F ×2, `normal` RGBA16F ×2,
  `matId` R32F ×2)
* working, ×1 each: `C0`, `C1` RGBA16F, `sigma2`, `sigma2b` R32F, `filtered` RGBA16F,
  `hdrSampled` RGBA16F — **6 images**
* `radianceIn` reuses the existing accumulation target.

Total new images ≈ **22** at ~8 MB each at 1920×1080 → ≈ 180 MB. Reductions, in the order
worth taking them: `demodAlbedo`→RGBA8 (unpacked to linear in the shader), `normal`→RGB10A2
with an oct-encoded 2-component variant if needed, `matId`+`n`+`nMin` flags packed into one
R32F, and — the big one — running `sigma2`/`sigma2b` at half resolution per §4.6. All of
them must be added to the resize/retire path in `src/frame.cpp` (the existing
"retire the accumulation target by moving it aside" logic) or a resize will read freed memory.
Descriptor sets: one per pass, `set = 1` so `set = 0` (`SceneBlock`) stays as-is.

---

## 8. Artifacts to expect, and the knob for each

| Artifact | Why it happens here | Knob |
|---|---|---|
| **Ghosting / light trails from the orbiting lights** | Static geometry ⇒ reprojection keeps succeeding while the illumination changes; nothing rejects the stale history. | `nMax`↓ (`20→8`), `alphaMin`↑; per-light motion-aware `alpha`: scale `alpha_i` by `1 - clamp(solidAngleDeltaOfLight/radius)`; last resort, force `n = 0` on pixels whose `matId` is a light. |
| **Over-blurring at grazing angles** | Depth gradient is huge, so `wz`'s plane denominator is huge and the filter happily averages a foreground sliver with the background; likewise the temporal depth test accepts nearly anything. | `sigma_z`↓ (1 → 0.5), and make the *temporal* `tau_grad` tighter (1.0 → 0.5); check that `depth` is linear view depth, not `1/z` or NDC z, which makes the gradient explode near the horizon. |
| **Temporal lag / smearing during fast camera moves** | Too much history weight; reprojection error sub-pixel bias accumulates. | `alphaMin`↑, `nMax`↓; verify `prevPos` uses the *previous* camera and matches the current one's `tanHalf`/aspect exactly (a stale `prevPos` formula shows up as one-frame-direction smear); keep the 3×3 retry — without it, silhouettes fall back to 1 spp and shimmer. |
| **Fireflies surviving (or spreading into streaks)** | A single huge `Lsamp` inflates `mu2`, so `V` is huge, so `wl` says "blur freely" and the spike gets averaged across 125 px of screen. | Radiance clamp in the ray tracer (already available as `CAM_POST.z`) to ~50× the local mean; the `k`-sigma soft clamp before the EMA (§6); `sigma_l`↓; note the clamp must happen *before* the EMA, not after the filter. |
| **Edges eaten when albedo and geometry disagree** | Same plane, different material (painted stripe, metal beside lambert): depth and normal agree, `V` says smooth, so the filter averages across the boundary. Demodulation removes the multiplicative part of this but not the BRDF part. | Tighter `sigma_l` (the only signal the filter can see); put the albedo/roughness difference into `matId` (quantise albedo luma into the id) so D1 and the `w` functions reject; for v1, accept it and document it as the reason for the v2 channel split. |
| (bonus) **Detail loss in slowly-moving scenes** | History accumulates the *filtered* colour, so bias compounds across frames. | Lower `nMax`; or feed the pre-filter sample into history instead of `C1` (§4.5) and accept more noise for less dissolution. |

---

## 9. Interaction with progressive accumulation

**They must not both run.** Concretely: `accumulationFrames = 1` (`src/scene.h`,
`scene.json`) while SVGF is enabled, `radianceIn` gets one path per pixel per frame written
with `imageStore` (not `+=`), and the present pass must receive `CAM_DISPLAY.x = 1` so its
`radiance /= samples` is a no-op. `CAM_FRAME.z` (reset accumulation) is reinterpreted as
"reset all SVGF history": on every reset clear `histMoments` (`n = 0`), and treat
`n = 0` pixels as disocclusions.

Why, stated plainly:

1. **The variance estimate assumes uncorrelated samples.** §4.4's propagation formula and
   §2.4's moments are both built on independent samples. Progressive accumulation makes the
   input to the temporal pass a *function of its own past*, so the sample-to-sample spread
   collapses toward 0, `sigma2 → 0`, and the filter concludes the image is noise-free — then
   refuses to filter, and the result is a noisy, frozen image.
2. **Double accumulation is not double sampling.** The EMA's `n` and the accumulation counter
   mean different things; with both enabled the effective sample count is wrong by a factor
   ~`nMax`, so nothing (not even the debug views) tells you the truth about convergence.
3. **History and accumulation disagree spatially.** Progressive accumulation averages in
   *screen* space at fixed `p`; SVGF averages along *reprojected* surface motion. Enabling
   both mixes an un-reprojected average into a reprojected one, which shows up as ghosting
   that no temporal parameter can remove.
4. **The whole point of SVGF is 1 spp per frame.** The paper is built for exactly one path per
   pixel per frame, and §5.6 of it warns that the design (EMA variance, `wl` scaling) does
   *not* carry over to higher spp: with more samples, a proper sample-variance estimator
   should be used instead of the EMA of moments.

Per-frame sample count: **1 spp.** If the camera and both lights are exactly still for ≥ 1 s,
a useful mode is: keep SVGF's temporal pass but let `nMax` grow (or disable SVGF entirely and
let `accumulationFrames` run) — they are two ways of doing the same thing, and the switch
should be explicit in the UI/console, never implicit. Note also that the existing capture path
(`--accum N`, `src/main.cpp`) accumulates in the ray tracer; with SVGF on it must either be
disabled or redefined as "run SVGF for N frames and save the last", otherwise reference
captures silently bypass the filter.

---

## 10. Notes on sources, and where this spec had to decide

* The preprint was read from `cg.ivd.kit.edu/publications/2017/svgf/svgf_preprint.pdf` (the
  NVIDIA research URL in the task returns 404). Verified verbatim: kernel
  `h = {1/16,1/4,3/8,1/4,1/16}`, `w = wz·wn·wl`, Eq. 3 depth weight with the clip-space plane
  model and `sigma_z = 1`, Eq. 4 `wn = max(0, n(p)·n(q))^sigma_n` with `sigma_n = 128`,
  Eq. 5 `wl` with `sigma_l = 4` over `sigma_3x3(Var(L))`, `alpha = 0.2`, five wavelet levels,
  "65×65", moments `sigma² = mu2 - mu1²`, spatial fallback "a 7×7 bilateral filter" when
  history `< 4` frames, 2×2-tap resampling with per-tap tests, weight redistribution to valid
  taps, 3×3 retry, first-iteration colour as history, 3×3 Gaussian variance prefilter used
  only for `wl` and not propagated, and the limitations list (over-blurred chrominance,
  detached shadows in motion, unstable noise in very low light, over-blurred specular
  reflections, over-blurred features under motion).
* **Disagreement 1** — à-trous footprint: paper's 65×65 vs the 125×125 you get by summing
  `2s` over `s = 1..16`. Cosmetic; the step sequence is what matters.
* **Disagreement 2** — `alpha`'s convention (new-sample weight vs history weight) differs
  between the paper's formula and widely used reimplementation defaults. This spec always
  uses the new-sample weight.
* **Judgement call** — `4 s²` scaling of the variance inside `sigma_l` (§4.3): taken from the
  common reference implementation, not printed in the paper, and in tension with the paper's
  own sentence about `wl` growing in influence. Default on, with the off-switch documented.
* **Judgement call** — full-resolution guidance instead of halved-resolution variance/normal/
  depth (§4.6), and the 3×3 variance prefilter folded into the à-trous passes rather than a
  separate dispatch.
* **Judgement call** — linear view depth for the plane model instead of clip-space depth
  (§2.2), with `1/z` named as the correct fallback.
* **Not in the paper, added here** — the `k`-sigma sample clamp, the `k`-sigma soft clamp
  before the EMA, and using quantised albedo inside `matId`. All are opt-in.
* The paper separates *direct/indirect*, not *diffuse/specular*; §5 says so and gives the
  roughness split as the v2 design.

## Open questions

1. Do we want the direct/indirect split (the paper's actual separation) instead of, or in
   addition to, the roughness split? Direct/indirect needs the raygen to write two radiance
   images, which is a small change; roughness needs only the G-buffer roughness channel.
2. Should `histColor` store the first-iteration output (paper) or the raw temporal average?
   Paper is better for noise, worse for detail dissolution under our long-ish `nMax = 20`.
3. Do the two orbiting lights justify a light-motion term in the temporal blend weight, or is
   `nMax = 20` + `matId` rejection enough in practice? Needs a visual test, not a formula.
4. Is RGBA32F history (`histMoments`) affordable at our target resolutions, or do we clamp
   `Lsamp ≤ 1000` before accumulating and drop to RGBA16F?
5. Do we add a post-reconstruction TAA pass (the paper runs one after tonemapping, for
   geometric edges)? It conflicts with our dither in `present.frag` and with `ssaa > 1`.
6. The `present.frag` multi-sample branch (`ssaa > 1`) takes 4 taps of the HDR target; with
   SVGF the filtered image must be sampled 1:1. Do we lock SVGF to `ssaa = 1` for v1?
7. Fallback path (`shaders/fallback.comp`, no RT extension): must it write all six G-buffer
   images too, or do we declare SVGF unsupported there and log it?
8. First-frame and post-resize behaviour: one frame of unfiltered noise, or run 5 extra
   temporal iterations in one frame to warm the history?
