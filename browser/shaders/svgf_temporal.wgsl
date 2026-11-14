// svgf_temporal.wgsl - browser twin of shaders/svgf_temporal.comp (Schied et al. 2017 temporal pass).
//
// Reprojects the previous filtered frame onto this one through the previous frame's camera basis,
// decides whether that history still describes this piece of the world, blends with an exponential
// moving average whose weight comes from a per-pixel history length, and tracks the first two moments
// of demodulated luminance because the spatial filter's edge-stopping threshold is driven by them.
//
// TEMPORAL_TRIVIAL compiles a copy-only variant for the timing table: an a-trous or temporal number
// that sits at the dispatch floor is only meaningful next to a pass that does the same texture traffic
// without the maths.

const TEMPORAL_TRIVIAL : u32 = 0u;

struct Params {
  originTan : vec4<f32>,
  rightAspect : vec4<f32>,
  upOne : vec4<f32>,
  fwdBounces : vec4<f32>,
  frame : vec4<f32>,          // x width, y height, z reset, w time
  post : vec4<f32>,
  display : vec4<f32>,
  sky : vec4<f32>,
  filt : vec4<f32>,         // x debug view, y history cap, z previous frame valid, w denoise on
  prevOrigin : vec4<f32>,
  prevRightAspect : vec4<f32>,
  prevUp : vec4<f32>,
  prevFwd : vec4<f32>,
  bvh : vec4<f32>,
  sigmas : vec4<f32>,
  check : vec4<f32>,
  misc : vec4<f32>,
};

@group(0) @binding(0) var<uniform> P : Params;
@group(0) @binding(1) var noisyTex : texture_2d<f32>;
@group(0) @binding(2) var albedoTex : texture_2d<f32>;
@group(0) @binding(3) var normalTex : texture_2d<f32>;
@group(0) @binding(4) var depthTex : texture_2d<f32>;
@group(0) @binding(5) var histColorTex : texture_2d<f32>;
@group(0) @binding(6) var histMomTex : texture_2d<f32>;
@group(0) @binding(7) var histDepTex : texture_2d<f32>;
@group(0) @binding(8) var outColor : texture_storage_2d<rgba16float, write>;
@group(0) @binding(9) var outMom : texture_storage_2d<rgba16float, write>;
@group(0) @binding(10) var outDep : texture_storage_2d<rgba16float, write>;

const LUM_EPS : f32 = 1e-3;

fn lum(c : vec3<f32>) -> f32 { return dot(c, vec3<f32>(0.2126, 0.7152, 0.0722)); }

// A point's screen coordinates in another camera. False when the point is behind it.
fn project(p : vec3<f32>) -> vec2<f32> {
  let v = p - P.prevOrigin.xyz;
  let c = dot(v, P.prevFwd.xyz);
  if (c <= 1e-4) { return vec2<f32>(-1.0, -1.0); }
  let a = dot(v, P.prevRightAspect.xyz);
  let b = dot(v, P.prevUp.xyz);
  let ndc = vec2<f32>((a / c) / (P.prevRightAspect.w * P.prevOrigin.w), b / c / P.prevOrigin.w);
  return ndc * 0.5 + 0.5;
}

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let pixel = vec2<i32>(gid.xy);
  let res = vec2<i32>(i32(P.frame.x), i32(P.frame.y));
  if (pixel.x >= res.x || pixel.y >= res.y) { return; }

  let al = textureLoad(albedoTex, pixel, 0);
  let dp = textureLoad(depthTex, pixel, 0);
  let radiance = textureLoad(noisyTex, pixel, 0).rgb;

  // The filter always writes something, even where it declines to filter, so the next stage never
  // reads an untouched texel.
  var demod = radiance;
  let canDemod = al.w > 0.5 && lum(al.rgb) > LUM_EPS;
  if (canDemod) { demod = radiance / al.rgb; }

  if (TEMPORAL_TRIVIAL == 1u) {
    textureStore(outColor, pixel, vec4<f32>(demod, 1.0));
    textureStore(outMom, pixel, vec4<f32>(lum(demod), lum(demod) * lum(demod), 1.0, 1.0));
    textureStore(outDep, pixel, dp);
    return;
  }

  if (P.frame.z > 0.5) {
    // A reset (resize, quality change, first frame): history at this resolution does not exist.
    let y = lum(demod);
    textureStore(outColor, pixel, vec4<f32>(demod, 1.0));
    textureStore(outMom, pixel, vec4<f32>(y, y * y, 1.0, 1.0));
    textureStore(outDep, pixel, dp);
    return;
  }

  var worldPos = P.originTan.xyz;
  if (dp.y > 0.5) {
    let t = 1.0 / max(dp.x, 1e-6) - 1.0;
    let resf = vec2<f32>(f32(res.x), f32(res.y));
    let ndc = ((vec2<f32>(f32(pixel.x), f32(pixel.y)) + vec2<f32>(0.5)) / resf) * 2.0 - 1.0;
    let dir = normalize(P.fwdBounces.xyz +
                        P.rightAspect.xyz * (ndc.x * P.rightAspect.w * P.originTan.w) +
                        P.upOne.xyz * (ndc.y * P.originTan.w));
    worldPos = worldPos + dir * t;
  }

  var hist = vec4<f32>(0.0);
  var histMom = vec4<f32>(0.0);
  var histDepth = 0.0;
  var histMat = 0.0;
  var histValid = false;
  if (P.filt.z > 0.5 && dp.y > 0.5) {
    let uv = project(worldPos);
    if (all(uv >= vec2<f32>(0.0)) && all(uv < vec2<f32>(1.0))) {
      var hp = vec2<i32>(i32(uv.x * f32(res.x)), i32(uv.y * f32(res.y)));
      hp = clamp(hp, vec2<i32>(0, 0), res - vec2<i32>(1, 1));
      hist = textureLoad(histColorTex, hp, 0);
      histMom = textureLoad(histMomTex, hp, 0);
      let hd = textureLoad(histDepTex, hp, 0);
      histDepth = hd.x;
      histMat = hd.z;
      histValid = histMom.w > 0.5;
      // Two independent reasons the reprojected texel might not describe this surface any more:
      // material (two objects must not trade samples) and depth, compared in the reciprocal domain
      // with a relative tolerance - the conservative choice, exactly as the source documents.
      let zc = dp.x;
      let depthOk = abs(zc - histDepth) <= 0.01 * max(zc, histDepth) + 1e-3;
      let matOk = abs(dp.z - histMat) < 0.5;
      histValid = histValid && depthOk && matOk;
    }
  }

  // History length drives the blend: a long history takes a small new-sample weight, which is what
  // makes a static region converge, and the cap is the only forgetting mechanism this scene has.
  let nMax = max(2.0, P.filt.y);
  let nPrev = select(0.0, max(1.0, histMom.z), histValid);
  let alpha = max(1.0 / (1.0 + nPrev), 1.0 / nMax);
  let n = min(nMax, alpha * nPrev + 1.0);

  let base = select(demod, hist.rgb, histValid);
  let filtered = mix(base, demod, alpha);
  let y = lum(demod);
  let yh = select(y, histMom.x, histValid);
  let y2h = select(y * y, histMom.y, histValid);
  let yMean = mix(yh, y, alpha);
  let y2Mean = mix(y2h, y * y, alpha);

  textureStore(outColor, pixel, vec4<f32>(max(filtered, vec3<f32>(0.0)), 1.0));
  textureStore(outMom, pixel, vec4<f32>(yMean, y2Mean, n, 1.0));
  textureStore(outDep, pixel, dp);
}
