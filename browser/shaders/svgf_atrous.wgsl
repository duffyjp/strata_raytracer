// svgf_atrous.wgsl - browser twin of shaders/svgf_atrous.comp (SVGF wavelet iteration).
//
// Five iterations with the dilation doubling give a 65x65 footprint at O(1) per pixel. Each tap is
// weighted by the separable 5-tap kernel times three edge-stopping terms: depth agreement in the
// reciprocal domain, a sharp power of the normal dot product, and luminance agreement scaled by a
// 3x3 blur of the local variance. The variance term is what makes the filter adaptive, and the
// threshold has to grow with the distance a tap reaches or the wide iterations stop blurring at all.
//
// Works on demodulated radiance; the final iteration multiplies albedo back in.
//
// ATROUS_TRIVIAL compiles the same texture traffic without the maths, exactly as the probe timed a
// "trivial" a-trous beside the real one - without it a floor-bound number is indistinguishable from a
// genuinely fast pass.

const ATROUS_TRIVIAL : u32 = 0u;

struct Params {
  originTan : vec4<f32>,
  rightAspect : vec4<f32>,
  upOne : vec4<f32>,
  fwdBounces : vec4<f32>,
  frame : vec4<f32>,
  post : vec4<f32>,
  display : vec4<f32>,
  sky : vec4<f32>,
  filt : vec4<f32>,
  prevOrigin : vec4<f32>,
  prevRightAspect : vec4<f32>,
  prevUp : vec4<f32>,
  prevFwd : vec4<f32>,
  bvh : vec4<f32>,
  sigmas : vec4<f32>,          // x = sigmaZ, y = sigmaN, z = sigmaL
  check : vec4<f32>,
  misc : vec4<f32>,            // x = step, y = first, z = last
};

@group(0) @binding(0) var<uniform> P : Params;
@group(0) @binding(1) var colorTex : texture_2d<f32>;
@group(0) @binding(2) var varTex : texture_2d<f32>;
@group(0) @binding(3) var albedoTex : texture_2d<f32>;
@group(0) @binding(4) var normalTex : texture_2d<f32>;
@group(0) @binding(5) var depthTex : texture_2d<f32>;
@group(0) @binding(6) var outColor : texture_storage_2d<rgba16float, write>;
@group(0) @binding(7) var outVar : texture_storage_2d<rgba16float, write>;

// Separable a-trous kernel. Sum is 1, so a flat region passes through unchanged apart from the edge
// weights - the property that keeps the filter from brightening or darkening the image.
fn kernelTap(i : i32) -> f32 {
  switch (i) {
    case -2: { return 1.0 / 16.0; }
    case -1: { return 1.0 / 4.0; }
    case 0: { return 3.0 / 8.0; }
    case 1: { return 1.0 / 4.0; }
    default: { return 1.0 / 16.0; }
  }
}

// 3x3 Gaussian used for the variance blur: 1 2 1 / 2 4 2 / 1 2 1, row-major, i in [0, 9).
fn gaussW(i : i32) -> f32 {
  let row = i / 3 - 1;
  let col = i % 3 - 1;
  let wr = select(1.0, 2.0, row == 0);
  let wc = select(1.0, 2.0, col == 0);
  return wr * wc;
}

fn lum(c : vec3<f32>) -> f32 { return dot(c, vec3<f32>(0.2126, 0.7152, 0.0722)); }

fn normAt(tex : texture_2d<f32>, p : vec2<i32>) -> vec3<f32> {
  return normalize(textureLoad(tex, p, 0).rgb + vec3<f32>(1e-6));
}

// Variance of a pixel in the form this iteration expects: the first iteration reads the temporal
// moments E[Y], E[Y^2]; the rest read propagated variance.
fn varAt(tex : texture_2d<f32>, p : vec2<i32>, first : bool) -> f32 {
  let m = textureLoad(tex, p, 0);
  if (first) { return max(0.0, m.y - m.x * m.x); }
  return max(0.0, m.x);
}

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let pixel = vec2<i32>(gid.xy);
  let res = vec2<i32>(i32(P.frame.x), i32(P.frame.y));
  if (pixel.x >= res.x || pixel.y >= res.y) { return; }

  let first = P.misc.y > 0.5;
  let last = P.misc.z > 0.5;
  let s = max(1, i32(P.misc.x));
  let c0 = textureLoad(colorTex, pixel, 0).rgb;

  if (ATROUS_TRIVIAL == 1u) {
    var outc = c0;
    if (last) {
      let al = textureLoad(albedoTex, pixel, 0);
      if (al.w > 0.5) { outc = outc * al.rgb; }
    }
    textureStore(outColor, pixel, vec4<f32>(max(outc, vec3<f32>(0.0)), 1.0));
    textureStore(outVar, pixel, vec4<f32>(varAt(varTex, pixel, first), 0.0, 0.0, 1.0));
    return;
  }

  let n0 = normAt(normalTex, pixel);
  let z0 = textureLoad(depthTex, pixel, 0).x;
  let l0 = lum(c0);

  // The luminance edge-stopping scale is set by a 3x3 blur of the variance, not the variance itself:
  // one noisy variance sample must not decide the threshold for its whole neighbourhood.
  var vBlur = 0.0;
  {
    var acc = 0.0;
    var wsum = 0.0;
    var i = 0;
    for (var dy = -1; dy <= 1; dy = dy + 1) {
      for (var dx = -1; dx <= 1; dx = dx + 1) {
        let q = clamp(pixel + vec2<i32>(dx, dy), vec2<i32>(0, 0), res - vec2<i32>(1, 1));
        acc = acc + gaussW(i) * varAt(varTex, q, first);
        wsum = wsum + gaussW(i);
        i = i + 1;
      }
    }
    vBlur = max(1e-6, acc / wsum);
  }

  // The threshold grows with the tap distance, because variance accumulates over that distance: two
  // samples two pixels apart disagree by more than two samples a hundred pixels apart would, purely
  // from noise.
  let fs = f32(s);
  let sigmaLEff = max(1e-6, P.sigmas.z * sqrt(4.0 * fs * fs * vBlur));

  var acc = vec3<f32>(0.0);
  var wTotal = 0.0;
  var varAcc = 0.0;      // propagated variance, summed as sum(h^2 w^2 V)
  var wLin = 0.0;        // sum(h w), squared at the end

  for (var dy = -2; dy <= 2; dy = dy + 1) {
    for (var dx = -2; dx <= 2; dx = dx + 1) {
      let hw = kernelTap(dy) * kernelTap(dx);
      let q = clamp(pixel + vec2<i32>(dx * s, dy * s), vec2<i32>(0, 0), res - vec2<i32>(1, 1));

      let cq = textureLoad(colorTex, q, 0).rgb;
      let zq = textureLoad(depthTex, q, 0).x;
      let vq = varAt(varTex, q, first);

      // Depth weight in the reciprocal domain: surfaces at very different distances must not smooth
      // into each other, which is what keeps foreground from painting the background behind it.
      let wz = exp(-abs(z0 - zq) / max(1e-6, P.sigmas.x));
      // Normal weight: a sharp power makes it a near-binary test, so creases survive the wide passes.
      let wn = pow(max(0.0, dot(n0, normAt(normalTex, q))), max(1.0, P.sigmas.y));
      let wl = exp(-abs(l0 - lum(cq)) / (sigmaLEff + 1e-6));

      let w = hw * wz * wn * wl;
      acc = acc + w * cq;
      wTotal = wTotal + w;
      varAcc = varAcc + hw * hw * w * w * vq;
      wLin = wLin + hw * w;
    }
  }

  var outc = acc / max(1e-6, wTotal);
  let outV = varAcc / max(1e-8, wLin * wLin);

  if (last) {
    let al = textureLoad(albedoTex, pixel, 0);
    if (al.w > 0.5) { outc = outc * al.rgb; }
  }

  textureStore(outColor, pixel, vec4<f32>(max(outc, vec3<f32>(0.0)), 1.0));
  textureStore(outVar, pixel, vec4<f32>(max(outV, 0.0), 0.0, 0.0, 1.0));
}
