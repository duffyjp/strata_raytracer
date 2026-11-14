// trace.wgsl - the browser twin of shaders/main.rgen + shaders/pt_common.glsl.
//
// Transliterated, not redesigned: every constant, offset and clamp comes from the GLSL it replaces so
// the two renderers agree to within floating-point divergence. Where the GLSL has an oddity that a
// "cleanup" would silently change, that is copied too and commented, because the point of this file is
// a measurement of the *same* algorithm.
//
// TRACE_MODE and COUNT_ON are substituted by renderer.js before compilation and gate work at compile
// time, so the dead code is really gone (a runtime branch would still be timed):
//   TRACE_MODE 0  full path trace (what ships)
//              1  primary visibility only: trace + features, no BRDF, no NEE, no bounce
//              2  one bounce, full shading with NEE, no recursion
//              3  full transport with next-event estimation switched off
//   COUNT_ON   1  a separate instrumented pipeline that also tallies rays/hits per workgroup, so the
//                 "did this kernel silently do nothing?" guard exists without the timed pipelines
//                 paying for it.

const TRACE_MODE : u32 = 0u;
const COUNT_ON : u32 = 0u;

const LIGHT_COUNT : u32 = 4u;
const KIND_SPHERE : u32 = 0u;
const KIND_CUBE : u32 = 1u;
const KIND_CONE : u32 = 2u;
const KIND_DISC : u32 = 3u;
const PI : f32 = 3.14159265358979323846;
const RAY_TMIN : f32 = 2e-3;          // main.rgen passes 2e-3 as the ray minimum to every trace
const STACK_CAP : u32 = 64u;

struct Params {
  originTan : vec4<f32>,      // xyz camera origin, w = tan(fovY/2)
  rightAspect : vec4<f32>,    // xyz right, w = aspect
  upOne : vec4<f32>,          // xyz up
  fwdBounces : vec4<f32>,     // xyz forward, w = max bounces
  frame : vec4<f32>,          // x = width, y = height, z = reset accumulation, w = time
  post : vec4<f32>,           // x = exposure, y = frame index, z = radiance clamp, w = sky mode
  display : vec4<f32>,        // x = samples for display, y = samples per pixel
  sky : vec4<f32>,            // xyz horizon colour, w = feature writes on
  filt : vec4<f32>,         // x = debug view, y = history cap, z = previous frame valid, w = denoise on
  prevOrigin : vec4<f32>,
  prevRightAspect : vec4<f32>,
  prevUp : vec4<f32>,
  prevFwd : vec4<f32>,
  bvh : vec4<f32>,            // x = root node, y = occluder-only root, z = triangle count
  sigmas : vec4<f32>,         // x = sigmaZ, y = sigmaN, z = sigmaL
  check : vec4<f32>,          // x = check pixels (0 = off)
  misc : vec4<f32>,
};

struct Mat {   // 64 bytes: the same four rows as Material in shaders/sceneblock.glsl
  albedo : vec4<f32>,         // rgb, w = diffuse amount
  emissive : vec4<f32>,
  params : vec4<f32>,         // x reflectivity, y roughness, z ior, w texture scale
  flags : vec4<f32>,          // x model (0 lambert 1 metal 2 glass 3 emissive), y texture slot or -1
};

struct Light {
  posRadius : vec4<f32>,      // xyz centre, w radius
  color : vec4<f32>,          // rgb * intensity, w on/off
};

struct Inst {                 // the 128 bytes fillInstanceMatrices uploads
  o2w : mat4x4<f32>,
  w2o : mat4x4<f32>,
};

struct Node {                 // see bvh.js: b < 0 marks a leaf holding [a, a + (-b-1))
  mnA : vec4<f32>,            // xyz = bounds min, w = left child when internal
  mxB : vec4<f32>,            // xyz = bounds max, w = right child, or -(count+1) when a leaf
};

struct Tri {
  v0 : vec4<f32>,
  v1 : vec4<f32>,
  v2 : vec4<f32>,
  info : vec4<u32>,           // x kind, y material, z instance, w occluder
};

struct Hit {
  hit : bool,
  t : f32,
  pos : vec3<f32>,
  posObj : vec3<f32>,
  normal : vec3<f32>,
  kind : u32,
  material : u32,
  frontFacing : bool,
};

@group(0) @binding(0) var<uniform> P : Params;
@group(0) @binding(1) var<storage, read> MATS : array<Mat>;
@group(0) @binding(2) var<storage, read> LIGHTS : array<Light>;
@group(0) @binding(3) var<storage, read> INST : array<Inst>;
@group(0) @binding(4) var<storage, read> NODES : array<Node>;
@group(0) @binding(5) var<storage, read> TRIS : array<Tri>;
@group(0) @binding(6) var tex0 : texture_2d<f32>;
@group(0) @binding(7) var tex1 : texture_2d<f32>;
@group(0) @binding(8) var samp : sampler;
// WebGPU will not give a read_write storage binding for rgba16float (only r32float/r32uint/r32sint may
// be read back inside the pass), and the error is not raised at createPipeline: it arrives as an
// uncaptured error and the whole dispatch is dropped, which from the CPU side looks like a render of
// zeros. So the accumulator is a write-only storage texture and the previous frame's total arrives as an
// ordinary texture to read. The native pass reads and writes one rgba16f image because Vulkan allows it;
// the browser equivalent is a ping-pong pair plus a copy, and renderer.js measures what that costs.
@group(0) @binding(9) var hdrTex : texture_storage_2d<rgba16float, write>;
@group(0) @binding(15) var hdrPrev : texture_2d<f32>;
@group(0) @binding(10) var gAlb : texture_storage_2d<rgba16float, write>;
@group(0) @binding(11) var gNrm : texture_storage_2d<rgba16float, write>;
@group(0) @binding(12) var gDep : texture_storage_2d<rgba16float, write>;
@group(0) @binding(13) var<storage, read_write> checkBuf : array<vec4<f32>>;
@group(0) @binding(14) var<storage, read_write> counters : array<atomic<u32>>;

// counters[0] pixels written, [1] primary hits, [2] shading rays, [3] occlusion rays,
// [4] occlusion hits. Per-workgroup tallies, so 0.9M threads do not serialise on one address.
var<workgroup> wgRays : array<u32, 64>;
var<workgroup> wgHits : array<u32, 64>;
var<workgroup> wgOccR : array<u32, 64>;
var<workgroup> wgOccH : array<u32, 64>;
var<private> gLane : u32 = 0u;      // this thread's slot in the tallies above, set once in main()
var<private> stack : array<u32, STACK_CAP>;

// --------------------------------------------------------------------------- rng
// pcgHash from pt_common.glsl, verbatim. u32 arithmetic wraps in WGSL the way it does in GLSL.
fn pcgHash(state : ptr<function, u32>) -> u32 {
  *state = *state * 747796405u + 2891336453u;
  let word = ((*state >> ((*state >> 28u) + 4u)) ^ *state) * 277803737u;
  return (word >> 22u) ^ word;
}

fn rnd(state : ptr<function, u32>) -> f32 {
  return f32(pcgHash(state)) * (1.0 / 4294967296.0);
}

fn rnd2(state : ptr<function, u32>) -> vec2<f32> {
  return vec2<f32>(rnd(state), rnd(state));
}

// --------------------------------------------------------------------------- primitives.glsl
// Declared ahead of traceRay because it computes the shading normal from the object-space hit point,
// exactly as hit.rchit does.
fn objectNormal(kind : u32, p : vec3<f32>) -> vec3<f32> {
  if (kind == KIND_SPHERE) {
    return normalize(p + vec3<f32>(1e-9));
  } else if (kind == KIND_CUBE) {
    let a = abs(p);
    var axis = vec3<f32>(0.0, 0.0, 1.0);
    if (a.x >= a.y && a.x >= a.z) {
      axis = vec3<f32>(1.0, 0.0, 0.0);
    } else if (a.y >= a.z) {
      axis = vec3<f32>(0.0, 1.0, 0.0);
    }
    return axis * sign(dot(axis, p) + 1e-9);
  } else if (kind == KIND_CONE) {
    let y = clamp(p.y, 0.0, 1.0);
    let rr = length(p.xz);
    if (y < 0.02 && rr > 0.02) { return vec3<f32>(0.0, -1.0, 0.0); }
    let g = vec3<f32>(2.0 * p.x, 2.0 * (1.0 - y), 2.0 * p.z);
    if (length(g.xz) < 1e-6) { return vec3<f32>(0.0, 1.0, 0.0); }
    return normalize(g);
  }
  return vec3<f32>(0.0, 1.0, 0.0);
}

fn objectNormalToWorld(w2o : mat4x4<f32>, nObj : vec3<f32>) -> vec3<f32> {
  let n = vec3<f32>(dot(w2o[0].xyz, nObj), dot(w2o[1].xyz, nObj), dot(w2o[2].xyz, nObj));
  let l = length(n);
  if (l > 1e-8) { return n / l; }
  return nObj;
}

fn objectUV(kind : u32, p : vec3<f32>) -> vec2<f32> {
  if (kind == KIND_DISC || kind == KIND_CUBE) { return vec2<f32>(p.x, p.z); }
  return vec2<f32>(atan2(p.z, p.x) / (2.0 * PI) + 0.5, p.y);
}

// --------------------------------------------------------------------------- traversal
fn boxT(ox : f32, oy : f32, oz : f32, idx : f32, idy : f32, idz : f32, mn : vec3<f32>,
        mx : vec3<f32>) -> f32 {
  let lox = (mn.x - ox) * idx;
  let hix = (mx.x - ox) * idx;
  let loy = (mn.y - oy) * idy;
  let hiy = (mx.y - oy) * idy;
  let loz = (mn.z - oz) * idz;
  let hiz = (mx.z - oz) * idz;
  let t1x = min(lox, hix);
  let t2x = max(lox, hix);
  let t1y = min(loy, hiy);
  let t2y = max(loy, hiy);
  let t1z = min(loz, hiz);
  let t2z = max(loz, hiz);
  let tn = max(max(t1x, t1y), t1z);
  let tf = min(min(t2x, t2y), t2z);
  if (tf < max(tn, 0.0)) { return -1.0; }
  // Clamped, not raw: a ray starting inside a box has a negative tn, and every caller here reads a
  // negative return as "no intersection". Without the clamp the camera - which is inside the scene's
  // own bounding box - rejects the root node and the whole frame renders as sky. That is exactly the
  // silent-no-op failure the BVH check exists to catch, and it is the check that caught it.
  return max(tn, 0.0);
}

// Moller-Trumbore, the same formulation and the same 1e-8 determinant guard the probe validated
// against an independent brute force at 40 of 40 sampled pixels.
fn triHit(ox : f32, oy : f32, oz : f32, dx : f32, dy : f32, dz : f32, t : Tri, tmax : f32) -> f32 {
  let e1 = t.v1.xyz - t.v0.xyz;
  let e2 = t.v2.xyz - t.v0.xyz;
  let px = dy * e2.z - dz * e2.y;
  let py = dz * e2.x - dx * e2.z;
  let pz = dx * e2.y - dy * e2.x;
  let det = e1.x * px + e1.y * py + e1.z * pz;
  if (abs(det) < 1e-8) { return -1.0; }
  let inv = 1.0 / det;
  let sx = ox - t.v0.x;
  let sy = oy - t.v0.y;
  let sz = oz - t.v0.z;
  let u = (sx * px + sy * py + sz * pz) * inv;
  if (u < 0.0 || u > 1.0) { return -1.0; }
  let qx = sy * e1.z - sz * e1.y;
  let qy = sz * e1.x - sx * e1.z;
  let qz = sx * e1.y - sy * e1.x;
  let v = (dx * qx + dy * qy + dz * qz) * inv;
  if (v < 0.0 || u + v > 1.0) { return -1.0; }
  let tt = (e2.x * qx + e2.y * qy + e2.z * qz) * inv;
  if (tt > RAY_TMIN && tt < tmax) { return tt; }
  return -1.0;
}

fn sceneRoot() -> u32 { return u32(P.bvh.x); }

// The native app gives the emissive orbs instance mask 2 and fires shadow rays with mask 1, so the
// orbs never occlude the light they are standing next to. The orbs live in their own BVH subtrees, so
// the browser equivalent is to start shadow rays at the node above the static geometry only.
fn occlRoot() -> u32 { return u32(P.bvh.y); }

fn traceRay(ox : f32, oy : f32, oz : f32, dx : f32, dy : f32, dz : f32, tmax : f32,
            root : u32, anyHit : bool) -> Hit {
  var h : Hit;
  h.hit = false;
  h.t = tmax;
  h.kind = 0u;
  h.material = 0u;
  h.frontFacing = false;
  let idx = 1.0 / dx;
  let idy = 1.0 / dy;
  let idz = 1.0 / dz;
  var tbest = tmax;
  var bestTri = 0u;
  var found = false;
  var done = false;

  var sp : u32 = 1u;
  stack[0] = root;
  while (sp > 0u && !done) {
    sp = sp - 1u;
    let nd = NODES[stack[sp]];
    if (boxT(ox, oy, oz, idx, idy, idz, nd.mnA.xyz, nd.mxB.xyz) < 0.0) { continue; }
    if (nd.mxB.w < 0.0) {
      let start = u32(nd.mnA.w);
      let cnt = u32(-nd.mxB.w - 1.0);
      for (var k = 0u; k < cnt; k = k + 1u) {
        let tri = TRIS[start + k];
        let t = triHit(ox, oy, oz, dx, dy, dz, tri, tbest);
        if (t > 0.0) {
          tbest = t;
          bestTri = start + k;
          found = true;
          if (anyHit) { done = true; break; }   // the native shadow ray asks for TerminateOnFirstHit
        }
      }
    } else {
      let l = u32(nd.mnA.w);
      let r = u32(nd.mxB.w);
      let ln = NODES[l];
      let rn = NODES[r];
      let tl = boxT(ox, oy, oz, idx, idy, idz, ln.mnA.xyz, ln.mxB.xyz);
      let tr = boxT(ox, oy, oz, idx, idy, idz, rn.mnA.xyz, rn.mxB.xyz);
      if (tl >= 0.0 && tr >= 0.0) {
        // Near child popped first: it lands the close hit early, which is what lets the far child's
        // box test reject instead of descend.
        if (tl < tr) { stack[sp] = r; stack[sp + 1u] = l; } else { stack[sp] = l; stack[sp + 1u] = r; }
        sp = sp + 2u;
      } else if (tl >= 0.0) {
        stack[sp] = l;
        sp = sp + 1u;
      } else if (tr >= 0.0) {
        stack[sp] = r;
        sp = sp + 1u;
      }
    }
  }

  if (found) {
    let tri = TRIS[bestTri];
    h.hit = true;
    h.t = tbest;
    h.pos = vec3<f32>(ox + dx * tbest, oy + dy * tbest, oz + dz * tbest);
    h.kind = tri.info.x;
    h.material = tri.info.y;
    let w2o = INST[tri.info.z].w2o;
    h.posObj = (w2o * vec4<f32>(h.pos, 1.0)).xyz;
    h.normal = objectNormalToWorld(w2o, objectNormal(h.kind, h.posObj));
    h.frontFacing = (dx * h.normal.x + dy * h.normal.y + dz * h.normal.z) < 0.0;
  }
  return h;
}

// --------------------------------------------------------------------------- sky.glsl
fn skyColor(d : vec3<f32>) -> vec3<f32> {
  let up = clamp(d.y, -1.0, 1.0);
  let horizon = P.sky.xyz;
  let zenith = mix(horizon, vec3<f32>(0.13, 0.24, 0.45), 0.92);
  var c = mix(horizon * 0.55, zenith, smoothstep(0.0, 0.55, up));
  c = mix(c, horizon * 0.10, smoothstep(0.0, -0.35, up));
  let lp = LIGHTS[0u].posRadius.xyz;
  let glow = pow(clamp(dot(d, normalize(lp)), 0.0, 1.0), 28.0);
  return c + LIGHTS[0u].color.rgb * 0.03 * glow;
}

// --------------------------------------------------------------------------- textures
// The native app uploads the .pak PNGs as R8G8B8A8_UNORM, not SRGB (core.cpp:900), so the shader reads
// the stored sRGB-encoded bytes with no decode and the procedural fallback below uses values picked to
// match those bytes. The browser has to do the same: rgba8unorm texture, no conversion.
fn sampleTexture(slot : i32, uv : vec2<f32>) -> vec3<f32> {
  var c = vec3<f32>(1.0);
  switch (slot) {
    case 0: { c = textureSampleLevel(tex0, samp, fract(uv), 0.0).rgb; }
    case 1: { c = textureSampleLevel(tex1, samp, fract(uv), 0.0).rgb; }
    default: {}
  }
  return c;
}

fn checkerFrac(x : f32) -> f32 { return x - floor(x); }

fn proceduralChecker(uv : vec2<f32>, scale : f32) -> vec3<f32> {
  let g = floor(uv * max(0.01, scale));
  let c = checkerFrac(g.x + g.y);
  return mix(vec3<f32>(0.60, 0.61, 0.65), vec3<f32>(0.23, 0.24, 0.28), c);
}

// --------------------------------------------------------------------------- BRDF
fn tangentFrameT(n : vec3<f32>) -> vec3<f32> {
  let up = select(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 1.0, 0.0), abs(n.y) < 0.9);
  return normalize(cross(up, n));
}

// NOTE: copied verbatim, including the fact that both angles come from u.x and u.y is thrown away.
// pt_common.glsl:48-55 does exactly this, so the native diffuse lobe has the same azimuth/polar
// coupling. "Fixing" it would put the two renderers further apart than floating-point noise does,
// which is the opposite of what this file is for.
fn cosineHemisphere(n : vec3<f32>, u : vec2<f32>) -> vec3<f32> {
  let phi = 2.0 * PI * u.x;
  let sinTheta = sqrt(max(0.0, u.x));
  let cosTheta = sqrt(max(0.0, 1.0 - u.x));
  let t = tangentFrameT(n);
  let b = cross(n, t);
  return normalize(t * (cos(phi) * sinTheta) + b * (sin(phi) * sinTheta) + n * cosTheta);
}

fn fresnelSchlick(cosTheta : f32, f0 : f32) -> f32 {
  let m = clamp(cosTheta, 0.0, 1.0);
  return f0 + (1.0 - f0) * pow(1.0 - m, 5.0);
}

fn fresnelSchlick3(cosTheta : f32, f0 : vec3<f32>) -> vec3<f32> {
  let m = clamp(cosTheta, 0.0, 1.0);
  return f0 + (vec3<f32>(1.0) - f0) * pow(1.0 - m, 5.0);
}

fn fresnelDielectric(cosI : f32, ior : f32) -> f32 {
  let f0c = (1.0 - ior) / (1.0 + ior);
  return fresnelSchlick(abs(cosI), f0c * f0c);
}

fn smithG1(ncos : f32, a2 : f32) -> f32 {
  let n = max(1e-5, abs(ncos));
  return 2.0 / (1.0 + sqrt(max(1e-8, 1.0 + a2 * (1.0 - n * n) / (n * n))));
}

// Importance sampling of the GGX NDF by half-vector, Fresnel evaluated at the half vector, and the
// separable Smith term clamped at 3 - all three decisions and their reasons are the source's.
fn sampleSpecular(wo : vec3<f32>, n : vec3<f32>, roughness : f32, u : vec2<f32>, f0 : vec3<f32>,
                  wi : ptr<function, vec3<f32>>, weight : ptr<function, vec3<f32>>) -> bool {
  *wi = wo;
  *weight = vec3<f32>(0.0);
  let a = max(0.004, roughness * roughness);
  let a2 = a * a;
  let phi = 2.0 * PI * u.x;
  let cosThetaH = sqrt(max(1e-6, (1.0 - u.y) / (1.0 + (a2 - 1.0) * u.y)));
  let sinThetaH = sqrt(max(0.0, 1.0 - cosThetaH * cosThetaH));
  let t = tangentFrameT(n);
  let b = cross(n, t);
  let TBN = mat3x3<f32>(t, b, n);
  let hh = normalize(TBN * vec3<f32>(sinThetaH * cos(phi), sinThetaH * sin(phi), cosThetaH));
  let cand = reflect(-wo, hh);
  let noL = dot(n, cand);
  let voH = dot(wo, hh);
  let noV = max(1e-4, dot(n, wo));
  if (noL <= 0.0 || voH <= 0.0) { return false; }
  *weight = min(vec3<f32>(3.0), fresnelSchlick3(voH, f0) * smithG1(noV, a2) * smithG1(noL, a2) *
                voH / (noV * max(1e-4, cosThetaH)));
  *wi = normalize(cand);
  return true;
}

// --------------------------------------------------------------------------- next-event estimation
fn directLighting(h : Hit, m : Mat, wo : vec3<f32>, albedo : vec3<f32>,
                  state : ptr<function, u32>) -> vec3<f32> {
  if (TRACE_MODE >= 3u) { return vec3<f32>(0.0); }   // measurement variant: isolate the NEE term
  var accum = vec3<f32>(0.0);
  let model = m.flags.x;
  if (model > 1.5) { return accum; }                 // glass is handled purely specularly
  if (model >= 0.5 && m.params.y < 0.06) { return accum; }   // a delta-ish lobe is not disc-samplable

  for (var i = 0u; i < LIGHT_COUNT; i = i + 1u) {
    let L = LIGHTS[i];
    if (L.color.w <= 0.0) { continue; }
    let cToL = L.posRadius.xyz - h.pos;
    let distC = length(cToL);
    let dirC = cToL / max(1e-5, distC);
    if (dot(h.normal, dirC) <= 0.0) { continue; }

    // A point on the light's disc, so shadows get a penumbra.
    let rL = max(1e-3, L.posRadius.w);
    let lt = tangentFrameT(dirC);
    let lb = cross(dirC, lt);
    let u = rnd2(state);
    let rr = rL * 0.9 * sqrt(u.x);
    let ang = 2.0 * PI * u.y;
    let lp = L.posRadius.xyz + lt * (cos(ang) * rr) + lb * (sin(ang) * rr);
    let sd = lp - h.pos;
    let sdLen = length(sd);
    let sdir = sd / max(1e-5, sdLen);
    let ndots = max(0.0, dot(h.normal, sdir));
    if (ndots <= 1e-5) { continue; }

    // Grazing-dependent shadow offset; a fixed epsilon is what made the floor draw bright rings.
    let graze = 1.0 - clamp(ndots, 0.0, 1.0);
    let shadowOrigin = h.pos + h.normal * (0.0015 + 0.055 * graze * graze);
    let blocked = traceRay(shadowOrigin.x, shadowOrigin.y, shadowOrigin.z, sdir.x, sdir.y, sdir.z,
                           sdLen - 0.004, occlRoot(), true);
    if (COUNT_ON == 1u) { tally(1u, 1u); }
    if (blocked.hit) {
      if (COUNT_ON == 1u) { tally(2u, 1u); }
      continue;
    }

    let pdf = sdLen * sdLen / max(1e-4, PI * rL * rL * 0.81);
    let falloff = 1.0 / (1.0 + 0.015 * sdLen * sdLen);
    let incoming = L.color.rgb * (falloff / max(1e-4, pdf));

    if (model < 0.5) {
      // Lambert: the cosine has to appear explicitly now the sampling density carries only solid angle.
      accum = accum + albedo * incoming * (ndots / PI);
    } else {
      // Glossy: the cosine at the shading point cancels the 1/cosine inside the BRDF, so this term
      // deliberately carries none.
      let hv = normalize(wo + sdir);
      let a = max(0.002, m.params.y * m.params.y);
      let a2 = a * a;
      let noh = max(0.0, dot(h.normal, hv));
      let gh = 1.0 + (a2 - 1.0) * noh * noh;
      let dd = a2 / max(1e-8, PI * gh * gh);
      let fres = fresnelSchlick(max(1e-4, dot(wo, hv)), clamp(m.params.x, 0.04, 1.0));
      let g = smithG1(dot(h.normal, wo), a2) * smithG1(dot(h.normal, sdir), a2);
      let noV = max(1e-4, dot(h.normal, wo));
      accum = accum + albedo * incoming * (dd * fres * g / (4.0 * noV));
    }
  }
  return accum;
}

// --------------------------------------------------------------------------- feature collection
// The gb* variables of pt_common.glsl: what the transport loop reports about the surface it shaded.
var<private> gbAlbedo : vec3<f32>;
var<private> gbNormal : vec3<f32>;
var<private> gbDepth : f32 = 0.0;
var<private> gbPrimary : bool = false;
var<private> gbHave : bool = false;
var<private> gbRough : f32 = 0.0;
var<private> gbMat : f32 = -1.0;

fn gbBegin() {
  gbAlbedo = vec3<f32>(0.0);
  gbNormal = vec3<f32>(0.0);
  gbDepth = 0.0;
  gbPrimary = false;
  gbHave = false;
  gbRough = 0.0;
  gbMat = -1.0;
}

// --------------------------------------------------------------------------- counters
// Lane-indexed tallies in workgroup memory: each thread only ever touches its own slot, so no atomics
// are needed for accumulation, only for the single per-workgroup publish at the end.
fn tally(slot : u32, amount : u32) {
  switch (slot) {
    case 0u: { wgRays[gLane] = wgRays[gLane] + amount; }
    case 1u: { wgOccR[gLane] = wgOccR[gLane] + amount; }
    case 2u: { wgOccH[gLane] = wgOccH[gLane] + amount; }
    default: { wgHits[gLane] = wgHits[gLane] + amount; }
  }
}

// No shuffle intrinsics in WGSL, so the reduction is serial in one lane. Only the instrumented
// pipeline compiles this, and it runs once per workgroup, not once per thread.
fn flushCounters(lane : u32) {
  workgroupBarrier();
  if (lane == 0u) {
    var sums = array<u32, 4u>(0u, 0u, 0u, 0u);
    for (var i = 0u; i < 64u; i = i + 1u) {
      sums[0u] = sums[0u] + wgRays[i];
      sums[1u] = sums[1u] + wgOccR[i];
      sums[2u] = sums[2u] + wgOccH[i];
      sums[3u] = sums[3u] + wgHits[i];
    }
    atomicAdd(&counters[0u], 64u);
    atomicAdd(&counters[1u], sums[3u]);
    atomicAdd(&counters[2u], sums[0u]);
    atomicAdd(&counters[3u], sums[1u]);
    atomicAdd(&counters[4u], sums[2u]);
  }
}

// --------------------------------------------------------------------------- the transport loop
fn pathTrace(origin : vec3<f32>, direction : vec3<f32>, stateIn : u32) -> vec3<f32> {
  gbBegin();
  var state = stateIn;
  var radiance = vec3<f32>(0.0);
  var throughput = vec3<f32>(1.0);
  let maxBounces = select(1u, u32(P.fwdBounces.w), TRACE_MODE != 2u);
  let clampV = P.post.z;
  var prevModel = -1.0;
  var o = origin;
  var d = direction;

  for (var bounce = 0u; bounce < maxBounces; bounce = bounce + 1u) {
    if (COUNT_ON == 1u) { tally(0u, 1u); }
    let h = traceRay(o.x, o.y, o.z, d.x, d.y, d.z, 1e5, sceneRoot(), false);
    if (!h.hit) {
      radiance = radiance + throughput * skyColor(d);
      break;
    }

    let m = MATS[h.material];
    // Double counting an emitter is not a subtle bias: the orb is bright enough that every floor point
    // which can see it saturates to white. Only diffuse surfaces suppress, because only they can
    // represent the light through NEE.
    let lightAlreadyCounted = (prevModel >= 0.0 && prevModel < 0.5);
    if (!lightAlreadyCounted) { radiance = radiance + throughput * m.emissive.rgb; }
    let model = m.flags.x;
    prevModel = model;
    if (model > 2.5) { break; }        // emitter: its shading contribution is the emissive term

    let wo = -normalize(d);
    let rough = clamp(m.params.y, 0.0, 1.0);
    let refl = clamp(m.params.x, 0.0, 1.0);
    let ior = select(1.5, m.params.z, m.params.z > 1.0001);

    var albedo = m.albedo.rgb;
    if (m.flags.y >= 0.0) {
      let uv = objectUV(h.kind, h.posObj) * (select(1.0, m.params.w, m.params.w > 0.0));
      var tex = sampleTexture(i32(m.flags.y), uv);
      if (max(max(tex.r, tex.g), tex.b) <= 1e-4) { tex = proceduralChecker(uv, m.params.w); }
      albedo = albedo * mix(vec3<f32>(1.0), tex, 0.85);
    }

    if (!gbPrimary) {
      // Depth for the disocclusion test is the geometry the eye rests on: the first hit, not the
      // feature surface below it.
      gbPrimary = true;
      gbDepth = h.t;
      gbMat = f32(h.material);
    }
    if (!gbHave) {
      // Features come from the first surface that scatters diffusely, following delta paths instead of
      // stopping at them; emissive orbs report none.
      let delta = (model >= 1.5) || (model >= 0.5 && rough < 0.03);
      if (!delta) {
        gbHave = true;
        gbAlbedo = clamp(albedo, vec3<f32>(0.0), vec3<f32>(1.0));
        gbNormal = normalize(h.normal);
        gbRough = rough;
      }
    }

    radiance = radiance + clamp(throughput * directLighting(h, m, wo, albedo, &state),
                                vec3<f32>(0.0), vec3<f32>(clampV));
    if (TRACE_MODE == 1u) { break; }   // primary-visibility variant: geometry and features only

    var nextDir = d;
    var attenuate = vec3<f32>(0.0);
    if (model < 0.5) {
      // Rough dielectric: mostly Lambertian with a glossy lobe driven by reflectivity.
      var wgt = vec3<f32>(0.0);
      if (refl > 0.02 && rnd(&state) < clamp(refl, 0.0, 0.9) &&
          sampleSpecular(wo, h.normal, max(0.03, rough), rnd2(&state),
                         vec3<f32>(max(0.02, refl)), &nextDir, &wgt)) {
        attenuate = wgt;              // neutral tint: the material colour belongs to the diffuse term
      } else {
        nextDir = cosineHemisphere(h.normal, rnd2(&state));
        attenuate = albedo;
      }
    } else if (model < 1.5) {
      // Metal: tinted Fresnel + GGX, reflectivity scaling the retained energy.
      var wgt = vec3<f32>(0.0);
      if (!sampleSpecular(wo, h.normal, max(0.004, rough), rnd2(&state),
                          clamp(albedo, vec3<f32>(0.02), vec3<f32>(1.0)), &nextDir, &wgt)) { break; }
      attenuate = wgt * max(0.05, refl);
    } else {
      // Glass: Fresnel-weighted reflection or refraction, with total internal reflection.
      let f = fresnelDielectric(dot(wo, h.normal), ior);
      if (rnd(&state) < f) {
        nextDir = reflect(-wo, h.normal);
        attenuate = vec3<f32>(f);
      } else {
        var n = h.normal;
        var eta = ior;
        if (!h.frontFacing) { n = -h.normal; eta = 1.0 / ior; }
        let refr = refract(-wo, n, 1.0 / eta);
        if (dot(refr, refr) < 1e-8) {
          nextDir = reflect(-wo, n);
          attenuate = vec3<f32>(1.0);
        } else {
          nextDir = normalize(refr);
          attenuate = vec3<f32>(1.0) - vec3<f32>(f);
        }
      }
      attenuate = attenuate * mix(vec3<f32>(1.0), m.albedo.rgb, 0.2);   // faint tint through the volume
    }

    throughput = throughput * attenuate;
    let lum = max(throughput.r, max(throughput.g, throughput.b));
    if (lum < 0.005) { break; }
    if (lum > 1.0) { throughput = throughput / lum; }   // bounded energy, hue preserved

    o = h.pos + nextDir * 0.0015;
    d = normalize(nextDir);
  }
  return radiance;
}

// --------------------------------------------------------------------------- main
@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>,
        @builtin(local_invocation_id) local_id : vec3<u32>) {
  let px = vec2<i32>(gid.xy);
  let res = vec2<i32>(i32(P.frame.x), i32(P.frame.y));
  let lane = (local_id.y * 8u) + local_id.x;
  gLane = lane;
  let inside = px.x < res.x && px.y < res.y;
  if (COUNT_ON == 1u) {
    wgRays[lane] = 0u;
    wgOccR[lane] = 0u;
    wgOccH[lane] = 0u;
    wgHits[lane] = 0u;
  }

  // The check pass re-does the *primary* ray for the first `checkN` threads, on pixel coordinates the
  // host put in checkBuf[0..255] as (x, y) float pairs, and writes what the BVH says at
  // checkBuf[256 + k*2]. JS then brute-forces the same rays independently. This is the guard against the
  // probe's failure mode: a hierarchy that traverses nothing still renders sky, so hit decisions have to
  // be compared, not inferred from a plausible picture. The second half checks the shadow-ray mask as
  // well, against a JS oracle that skips the non-occluder orbs.
  let checkN = u32(P.check.x);
  if (gid.x == 0u && gid.y == 0u) {
    // Unconditional, outside the checkN guard: distinguishes "this dispatch never ran" from "it ran and
    // the guard was false" from "it ran but P is not what the host wrote". Those three look the same
    // from the CPU otherwise.
    checkBuf[901u] = vec4<f32>(P.check.x, P.frame.x, P.bvh.x, 7.0);
  }
  if (checkN > 0u) {
    let k = gid.y * u32(res.x) + gid.x;
    if (k == 0u) {
      // What the shader actually sees, published where the host can read it back. Without this, a
      // check pass that never runs and a check pass that runs and finds nothing look identical from
      // the CPU side - both leave the result slots at zero - and "the geometry is wrong" gets blamed
      // for what is really "the parameters never arrived".
      checkBuf[900u] = vec4<f32>(f32(checkN), f32(res.x), f32(res.y), 42.0);
    }
    if (k < checkN) {
      let c = checkBuf[k * 2u];
      let cq = vec2<i32>(i32(c.x), i32(c.y));
      if (cq.x < res.x && cq.y < res.y) {
      let tanHalf = P.originTan.w;
      let resf = vec2<f32>(f32(res.x), f32(res.y));
      let ndc = ((vec2<f32>(f32(cq.x), f32(cq.y)) + vec2<f32>(0.5)) / resf) * 2.0 - 1.0;
      let dir = normalize(P.fwdBounces.xyz +
                          P.rightAspect.xyz * (ndc.x * P.rightAspect.w * tanHalf) +
                          P.upOne.xyz * (ndc.y * tanHalf));
      let hb = traceRay(P.originTan.x, P.originTan.y, P.originTan.z, dir.x, dir.y, dir.z,
                        1e5, sceneRoot(), false);
      var occ = vec4<f32>(0.0);
      if (hb.hit) {
        let lp = LIGHTS[0u].posRadius.xyz;
        let sd = lp - hb.pos;
        let dl = length(sd);
        let sdir = sd / max(1e-5, dl);
        let shadowOrigin = hb.pos + hb.normal * 0.002;
        let oh = traceRay(shadowOrigin.x, shadowOrigin.y, shadowOrigin.z, sdir.x, sdir.y, sdir.z,
                          dl - 0.004, occlRoot(), true);
        occ = vec4<f32>(dl, select(0.0, 1.0, oh.hit), 0.0, 0.0);
      }
      checkBuf[256u + k * 2u] = vec4<f32>(select(1e30, hb.t, hb.hit), select(0.0, 1.0, hb.hit),
                                          f32(hb.material), 0.0);
      checkBuf[257u + k * 2u] = occ;
      }
    }
  }

  var primaryHit = false;
  var fAlb = vec4<f32>(0.0);
  var fNrm = vec4<f32>(0.0);
  var fDepth = 1e5;
  var fMat = 0.0;

  if (inside) {
    let spp = max(1u, u32(P.display.y));
    let base = u32(px.x) * 1973u + u32(px.y) * 9277u + u32(P.post.y) * 26699u + 1u;
    let tanHalf = P.originTan.w;
    let resf = vec2<f32>(f32(res.x), f32(res.y));
    var sum = vec3<f32>(0.0);

    for (var s = 0u; s < spp; s = s + 1u) {
      var state = base + s * 7919u;
      state = pcgHash(&state);
      let jitter = vec2<f32>(rnd(&state), rnd(&state)) - vec2<f32>(0.5);
      let ndc = ((vec2<f32>(f32(px.x), f32(px.y)) + vec2<f32>(0.5) + jitter) / resf) * 2.0 - 1.0;
      let dir = normalize(P.fwdBounces.xyz +
                          P.rightAspect.xyz * (ndc.x * P.rightAspect.w * tanHalf) +
                          P.upOne.xyz * (ndc.y * tanHalf));
      sum = sum + pathTrace(P.originTan.xyz, dir, state);
      if (s == 0u) {
        // One set of features per pixel, from the first sample: averaging them across samples would
        // soften exactly the edges the filter is supposed to protect.
        fAlb = vec4<f32>(gbAlbedo, select(0.0, 1.0, gbHave));
        fNrm = vec4<f32>(gbNormal, gbRough);
        fDepth = select(1e5, gbDepth, gbPrimary);
        fMat = gbMat + 1.0;
        primaryHit = gbPrimary;
      }
    }

    if (P.sky.w > 0.5) {
      textureStore(gAlb, px, fAlb);
      textureStore(gNrm, px, fNrm);
      textureStore(gDep, px, vec4<f32>(1.0 / (1.0 + fDepth), select(0.0, 1.0, gbPrimary), fMat, 1.0));
    }

    // Channel view: the same three features the native --gbuf flag writes, which is what lets the
    // browser's feature dump be compared pixel-for-pixel with a native capture of the same camera.
    let dbg = i32(P.filt.x);
    if (dbg == 1) {
      textureStore(hdrTex, px, vec4<f32>(fAlb.rgb, fAlb.a));
    } else if (dbg == 2) {
      textureStore(hdrTex, px, vec4<f32>(fNrm.rgb * 0.5 + 0.5, fNrm.w));
    } else if (dbg == 3) {
      textureStore(hdrTex, px, vec4<f32>(vec3<f32>(clamp(fDepth / 30.0, 0.0, 1.0)), 1.0));
    } else {
      // While filtering, each frame is written fresh and the filter's history is the only memory:
      // pre-averaging would make its variance estimate read far too low.
      let fresh = P.frame.z > 0.5 || P.filt.w > 0.5;
      var prev = vec3<f32>(0.0);
      if (!fresh) { prev = textureLoad(hdrPrev, px, 0).rgb; }
      textureStore(hdrTex, px, vec4<f32>(prev + max(sum, vec3<f32>(0.0)) / f32(spp), 1.0));
    }
      // Slot 3 is the primary-hit tally, so it has to be guarded: incremented unconditionally it counts
    // every shaded pixel, and the derived hit fraction reads exactly 1.0000 no matter what the scene
    // is - which is the same "number that cannot be wrong" mistake the probe warned about.
    if (COUNT_ON == 1u && primaryHit) { tally(3u, 1u); }
  }
  if (COUNT_ON == 1u) { flushCounters(lane); }
}
