// present.wgsl - tone map and encode, the browser twin of shaders/present.frag for the drawn path and
// of writePng() in src/frame.cpp for the captured path.
//
// Those two are not the same function and the difference matters for a pixel comparison:
//   present.frag  divides by the sample count, tone maps, THEN multiplies by the vignette, and dithers.
//   writePng      multiplies by exposure and vignette in linear space, THEN tone maps, and does not
//                 dither - and it derives the vignette from the integer pixel index, not the centre.
// The reference stills this demo is checked against came from writePng, so the mode is selectable per
// pass: 0 = capture (writePng), 1 = window (present.frag).
//
// It is a compute pass into an rgba8unorm texture rather than a fragment pass so the exact bytes that
// reach the screen can be copied out and compared without a second, drifting implementation of the same
// colour pipeline. The blit to the canvas afterwards only changes channel order in memory, not values.
//
// Feature views are not done here. The native app writes the debug channel into the HDR target from the
// raygen shader (main.rgen's dbg branches) and writePng reads that target, and trace.wgsl does the
// same - so a feature dump stays on exactly the path the reference was captured through.

struct Params {
  originTan : vec4<f32>,
  rightAspect : vec4<f32>,
  upOne : vec4<f32>,
  fwdBounces : vec4<f32>,
  frame : vec4<f32>,
  post : vec4<f32>,           // x = exposure
  display : vec4<f32>,        // x = samples accumulated for display
  sky : vec4<f32>,
  filt : vec4<f32>,         // w = denoiser output is the thing to show
  prevOrigin : vec4<f32>,
  prevRightAspect : vec4<f32>,
  prevUp : vec4<f32>,
  prevFwd : vec4<f32>,
  bvh : vec4<f32>,
  sigmas : vec4<f32>,
  check : vec4<f32>,
  misc : vec4<f32>,           // x = dither on, y = present mode (0 capture, 1 window)
};

@group(0) @binding(0) var<uniform> P : Params;
@group(0) @binding(1) var hdrSam : texture_2d<f32>;
@group(0) @binding(2) var denSam : texture_2d<f32>;
@group(0) @binding(3) var outTex : texture_storage_2d<rgba8unorm, write>;

fn acesApprox(x : vec3<f32>) -> vec3<f32> {
  let a = 2.51; let b = 0.03; let c = 2.43; let d = 0.59; let e = 0.14;
  return clamp((x * (a * x + b)) / (x * (c * x + d) + e), vec3<f32>(0.0), vec3<f32>(1.0));
}

fn aces1(x : f32) -> f32 {
  let a = 2.51; let b = 0.03; let c = 2.43; let d = 0.59; let e = 0.14;
  return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

fn encodeSrgb(c : f32) -> f32 {
  if (c <= 0.0031308) { return c * 12.92; }
  return 1.055 * pow(max(c, 1e-6), 1.0 / 2.4) - 0.055;
}

// fract(sin(dot(p, (12.9898, 78.233))) * 43758.5453), as present.frag writes it.
fn ditherNoise(p : vec2<f32>) -> f32 {
  return fract(sin(dot(p, vec2<f32>(12.9898, 78.233))) * 43758.5453);
}

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let dst = vec2<i32>(gid.xy);                        // y = 0 is the TOP of the image
  let res = vec2<i32>(i32(P.frame.x), i32(P.frame.y));
  if (dst.x >= res.x || dst.y >= res.y) { return; }

  // The trace target has row 0 at the bottom - that is where the ray origin's rows start, and why
  // writePng flips - while the presentation texture does not, so sample the flipped row.
  let src = vec2<i32>(dst.x, res.y - 1 - dst.y);

  var colour : vec3<f32>;
  if (P.filt.w > 0.5) {
    // The denoiser's output is already a mean radiance, so it replaces the accumulated value rather
    // than being divided by the frame count a second time.
    colour = textureLoad(denSam, src, 0).rgb;
  } else {
    colour = textureLoad(hdrSam, src, 0).rgb / max(1.0, P.display.x);
  }

  let exposure = P.post.x;
  let qx = f32(dst.x) / f32(res.x) - 0.5;
  let qy = f32(dst.y) / f32(res.y) - 0.5;
  let vign = 1.0 - 0.45 * (qx * qx + qy * qy);

  var enc : vec3<f32>;
  if (P.misc.y < 0.5) {
    // writePng(): linear vignette, then per-channel ACES, then sRGB. No dither.
    let v = colour * (exposure * vign);
    enc = vec3<f32>(encodeSrgb(clamp(aces1(v.r), 0.0, 1.0)),
                    encodeSrgb(clamp(aces1(v.g), 0.0, 1.0)),
                    encodeSrgb(clamp(aces1(v.b), 0.0, 1.0)));
  } else {
    // present.frag(): ACES first, vignette after, plus a dither of +-0.5/255.
    var c = acesApprox(colour * exposure) * vign;
    enc = vec3<f32>(encodeSrgb(clamp(c.r, 0.0, 1.0)),
                    encodeSrgb(clamp(c.g, 0.0, 1.0)),
                    encodeSrgb(clamp(c.b, 0.0, 1.0)));
    if (P.misc.x > 0.5) {
      let n = ditherNoise(vec2<f32>(f32(dst.x) + 0.5, f32(res.y - dst.y) - 0.5));
      enc = enc + vec3<f32>((n - 0.5) / 255.0);
    }
  }

  textureStore(outTex, dst, vec4<f32>(clamp(enc, vec3<f32>(0.0), vec3<f32>(1.0)), 1.0));
}
