// bvh.js - the acceleration structure a WebGPU port has to build itself: WebGPU has no ray-tracing
// API of any kind, no BLAS, no TLAS, no driver build and no refit.
//
// Structure: a top-down binned-SAH binary BVH over world-space triangles, with one deliberate
// constraint - the two emissive orbs are kept in their own subtrees and grafted under two extra
// internal nodes at the top. Reason: the orbs are the only geometry that moves, and with that
// partition a frame that moves them re-bounds 2 x (2304 triangles + ~2300 nodes) plus two graft
// nodes instead of all 14 156 triangles. Build and refit are timed separately and reported
// separately, because "what does a port pay per frame to keep its structure valid" is a question the
// probe never answered.
//
// Triangles are physically permuted at the end so a leaf's (start, count) addresses the uploaded
// array directly - no index indirection in the hot loop.
//
// Node layout (matches `struct Node` in trace.wgsl): 8 floats
//   [ min.x min.y min.z  a ]   a >= 0 -> internal: a = left child index
//   [ max.x max.y max.z  b ]   b >= 0 -> internal: b = right child index
//                               b <  0 -> leaf:    a = first triangle index, count = -b - 1
// Nodes are allocated parent-before-children, so descending index order is a valid bottom-up order.

const LEAF_SIZE = 4;
const BINS = 16;

class NodeList {
  constructor(cap) { this.n = new Float32Array(cap * 8); this.count = 0; }
  add(min, max, a, b) {
    const o = this.count * 8;
    this.n[o] = min[0]; this.n[o + 1] = min[1]; this.n[o + 2] = min[2]; this.n[o + 3] = a;
    this.n[o + 4] = max[0]; this.n[o + 5] = max[1]; this.n[o + 6] = max[2]; this.n[o + 7] = b;
    return this.count++;
  }
}

/**
 * A node's bounds come out of rangeBounds as one six-element array [min.xyz, max.xyz]. NodeList.add
 * takes them as two arguments, and passing the six-array twice - which reads harmlessly enough -
 * silently puts the MINIMUM in the max slot: every box degenerates to a point, every ray rejects the
 * root, and the whole frame renders sky. The BVH check that compares GPU hits against a JS brute force
 * is what caught it (0.000 vs 0.625), because the brute force never looks at boxes.
 */
const boxMin = (b) => [b[0], b[1], b[2]];
const boxMax = (b) => [b[3], b[4], b[5]];

function rangeBounds(tris, ids, from, to) {
  let mnx = Infinity, mny = Infinity, mnz = Infinity, mxx = -Infinity, mxy = -Infinity, mxz = -Infinity;
  for (let k = from; k < to; ++k) {
    const o = ids[k] * 9;
    for (let v = 0; v < 3; ++v) {
      const x = tris[o + v * 3], y = tris[o + v * 3 + 1], z = tris[o + v * 3 + 2];
      if (x < mnx) mnx = x; if (x > mxx) mxx = x;
      if (y < mny) mny = y; if (y > mxy) mxy = y;
      if (z < mnz) mnz = z; if (z > mxz) mxz = z;
    }
  }
  return [mnx, mny, mnz, mxx, mxy, mxz];
}

function boundsOf(tris, t) {
  let mnx = Infinity, mny = Infinity, mnz = Infinity, mxx = -Infinity, mxy = -Infinity, mxz = -Infinity;
  for (let v = 0; v < 3; ++v) {
    const x = tris[t + v * 3], y = tris[t + v * 3 + 1], z = tris[t + v * 3 + 2];
    if (x < mnx) mnx = x; if (x > mxx) mxx = x;
    if (y < mny) mny = y; if (y > mxy) mxy = y;
    if (z < mnz) mnz = z; if (z > mxz) mxz = z;
  }
  return [mnx, mny, mnz, mxx, mxy, mxz];
}

const area = (b) => {
  const x = Math.max(0, b[3] - b[0]), y = Math.max(0, b[4] - b[1]), z = Math.max(0, b[5] - b[2]);
  return 2 * (x * y + x * z + y * z);
};

function partition(ids, from, to, pred) {
  let i = from, j = to - 1;
  while (i <= j) {
    while (i <= j && pred(ids[i])) i++;
    while (i <= j && !pred(ids[j])) j--;
    if (i <= j) { const t = ids[i]; ids[i] = ids[j]; ids[j] = t; i++; j--; }
  }
  return i;
}

/** Recursive binned-SAH build. Returns the node index of the subtree root. */
function buildRange(nodes, tris, cent, ids, from, to, parentBox, depth) {
  const b = rangeBounds(tris, ids, from, to);
  const n = to - from;
  if (n <= LEAF_SIZE || depth >= 48) return nodes.add(boxMin(b), boxMax(b), from, -(n + 1));

  const extent = [b[3] - b[0], b[4] - b[1], b[5] - b[2]];
  const parentArea = area(parentBox) || 1e-12;
  let bestAxis = -1, bestSplit = 0, bestCost = area(b) * n;   // cost of not splitting

  for (let axis = 0; axis < 3; ++axis) {
    if (!(extent[axis] > 1e-9)) continue;
    const lo = b[axis], span = extent[axis];
    const cnt = new Int32Array(BINS);
    const bmin = new Float64Array(BINS * 3).fill(Infinity);
    const bmax = new Float64Array(BINS * 3).fill(-Infinity);
    for (let k = from; k < to; ++k) {
      const c = cent[ids[k] * 3 + axis];
      let bi = Math.floor(((c - lo) / span) * BINS);
      if (bi >= BINS) bi = BINS - 1; if (bi < 0) bi = 0;
      cnt[bi]++;
      const o = ids[k] * 9;
      for (let ax = 0; ax < 3; ++ax) {
        for (let v = 0; v < 3; ++v) {
          const x = tris[o + v * 3 + ax];
          if (x < bmin[bi * 3 + ax]) bmin[bi * 3 + ax] = x;
          if (x > bmax[bi * 3 + ax]) bmax[bi * 3 + ax] = x;
        }
      }
    }
    const lcost = new Float64Array(BINS), rcost = new Float64Array(BINS);
    const box = [Infinity, Infinity, Infinity, -Infinity, -Infinity, -Infinity];
    let num = 0;
    for (let i = 0; i < BINS - 1; ++i) {
      if (cnt[i]) {
        num += cnt[i];
        for (let ax = 0; ax < 3; ++ax) {
          if (bmin[i * 3 + ax] < box[ax]) box[ax] = bmin[i * 3 + ax];
          if (bmax[i * 3 + ax] > box[3 + ax]) box[3 + ax] = bmax[i * 3 + ax];
        }
      }
      lcost[i] = num ? (area(box) / parentArea) * num : Infinity;
    }
    box[0] = box[1] = box[2] = Infinity; box[3] = box[4] = box[5] = -Infinity;
    num = 0;
    for (let i = BINS - 1; i >= 1; --i) {
      if (cnt[i]) {
        num += cnt[i];
        for (let ax = 0; ax < 3; ++ax) {
          if (bmin[i * 3 + ax] < box[ax]) box[ax] = bmin[i * 3 + ax];
          if (bmax[i * 3 + ax] > box[3 + ax]) box[3 + ax] = bmax[i * 3 + ax];
        }
      }
      rcost[i] = num ? (area(box) / parentArea) * num : Infinity;
    }
    for (let i = 0; i < BINS - 1; ++i) {
      const c = lcost[i] + rcost[i + 1];
      if (c < bestCost) { bestCost = c; bestAxis = axis; bestSplit = lo + ((i + 1) / BINS) * span; }
    }
  }

  let mid = -1;
  if (bestAxis >= 0) {
    mid = partition(ids, from, to, (id) => cent[id * 3 + bestAxis] < bestSplit);
    if (mid <= from || mid >= to) mid = -1;
  }
  if (mid < 0) {
    let ax = 0;
    if (extent[1] > extent[ax]) ax = 1;
    if (extent[2] > extent[ax]) ax = 2;
    if (!(extent[ax] > 1e-9)) return nodes.add(boxMin(b), boxMax(b), from, -(n + 1));
    const sorted = Array.prototype.slice.call(ids, from, to).sort((p, q) => cent[p * 3 + ax] - cent[q * 3 + ax]);
    for (let k = 0; k < n; ++k) ids[from + k] = sorted[k];
    mid = from + (n >> 1);
  }

  const self = nodes.add(boxMin(b), boxMax(b), 0, 0);
  const left = buildRange(nodes, tris, cent, ids, from, mid, b, depth + 1);
  const right = buildRange(nodes, tris, cent, ids, mid, to, b, depth + 1);
  const o = self * 8;
  nodes.n[o + 3] = left;
  nodes.n[o + 7] = right;
  return self;
}

export class BVH {
  /**
   * @param {Float32Array} tris  9 world-space floats per triangle
   * @param {Uint32Array}  meta  4 u32 per triangle: kind, material, instance, occluder
   * @param {number[][]}   lightGroups  [start,end) ranges (in the input indexing) of moving geometry
   */
  constructor(tris, meta, triCount, lightGroups) {
    this.srcTris = tris;
    this.srcMeta = meta;
    this.inputLightGroups = lightGroups || [];
    this.count = triCount;

    const cent = new Float32Array(triCount * 3);
    for (let i = 0; i < triCount; ++i) {
      const o = i * 9;
      cent[i * 3] = (tris[o] + tris[o + 3] + tris[o + 6]) / 3;
      cent[i * 3 + 1] = (tris[o + 1] + tris[o + 4] + tris[o + 7]) / 3;
      cent[i * 3 + 2] = (tris[o + 2] + tris[o + 5] + tris[o + 8]) / 3;
    }

    const lightSet = new Set();
    for (const [s, e] of this.inputLightGroups) for (let i = s; i < e; ++i) lightSet.add(i);
    const groups = [];
    const stat = [];
    for (let i = 0; i < triCount; ++i) if (!lightSet.has(i)) stat.push(i);
    if (stat.length) groups.push(Int32Array.from(stat));
    for (const [s, e] of this.inputLightGroups) {
      const g = [];
      for (let i = s; i < e; ++i) g.push(i);
      groups.push(Int32Array.from(g));
    }
    this.groups = groups;                     // groups[0] = static, then one per light group

    const nodes = new NodeList(triCount * 2 + 16);
    this.subtreeRanges = [];
    this.groupOffset = [];
    this.groupIds = [];
    const roots = [];

    let offset = 0;
    for (let gi = 0; gi < groups.length; ++gi) {
      const ids = groups[gi];
      const start = nodes.count;
      buildRange(nodes, tris, cent, ids, 0, ids.length, [0, 0, 0, 1, 1, 1], 0);
      // Leaf ranges were local to this group's id array; make them address the permuted global array.
      for (let i = start; i < nodes.count; ++i) {
        if (nodes.n[i * 8 + 7] < 0) nodes.n[i * 8 + 3] += offset;
      }
      this.groupOffset.push(offset);
      this.groupIds.push(ids);
      this.subtreeRanges.push([start, nodes.count]);
      roots.push(start);            // the first node written for a group is its root (parent-first)
      offset += ids.length;
    }
    this.totalTris = offset;

    // Graft: a small spine above the subtree roots so there is exactly one root to descend from.
    const firstGraft = nodes.count;
    let top = roots.length - 1;
    while (roots.length > 1) {
      const a = roots.pop(), b = roots.pop();
      const mn = [Infinity, Infinity, Infinity], mx = [-Infinity, -Infinity, -Infinity];
      for (const i of [a, b]) {
        for (let k = 0; k < 3; ++k) {
          mn[k] = Math.min(mn[k], nodes.n[i * 8 + k]);
          mx[k] = Math.max(mx[k], nodes.n[i * 8 + 4 + k]);
        }
      }
      roots.push(nodes.add(mn, mx, b, a));
      top = roots[roots.length - 1];
    }
    void top;
    this.root = roots[0];
    this.graftFrom = firstGraft;
    this.nodeCount = nodes.count;

    // Permute the triangle arrays into traversal order.
    const T = new Float32Array(this.totalTris * 9);
    const M = new Uint32Array(this.totalTris * 4);
    for (let gi = 0; gi < groups.length; ++gi) {
      const ids = this.groupIds[gi], base = this.groupOffset[gi];
      for (let k = 0; k < ids.length; ++k) {
        T.set(tris.subarray(ids[k] * 9, ids[k] * 9 + 9), (base + k) * 9);
        M.set(meta.subarray(ids[k] * 4, ids[k] * 4 + 4), (base + k) * 4);
      }
    }
    this.tris = T;
    this.meta = M;
    this.cent = cent;

    // Slot map for the moving groups: refit writes unit-mesh triangle i of light group g into
    // lightSlots[g][i], so regeneration does not have to re-derive the permutation.
    this.lightSlots = [];
    for (let g = 0; g < this.inputLightGroups.length; ++g) {
      const gi = g + (stat.length ? 1 : 0);
      const ids = this.groupIds[gi], base = this.groupOffset[gi];
      const [s, e] = this.inputLightGroups[g];
      const map = new Int32Array(e - s);
      for (let k = 0; k < ids.length; ++k) map[ids[k] - s] = base + k;
      this.lightSlots.push(map);
    }

    this._rawNodes = nodes.n;      // capacity-length; indices are all below nodeCount * 8

    let leaves = 0;
    for (let i = 0; i < this.nodeCount; ++i) if (this._rawNodes[i * 8 + 7] < 0) leaves++;
    this.stats = {
      nodes: this.nodeCount, leaves,
      maxDepth: this.#depth(this.root, 0),
      bytes: this.nodeCount * 32 + this.totalTris * 36,
      groups: groups.length,
    };
  }

  #depth(i, d) {
    const N = this._rawNodes;
    if (N[i * 8 + 7] < 0) return d;
    return Math.max(this.#depth(N[i * 8 + 3], d + 1), this.#depth(N[i * 8 + 7], d + 1));
  }

  /**
   * Refit after moving the light orbs. `regen(slotMap, groupIndex)` must write 9 floats per triangle
   * of that group into this.tris at the given slots. Returns nodes touched.
   */
  refit(regen) {
    for (let g = 0; g < this.lightSlots.length; ++g) regen(this.lightSlots[g], g);
    let touched = 0;
    const refitFrom = this.subtreeRanges[0][1];      // skip the static subtree: it cannot have moved
    for (let i = this.nodeCount - 1; i >= refitFrom; --i) {
      const o = i * 8;
      if (this._rawNodes[o + 7] < 0) {
        const start = this._rawNodes[o + 3], cnt = -this._rawNodes[o + 7] - 1;
        let mnx = Infinity, mny = Infinity, mnz = Infinity, mxx = -Infinity, mxy = -Infinity, mxz = -Infinity;
        for (let k = 0; k < cnt; ++k) {
          const t = (start + k) * 9;
          for (let v = 0; v < 3; ++v) {
            const x = this.tris[t + v * 3], y = this.tris[t + v * 3 + 1], z = this.tris[t + v * 3 + 2];
            if (x < mnx) mnx = x; if (x > mxx) mxx = x;
            if (y < mny) mny = y; if (y > mxy) mxy = y;
            if (z < mnz) mnz = z; if (z > mxz) mxz = z;
          }
        }
        this._rawNodes[o] = mnx; this._rawNodes[o + 1] = mny; this._rawNodes[o + 2] = mnz;
        this._rawNodes[o + 4] = mxx; this._rawNodes[o + 5] = mxy; this._rawNodes[o + 6] = mxz;
      } else {
        const l = this._rawNodes[o + 3] * 8, r = this._rawNodes[o + 7] * 8;
        for (let a = 0; a < 3; ++a) {
          this._rawNodes[o + a] = Math.min(this._rawNodes[l + a], this._rawNodes[r + a]);
          this._rawNodes[o + 4 + a] = Math.max(this._rawNodes[l + 4 + a], this._rawNodes[r + 4 + a]);
        }
      }
      touched++;
    }
    return touched;
  }

  /** Independent brute-force hit over every triangle: the oracle the GPU traversal is checked against. */
  bruteHit(ox, oy, oz, dx, dy, dz, occludersOnly, tmax) {
    let best = tmax, hitTri = -1;
    for (let i = 0; i < this.totalTris; ++i) {
      if (occludersOnly && !this.meta[i * 4 + 3]) continue;
      const o = i * 9;
      const t = triHitJS(ox, oy, oz, dx, dy, dz,
        this.tris[o], this.tris[o + 1], this.tris[o + 2],
        this.tris[o + 3], this.tris[o + 4], this.tris[o + 5],
        this.tris[o + 6], this.tris[o + 7], this.tris[o + 8], best);
      if (t > 0 && t < best) { best = t; hitTri = i; }
    }
    return { t: best, tri: hitTri, hit: hitTri >= 0 };
  }
}

/** Möller-Trumbore, the same formulation the WGSL uses, in JS. tMin 2e-3 like the native trace. */
export function triHitJS(ox, oy, oz, dx, dy, dz, v0x, v0y, v0z, v1x, v1y, v1z, v2x, v2y, v2z, tmax) {
  const e1x = v1x - v0x, e1y = v1y - v0y, e1z = v1z - v0z;
  const e2x = v2x - v0x, e2y = v2y - v0y, e2z = v2z - v0z;
  const px = dy * e2z - dz * e2y, py = dz * e2x - dx * e2z, pz = dx * e2y - dy * e2x;
  const det = e1x * px + e1y * py + e1z * pz;
  if (Math.abs(det) < 1e-8) return -1;
  const inv = 1 / det;
  const sx = ox - v0x, sy = oy - v0y, sz = oz - v0z;
  const u = (sx * px + sy * py + sz * pz) * inv;
  if (u < 0 || u > 1) return -1;
  const qx = sy * e1z - sz * e1y, qy = sz * e1x - sx * e1z, qz = sx * e1y - sy * e1x;
  const v = (dx * qx + dy * qy + dz * qz) * inv;
  if (v < 0 || u + v > 1) return -1;
  const t = (e2x * qx + e2y * qy + e2z * qz) * inv;
  return (t > 2e-3 && t < tmax) ? t : -1;
}
