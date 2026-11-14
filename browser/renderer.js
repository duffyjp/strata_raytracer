// renderer.js - WebGPU resource setup, the per-pass encoders, the timing harness and the readback
// paths for the browser demo.
//
// Two things here are deliberate and worth stating before reading the code:
//
// 1. Bind group layouts are written out explicitly, not 'auto'. The trace kernel is compiled in four
//    measurement variants that const-fold whole sections of the shader away, and with layout:'auto' a
//    variant that does not read the material table would silently get a *different* layout - so the
//    bind group built for the full kernel would be rejected by the primary-only kernel, or (worse) the
//    two would not be comparable. One layout for all variants is the only way the numbers mean the
//    same thing across a row of the table.
//
// 2. The measurement mode synchronises per pass, which is exactly what a shipping renderer must not
//    do. It is here because web content has no trustworthy GPU timestamps, so the only way to
//    attribute milliseconds to a pass is a submit + onSubmittedWorkDone() around it - and that costs
//    0.17 - 1.0 ms per dispatch on this machine (measured in the probe). Every number therefore comes
//    with the empty-dispatch floor measured alongside it, in the same run, and anything within about
//    twice the floor is reported as a bound rather than a measurement.

// Relative to this module, not to the document: demo.html lives at /web/ while the server root is the
// repository root (it has to be, or scene.pak is unreachable), so a path written from the document's
// point of view resolves to /web/web/shaders/ and 404s.
const SHADER_DIR = new URL('./shaders/', import.meta.url).href;

export const TRACE_MODE = { FULL: 0, PRIMARY: 1, ONE_BOUNCE: 2, NO_NEE: 3 };

const PARAM_FLOATS = 17 * 4;          // 17 vec4 rows, matching `struct Params` in every .wgsl

function align(v, a) { return Math.ceil(v / a) * a; }

// --------------------------------------------------------------------------- usage auditing
// Every GPU object created here carries a non-empty label, and every view carries the name of the
// texture it came from. That is not decoration. The play-mode failure recorded in
// docs/web-port-notes.md section 10 arrived as
//
//   "Usage flags TextureUsages(COPY_SRC | COPY_DST | STORAGE_BINDING) of TextureView with '' label
//    do not contain required usage flags TextureUsages(TEXTURE_BIND)"
//
// and with no names anywhere in it there were sixteen textures that could have been meant, so the
// diagnosis had to be guessed. A labelled object makes the next such error name itself, and
// #needUsage below goes one step further and turns it into a thrown JS error at the call site, before
// the GPU ever sees it.
const USAGE_BITS = [
  ['COPY_SRC', 'CopySrc'], ['COPY_DST', 'CopyDst'], ['TEXTURE_BINDING', 'TextureBinding'],
  ['STORAGE_BINDING', 'StorageBinding'], ['RENDER_ATTACHMENT', 'RenderAttachment'],
];
const usageBit = (k) => (typeof GPUTextureUsage !== 'undefined' ? GPUTextureUsage[k] : 0);

/** 'CopySrc|StorageBinding' for a usage bitmask, so an error message can be read by a human. */
export function usageName(flags) {
  if (typeof flags !== 'number') return 'unknown';
  const set = USAGE_BITS.filter(([k]) => (flags & usageBit(k)) !== 0).map(([, n]) => n);
  return set.length ? set.join('|') : 'none';
}

async function fetchShader(url) {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`cannot fetch ${url}: HTTP ${res.status}`);
  return await res.text();
}

export class Renderer {
  constructor(device, ctx) {
    this.device = device;
    this.ctx = ctx;
    this.W = 0; this.H = 0;
    this.src = {};
    this.log = [];
  }

  static async load(device, ctx) {
    const r = new Renderer(device, ctx);
    for (const name of ['trace', 'svgf_temporal', 'svgf_atrous', 'present', 'empty']) {
      r.src[name] = await fetchShader(`${SHADER_DIR}${name}.wgsl`);
    }
    await r.#buildPipelines();
    return r;
  }

  async #module(name, code) {
    const device = this.device;
    const m = device.createShaderModule({ code, label: name });
    const info = await m.getCompilationInfo();
    const errs = info.messages.filter((x) => x.type === 'error');
    if (errs.length) {
      const at = errs[0];
      // WGSLCompilationMessage carries lineNum/linePos/offset; the earlier lineOffset/lineNumber names
      // silently produced "undefined:undefined" and made every shader error a guessing game.
      const where = `line ${at.lineNum} pos ${at.linePos} offset ${at.offset}`;
      const more = errs.length > 1 ? ` (+${errs.length - 1} more: ${errs.slice(1, 4).map((x) => x.message).join(' | ')})` : '';
      throw new Error(`WGSL ${name}: ${where}: ${at.message}${more}`);
    }
    return m;
  }

  // ------------------------------------------------------------------------- layouts
  #bgl(entries, label) {
    return this.device.createBindGroupLayout({ entries, label: label || 'bgl' });
  }

  #storageBinding(b, vis = GPUShaderStage.COMPUTE) {
    return { binding: b, visibility: vis, buffer: { type: 'read-only-storage' } };
  }

  #bvhBinding(b) {
    return { binding: b, visibility: GPUShaderStage.COMPUTE, buffer: { type: 'storage' } };
  }

  /**
   * Throw if `tex` was not created with the usage the binding is about to ask for. The GPU's own
   * version of this complaint names no object ("TextureView with '' label"), which is how a whole
   * animated mode ended up drawing nothing while the page printed `done`; catching it here names the
   * texture and the flags involved.
   */
  #needUsage(tex, flagKey, what) {
    const need = usageBit(flagKey);
    if (!need || typeof tex.usage !== 'number') return;      // no GPUTextureUsage constants to check
    if (!tex.label) throw new Error(`${what}: texture has an empty label - every texture needs one`);
    if ((tex.usage & need) === 0) {
      throw new Error(`texture "${tex.label}" is bound as ${what} but was created with usage ` +
        `${usageName(tex.usage)}; it needs ${flagKey}`);
    }
  }

  /** A view for a `texture_2d` (sampled) binding: needs TEXTURE_BINDING, and carries a label. */
  sampledView(tex, label) {
    const name = label || tex.label;
    this.#needUsage(tex, 'TEXTURE_BINDING', `sampled binding "${name}"`);
    const v = tex.createView({ label: `${name}:sampled` });
    this.#audit(name, tex, 'sampled (texture_2d)');
    return v;
  }

  /** A view for a `texture_storage_2d` binding: needs STORAGE_BINDING, and carries a label. */
  storageView(tex, label) {
    const name = label || tex.label;
    this.#needUsage(tex, 'STORAGE_BINDING', `"${name}" storage-bound`);
    const v = tex.createView({ label: `${name}:storage` });
    this.#audit(name, tex, 'storage (texture_storage_2d)');
    return v;
  }

  /** Record what every bind group actually asks for, so the audit table is built by the code that runs. */
  #audit(name, tex, kind) {
    if (!this.audit) this.audit = [];
    const key = `${name}|${tex.label}|${kind}`;
    if (!this.audit.some((r) => r.key === key)) {
      this.audit.push({ key, view: name, texture: tex.label, kind, format: tex.format, usage: tex.usage });
    }
  }

  #tex(b) { return { binding: b, visibility: GPUShaderStage.COMPUTE, texture: { sampleType: 'float' } }; }
  #samp(b) { return { binding: b, visibility: GPUShaderStage.COMPUTE, sampler: { type: 'filtering' } }; }
  #stor(b, fmt) {
    return { binding: b, visibility: GPUShaderStage.COMPUTE, storageTexture: { access: 'write-only', format: fmt } };
  }
  #storRW(b, fmt) {
    return { binding: b, visibility: GPUShaderStage.COMPUTE, storageTexture: { access: 'read-write', format: fmt } };
  }
  #uniform(b) { return { binding: b, visibility: GPUShaderStage.COMPUTE, buffer: { type: 'uniform' } }; }

  async #buildPipelines() {
    const d = this.device;
    const F16 = 'rgba16float';

    // ---- trace: 16 bindings, identical for every variant. Binding 9 is the accumulator written,
    // binding 15 the previous frame's accumulator read - see the note in trace.wgsl on why that split
    // is forced: rgba16float cannot be a read_write storage texture.
    this.traceLayout = this.#bgl([
      this.#uniform(0),
      this.#storageBinding(1), this.#storageBinding(2), this.#storageBinding(3),
      this.#storageBinding(4), this.#storageBinding(5),
      this.#tex(6), this.#tex(7), this.#samp(8),
      this.#stor(9, F16), this.#stor(10, F16), this.#stor(11, F16), this.#stor(12, F16),
      this.#bvhBinding(13), this.#bvhBinding(14),
      this.#tex(15),
    ], 'bgl_trace');

    this.trace = {};
    for (const [name, mode] of Object.entries({ full: 0, primary: 1, oneBounce: 2, noNEE: 3 })) {
      const code = this.src.trace.replace('const TRACE_MODE : u32 = 0u;', `const TRACE_MODE : u32 = ${mode}u;`);
      this.trace[name] = await this.#pipe('trace_' + name, code, this.traceLayout);
    }
    const counted = this.src.trace
      .replace('const TRACE_MODE : u32 = 0u;', 'const TRACE_MODE : u32 = 0u;')
      .replace('const COUNT_ON : u32 = 0u;', 'const COUNT_ON : u32 = 1u;');
    this.trace.instrumented = await this.#pipe('trace_instrumented', counted, this.traceLayout);

    // ---- temporal
    this.temporalLayout = this.#bgl([
      this.#uniform(0),
      this.#tex(1), this.#tex(2), this.#tex(3), this.#tex(4),
      this.#tex(5), this.#tex(6), this.#tex(7),
      this.#stor(8, F16), this.#stor(9, F16), this.#stor(10, F16),
    ], 'bgl_temporal');
    this.temporal = {
      real: await this.#pipe('temporal', this.src.svgf_temporal, this.temporalLayout),
      trivial: await this.#pipe('temporal_trivial',
        this.src.svgf_temporal.replace('const TEMPORAL_TRIVIAL : u32 = 0u;',
          'const TEMPORAL_TRIVIAL : u32 = 1u;'), this.temporalLayout),
    };

    // ---- a-trous
    this.atrousLayout = this.#bgl([
      this.#uniform(0),
      this.#tex(1), this.#tex(2), this.#tex(3), this.#tex(4), this.#tex(5),
      this.#stor(6, F16), this.#stor(7, F16),
    ], 'bgl_atrous');
    this.atrous = {
      real: await this.#pipe('atrous', this.src.svgf_atrous, this.atrousLayout),
      trivial: await this.#pipe('atrous_trivial',
        this.src.svgf_atrous.replace('const ATROUS_TRIVIAL : u32 = 0u;',
          'const ATROUS_TRIVIAL : u32 = 1u;'), this.atrousLayout),
    };

    // ---- present
    // Bindings 1 and 2 are read with textureLoad in present.wgsl (verified, not assumed: grep the file
    // for textureSample* and there is none), so those rgba16float views do not need a filterable-float
    // path. The sampler that does filter is the blit sampler in drawToCanvas, and it filters the
    // rgba8unorm presentation target with a NEAREST filter.
    this.presentLayout = this.#bgl([
      this.#uniform(0), this.#tex(1), this.#tex(2), this.#stor(3, 'rgba8unorm'),
    ], 'bgl_present');
    this.present = await this.#pipe('present', this.src.present, this.presentLayout);

    // ---- calibration: an empty kernel, two grid sizes, no bindings at all
    this.emptyLayout = this.#bgl([], 'bgl_empty');
    this.empty = {
      one: await this.#pipeEntry('empty_one', this.src.empty, this.emptyLayout, 'main'),
      grid: await this.#pipeEntry('empty_grid', this.src.empty, this.emptyLayout, 'mainGrid'),
    };

    // No synthetic-fill kernel here: it needs five storage textures and the default compute limit is
    // four per stage (the adapter allows eight only if requested, and the renderer never needs that).
    // The filter checks write their input textures from JS instead, which is both legal and easier to
    // reason about than a shader that manufactures test data.
  }

  async #pipe(name, code, layout) {
    return this.#pipeEntry(name, code, layout, 'main');
  }

  async #pipeEntry(name, code, layout, entryPoint) {
    const t0 = performance.now();
    const module = await this.#module(name, code);
    const p = this.device.createComputePipeline({
      label: `pipe_${name}`,
      layout: this.device.createPipelineLayout({ bindGroupLayouts: [layout], label: `layout_${name}` }),
      compute: { module, entryPoint },
    });
    // Pipeline compilation is lazy in Dawn; awaiting its info here means a bad TRACE_MODE or
    // ATROUS_TRIVIAL variant is reported now, at load, with a line number, rather than as a silent
    // no-op dispatch during a timed pass.
    const info = typeof p.getCompilationInfo === 'function' ? await p.getCompilationInfo() : null;
    const errs = info ? info.messages.filter((x) => x.type === 'error') : [];
    if (errs.length) {
      throw new Error(`pipeline ${name}: ${errs[0].message} (line ${errs[0].lineNum})`);
    }
    this.log.push(`${name} [${entryPoint}] ${(performance.now() - t0).toFixed(1)} ms`);
    return p;
  }

  // ------------------------------------------------------------------------- static resources
  /** materials / lights / instances / BVH nodes / triangles / textures. Called once, then on rebuild. */
  setGeometry({ materials, lights, instances, nodes, tris, root, occlRoot, triCount }) {
    const d = this.device;
    const mk = (label, data, usage = GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC) => {
      const buf = d.createBuffer({
        label, size: align(data.byteLength, 4), usage: usage | GPUBufferUsage.COPY_DST,
      });
      d.queue.writeBuffer(buf, 0, data);
      return buf;
    };
    if (this.matBuf) this.matBuf.destroy();
    if (this.lightBuf) this.lightBuf.destroy();
    if (this.instBuf) this.instBuf.destroy();
    if (this.nodeBuf) this.nodeBuf.destroy();
    if (this.triBuf) this.triBuf.destroy();
    this.matBuf = mk('buf_materials', materials);
    this.lightBuf = mk('buf_lights', lights);
    this.instBuf = mk('buf_instances', instances, GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST);
    this.nodeBuf = mk('buf_bvh_nodes', nodes, GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST);
    this.triBuf = mk('buf_triangles', tris);
    this.root = root; this.occlRoot = occlRoot; this.triCount = triCount;
  }

  /**
   * The texture sampler, split out of setTextures() for the port under browser/: the exe's log
   * prints `step: command pool, sync objects, sampler` at the moment the sampler exists
   * (src/main.cpp:272), and saying that sentence truthfully needs the sampler to exist there rather
   * than at the first texture upload. Same descriptor, same label - setTextures() now calls this and
   * gets the identical object.
   */
  ensureSampler() {
    if (!this.sampler) {
      // Linear, and legal: the two scene textures are rgba8unorm, a filterable format. The rgba16float
      // targets are never sampled with a filter (see the present/blit note in #buildPipelines), which
      // is why this sampler does not need the float32-filterable path.
      this.sampler = this.device.createSampler({
        label: 'sampler_texture_linear',
        magFilter: 'linear', minFilter: 'linear', mipmapFilter: 'linear',
        addressModeU: 'repeat', addressModeV: 'repeat', addressModeW: 'repeat',
      });
    }
    return this.sampler;
  }

  setTextures(bitmaps) {
    const d = this.device;
    const mkTex = (label, bmp) => {
      const t = d.createTexture({
        label, size: [bmp.width, bmp.height], format: 'rgba8unorm',
        usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST
          // Dawn requires RenderAttachment as well for copyExternalImageToTexture, and says so only in
          // an uncaptured error, which is how the textures ended up silently un-uploaded.
          | GPUTextureUsage.RENDER_ATTACHMENT,
      });
      d.queue.copyExternalImageToTexture({ source: bmp }, { texture: t }, [bmp.width, bmp.height]);
      return t;
    };
    if (this.tex0) this.tex0.destroy();
    if (this.tex1) this.tex1.destroy();
    // UNORM, not sRGB: the native app uploads the pak PNGs as R8G8B8A8_UNORM (core.cpp:900) and the
    // GLSL then reads the stored sRGB-encoded bytes with no decode. Matching that byte-for-byte is part
    // of matching the picture.
    this.tex0 = mkTex('tex_checker_rgba8', bitmaps[0] || mkFallbackBitmap());
    this.tex1 = mkTex('tex_emissive_ramp_rgba8', bitmaps[1] || mkFallbackBitmap());
    if (!this.sampler) {
      // Linear, and legal: these two are rgba8unorm, a filterable format. The rgba16float targets are
      // never sampled with a filter (see the present/blit note in #buildPipelines), which is why this
      // sampler does not need the float32-filterable path.
      this.sampler = d.createSampler({
        label: 'sampler_texture_linear',
        magFilter: 'linear', minFilter: 'linear', mipmapFilter: 'linear',
        addressModeU: 'repeat', addressModeV: 'repeat', addressModeW: 'repeat',
      });
    }
  }

  // ------------------------------------------------------------------------- per-resolution targets
  resize(w, h) {
    const d = this.device;
    if (this.W === w && this.H === h && this.hdr) return;
    // The bind groups below hold views of the textures being dropped, so they have to go with them: a
    // bind group pointing at a destroyed texture is not an error when it is made stale, only at the
    // next set_bind_group - which is the worst possible place for it to surface. Guarded, because
    // GPUBindGroup.destroy() is newer than the rest of the API and Edge 157 does not have it (measured:
    // it threw "b.destroy is not a function" here), where leaving them for the GC is all that happens.
    const softDestroy = (o) => { if (o && typeof o.destroy === 'function') o.destroy(); };
    for (const b of new Set([this.traceBind, ...(this.traceBindPair || []), this.temporalBind,
      ...(this.atrousBind || []), this.presentBind, this.blitBind])) softDestroy(b);
    this.blitBind = null; this.blitBindSrc = null;
    for (const t of [this.hdr, ...(this.hdrPair || []), this.gAlb, this.gNrm, this.gDep, this.tmpColor, this.tmpMom, this.tmpDep,
      this.histColor, this.histMom, this.histDep, this.pp0Color, this.pp0Var, this.pp1Color,
      this.pp1Var, this.presentTex]) if (t) t.destroy();

    const F16 = 'rgba16float';
    // TEXTURE_BINDING is in here because these are read back as `texture_2d` by later passes (and, for
    // hdr, by readHalf through COPY_SRC). Any texture that ends up in a sampled binding needs it, and
    // sampledView() now refuses to build a bind group for one that does not have it.
    const ST = GPUTextureUsage.STORAGE_BINDING | GPUTextureUsage.TEXTURE_BINDING |
      GPUTextureUsage.COPY_SRC | GPUTextureUsage.COPY_DST;
    const t16 = (label) => d.createTexture({
      label, size: [w, h], format: F16, usage: ST,
    });
    this.hdr = t16('hdr');
    // Ping-pong pair for the accumulator: the pass writes one and reads the other. `hdr` is the copy
    // every downstream pass and every readback reads, which is the extra step the format restriction
    // forces; encodeAccumCopy exists so its cost can be measured instead of guessed at.
    // Both slots carry TEXTURE_BINDING because binding 15 of the trace layout reads the previous slot
    // as a texture - the accumulator was the suspected culprit in the play-mode failure but it was
    // already legal, and #needUsage would now say so out loud. See docs/web-port-notes.md section 10.
    this.hdrPair = [t16('accum_a'), t16('accum_b')];
    this.hdrCur = 0;
    this.gAlb = t16('gbuf_albedo');
    this.gNrm = t16('gbuf_normal');
    this.gDep = t16('gbuf_depth');
    this.tmpColor = t16('tmp_color');
    this.tmpMom = t16('tmp_moments');
    this.tmpDep = t16('tmp_depth');
    this.histColor = t16('hist_color');
    this.histMom = t16('hist_moments');
    this.histDep = t16('hist_depth');
    this.pp0Color = t16('pp0_color');
    this.pp0Var = t16('pp0_var');
    this.pp1Color = t16('pp1_color');
    this.pp1Var = t16('pp1_var');
    // The one that was missing a flag. present.wgsl writes this as a storage texture, and drawToCanvas
    // then samples it to fill the <canvas>, so it needs STORAGE_BINDING *and* TEXTURE_BINDING. Without
    // the second, createBindGroup rejected the blit, the bind group came back invalid, every submit
    // from then on was rejected, and animated mode drew nothing while the page printed `done`.
    this.presentTex = d.createTexture({
      label: 'present_rgba8', size: [w, h], format: 'rgba8unorm',
      usage: GPUTextureUsage.STORAGE_BINDING | GPUTextureUsage.TEXTURE_BINDING |
        GPUTextureUsage.COPY_SRC | GPUTextureUsage.COPY_DST,
    });

    if (!this.paramsMain) {
      const ub = (label) => d.createBuffer({
        label, size: PARAM_FLOATS * 4, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
      });
      this.paramsMain = ub('params_trace');
      this.paramsPresent = ub('params_present');
      this.paramsAtrous = [0, 1, 2, 3, 4].map((i) => ub(`params_atrous_${i}`));
      this.checkBuf = d.createBuffer({
        label: 'buf_check', size: 16384,
        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC | GPUBufferUsage.COPY_DST,
      });
      this.counterBuf = d.createBuffer({
        label: 'buf_counters', size: 64,
        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC | GPUBufferUsage.COPY_DST,
      });
      this.dummyBuf = d.createBuffer({
        label: 'buf_dummy', size: 64, usage: GPUBufferUsage.STORAGE,
      });
      this.emptyBuf = d.createBuffer({
        label: 'buf_empty_target', size: 64,
        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC | GPUBufferUsage.COPY_DST,
      });
      this.emptyBind = d.createBindGroup({ layout: this.emptyLayout, entries: [], label: 'bind_empty' });
    }

    const traceBindFor = (slot) => d.createBindGroup({
      label: `bind_trace_${slot === 0 ? 'a' : 'b'}`,
      layout: this.traceLayout,
      entries: [
        { binding: 0, resource: { buffer: this.paramsMain } },
        { binding: 1, resource: { buffer: this.matBuf } },
        { binding: 2, resource: { buffer: this.lightBuf } },
        { binding: 3, resource: { buffer: this.instBuf } },
        { binding: 4, resource: { buffer: this.nodeBuf } },
        { binding: 5, resource: { buffer: this.triBuf } },
        { binding: 6, resource: this.sampledView(this.tex0) },
        { binding: 7, resource: this.sampledView(this.tex1) },
        { binding: 8, resource: this.sampler },
        // 9 writes the slot named in the label, 15 reads the other one.
        { binding: 9, resource: this.storageView(this.hdrPair[slot]) },
        { binding: 10, resource: this.storageView(this.gAlb) },
        { binding: 11, resource: this.storageView(this.gNrm) },
        { binding: 12, resource: this.storageView(this.gDep) },
        { binding: 13, resource: { buffer: this.checkBuf } },
        { binding: 14, resource: { buffer: this.counterBuf } },
        { binding: 15, resource: this.sampledView(this.hdrPair[slot ^ 1]) },
      ],
    });
    this.traceBindPair = [traceBindFor(0), traceBindFor(1)];
    // The non-accumulating binding used by the per-pass timing runs: it always writes the same slot and
    // always reads the other one, so repeating it measures the pass without a feedback loop.
    this.traceBind = this.traceBindPair[0];

    this.temporalBind = d.createBindGroup({
      label: 'bind_temporal',
      layout: this.temporalLayout,
      entries: [
        { binding: 0, resource: { buffer: this.paramsMain } },
        { binding: 1, resource: this.sampledView(this.hdr) },
        { binding: 2, resource: this.sampledView(this.gAlb) },
        { binding: 3, resource: this.sampledView(this.gNrm) },
        { binding: 4, resource: this.sampledView(this.gDep) },
        { binding: 5, resource: this.sampledView(this.histColor) },
        { binding: 6, resource: this.sampledView(this.histMom) },
        { binding: 7, resource: this.sampledView(this.histDep) },
        { binding: 8, resource: this.storageView(this.tmpColor) },
        { binding: 9, resource: this.storageView(this.tmpMom) },
        { binding: 10, resource: this.storageView(this.tmpDep) },
      ],
    });

    // Iteration 0 reads the temporal output; the rest ping-pong. Same order the native app uses, which
    // leaves the result in pp0Color after five iterations.
    const srcs = [[this.tmpColor, this.tmpMom], [this.pp0Color, this.pp0Var], [this.pp1Color, this.pp1Var]];
    const dsts = [[this.pp0Color, this.pp0Var], [this.pp1Color, this.pp1Var], [this.pp0Color, this.pp0Var]];
    const setOrder = [0, 1, 2, 1, 2];
    this.atrousBind = [];
    for (let it = 0; it < 5; ++it) {
      const k = setOrder[it];
      this.atrousBind.push(this.device.createBindGroup({
        label: `bind_atrous_${it}`,
        layout: this.atrousLayout,
        entries: [
          { binding: 0, resource: { buffer: this.paramsAtrous[it] } },
          { binding: 1, resource: this.sampledView(srcs[k][0]) },
          { binding: 2, resource: this.sampledView(srcs[k][1]) },
          { binding: 3, resource: this.sampledView(this.gAlb) },
          { binding: 4, resource: this.sampledView(this.gNrm) },
          { binding: 5, resource: this.sampledView(this.gDep) },
          { binding: 6, resource: this.storageView(dsts[k][0]) },
          { binding: 7, resource: this.storageView(dsts[k][1]) },
        ],
      }));
    }

    this.presentBind = d.createBindGroup({
      label: 'bind_present',
      layout: this.presentLayout,
      entries: [
        { binding: 0, resource: { buffer: this.paramsPresent } },
        { binding: 1, resource: this.sampledView(this.hdr) },
        { binding: 2, resource: this.sampledView(this.pp0Color) },
        { binding: 3, resource: this.storageView(this.presentTex) },
      ],
    });

    this.W = w; this.H = h;
    this.grid = [Math.ceil(w / 8), Math.ceil(h / 8)];
    this.historyValid = false;
    this.prevBasis = null;
  }

  // ------------------------------------------------------------------------- params
  /**
   * Build the 17-row Params block. Everything the shaders read comes from here, so the row order is
   * the single place where JS and WGSL have to agree; it is written once and copied to the per-pass
   * buffers.
   */
  buildParams(o) {
    const a = new Float32Array(PARAM_FLOATS);
    const put = (row, x, y, z, w) => {
      const i = row * 4;
      a[i] = x; a[i + 1] = y; a[i + 2] = z; a[i + 3] = w === undefined ? 0 : w;
    };
    const cam = o.cam, prev = o.prev || o.cam;
    put(0, cam.origin[0], cam.origin[1], cam.origin[2], cam.tanHalf);
    put(1, cam.right[0], cam.right[1], cam.right[2], cam.aspect);
    put(2, cam.up[0], cam.up[1], cam.up[2], 1);
    put(3, cam.forward[0], cam.forward[1], cam.forward[2], o.maxBounces);
    put(4, this.W, this.H, o.reset ? 1 : 0, o.time);
    put(5, o.exposure, o.frameIndex, o.radianceClamp, o.skyMode);
    put(6, o.samplesForDisplay, o.spp);
    put(7, o.skyColor[0], o.skyColor[1], o.skyColor[2], o.featureWrites ? 1 : 0);
    put(8, o.debugView || 0, o.historyCap, o.prevValid ? 1 : 0, o.denoise ? 1 : 0);
    put(9, prev.origin[0], prev.origin[1], prev.origin[2], prev.tanHalf);
    put(10, prev.right[0], prev.right[1], prev.right[2], prev.aspect);
    put(11, prev.up[0], prev.up[1], prev.up[2], 1);
    put(12, prev.forward[0], prev.forward[1], prev.forward[2], prev.valid ? 1 : 0);
    put(13, this.root, this.occlRoot, this.triCount);
    put(14, o.sigmaZ, o.sigmaN, o.sigmaL);
    put(15, o.checkPixels || 0);
    put(16, 0, 0, 0, 0);
    return a;
  }

  writeParams(o) {
    const base = this.buildParams(o);
    this._lastBase = base;
    this.device.queue.writeBuffer(this.paramsMain, 0, base);
    const pres = new Float32Array(base);
    const i16 = 16 * 4;
    pres[i16] = o.dither ? 1 : 0;
    pres[i16 + 1] = o.presentMode || 0;
    this.device.queue.writeBuffer(this.paramsPresent, 0, pres);
    const steps = [1, 2, 4, 8, 16];
    for (let it = 0; it < 5; ++it) {
      const p = new Float32Array(base);
      p[i16] = steps[it];
      p[i16 + 1] = it === 0 ? 1 : 0;
      p[i16 + 2] = it === 4 ? 1 : 0;
      this.device.queue.writeBuffer(this.paramsAtrous[it], 0, p);
    }
  }

  /**
   * An extra a-trous bind group with an arbitrary (step, first, last) and a chosen src/dst pair, for
   * the closed-form kernel check. The production chain uses the five built in resize().
   * Names: 'src' is the temporal output pair, 'dst0'/'dst1' the ping-pong scratch pairs.
   */
  atrousBindFor({ step, first, last }, srcName, dstName, label = 'atrous_probe') {
    const d = this.device;
    const ub = d.createBuffer({
      label: `params_${label}`,
      size: PARAM_FLOATS * 4, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
    });
    const p = new Float32Array(this._lastBase);
    p[16 * 4] = step;
    p[16 * 4 + 1] = first ? 1 : 0;
    p[16 * 4 + 2] = last ? 1 : 0;
    d.queue.writeBuffer(ub, 0, p);
    const src = this.atrousPair(srcName), dst = this.atrousPair(dstName);
    const bind = d.createBindGroup({
      label: `bind_${label}`,
      layout: this.atrousLayout,
      entries: [
        { binding: 0, resource: { buffer: ub } },
        { binding: 1, resource: this.sampledView(src[0], `${label}_src`) },
        { binding: 2, resource: this.sampledView(src[1], `${label}_src`) },
        { binding: 3, resource: this.sampledView(this.gAlb) },
        { binding: 4, resource: this.sampledView(this.gNrm) },
        { binding: 5, resource: this.sampledView(this.gDep) },
        { binding: 6, resource: this.storageView(dst[0], `${label}_dst`) },
        { binding: 7, resource: this.storageView(dst[1], `${label}_dst`) },
      ],
    });
    return { bind, params: ub };
  }

  atrousPair(name) {
    return {
      src: [this.tmpColor, this.tmpMom],
      dst0: [this.pp0Color, this.pp0Var],
      dst1: [this.pp1Color, this.pp1Var],
    }[name];
  }

  // ------------------------------------------------------------------------- encoders
  #dispatch(enc, pipe, bind, grid, label) {
    const pass = enc.beginComputePass({ label });
    pass.setPipeline(pipe);
    if (bind) pass.setBindGroup(0, bind);
    pass.dispatchWorkgroups(grid[0], grid[1]);
    pass.end();
  }

  /**
   * The trace pass. With `accum` it writes the current slot of the ping-pong pair, copies it to the
   * texture every downstream pass reads, and flips which slot is current - that is what a real
   * accumulation frame does. Without it, the pass is dispatched against a fixed pair so a timing run
   * measures exactly one dispatch and nothing else.
   */
  encodeTrace(enc, pipe = this.trace.full, { accum = false } = {}) {
    if (!accum) {
      this.#dispatch(enc, pipe, this.traceBind, this.grid, 'trace');
      return;
    }
    const cur = this.hdrCur ^ 1;
    this.#dispatch(enc, pipe, this.traceBindPair[cur], this.grid, 'trace');
    // Flip before publishing: encodeAccumCopy copies whichever slot is marked current, so flipping
    // afterwards publishes the slot this frame did not write. Forty frames into an accumulation the
    // picture is one frame stale and looks completely normal; it only shows up when a single-frame
    // debug dump is compared against a reference, which is exactly how it was found here.
    this.hdrCur = cur;
    this.encodeAccumCopy(enc);
  }

  /** accum[written] -> accum[displayed]: the copy the read_write format restriction costs. */
  encodeAccumCopy(enc) {
    enc.copyTextureToTexture({ texture: this.hdrPair[this.hdrCur] }, { texture: this.hdr },
      [this.W, this.H]);
  }

  encodeTemporal(enc, pipe = this.temporal.real) {
    this.#dispatch(enc, pipe, this.temporalBind, this.grid, 'temporal');
    // The filtered state becomes this slot's history for the next frame. A copy rather than a
    // ping-pong, so one bind group serves every frame - the native app does the same with three
    // vkCmdCopyImage calls right after the temporal dispatch.
    for (const [src, dst] of [[this.tmpColor, this.histColor], [this.tmpMom, this.histMom],
      [this.tmpDep, this.histDep]]) {
      enc.copyTextureToTexture({ texture: src }, { texture: dst }, [this.W, this.H]);
    }
  }

  encodeAtrous(enc, pipe = this.atrous.real) {
    for (let it = 0; it < 5; ++it) {
      this.#dispatch(enc, pipe, this.atrousBind[it], this.grid, `atrous_${it}`);
    }
  }

  encodePresent(enc) {
    this.#dispatch(enc, this.present, this.presentBind, this.grid, 'present');
  }

  /** A dispatch of a kernel that does nothing, at the grid the caller names. */
  emptyDispatch(enc, pipe, grid) {
    this.#dispatch(enc, pipe, null, grid, 'empty');
  }

  /** One frame the way a shipping renderer would do it: every dispatch in one submission. */
  encodeFullFrame(enc, { denoise, tracePipe, atrousPipe, temporalPipe, present = true, accum = true }) {
    this.encodeTrace(enc, tracePipe || this.trace.full, { accum });
    if (denoise) {
      this.encodeTemporal(enc, temporalPipe);
      this.encodeAtrous(enc, atrousPipe);
    }
    if (present) this.encodePresent(enc);
  }

  // ------------------------------------------------------------------------- timing
  /**
   * Time one pass in isolation: performance.now() around a submission that contains exactly `iters`
   * dispatches of it, then wait for the queue. Returns ms per dispatch, median and minimum over rounds.
   *
   * `iters` back-to-back dispatches share a single round trip, which is what makes the fixed cost per
   * submission amortise instead of dominating. It also means a pass that writes the same texture it
   * reads accumulates across the iterations - harmless for these kernels (an idempotent smoothing or a
   * clamped tonemap), and it is why the calibration dispatches are timed with the same `iters`.
   */
  async timePass(encode, iters, rounds, capMs) {
    const d = this.device;
    const warm = d.createCommandEncoder();
    for (let i = 0; i < iters; ++i) encode(warm, i);
    d.queue.submit([warm.finish()]);
    await d.queue.onSubmittedWorkDone();

    if (capMs) {
      const enc = d.createCommandEncoder();
      encode(enc, 0);
      const t0 = performance.now();
      d.queue.submit([enc.finish()]);
      await d.queue.onSubmittedWorkDone();
      const single = performance.now() - t0;
      if (single > capMs) return { med: single, min: single, rounds: [single], capped: true };
    }

    const samples = [];
    for (let r = 0; r < rounds; ++r) {
      const enc = d.createCommandEncoder();
      for (let i = 0; i < iters; ++i) encode(enc, i);
      const t0 = performance.now();
      d.queue.submit([enc.finish()]);
      await d.queue.onSubmittedWorkDone();
      samples.push((performance.now() - t0) / iters);
    }
    const sorted = samples.slice().sort((x, y) => x - y);
    return { med: sorted[sorted.length >> 1], min: sorted[0], rounds: samples, capped: false };
  }

  /** Time a whole submission (all passes at once) - the number that describes a real frame. */
  async timeSubmission(encode, rounds) {
    const d = this.device;
    const warm = d.createCommandEncoder();
    encode(warm);
    d.queue.submit([warm.finish()]);
    await d.queue.onSubmittedWorkDone();
    const samples = [];
    for (let r = 0; r < rounds; ++r) {
      const enc = d.createCommandEncoder();
      encode(enc);
      const t0 = performance.now();
      d.queue.submit([enc.finish()]);
      await d.queue.onSubmittedWorkDone();
      samples.push(performance.now() - t0);
    }
    const sorted = samples.slice().sort((x, y) => x - y);
    return { med: sorted[sorted.length >> 1], min: sorted[0], rounds: samples };
  }

  // ------------------------------------------------------------------------- readback
  async readTexture(tex, { bytes = 4 } = {}) {
    const d = this.device;
    const bpr = align(this.W * (bytes === 8 ? 8 : 4), 256);
    const buf = d.createBuffer({
      label: `buf_readback_${tex.label || 'tex'}`,
      size: bpr * this.H, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ,
    });
    const enc = d.createCommandEncoder({ label: `enc_readback_${tex.label || 'tex'}` });
    enc.copyTextureToBuffer({ texture: tex },
      { buffer: buf, bytesPerRow: bpr, rowsPerImage: this.H }, { width: this.W, height: this.H });
    d.queue.submit([enc.finish()]);
    await buf.mapAsync(GPUMapMode.READ);
    const out = new Uint8Array(buf.getMappedRange().slice(0));
    buf.unmap();
    buf.destroy();
    return { bytes: out, bytesPerRow: bpr };
  }

  /**
   * Copy the live <canvas> back into a texture and read it, so "the canvas is not black" can be
   * answered from the canvas itself and not only from the texture the blit samples. Returns null when
   * the browser refuses (copyExternalImageToTexture from a webgpu canvas is the part of this that is
   * least uniformly implemented); the caller then says so instead of guessing.
   */
  async readCanvasPixels(canvas) {
    const d = this.device;
    const w = canvas.width, h = canvas.height;
    const usage = GPUTextureUsage.COPY_DST | GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC;
    const t = d.createTexture({ label: 'canvas_copy', size: [w, h], format: 'rgba8unorm', usage });
    try {
      d.queue.copyExternalImageToTexture({ source: canvas }, { texture: t }, [w, h]);
      const bpr = align(w * 4, 256);
      const buf = d.createBuffer({
        label: 'buf_canvas_readback', size: bpr * h,
        usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ,
      });
      const enc = d.createCommandEncoder({ label: 'enc_canvas_readback' });
      enc.copyTextureToBuffer({ texture: t }, { buffer: buf, bytesPerRow: bpr, rowsPerImage: h },
        { width: w, height: h });
      d.queue.submit([enc.finish()]);
      await buf.mapAsync(GPUMapMode.READ);
      const raw = new Uint8Array(buf.getMappedRange().slice(0));
      buf.unmap(); buf.destroy();
      const tight = new Uint8Array(w * h * 4);
      if (bpr === w * 4) tight.set(raw.subarray(0, tight.length));
      else for (let y = 0; y < h; ++y) tight.set(raw.subarray(y * bpr, y * bpr + w * 4), y * w * 4);
      return { rgba: tight, w, h };
    } finally {
      t.destroy();
    }
  }

  /** RGBA8 bytes of the presentation target, in top-row-first order (what a PNG wants). */
  async readPresent() {
    const { bytes, bytesPerRow } = await this.readTexture(this.presentTex, { bytes: 4 });
    if (bytesPerRow === this.W * 4) return bytes;
    const tight = new Uint8Array(this.W * this.H * 4);
    for (let y = 0; y < this.H; ++y) {
      tight.set(bytes.subarray(y * bytesPerRow, y * bytesPerRow + this.W * 4), y * this.W * 4);
    }
    return tight;
  }

  async readHalf(tex) {
    const { bytes, bytesPerRow } = await this.readTexture(tex, { bytes: 8 });
    const out = new Uint16Array(this.W * this.H * 4);
    const src = new Uint16Array(bytes.buffer);
    for (let y = 0; y < this.H; ++y) {
      const s = (y * bytesPerRow) / 2, d0 = y * this.W * 4;
      for (let x = 0; x < this.W * 4; ++x) out[d0 + x] = src[s + x];
    }
    return out;
  }

  async readCheck() {
    const d = this.device;
    const n = 16384;
    const buf = d.createBuffer({
      label: 'buf_check_readback',
      size: n, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ,
    });
    const enc = d.createCommandEncoder({ label: 'enc_check_readback' });
    enc.copyBufferToBuffer(this.checkBuf, 0, buf, 0, n);
    d.queue.submit([enc.finish()]);
    await buf.mapAsync(GPUMapMode.READ);
    const out = new Float32Array(buf.getMappedRange().slice(0));
    buf.unmap(); buf.destroy();
    return out;
  }

  /** Pixels the check pass should re-trace, written into the input half of checkBuf. */
  writeCheckPixels(list) {
    // checkBuf is read by the shader as array<vec4<f32>>, so a pixel coordinate is one vec4 element:
    // eight floats apart, not two. The results start at element 256 (float 1024), which caps the list
    // at (4096-1024)/8 = 384 pixels; the demo asks for 64.
    const f = new Float32Array(4096);
    const n = Math.min(list.length, 384);
    for (let i = 0; i < n; ++i) { f[i * 8] = list[i][0]; f[i * 8 + 1] = list[i][1]; }
    this.device.queue.writeBuffer(this.checkBuf, 0, f, 0, 4096);
  }

  async readCounters() {
    const d = this.device;
    const buf = d.createBuffer({
      label: 'buf_counter_readback', size: 64,
      usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ,
    });
    const enc = d.createCommandEncoder({ label: 'enc_counter_readback' });
    enc.copyBufferToBuffer(this.counterBuf, 0, buf, 0, 64);
    d.queue.submit([enc.finish()]);
    await buf.mapAsync(GPUMapMode.READ);
    const out = new Uint32Array(buf.getMappedRange().slice(0));
    buf.unmap(); buf.destroy();
    return out;
  }

  async zeroCounters() {
    this.device.queue.writeBuffer(this.counterBuf, 0, new Uint32Array(16));
  }

  /** Run the instrumented trace once and report what it says it did. */
  async measureWorkload(pipe = this.trace.instrumented) {
    await this.zeroCounters();
    const enc = this.device.createCommandEncoder();
    this.encodeTrace(enc, pipe);
    this.device.queue.submit([enc.finish()]);
    await this.device.queue.onSubmittedWorkDone();
    const c = await this.readCounters();
    const pixels = this.W * this.H;
    return {
      pixelsCounted: c[0], primaryHits: c[1], shadingRays: c[2], occRays: c[3], occHits: c[4],
      hitFraction: c[0] ? c[1] / Math.min(c[0], pixels) : 0,
      raysPerPixel: c[0] ? c[2] / Math.min(c[0], pixels) : 0,
      occHitFraction: c[3] ? c[4] / c[3] : 0,
    };
  }

  /**
   * Blit the presentation texture to the canvas: one full-screen triangle, no second colour pipeline.
   * Size-independent on purpose - the presentation texture is whatever the current render scale made
   * it (renderer.resize), the canvas is the canvas, and the two differ as soon as the adaptive
   * controller trades resolution for framerate.
   */
  drawToCanvas() {
    const d = this.device;
    if (!this.blitPipe) {
      const module = d.createShaderModule({
        label: 'shader_blit',
        code: `
@group(0) @binding(0) var img : texture_2d<f32>;
@group(0) @binding(1) var samp : sampler;

struct Out {
  @builtin(position) pos : vec4<f32>,
  @location(0) uv : vec2<f32>,
};

@vertex
fn vs(@builtin(vertex_index) i : u32) -> Out {
  var p = array<vec2<f32>, 3>(vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
  let q = p[i];
  // uv comes from the clip-space corner, so it spans exactly 0..1 across the viewport whatever the
  // destination is measured in, and it is the *drawn* rectangle that defines the range - not the
  // source. (1 - q.y) / 2 puts uv.y = 0 at the top of the framebuffer, which is the convention the
  // presentation texture is written in (present.wgsl: "y = 0 is the TOP of the image").
  return Out(vec4<f32>(q, 0.0, 1.0), vec2<f32>((q.x + 1.0) * 0.5, (1.0 - q.y) * 0.5));
}

@fragment
fn fs(@location(0) uv : vec2<f32>) -> @location(0) vec4<f32> {
  // NOT 'framebuffer_xy / textureDimensions(img)'. That divides canvas coordinates by the *source*
  // dimensions, so it is only a 1:1 copy while the render scale is 1: once the adaptive controller
  // drops it (measured: 'frame 42: scale -> 0.5', a 1280x720 canvas sampling a 640x360 source) the
  // coordinates run to 2.0, clamp-to-edge returns the last column and the last row, and the presented
  // frame becomes one correct quadrant plus a replicated edge smeared over the rest - 50.0 % of the
  // canvas columns byte-identical to their neighbour, right half's horizontal gradient exactly zero.
  // Zero validation errors, and every "is there a picture in here" statistic still passes on it. See
  // docs/web-port-notes.md section 11.
  return textureSampleLevel(img, samp, uv, 0.0);
}
`,
      });
      this.blitLayout = d.createBindGroupLayout({
        label: 'bgl_blit',
        entries: [
          { binding: 0, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'float' } },
          { binding: 1, visibility: GPUShaderStage.FRAGMENT, sampler: { type: 'filtering' } },
        ],
      });
      this.blitPipe = d.createRenderPipeline({
        label: 'pipe_blit',
        layout: d.createPipelineLayout({ bindGroupLayouts: [this.blitLayout], label: 'layout_blit' }),
        vertex: { module, entryPoint: 'vs' },
        fragment: { module, entryPoint: 'fs', targets: [{ format: this.ctxformat }] },
        primitive: { topology: 'triangle-list' },
      });
      // NEAREST, and still deliberately - but note what that means now that the sizes can differ. At
      // scale 1 this remains a bit-exact 1:1 copy, which is what the still path's pixel comparison
      // depends on. Below scale 1 it is an honest blocky magnification: the picture really was rendered
      // at 640x360, and showing those pixels rather than inventing intermediate ones is the truth about
      // what the controller traded away (and matches the page's own `image-rendering: pixelated`).
      // Linear would also be correct here, and is not chosen partly because the same sampler slot has
      // to stay valid for a half-float source, where linear filtering needs a filterable-float path the
      // demo does not have. Measured, not assumed: demo.js runs checks.floatFilterProbe() and prints
      // whether rgba16float samples with a linear filter on this adapter.
      this.blitSampler = d.createSampler({ label: 'sampler_blit_nearest', magFilter: 'nearest', minFilter: 'nearest' });
    }
    // Keyed on the texture's identity, not on W: resize() rebuilds presentTex, and a bind group holding
    // a view of the destroyed one is only reported when the next frame draws.
    if (!this.blitBind || this.blitBindSrc !== this.presentTex) {
      if (this.blitBind && typeof this.blitBind.destroy === 'function') this.blitBind.destroy();
      this.blitBind = d.createBindGroup({
        label: 'bind_blit',
        layout: this.blitLayout,
        entries: [
          { binding: 0, resource: this.sampledView(this.presentTex, 'blit_source_present') },
          { binding: 1, resource: this.blitSampler },
        ],
      });
      this.blitBindSrc = this.presentTex;
    }
    const enc = d.createCommandEncoder({ label: 'enc_blit' });
    const tex = this.ctx.getCurrentTexture();
    const pass = enc.beginRenderPass({
      label: 'pass_blit',
      colorAttachments: [{
        view: tex.createView({ label: 'canvas_target' }), clearValue: { r: 0, g: 0, b: 0, a: 1 },
        loadOp: 'clear', storeOp: 'store',
      }],
    });
    pass.setPipeline(this.blitPipe);
    pass.setBindGroup(0, this.blitBind);
    pass.draw(3);
    pass.end();
    d.queue.submit([enc.finish()]);
  }

  destroy() {
    for (const t of [this.hdr, ...(this.hdrPair || []), this.gAlb, this.gNrm, this.gDep, this.tmpColor, this.tmpMom, this.tmpDep,
      this.histColor, this.histMom, this.histDep, this.pp0Color, this.pp0Var, this.pp1Color,
      this.pp1Var, this.presentTex, this.tex0, this.tex1]) if (t) t.destroy();
    const softDestroy = (o) => { if (o && typeof o.destroy === 'function') o.destroy(); };
    for (const b of new Set([this.traceBind, ...(this.traceBindPair || []), this.temporalBind,
      ...(this.atrousBind || []), this.presentBind, this.blitBind, this.emptyBind])) softDestroy(b);
    for (const b of [this.matBuf, this.lightBuf, this.instBuf, this.nodeBuf, this.triBuf,
      this.checkBuf, this.counterBuf]) if (b) b.destroy();
  }
}

function mkFallbackBitmap() {
  const c = new OffscreenCanvas(1, 1);
  const g = c.getContext('2d');
  g.fillStyle = '#ffffff';
  g.fillRect(0, 0, 1, 1);
  return c;
}
