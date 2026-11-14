// geometry.js - the world-space triangle set, packed the way the WGSL expects to read it.
//
// The native app builds four unit meshes and instances them through a 3x4 transform (src/scene.cpp,
// core.cpp:1037-1040, core2.cpp:computeInstances). The same is done here, with two details that matter
// for agreeing with a native capture:
//
//  * Triangles are transformed to world space once and traversed there, so the silhouette the browser
//    traces is literally the same tessellation the driver sees, including the disc's 128 straight
//    edges and the cone's 96 side panels.
//  * Shading normals are NOT taken from the mesh. hit.rchit computes the object-space hit point with
//    the instance's world-to-object matrix and derives the normal analytically from the primitive kind
//    (shaders/primitives.glsl). trace.wgsl does exactly the same, so a per-triangle record carries the
//    instance index rather than any normal.

import { computeInstances, instanceMatrices, kindOfObject, gpuLights } from './scene.js';

const MAX_INSTANCES = 64;      // kMaxInstances
const MAX_MATERIALS = 32;      // kMaxMaterials
const MAX_LIGHTS = 4;          // kMaxLights

/**
 * Build the triangle set at time t.
 * @returns {tris, meta, count, lightRanges, unitMeshes}
 *  tris: 9 floats per triangle (world), meta: 4 u32 (kind, material, instance, occluder)
 *  lightRanges: [start,end) triangle ranges belonging to each moving light instance
 */
export function buildWorldTriangles(cfg, meshes, t) {
  const instances = computeInstances(cfg, t);
  const matList = [];
  const triList = [];
  const lightRanges = [];

  instances.forEach((inst, ii) => {
    const xf = inst.xf;
    // object -> world, as a 3x4 applied to unit-mesh vertices.
    const m = xf;
    const apply = (p) => [
      m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3],
      m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7],
      m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11],
    ];
    const mesh = inst.kind === 1 ? meshes.cube : inst.kind === 2 ? meshes.cone
      : inst.kind === 3 ? meshes.disc : meshes.sphere;
    const start = triList.length;
    for (let k = 0; k < mesh.idx.length; k += 3) {
      for (let v = 0; v < 3; ++v) {
        const p = apply(mesh.pos[mesh.idx[k + v]]);
        triList.push(p[0], p[1], p[2]);
      }
      matList.push(inst.kind, inst.material, ii, inst.isLight ? 0 : 1);
    }
    if (inst.isLight) lightRanges.push([start / 9, triList.length / 9]);
  });

  return {
    tris: new Float32Array(triList),
    meta: new Uint32Array(matList),
    count: matList.length / 4,
    lightRanges,
    instances,
  };
}

/** Interleave into the WGSL Tri record: vec4 v0, vec4 v1, vec4 v2, vec4<u32> meta. */
export function packTriangles(tris, meta, count) {
  const buf = new ArrayBuffer(count * 64);
  const f = new Float32Array(buf);
  const u = new Uint32Array(buf);
  packTrianglesInto(tris, meta, f, u, count);
  return f;
}

/** Same, into existing views: the per-frame refit path re-packs in place instead of allocating. */
export function packTrianglesInto(tris, meta, f, u, count) {
  for (let i = 0; i < count; ++i) {
    const o = i * 16;
    f[o + 0] = tris[i * 9]; f[o + 1] = tris[i * 9 + 1]; f[o + 2] = tris[i * 9 + 2];
    f[o + 4] = tris[i * 9 + 3]; f[o + 5] = tris[i * 9 + 4]; f[o + 6] = tris[i * 9 + 5];
    f[o + 8] = tris[i * 9 + 6]; f[o + 9] = tris[i * 9 + 7]; f[o + 10] = tris[i * 9 + 8];
    u[o + 12] = meta[i * 4]; u[o + 13] = meta[i * 4 + 1];
    u[o + 14] = meta[i * 4 + 2]; u[o + 15] = meta[i * 4 + 3];
  }
}

/** Per-instance object->world and world->object, padded to kMaxInstances as the native does. */
export function packInstances(instances) {
  const out = new Float32Array(MAX_INSTANCES * 32);
  instances.forEach((inst, i) => {
    if (i >= MAX_INSTANCES) return;
    out.set(instanceMatrices(inst.xf), i * 32);
  });
  return out;
}

/** Rebuild the instance rows for a frame in which only the light orbs moved. */
export function updateInstanceRows(out, cfg, t) {
  const instances = computeInstances(cfg, t);
  instances.forEach((inst, i) => {
    if (i >= MAX_INSTANCES) return;
    out.set(instanceMatrices(inst.xf), i * 32);
  });
  return instances;
}

export function packMaterials(cfg) {
  const out = new Float32Array(MAX_MATERIALS * 16);
  cfg.materials.slice(0, MAX_MATERIALS).forEach((m, i) => {
    const o = i * 16;
    out[o + 0] = m.albedo[0]; out[o + 1] = m.albedo[1]; out[o + 2] = m.albedo[2];
    out[o + 3] = m.diffuse;
    out[o + 4] = m.emissive[0]; out[o + 5] = m.emissive[1]; out[o + 6] = m.emissive[2];
    out[o + 7] = 0;
    out[o + 8] = m.reflectivity; out[o + 9] = m.roughness; out[o + 10] = m.ior;
    out[o + 11] = m.textureScale;
    out[o + 12] = m.model; out[o + 13] = m.textureSlot; out[o + 14] = 0; out[o + 15] = 0;
  });
  return out;
}

export function packLights(cfg, t) { return gpuLights(cfg, t); }

/**
 * A regen callback for BVH.refit(): rewrite the moving light triangles from the unit mesh at the new
 * instance transform, into the BVH's permuted slots.
 */
export function makeLightRegen(meshes, cfg) {
  return (bvh, t, groupIndex) => {
    const instances = computeInstances(cfg, t);
    const lightInstances = [];
    instances.forEach((inst, i) => { if (inst.isLight) lightInstances.push({ inst, i }); });
    const picked = lightInstances[groupIndex];
    if (!picked) return;
    const mesh = kindOfObject('sphere') === picked.inst.kind ? meshes.sphere : meshes.sphere;
    const m = picked.inst.xf;
    const slots = bvh.lightSlots[groupIndex];
    for (let k = 0, tri = 0; k < mesh.idx.length; k += 3, ++tri) {
      const slot = slots[tri];
      const o = slot * 9;
      for (let v = 0; v < 3; ++v) {
        const p = mesh.pos[mesh.idx[k + v]];
        bvh.tris[o + v * 3] = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
        bvh.tris[o + v * 3 + 1] = m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7];
        bvh.tris[o + v * 3 + 2] = m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11];
      }
    }
  };
}

export { MAX_INSTANCES, MAX_MATERIALS, MAX_LIGHTS };
