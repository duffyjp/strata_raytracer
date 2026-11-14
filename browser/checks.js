// checks.js - correctness checks for the kernels this demo measures, and the CPU references they are
// checked against.
//
// This file exists because of two bugs the probe shipped and then found in itself (see
// docs/web-port-notes.md section 3): a hierarchy that traversed nothing reported "127x", and an output
// buffer without COPY_SRC compared as "24 of 40 pixels agree". Both looked like success. So every
// kernel here carries a check that can actually fail, and each check reports an agreement count plus a
// "did anything change" count, so a kernel that silently does nothing cannot be read as a speedup.
//
// The references are independent implementations of the same maths in JS, run on synthetic inputs whose
// expected output is also computable in closed form - a wavelet over a single impulse is just a kernel
// tap over the sum of the taps, which shares no code with the shader's loop.

// --------------------------------------------------------------------------- half float
// rgba16float is the accumulation and history format, so check inputs are written as IEEE-754 half and
// outputs read back the same way. Float16Array is not assumed: it is encoded and decoded by hand, with
// round-to-nearest-even on the way in.
export function f32ToF16Bits(v) {
  if (!Number.isFinite(v)) return v > 0 ? 0x7c00 : 0xfc00;
  const bits = new DataView(new ArrayBuffer(4));
  bits.setFloat32(0, v);
  const x = bits.getUint32(0);
  const sign = (x >>> 16) & 0x8000;
  let mant = x & 0x7fffff;
  const exp = (x >>> 23) & 0xff;
  if (exp === 0) return sign;                       // zero or f32 subnormal -> f16 zero
  let e = exp - 127 + 15;
  if (e >= 0x1f) return sign | 0x7c00;              // overflow -> infinity
  if (e <= 0) {
    if (e < -10) return sign;                       // underflow -> zero
    mant |= 0x800000;
    let m = mant >>> (14 - e);
    if ((mant >>> (13 - e)) & 1) m++;
    return sign | m;
  }
  let m = mant >>> 13;
  if ((mant & 0x1fff) > 0x1000 || ((mant & 0x1fff) === 0x1000 && (m & 1))) m++;
  if (m > 0x3ff) { m = 0; e += 1; if (e >= 0x1f) return sign | 0x7c00; }
  return sign | (e << 10) | m;
}

export function f16BitsToF32(h) {
  const sign = (h & 0x8000) ? -1 : 1;
  const e = (h >>> 10) & 0x1f;
  const f = h & 0x3ff;
  if (e === 0) return sign * Math.pow(2, -14) * (f / 1024);
  if (e === 0x1f) return f ? NaN : sign * Infinity;
  return sign * Math.pow(2, e - 15) * (1 + f / 1024);
}

const H5 = [1 / 16, 1 / 4, 3 / 8, 1 / 4, 1 / 16];
const h5 = (i) => H5[i + 2];
const lum = (c) => c[0] * 0.2126 + c[1] * 0.7152 + c[2] * 0.0722;
const clampi = (v, lo, hi) => Math.min(Math.max(v, lo), hi);
const rowsOf = (w, h, fn) => {
  const a = new Array(w * h);
  for (let y = 0; y < h; ++y) for (let x = 0; x < w; ++x) a[y * w + x] = fn(x, y);
  return a;
};

// --------------------------------------------------------------------------- JS reference: a-trous
// A transcription of svgf_atrous.wgsl for arbitrary small inputs, written independently of the shader
// so indexing, dilation, clamping, normalisation and remodulation errors show up as disagreement.
export function refAtrousIteration(src, variance, depth, normal, albedo, w, h, o) {
  const { step, first, last, sigmaZ, sigmaN, sigmaL } = o;
  const outC = new Array(w * h);
  const outV = new Array(w * h);
  const varAt = (i) => (first ? Math.max(0, variance[i][1] - variance[i][0] * variance[i][0])
    : Math.max(0, variance[i][0]));
  const normAt = (i) => {
    const n = normal[i];
    const v = [n[0] + 1e-6, n[1] + 1e-6, n[2] + 1e-6];
    const l = Math.hypot(v[0], v[1], v[2]) || 1;
    return [v[0] / l, v[1] / l, v[2] / l];
  };
  const G = [1, 2, 1, 2, 4, 2, 1, 2, 1];
  for (let y = 0; y < h; ++y) {
    for (let x = 0; x < w; ++x) {
      const p = y * w + x;
      const c0 = src[p];
      const n0 = normAt(p);
      const z0 = depth[p][0];
      const l0 = lum(c0);
      let acc = 0, wsum = 0, gi = 0;
      for (let dy = -1; dy <= 1; ++dy) {
        for (let dx = -1; dx <= 1; ++dx) {
          const q = clampi(y + dy, 0, h - 1) * w + clampi(x + dx, 0, w - 1);
          acc += G[gi] * varAt(q);
          wsum += G[gi];
          ++gi;
        }
      }
      const vBlur = Math.max(1e-6, acc / wsum);
      const sigmaLEff = Math.max(1e-6, sigmaL * Math.sqrt(4 * step * step * vBlur));
      let aR = 0, aG = 0, aB = 0, wTotal = 0, varAcc = 0, wLin = 0;
      for (let dy = -2; dy <= 2; ++dy) {
        for (let dx = -2; dx <= 2; ++dx) {
          const hw = h5(dy) * h5(dx);
          const q = clampi(y + dy * step, 0, h - 1) * w + clampi(x + dx * step, 0, w - 1);
          const cq = src[q];
          const wz = Math.exp(-Math.abs(z0 - depth[q][0]) / Math.max(1e-6, sigmaZ));
          const nq = normAt(q);
          const dotn = Math.max(0, n0[0] * nq[0] + n0[1] * nq[1] + n0[2] * nq[2]);
          const wn = Math.pow(dotn, Math.max(1, sigmaN));
          const wl = Math.exp(-Math.abs(l0 - lum(cq)) / (sigmaLEff + 1e-6));
          const wgt = hw * wz * wn * wl;
          aR += wgt * cq[0]; aG += wgt * cq[1]; aB += wgt * cq[2];
          wTotal += wgt;
          varAcc += hw * hw * wgt * wgt * varAt(q);
          wLin += hw * wgt;
        }
      }
      let outc = [Math.max(0, aR / Math.max(1e-6, wTotal)), Math.max(0, aG / Math.max(1e-6, wTotal)),
        Math.max(0, aB / Math.max(1e-6, wTotal))];
      if (last && albedo[p][3] > 0.5) outc = [outc[0] * albedo[p][0], outc[1] * albedo[p][1],
        outc[2] * albedo[p][2]];
      outC[p] = outc;
      outV[p] = [Math.max(0, varAcc / Math.max(1e-8, wLin * wLin)), 0, 0, 1];
    }
  }
  return { color: outC, variance: outV };
}

/**
 * Closed form for one pass over a single bright pixel with constant geometry and a variance large
 * enough that only the luminance edge term is left. Nothing here is shared with the shader's loop.
 */
export function expectImpulseResponse(px, py, step, sigmaL, varValue, impulseLum = 1) {
  const sigmaLEff = Math.max(1e-6, sigmaL * Math.sqrt(4 * step * step * varValue));
  let acc = 0, wTotal = 0;
  const l0 = (px === 0 && py === 0) ? impulseLum : 0;
  for (let ty = -2; ty <= 2; ++ty) {
    for (let tx = -2; tx <= 2; ++tx) {
      const hit = (px + tx * step === 0) && (py + ty * step === 0);
      const lq = hit ? impulseLum : 0;
      const wl = Math.exp(-Math.abs(l0 - lq) / (sigmaLEff + 1e-6));
      const w = h5(ty) * h5(tx) * wl;
      acc += w * lq;
      wTotal += w;
    }
  }
  return acc / Math.max(1e-6, wTotal);
}

// --------------------------------------------------------------------------- format capability probe
// Section 9.6 of the port notes records WebGPU restrictions that were discovered by being violated. Two
// of them decide what the demo is allowed to bind, and both were assertions until this probe measured
// them on the adapter actually in use:
//
//   * can a rgba16float texture be *sampled* with a LINEAR filter (a filtering sampler binding)?
//   * can it be sampled with a NEAREST filter through a non-filtering sampler binding?
//
// Each case builds a bind group + a compute dispatch that samples the texture, inside its own validation
// error scope, so the answer comes back as a measurement and the browser's own wording, and nothing
// escapes into the run's uncaptured-error count. That count has to stay at zero for a pass, so these
// probes must be scoped, and the scope has to be popped even when the call throws.
const FILTER_PROBE_WGSL = `
@group(0) @binding(0) var tex : texture_2d<f32>;
@group(0) @binding(1) var samp : sampler;
@group(0) @binding(2) var<storage, read_write> out : array<f32>;

@compute @workgroup_size(1)
fn main() {
  let c = textureSampleLevel(tex, samp, vec2<f32>(0.5, 0.5), 0.0);
  out[0] = c.r + c.g + c.b;
}
`;

async function filterProbeCase(device, { format, filter, bindingType, control = false }) {
  const d = device;
  const errors = [];
  let open = 0;                       // error scopes currently pushed: pop exactly this many, never more
  const scoped = async (stage, fn) => {
    await d.pushErrorScope('validation'); ++open;
    let r;
    try { r = fn(); } catch (e) { errors.push(`${stage}: threw ${String(e.message).slice(0, 160)}`); }
    const e = await d.popErrorScope(); --open;
    if (e) errors.push(`${stage}: ${String(e.message).replace(/\s+/g, ' ').slice(0, 200)}`);
    return r;
  };
  const name = `${format}_${filter}_${bindingType}${control ? '_control' : ''}`;
  // The texture is pre-filled with 0.5 in rgb, so a kernel that really sampled it writes 1.5 and a
  // dispatch that was silently dropped leaves 0. Without that, "no error" would be indistinguishable
  // from "the work never ran" - the exact mistake docs section 3 records for the probe's BVH.
  const tex = d.createTexture({
    label: `probe_${name}`, size: [8, 8], format,
    usage: (control ? 0 : GPUTextureUsage.TEXTURE_BINDING) | GPUTextureUsage.COPY_DST,
  });
  const fill = format === 'rgba16float'
    ? (() => { const a = new Uint16Array(8 * 8 * 4); for (let i = 0; i < a.length; ++i) a[i] = f32ToF16Bits(0.5); return a; })()
    : (() => { const a = new Uint8Array(8 * 8 * 4); for (let i = 0; i < a.length; ++i) a[i] = 128; return a; })();
  d.queue.writeTexture({ texture: tex }, fill, { bytesPerRow: 8 * (format === 'rgba16float' ? 8 : 4) }, [8, 8]);
  const samp = d.createSampler({ label: `probe_sampler_${name}`, magFilter: filter, minFilter: filter });
  const buf = d.createBuffer({
    label: `probe_out_${name}`, size: 16, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC,
  });
  let layout = null, bind = null, pipe = null;
  let sampledValue = null;
  try {
    layout = await scoped('bindGroupLayout', () => d.createBindGroupLayout({
      label: `bgl_probe_${name}`,
      entries: [
        { binding: 0, visibility: GPUShaderStage.COMPUTE, texture: { sampleType: 'float' } },
        { binding: 1, visibility: GPUShaderStage.COMPUTE, sampler: { type: bindingType } },
        { binding: 2, visibility: GPUShaderStage.COMPUTE, buffer: { type: 'storage' } },
      ],
    }));
    bind = await scoped('createBindGroup', () => d.createBindGroup({
      label: `bind_probe_${name}`, layout,
      entries: [
        { binding: 0, resource: tex.createView({ label: `probe_view_${name}` }) },
        { binding: 1, resource: samp },
        { binding: 2, resource: { buffer: buf } },
      ],
    }));
    const module = d.createShaderModule({ code: FILTER_PROBE_WGSL, label: `shader_probe_${name}` });
    pipe = await scoped('createComputePipeline', () => d.createComputePipeline({
      label: `pipe_probe_${name}`,
      layout: d.createPipelineLayout({ bindGroupLayouts: [layout], label: `layout_probe_${name}` }),
      compute: { module, entryPoint: 'main' },
    }));
    await scoped('dispatch+submit', () => {
      const enc = d.createCommandEncoder({ label: `enc_probe_${name}` });
      const pass = enc.beginComputePass({ label: `pass_probe_${name}` });
      pass.setPipeline(pipe);
      pass.setBindGroup(0, bind);
      pass.dispatchWorkgroups(1);
      pass.end();
      d.queue.submit([enc.finish()]);
      return d.queue.onSubmittedWorkDone();
    });
    const rb = d.createBuffer({
      label: `probe_readback_${name}`, size: 16,
      usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ,
    });
    await scoped('readback', () => {
      const enc = d.createCommandEncoder({ label: `enc_probe_readback_${name}` });
      enc.copyBufferToBuffer(buf, 0, rb, 0, 16);
      d.queue.submit([enc.finish()]);
      return rb.mapAsync(GPUMapMode.READ);
    });
    try {
      sampledValue = new Float32Array(rb.getMappedRange()).slice(0, 1)[0];
      rb.unmap();
    } catch (e) { sampledValue = null; }
    rb.destroy();
    return { format, filter, bindingType, control, ok: errors.length === 0, errors, sampledValue };
  } catch (e) {
    return { format, filter, bindingType, control, ok: false, errors: errors.concat(['threw: ' + e.message]), sampledValue };
  } finally {
    // Drain any scope left open by a throw, so a probe never borrows an error from the run's own count.
    while (open > 0) { try { await d.popErrorScope(); } catch (e) { break; } --open; }
    tex.destroy(); buf.destroy();
    if (typeof samp.destroy === 'function') samp.destroy();
  }
}

/** Run the filterability cases. Returns {cases:[...], verdict:{...}} - never throws. */
export async function floatFilterProbe(device) {
  const cases = [];
  for (const c of [
    { format: 'rgba16float', filter: 'linear', bindingType: 'filtering' },
    { format: 'rgba16float', filter: 'nearest', bindingType: 'filtering' },
    { format: 'rgba16float', filter: 'nearest', bindingType: 'non-filtering' },
    { format: 'rgba8unorm', filter: 'linear', bindingType: 'filtering' },
    // Control: a texture bound for sampling without TEXTURE_BINDING. This one MUST be rejected, or the
    // four results above are measuring nothing. It is the same defect that killed play mode, run as a
    // check instead of as an accident.
    { format: 'rgba8unorm', filter: 'linear', bindingType: 'filtering', control: true },
  ]) {
    try { cases.push(await filterProbeCase(device, c)); } catch (e) {
      cases.push({ ...c, ok: false, errors: ['threw: ' + e.message] });
    }
  }
  const by = (k) => cases.find((c) => `${c.format}/${c.filter}/${c.bindingType}${c.control ? '/control' : ''}` === k);
  const control = by('rgba8unorm/linear/filtering/control');
  return {
    cases,
    control,
    controlDetected: !!control && !control.ok,
    f16Linear: by('rgba16float/linear/filtering'),
    f16Nearest: by('rgba16float/nearest/filtering'),
    f16NearestNonFiltering: by('rgba16float/nearest/non-filtering'),
    rgba8Linear: by('rgba8unorm/linear/filtering'),
  };
}

export class Checks {
  /** @param base the per-frame Params object demo.js is using; checks clone it with overrides */
  constructor(renderer, base) { this.r = renderer; this.base = base; }

  params(over) { this.r.writeParams(Object.assign({}, this.base, over)); }

  writeHalf(tex, rows, w, h) {
    const data = new Uint16Array(w * h * 4);
    for (let i = 0; i < w * h; ++i) {
      for (let c = 0; c < 4; ++c) data[i * 4 + c] = f32ToF16Bits(rows[i][c] || 0);
    }
    this.r.device.queue.writeTexture({ texture: tex }, data, { bytesPerRow: w * 8 }, [w, h]);
  }

  async readHalfTex(tex, w, h) {
    const d = this.r.device;
    const bpr = Math.ceil((w * 8) / 256) * 256;
    const buf = d.createBuffer({ size: bpr * h, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
    const enc = d.createCommandEncoder();
    enc.copyTextureToBuffer({ texture: tex }, { buffer: buf, bytesPerRow: bpr, rowsPerImage: h },
      { width: w, height: h });
    d.queue.submit([enc.finish()]);
    await buf.mapAsync(GPUMapMode.READ);
    const u16 = new Uint16Array(buf.getMappedRange().slice(0));
    buf.unmap(); buf.destroy();
    const rows = new Array(w * h);
    const per = bpr / 2;
    for (let y = 0; y < h; ++y) {
      for (let x = 0; x < w; ++x) {
        const s = y * per + x * 4;
        rows[y * w + x] = [f16BitsToF32(u16[s]), f16BitsToF32(u16[s + 1]), f16BitsToF32(u16[s + 2]),
          f16BitsToF32(u16[s + 3])];
      }
    }
    return rows;
  }

  async run(encoders) {
    const d = this.r.device;
    const enc = d.createCommandEncoder();
    for (const fn of encoders) fn(enc);
    d.queue.submit([enc.finish()]);
    await d.queue.onSubmittedWorkDone();
  }

  /**
   * Five-iteration wavelet chain, GPU against the JS reference, on a synthetic frame whose structure
   * (an impulse, a low-amplitude texture, a normal discontinuity, constant depth) leaves nowhere for an
   * indexing or dilation error to hide.
   */
  async atrousChain(w, h, seed = 0) {
    const r = this.r;
    const n = w * h;
    const cx = w >> 1, cy = h >> 1;
    const src = rowsOf(w, h, (x, y) => (x === cx && y === cy)
      ? [1, 1, 1, 1]
      : [0.02 * ((x * 7 + y * 13 + seed) % 5), 0.02 * ((x * 11 + y * 3 + seed) % 5),
        0.02 * ((x * 5 + y * 17 + seed) % 5), 1]);
    // first iteration reads moments E[Y], E[Y^2]; variance = 900, which is what makes the luminance
    // edge weight near 1 so the kernel itself is what is being tested.
    const variance = rowsOf(w, h, () => [0, 900, 0, 1]);
    const depth = rowsOf(w, h, () => [0.5, 1, 1, 1]);
    const normal = rowsOf(w, h, (x) => (x < (w >> 2) ? [0, 1, 0, 0] : [0.6, 0.8, 0, 0]));
    const albedo = rowsOf(w, h, () => [0.8, 0.7, 0.6, 1]);

    this.params({ denoise: true, featureWrites: true, reset: false, prevValid: false });
    this.writeHalf(r.tmpColor, src, w, h);
    this.writeHalf(r.tmpMom, variance, w, h);
    this.writeHalf(r.gDep, depth, w, h);
    this.writeHalf(r.gNrm, normal, w, h);
    this.writeHalf(r.gAlb, albedo, w, h);
    await this.run([(enc) => r.encodeAtrous(enc)]);

    let c = src, v = variance;
    for (let it = 0; it < 5; ++it) {
      const res = refAtrousIteration(c, v, depth, normal, albedo, w, h, {
        step: 1 << it, first: it === 0, last: it === 4,
        sigmaZ: this.base.sigmaZ, sigmaN: this.base.sigmaN, sigmaL: this.base.sigmaL,
      });
      c = res.color; v = res.variance;
    }

    const got = await this.readHalfTex(r.pp0Color, w, h);
    let agree = 0, maxErr = 0, changed = 0;
    for (let i = 0; i < n; ++i) {
      const e = Math.max(Math.abs(got[i][0] - c[i][0]), Math.abs(got[i][1] - c[i][1]),
        Math.abs(got[i][2] - c[i][2]));
      maxErr = Math.max(maxErr, e);
      if (e <= 3e-3) agree++;
      if (Math.abs(got[i][0] - src[i][0]) > 1e-4 || Math.abs(got[i][1] - src[i][1]) > 1e-4) changed++;
    }
    return { agree, total: n, maxErr, changed };
  }

  /** One pass at a chosen dilation, checked against the closed form for an impulse. */
  async atrousClosedForm(w, h, step) {
    const r = this.r;
    const cx = w >> 1, cy = h >> 1;
    const src = rowsOf(w, h, (x, y) => (x === cx && y === cy) ? [1, 1, 1, 1] : [0, 0, 0, 1]);
    const variance = rowsOf(w, h, () => [0, 900, 0, 1]);
    const depth = rowsOf(w, h, () => [0.5, 1, 1, 1]);
    const normal = rowsOf(w, h, () => [0, 1, 0, 0]);
    const albedo = rowsOf(w, h, () => [1, 1, 1, 1]);
    this.params({ denoise: true, featureWrites: true });
    this.writeHalf(r.tmpColor, src, w, h);
    this.writeHalf(r.tmpMom, variance, w, h);
    this.writeHalf(r.gDep, depth, w, h);
    this.writeHalf(r.gNrm, normal, w, h);
    this.writeHalf(r.gAlb, albedo, w, h);

    const bg = r.atrousBindFor({ step, first: true, last: false }, 'src', 'dst0');
    await this.run([(enc) => {
      const pass = enc.beginComputePass({ label: 'atrous_closed' });
      pass.setPipeline(r.atrous.real);
      pass.setBindGroup(0, bg.bind);
      pass.dispatchWorkgroups(Math.ceil(w / 8), Math.ceil(h / 8));
      pass.end();
    }]);

    const got = await this.readHalfTex(r.pp0Color, w, h);
    const probes = [[0, 0], [step, 0], [-step, 0], [0, step], [step, step], [2 * step, 0],
      [-2 * step, 0], [1, 0], [step + 1, step]];
    let agree = 0, total = 0;
    const detail = [];
    for (const [ox, oy] of probes) {
      const x = cx + ox, y = cy + oy;
      if (x < 0 || y < 0 || x >= w || y >= h) continue;
      ++total;
      const want = expectImpulseResponse(ox, oy, step, this.base.sigmaL, 900, 1);
      const g = Math.max(got[y * w + x][0], got[y * w + x][1], got[y * w + x][2]);
      if (Math.abs(g - want) <= 3e-3) ++agree;
      detail.push(`d(${ox},${oy}) gpu ${g.toFixed(4)} want ${want.toFixed(4)}`);
    }
    bg.params.destroy();
    return { agree, total, detail };
  }

  /** Temporal pass: reset behaviour, then a history blend whose alpha is exactly predictable. */
  async temporalCheck(w, h) {
    const r = this.r;
    const n = w * h;
    const radiance = rowsOf(w, h, (x, y) => [0.3 + 0.4 * (x / w), 0.2 + 0.3 * (y / Math.max(1, h - 1)),
      0.5, 1]);
    const albedo = rowsOf(w, h, (x) => [0.5 + 0.4 * (x / w), 0.6, 0.7, 1]);
    const depth = rowsOf(w, h, () => [0.25, 1, 3, 1]);
    const normal = rowsOf(w, h, () => [0, 1, 0, 0]);

    // --- 1. a reset frame: the output must be the demodulated radiance and the first moments.
    this.params({ denoise: true, featureWrites: true, reset: true, prevValid: false });
    this.writeHalf(r.hdr, radiance, w, h);
    this.writeHalf(r.gAlb, albedo, w, h);
    this.writeHalf(r.gDep, depth, w, h);
    this.writeHalf(r.gNrm, normal, w, h);
    await this.run([(enc) => r.encodeTemporal(enc)]);
    const demod = await this.readHalfTex(r.tmpColor, w, h);
    const moms = await this.readHalfTex(r.tmpMom, w, h);

    let resetAgree = 0, resetMax = 0;
    for (let i = 0; i < n; ++i) {
      const want = [radiance[i][0] / albedo[i][0], radiance[i][1] / albedo[i][1],
        radiance[i][2] / albedo[i][2]];
      let e = Math.max(Math.abs(demod[i][0] - want[0]), Math.abs(demod[i][1] - want[1]),
        Math.abs(demod[i][2] - want[2]));
      const y = lum(want);
      e = Math.max(e, Math.abs(moms[i][0] - y), Math.abs(moms[i][1] - y * y),
        Math.abs(moms[i][2] - 1), Math.abs(moms[i][3] - 1));
      resetMax = Math.max(resetMax, e);
      if (e <= 1e-2) resetAgree++;          // half precision: 1e-2 of radiance is ~4 codes
    }

    // --- 2. a blend. Same camera and same depth, so the reprojection lands on the same pixel and the
    //        alpha is exactly 1/(1+nPrev), which makes the whole pass predictable per pixel.
    const nPrev = 3;
    const histRows = rowsOf(w, h, (x, y) => {
      const i = y * w + x;
      return [Math.min(60000, demod[i][0] * 2), demod[i][1] * 2, demod[i][2] * 2, 1];
    });
    const histMom = rowsOf(w, h, (x, y) => {
      const i = y * w + x;
      const yv = lum(histRows[i]);
      return [yv, Math.min(60000, yv * yv), nPrev, 1];
    });
    this.writeHalf(r.histColor, histRows, w, h);
    this.writeHalf(r.histMom, histMom, w, h);
    this.writeHalf(r.histDep, depth, w, h);
    this.params({ denoise: true, featureWrites: true, reset: false, prevValid: true });
    await this.run([(enc) => r.encodeTemporal(enc)]);
    const blended = await this.readHalfTex(r.tmpColor, w, h);

    const alpha = Math.max(1 / (1 + nPrev), 1 / Math.max(2, this.base.historyCap));
    let blendAgree = 0, blendMax = 0, differsFromFresh = 0;
    for (let i = 0; i < n; ++i) {
      const want = [
        histRows[i][0] + (demod[i][0] - histRows[i][0]) * alpha,
        histRows[i][1] + (demod[i][1] - histRows[i][1]) * alpha,
        histRows[i][2] + (demod[i][2] - histRows[i][2]) * alpha,
      ];
      const e = Math.max(Math.abs(blended[i][0] - want[0]), Math.abs(blended[i][1] - want[1]),
        Math.abs(blended[i][2] - want[2]));
      blendMax = Math.max(blendMax, e);
      if (e <= 1e-2) blendAgree++;
      // A temporal pass that silently did nothing would return the fresh demodulation everywhere.
      if (Math.abs(blended[i][0] - demod[i][0]) > 1e-3) differsFromFresh++;
    }
    return { resetAgree, blendAgree, total: n, resetMax, blendMax, alpha, nPrev, differsFromFresh };
  }

  /** Present pass: exact sRGB codes against a CPU computation, plus the row-flip orientation. */
  async presentCheck(w, h) {
    const r = this.r;
    const n = w * h;
    // A gradient monotonic in y, so a flipped row shows up as a wrong value rather than as a picture
    // that merely looks plausible.
    const rows = rowsOf(w, h, (x, y) => [0.05 + 0.9 * (y / Math.max(1, h - 1)), 0.2, 0.4, 1]);
    this.params({ denoise: false, featureWrites: false, reset: true, samplesForDisplay: 1,
      dither: false, presentMode: 0 });
    this.writeHalf(r.hdr, rows, w, h);
    await this.run([(enc) => r.encodePresent(enc)]);
    const bytes = await r.readPresent();

    const aces = (v) => Math.min(1, Math.max(0, (v * (2.51 * v + 0.03)) / (v * (2.43 * v + 0.59) + 0.14)));
    const srgb = (c) => (c <= 0.0031308 ? c * 12.92 : 1.055 * Math.pow(Math.max(c, 1e-6), 1 / 2.4) - 0.055);
    const exposure = this.base.exposure;

    let agree = 0, maxCode = 0;
    for (let y = 0; y < h; ++y) {
      for (let x = 0; x < w; ++x) {
        const i = (y * w + x) * 4;
        const src = rows[(h - 1 - y) * w + x];
        const vx = x / w - 0.5, vy = y / h - 0.5;
        const vign = 1 - 0.45 * (vx * vx + vy * vy);
        let maxc = 0;
        for (let c = 0; c < 3; ++c) {
          const want = Math.round(Math.min(1, Math.max(0, srgb(aces(src[c] * exposure * vign)))) * 255);
          maxc = Math.max(maxc, Math.abs(want - bytes[i + c]));
        }
        maxCode = Math.max(maxCode, maxc);
        if (maxc <= 2) agree++;
      }
    }
    // Row 0 of the presentation texture is the top of the image, i.e. the small-y (dark) end.
    const top = bytes[2];
    const bottom = bytes[((h - 1) * w + 0) * 4 + 2];
    return { agree, total: n, maxCode, flipOk: top < bottom, top, bottom };
  }
}
