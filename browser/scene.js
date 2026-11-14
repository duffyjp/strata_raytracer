// scene.js - the browser-side twin of src/scene.cpp + the parts of src/core2.cpp that turn the
// authored scene into GPU data.
//
// Everything here is a transliteration, deliberately down to the loops and the winding order, because
// the whole point is that the triangle set the browser traces is the same triangle set the native
// app hands the driver. Native reference points are named in comments.
//
// Meshes are authored at unit size and scaled by the instance transform (see shaders/primitives.glsl):
//   sphere: radius 1 at the origin          (makeSphere(1, 48, 24) -> 2304 triangles)
//   cube:   half-extent 0.5 at the origin   (makeCube(1)          ->   12 triangles)
//   cone:   base radius 1 in y=0, apex y=1  (makeCone(1, 1, 96)   ->  192 triangles)
//   disc:   radius 1 in y=0, facing +y      (makeDisc(1, 128)     ->  128 triangles)

const M_PI = Math.PI;

export const KIND_SPHERE = 0, KIND_CUBE = 1, KIND_CONE = 2, KIND_DISC = 3;

function norm3(v) {
  const l = Math.hypot(v[0], v[1], v[2]) || 1;
  return [v[0] / l, v[1] / l, v[2] / l];
}

// makeSphere(): a lat-long grid, normals = radial, indices {a, b, a+1, b, b+1, a+1}.
// Vertex count (segments+1)*(rings+1) = 49*25 = 1225 and index count 6912 - which is exactly what
// the native log prints ("mesh sphere: uploading 1225 verts / 6912 indices").
export function makeSphere(radius, segments, rings) {
  const pos = [], idx = [];
  const stride = segments + 1;
  for (let j = 0; j <= rings; ++j) {
    const phi = (j / rings) * M_PI;
    for (let i = 0; i <= segments; ++i) {
      const th = (i / segments) * 2 * M_PI;
      const n = [Math.sin(phi) * Math.cos(th), Math.cos(phi), Math.sin(phi) * Math.sin(th)];
      // Only the position is stored: hit.rchit derives the normal analytically from the primitive kind
      // and the object-space hit point (shaders/primitives.glsl), so the mesh's normal channel is
      // uploaded by the native app and then never read.
      pos.push([n[0] * radius, n[1] * radius, n[2] * radius]);
    }
  }
  for (let j = 0; j < rings; ++j) {
    for (let i = 0; i < segments; ++i) {
      const a = j * stride + i, b = a + stride;
      idx.push(a, b, a + 1, b, b + 1, a + 1);
    }
  }
  return { pos, idx };
}

// makeCube(): six quads as two triangles each, per-face flat normals, in the source's face order.
export function makeCube(size) {
  const h = size * 0.5;
  const faces = [
    { n: [0, 1, 0], v: [[-h, h, -h], [-h, h, h], [h, h, h], [h, h, -h]] },
    { n: [0, -1, 0], v: [[-h, -h, -h], [h, -h, -h], [h, -h, h], [-h, -h, h]] },
    { n: [1, 0, 0], v: [[h, -h, -h], [h, h, -h], [h, h, h], [h, -h, h]] },
    { n: [-1, 0, 0], v: [[-h, -h, -h], [-h, -h, h], [-h, h, h], [-h, h, -h]] },
    { n: [0, 0, 1], v: [[-h, -h, h], [h, -h, h], [h, h, h], [-h, h, h]] },
    { n: [0, 0, -1], v: [[-h, -h, -h], [-h, h, -h], [h, h, -h], [h, -h, -h]] },
  ];
  const pos = [], nrm = [], idx = [];
  for (const f of faces) {
    for (const tri of [[0, 1, 2], [0, 2, 3]]) {
      const base = pos.length;
      for (const k of tri) { pos.push(f.v[k]); nrm.push(f.n); }
      idx.push(base, base + 1, base + 2);
    }
  }
  return { pos, nrm, idx };
}

// makeCone(): side triangles with slant-perpendicular smooth normals, then a flat base disc.
export function makeCone(radius, height, segments) {
  const apex = [0, height, 0];
  const pos = [], idx = [];
  for (let i = 0; i < segments; ++i) {
    const t0 = (i / segments) * 2 * M_PI, t1 = ((i + 1) / segments) * 2 * M_PI;
    const b0 = [Math.cos(t0) * radius, 0, Math.sin(t0) * radius];
    const b1 = [Math.cos(t1) * radius, 0, Math.sin(t1) * radius];
    void norm3([Math.cos(t0) * height, radius, Math.sin(t0) * height]);   // unused: the shader
    void norm3([Math.cos(t1) * height, radius, Math.sin(t1) * height]);   // derives normals itself
    pos.push(apex, b0, b1);
    idx.push(idx.length, idx.length + 1, idx.length + 2);
  }
  const c = [0, 0, 0];
  for (let i = 0; i < segments; ++i) {
    const t0 = (i / segments) * 2 * M_PI, t1 = ((i + 1) / segments) * 2 * M_PI;
    const b0 = [Math.cos(t0) * radius, 0, Math.sin(t0) * radius];
    const b1 = [Math.cos(t1) * radius, 0, Math.sin(t1) * radius];
    const base = pos.length;
    pos.push(c, b1, b0);
    idx.push(base, base + 1, base + 2);
  }
  return { pos, idx };
}

// makeDisc(): a fan of `sectors` triangles in the y=0 plane facing +y, wound (c, b0, b1).
export function makeDisc(radius, sectors) {
  const pos = [], idx = [];
  const c = [0, 0, 0];
  for (let i = 0; i < sectors; ++i) {
    const t0 = (i / sectors) * 2 * M_PI, t1 = ((i + 1) / sectors) * 2 * M_PI;
    const b0 = [Math.cos(t0) * radius, 0, Math.sin(t0) * radius];
    const b1 = [Math.cos(t1) * radius, 0, Math.sin(t1) * radius];
    const base = pos.length;
    pos.push(c, b0, b1);
    idx.push(base, base + 1, base + 2);
  }
  return { pos, idx };
}

// makeTransform() from src/scene.cpp: rotation about Y, then per-axis scale, then translation.
// Row-major 3x4:  [ c*sx  0  -sz  px ]
//                 [  0    sy  0   py ]
//                 [  sx*? ...         ]   (see the source for the exact terms; copied below)
export function makeTransform(pos, scale, rotY) {
  const c = Math.cos(rotY), s = Math.sin(rotY);
  return new Float32Array([
    c * scale[0], 0, -s * scale[2], pos[0],
    0, scale[1], 0, pos[1],
    s * scale[0], 0, c * scale[2], pos[2],
  ]);
}

// fillInstanceMatrices(): object->world (column-major mat4) and world->object, as the native
// uploads 128 bytes per instance. Returned as 32 floats: [o2w(16), w2o(16)] in column-major order.
export function instanceMatrices(t) {
  const a00 = t[0], a01 = t[1], a02 = t[2], tx = t[3];
  const a10 = t[4], a11 = t[5], a12 = t[6], ty = t[7];
  const a20 = t[8], a21 = t[9], a22 = t[10], tz = t[11];

  const o = new Float32Array(32);
  o[0] = a00; o[1] = a10; o[2] = a20; o[3] = 0;
  o[4] = a01; o[5] = a11; o[6] = a21; o[7] = 0;
  o[8] = a02; o[9] = a12; o[10] = a22; o[11] = 0;
  o[12] = tx; o[13] = ty; o[14] = tz; o[15] = 1;

  const c00 = a11 * a22 - a12 * a21, c01 = -(a10 * a22 - a12 * a20), c02 = a10 * a21 - a11 * a20;
  const c10 = -(a01 * a22 - a02 * a21), c11 = a00 * a22 - a02 * a20, c12 = -(a00 * a21 - a01 * a20);
  const c20 = a01 * a12 - a02 * a11, c21 = -(a00 * a12 - a02 * a10), c22 = a00 * a11 - a01 * a10;
  let det = a00 * c00 + a01 * c01 + a02 * c02;
  if (Math.abs(det) < 1e-12) det = det < 0 ? -1e-12 : 1e-12;
  const id = 1 / det;
  const i00 = c00 * id, i01 = c10 * id, i02 = c20 * id;
  const i10 = c01 * id, i11 = c11 * id, i12 = c21 * id;
  const i20 = c02 * id, i21 = c12 * id, i22 = c22 * id;
  const ex = -(i00 * tx + i01 * ty + i02 * tz);
  const ey = -(i10 * tx + i11 * ty + i12 * tz);
  const ez = -(i20 * tx + i21 * ty + i22 * tz);
  o[16] = i00; o[17] = i10; o[18] = i20; o[19] = 0;
  o[20] = i01; o[21] = i11; o[22] = i21; o[23] = 0;
  o[24] = i02; o[25] = i12; o[26] = i22; o[27] = 0;
  o[28] = ex; o[29] = ey; o[30] = ez; o[31] = 1;
  return o;
}

// ---------------------------------------------------------------------------
// Authored defaults: SceneConfig::loadDefaults() from src/scene.cpp, materials first (slot order
// matters: the shader looks materials up by index), then the seven static objects, then the two
// emissive orbs that follow the analytic lights, then the two textures.
// ---------------------------------------------------------------------------
function addMat(name, model, albedo, reflectivity, roughness, ior, emissive) {
  return {
    name, model, albedo: albedo.slice(),
    emissive: (emissive || [0, 0, 0]).slice(),
    reflectivity, roughness, ior,
    diffuse: (model === 1 || model === 2 || model === 3) ? 0 : 1,
    textureScale: 1, textureSlot: -1,
  };
}

export function defaultScene() {
  const materials = [
    addMat('ground', 0, [0.52, 0.53, 0.56], 0.12, 0.35, 1.4),
    addMat('mirror', 1, [0.95, 0.96, 0.97], 0.95, 0.02, 1.0),
    addMat('satin', 1, [0.86, 0.87, 0.90], 0.70, 0.16, 1.0),
    addMat('red_dull', 0, [0.82, 0.20, 0.16], 0.05, 0.60, 1.0),
    addMat('glass', 2, [1, 1, 1], 0.05, 0.01, 1.52),
    addMat('mirror_cube', 1, [0.90, 0.92, 0.95], 0.85, 0.05, 1.0),
    addMat('teal', 0, [0.18, 0.55, 0.47], 0.06, 0.55, 1.0),
    addMat('light_warm', 3, [0.10, 0.08, 0.06], 0, 0, 1.0, [13, 8.6, 4.8]),
    addMat('light_cool', 3, [0.06, 0.08, 0.10], 0, 0, 1.0, [5.2, 8.4, 13]),
  ];
  const O = (name, kind, material, p, s, rotY = 0, extra = {}) =>
    Object.assign({ name, kind, material, position: p, scale: s, rotationY: rotY }, extra);
  const objects = [
    O('ground', 'disc', 'ground', [0, 0, 0], [12, 1, 12]),
    O('mirror_sphere', 'sphere', 'mirror', [-1.6, 1.15, 0.3], [1.15, 1.15, 1.15]),
    O('satin_sphere', 'sphere', 'satin', [1.7, 0.85, -1.5], [0.85, 0.85, 0.85]),
    O('red_sphere', 'sphere', 'red_dull', [3.0, 0.8, 1.4], [0.8, 0.8, 0.8]),
    O('glass_sphere', 'sphere', 'glass', [-0.1, 0.95, 2.3], [0.95, 0.95, 0.95]),
    O('mirror_cube', 'cube', 'mirror_cube', [1.15, 0.72, -0.25], [1.44, 1.44, 1.44], 0.42),
    O('teal_cone', 'cone', 'teal', [-2.8, 0, -1.8], [0.8, 1.9, 0.8]),
    O('light_key', 'sphere', 'light_warm', [0, 0, 0], [0.34, 0.34, 0.34], 0,
      { isLight: true, lightIndex: 0 }),
    O('light_fill', 'sphere', 'light_cool', [0, 0, 0], [0.27, 0.27, 0.27], 0,
      { isLight: true, lightIndex: 1 }),
  ];
  const lights = [
    { name: 'key', color: [1.0, 0.78, 0.55], intensity: 16, radius: 0.34, orbitRadius: 3.9,
      orbitHeight: 2.6, speed: 0.55, phase: 0, on: true },
    { name: 'fill', color: [0.45, 0.66, 1.0], intensity: 10, radius: 0.27, orbitRadius: 4.7,
      orbitHeight: 1.7, speed: -0.40, phase: 2.1, on: true },
  ];
  const textures = [
    { name: 'checker', file: 'textures/checker.png', scale: 6.0 },
    { name: 'ramp', file: 'textures/emissive_ramp.png', scale: 1.0 },
  ];
  materials[0].textureSlot = 0;      // ground uses the packed checker, as in loadDefaults()
  return {
    camera: { orbitRadius: 9.5, orbitHeight: 3.2, heightBob: 0.6, speed: 0.24, fovDeg: 42,
      lookAt: [0, 0.85, 0] },
    exposure: 1.7, maxBounces: 6, samplesPerFrame: 1, accumulationFrames: 1,
    radianceClamp: 12, skyMode: 0, skyColor: [0.4, 0.45, 0.54],
    denoise: true, denoiseHistoryCap: 20, denoiseSigmaZ: 0.02, denoiseSigmaL: 24,
    denoiseSigmaN: 128,
    autoQuality: true, targetFps: 60, qualityHeadroom: 0.3, maxSamplesPerFrame: 16,
    minRenderScale: 0.4, baseSamplesPerFrame: 4, maxRenderScale: 2.0, maxTracePixels: 8.5e6,
    materials, objects, lights, textures,
  };
}

// ---------------------------------------------------------------------------
// scene.json overlay: SceneConfig::applyJson(). Only the fields that scene.json actually carries.
// ---------------------------------------------------------------------------
function num(o, k, d) { return (typeof o[k] === 'number') ? o[k] : d; }
function bool(o, k, d) { return (typeof o[k] === 'boolean') ? o[k] : d; }
function str(o, k, d) { return (typeof o[k] === 'string') ? o[k] : d; }
function vecN(o, k, dst) {
  if (Array.isArray(o[k])) for (let i = 0; i < dst.length && i < o[k].length; ++i) dst[i] = o[k][i];
  return dst;
}

export function applyJson(cfg, root) {
  cfg.exposure = num(root, 'exposure', cfg.exposure);
  cfg.maxBounces = num(root, 'maxBounces', cfg.maxBounces);
  cfg.accumulationFrames = num(root, 'accumulationFrames', cfg.accumulationFrames);
  cfg.samplesPerFrame = Math.min(4, Math.max(1, num(root, 'samplesPerFrame', cfg.samplesPerFrame)));
  cfg.radianceClamp = num(root, 'radianceClamp', cfg.radianceClamp);
  cfg.autoQuality = bool(root, 'autoQuality', cfg.autoQuality);
  cfg.targetFps = Math.min(240, Math.max(20, num(root, 'targetFps', cfg.targetFps)));
  cfg.qualityHeadroom = Math.min(0.5, Math.max(0, num(root, 'qualityHeadroom', cfg.qualityHeadroom)));
  cfg.maxSamplesPerFrame = Math.min(32, Math.max(1, num(root, 'maxSamplesPerFrame', cfg.maxSamplesPerFrame)));
  cfg.minRenderScale = Math.min(1, Math.max(0.25, num(root, 'minRenderScale', cfg.minRenderScale)));
  cfg.baseSamplesPerFrame = Math.min(cfg.maxSamplesPerFrame, Math.max(1, num(root, 'baseSamplesPerFrame', cfg.baseSamplesPerFrame)));
  cfg.maxRenderScale = Math.min(3, Math.max(1, num(root, 'maxRenderScale', cfg.maxRenderScale)));
  cfg.maxTracePixels = num(root, 'maxTracePixels', cfg.maxTracePixels);
  cfg.skyMode = num(root, 'skyMode', cfg.skyMode);
  vecN(root, 'skyColor', cfg.skyColor);
  vecN(root, 'denoiseSigmaL', [cfg.denoiseSigmaL]);   // not in scene.json; kept for clarity

  if (root.camera) {
    const C = cfg.camera;
    C.orbitRadius = num(root.camera, 'orbitRadius', C.orbitRadius);
    C.orbitHeight = num(root.camera, 'orbitHeight', C.orbitHeight);
    C.heightBob = num(root.camera, 'heightBob', C.heightBob);
    C.speed = num(root.camera, 'speed', C.speed);
    C.fovDeg = num(root.camera, 'fovDeg', C.fovDeg);
    vecN(root.camera, 'lookAt', C.lookAt);
  }
  const byName = (arr, n) => arr.findIndex((m) => m.name === n);
  if (Array.isArray(root.materials)) {
    for (const mv of root.materials) {
      let idx = mv.name ? byName(cfg.materials, mv.name) : -1;
      if (idx < 0) {
        cfg.materials.push(Object.assign(addMat(mv.name || 'mat', 0, [0.7, 0.7, 0.7], 0, 0.05, 1.5),
          { diffuse: 1 }));
        idx = cfg.materials.length - 1;
      }
      const md = cfg.materials[idx];
      vecN(mv, 'albedo', md.albedo);
      vecN(mv, 'emissive', md.emissive);
      md.reflectivity = num(mv, 'reflectivity', md.reflectivity);
      md.roughness = num(mv, 'roughness', md.roughness);
      md.ior = num(mv, 'ior', md.ior);
      md.diffuse = num(mv, 'diffuse', md.diffuse);
      md.model = num(mv, 'model', md.model);
      md.textureScale = num(mv, 'textureScale', md.textureScale);
      if (mv.texture) {
        const t = byName(cfg.textures, mv.texture);
        if (t >= 0) md.textureSlot = t;
      }
    }
  }
  if (Array.isArray(root.lights)) {
    root.lights.forEach((lv, i) => {
      if (i >= cfg.lights.length) return;
      const ld = cfg.lights[i];
      if (lv.name) ld.name = lv.name;
      vecN(lv, 'color', ld.color);
      ld.intensity = num(lv, 'intensity', ld.intensity);
      ld.radius = num(lv, 'radius', ld.radius);
      ld.orbitRadius = num(lv, 'orbitRadius', ld.orbitRadius);
      ld.orbitHeight = num(lv, 'orbitHeight', ld.orbitHeight);
      ld.speed = num(lv, 'speed', ld.speed);
      ld.phase = num(lv, 'phase', ld.phase);
      ld.on = bool(lv, 'on', ld.on);
    });
  }
  if (Array.isArray(root.textures)) {
    for (const tv of root.textures) {
      let idx = tv.name ? byName(cfg.textures, tv.name) : -1;
      if (idx < 0) {
        cfg.textures.push({ name: tv.name, file: tv.file || '', scale: 1 });
        idx = cfg.textures.length - 1;
      }
      cfg.textures[idx].file = str(tv, 'file', cfg.textures[idx].file);
      cfg.textures[idx].scale = num(tv, 'scale', cfg.textures[idx].scale);
    }
  }
  return cfg;
}

// ---------------------------------------------------------------------------
// Mesh table + instances
// ---------------------------------------------------------------------------
export function buildMeshes() {
  return {
    sphere: makeSphere(1, 48, 24),     // core.cpp:1037
    cube: makeCube(1),                 // core.cpp:1038
    cone: makeCone(1, 1, 96),          // core.cpp:1039
    disc: makeDisc(1, 128),            // core.cpp:1040
  };
}

export const kindOfObject = (kind) =>
  (kind === 'cube' ? KIND_CUBE : kind === 'cone' ? KIND_CONE
    : (kind === 'disc' || kind === 'ground' || kind === 'plane') ? KIND_DISC : KIND_SPHERE);

/**
 * Instance list, mirroring computeInstances() in src/core2.cpp: the light orbs ride their analytic
 * light's orbit, everything else is where the scene put it.
 */
export function computeInstances(cfg, t) {
  const out = [];
  for (const obj of cfg.objects) {
    const pos = obj.position.slice();
    if (obj.isLight && obj.lightIndex >= 0 && obj.lightIndex < cfg.lights.length) {
      const L = cfg.lights[obj.lightIndex];
      const ang = t * L.speed + L.phase;
      pos[0] = Math.cos(ang) * L.orbitRadius;
      pos[1] = L.orbitHeight;
      pos[2] = Math.sin(ang) * L.orbitRadius;
    }
    out.push({
      kind: kindOfObject(obj.kind),
      material: Math.max(0, cfg.materials.findIndex((m) => m.name === obj.material)),
      isLight: !!obj.isLight,
      xf: makeTransform(pos, obj.scale, obj.rotationY),
    });
  }
  return out;
}

/** Light table for the GPU: posRadius + colour premultiplied by intensity, as fillSceneData does. */
export function gpuLights(cfg, t) {
  const out = new Float32Array(4 * 2 * 4);
  for (let i = 0; i < cfg.lights.length && i < 4; ++i) {
    const L = cfg.lights[i];
    const la = t * L.speed + L.phase;
    const o = i * 8;
    out[o + 0] = Math.cos(la) * L.orbitRadius;
    out[o + 1] = L.orbitHeight;
    out[o + 2] = Math.sin(la) * L.orbitRadius;
    out[o + 3] = L.radius;
    out[o + 4] = L.color[0] * L.intensity;
    out[o + 5] = L.color[1] * L.intensity;
    out[o + 6] = L.color[2] * L.intensity;
    out[o + 7] = L.on ? 1 : 0;
  }
  return out;
}

/**
 * Camera basis, from fillSceneData() in src/core2.cpp (no free-camera blend: the demo only orbits).
 * right = normalize(cross(forward, worldUp)) with cross(f,(0,1,0)) = (-f.z, 0, f.x).
 */
export function cameraBasis(cfg, t, w, h) {
  const C = cfg.camera;
  const ang = t * C.speed;
  const height = C.orbitHeight + C.heightBob * Math.sin(t * 0.23);
  const o = [Math.cos(ang) * C.orbitRadius, height, Math.sin(ang) * C.orbitRadius];
  let f = [C.lookAt[0] - o[0], C.lookAt[1] - o[1], C.lookAt[2] - o[2]];
  f = norm3(f);
  let r = [-f[2], 0, f[0]];
  const rl = Math.hypot(r[0], r[1], r[2]);
  r = rl < 1e-5 ? [1, 0, 0] : [r[0] / rl, r[1] / rl, r[2] / rl];
  const u = [r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]];
  return {
    origin: o, right: r, up: u, forward: f,
    aspect: h ? w / h : 1,
    tanHalf: Math.tan((C.fovDeg * Math.PI) / 360),
    valid: true,
  };
}
