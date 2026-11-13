# Vulkan Ray Tracer — Design Specification

Target platform: Windows 11 x64. Deliverable: **one self-contained `.exe`** + **one `.pak` asset archive**.

---

## 1. Requirements (as given)

| # | Requirement | Resolution |
|---|---|---|
| R1 | Single self-contained Windows executable | `raytracer.exe`, statically linked CRT (`/MT`), no third-party DLLs. Only OS + the Vulkan loader (installed with the GPU driver) are imported. |
| R2 | Single `.pak` for later assets (images/audio) | `scene.pak` = uncompressed/deflated ZIP read natively by the app; supports adding images, audio, config, or replacement shaders later with no rebuild. |
| R3 | Math on the GPU, not a software renderer | Vulkan ray-tracing pipeline (RT cores) + a compute-shader fallback. No CPU ray casting anywhere. |
| R4 | Open standards; no NVIDIA-specific RTX | `VK_KHR_ray_tracing_pipeline` / `VK_KHR_acceleration_structure` / `VK_KHR_ray_query` — Khronos cross-vendor standards. No OptiX, no `VK_NV_ray_tracing`, no DXR. Runs unmodified on AMD/Intel Arc. |
| R5 | Don't reinvent wheels | GLFW (windowing), volk (Vulkan loader/function pointer resolution), glslang (GLSL→SPIR-V), stb (PNG writing), zlib/deflate (inside the pak reader). |
| R6 | Cube, cone, spheres, varied reflectivity | See §6. |
| R7 | A couple of light sources | Two animated emissive light spheres + emissive materials. |
| R8 | Full ray tracing through a camera circling the scene | Progressive path tracer, camera on an animated orbit; no user input required. |
| R9 | ESC exits | Raw-key + window-close handling through GLFW. |
| R10 | Freely resizable via standard Windows mechanisms | Vulkan swapchain recreation on `VK_ERROR_OUT_OF_DATE_KHR` / `VK_SUBOPTIMAL_KHR` + GLFW framebuffer-size callback. |
| R11 | One-shot finished product | Headless verification mode (`--capture`) so correctness is provable without a human watching the window. |

## 2. Machine profile this was designed against

Verified on this host (not assumed):

* GPU0 **NVIDIA RTX PRO 5000 Blackwell**, Vulkan 1.4.329, driver 596.71, conformance 1.4.3.3 → full ray tracing.
  Reported RT properties: `shaderGroupHandleSize=32`, `shaderGroupHandleAlignment=32`,
  `shaderGroupBaseAlignment=64`, `maxRayRecursionDepth=31`, `maxRayHitAttributeSize=32`.
* GPU1 **Intel UHD Graphics 770**, Vulkan 1.3.295 — **no** `VK_KHR_ray_tracing_pipeline`
  (Xe-LP iGPU is DXR-only). Device selection therefore prefers `DISCRETE_GPU` and the app
  keeps a compute-shader fallback so the binary never hard-fails on a non-RT GPU.
* Vulkan loader `vulkan-1.dll` 1.4.341 present; no Vulkan SDK installed → the app resolves
  every entry point at runtime through volk and needs **no** `vulkan-1.lib`.
* Toolchain: VS Build Tools (MSVC 14.51), CMake + Ninja shipped inside VS, Windows SDK 10.0.26100.
* Shaders: official prebuilt **glslang 16.6.0** CLI (`tools/glslang/bin/glslang.exe`) — the
  ~1 GB Vulkan SDK installer is not needed.

## 3. Architecture

```
                    ┌──────────────────────────────────────────────┐
  GLFW window ──────│ raytracer.exe  (/MT, single binary)          │
                    │                                              │
  scene.pak ────────│ Pak ──► SceneConfig ──► CPU mesh build ──┐   │
  (ZIP: stored/     │ (ZIP reader)   (scene.json)               │   │
   deflated)        │                                          ▼   │
                    │  volk ──► Vulkan loader ──► BLAS/TLAS build   │
                    │                  │                │           │
                    │                  ▼                ▼           │
                    │        RT pipeline ──► HDR accumulation image │
                    │        (raygen/chit/miss/callable) │          │
                    │                                    ▼          │
                    │        present pipeline ──► ACES tonemap ──► swapchain
                    └──────────────────────────────────────────────┘
```

Two Vulkan pipelines:

1. **Ray-tracing pipeline** (`VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR`): raygen + closest-hit
   groups + miss + callable. Traces one camera ray per pixel, plus shadow/reflection bounces,
   into an offscreen `R16G16B16A16_SFLOAT` accumulation image (additive, progressive).
2. **Graphics pipeline**: one-triangle fullscreen draw that divides by the accumulated sample
   count, applies exposure + ACES-ish filmic tonemap + sRGB encode, and writes the back buffer.

Rationale for an offscreen HDR target rather than tracing straight into the swapchain image:
it enables progressive accumulation across frames, decouples resize from the trace resolution
(adaptive scale under load), and allows the identical shader to render headless PNG captures.

## 4. Standards choices

* **API level**: instance created with `apiVersion = VK_API_VERSION_1_3`. Vulkan 1.3 promoted the
  ray-tracing work *without renaming it*, so `_KHR` entry points/types/sTypes are used throughout
  (there are no unsuffixed aliases). RT features are enabled by chaining
  `VkPhysicalDeviceAccelerationStructureFeaturesKHR`,
  `VkPhysicalDeviceRayTracingPipelineFeaturesKHR`, `VkPhysicalDeviceRayQueryFeaturesKHR` and
  `VkPhysicalDeviceVulkan12Features{bufferDeviceAddress, scalarBlockLayout, descriptorIndexing}`.
  **Corrected during implementation:** the earlier note here said the promoted extensions must *not*
  be passed to `vkCreateDevice`. That is wrong for this API family. Because the promoted commands
  keep their `_KHR` spelling, the loader hands out NULL function pointers for them unless the
  extension is enabled on the device — observed directly: every RT entry point came back `0` with a
  1.3 device and no extensions, and the app faulted on the first call. `vkCreateDevice` therefore
  enables `VK_KHR_acceleration_structure`, `VK_KHR_ray_tracing_pipeline`, `VK_KHR_ray_query`,
  `VK_KHR_deferred_host_operations` and `VK_KHR_pipeline_library`, filtered against what the
  adapter actually advertises.
* **No ray-tracing queue flag exists.** `VK_KHR_ray_tracing_pipeline` deliberately did not carry over
  `VK_QUEUE_RAY_TRACING_BIT_NV`, so the queue family is chosen on `GRAPHICS | COMPUTE` only (that
  constant is not even present in Vulkan-Headers 1.4).
* **Shading language**: GLSL 460 → SPIR-V via `#extension GL_EXT_ray_tracing`,
  `GL_EXT_ray_query`, `GL_EXT_scalar_block_layout`, `GL_EXT_nonuniform_qualifier`;
  compiled with `-V --target-env vulkan1.3` (RT needs SPIR-V ≥ 1.4 for `RayTracingKHR` /
  `ShaderCallabilityKHR`; glslang does not raise the version by itself).
  **Gotcha recorded during validation:** in GLSL, `traceRayEXT`'s final parameter is the payload
  *location number* (`int`), not the payload variable, and it takes **no** `sbtRecordStride`
  argument (unlike OptiX/DXR). `executeCallableEXT(sbtRecordIndex, payloadLocation)`.
  Callable stages take `callableDataInEXT`/`callableDataEXT` and may not declare ray payloads;
  `shaderRecordEXT` may only decorate a `buffer` block (never `uniform`), one per stage.
* **Formats**: present `B8G8R8A8_SRGB` (fallback `R8G8B8A8_SRGB`, then UNORM variants);
  HDR working space `R16G16B16A16_SFLOAT`.
* **Geometry data**: triangle meshes in device buffers + `VkAccelerationStructureInstanceKHR`
  TLAS instances, i.e. the standard hardware path — not a bespoke SDF traversal.

## 5. Acceleration structure strategy

* One **BLAS per object type** (sphere, cube, cone, ground disc, light sphere), built once.
* One **TLAS** with one instance per object, rebuilt every frame on the GPU
  (`VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR`, `PREFER_FAST_BUILD`). Instance transforms
  come from a device-side instance buffer that is updated by a `VkBufferCopy` from a staging
  upload, so the orbiting lights cost one small copy + one TLAS rebuild per frame.
* Vertices are `float3` position + `float3` normal, indexed `uint32`; non-uniform vertex
  component alignment issues are avoided by the 24-byte stride.
* Scratch: one persistent buffer sized for `max(BLAS buildScratchSize, TLAS buildScratchSize)`;
  its device address is aligned to
  `VkPhysicalDeviceAccelerationStructurePropertiesKHR::minAccelerationStructureScratchOffsetAlignment`.
* Barriers (sync2): upload copy → `ACCELERATION_STRUCTURE_BUILD` with `SHADER_READ`;
  build → `RAY_TRACING_SHADER` with `ACCELERATION_STRUCTURE_WRITE → READ`; plus the WAR
  barrier before the next frame's rebuild.
* **Shader binding table**: single buffer, four regions each based at a multiple of
  `shaderGroupBaseAlignment` (64) with stride 64 (multiple of 16). Hit-group records carry a
  4-byte material index — the standard way to give every object its own BRDF while sharing one
  compiled closest-hit shader.

## 6. Scene

Objects (all instanced from shared BLASes):

| Object | Geometry | Material |
|---|---|---|
| Ground | radius-9 disc | rough dielectric, albedo (0.55), IOR 1.4, low specular |
| Mirror sphere | UV sphere r=1.15 @ (-1.6, 1.15, 0.3) | metal, reflectivity 0.95, roughness 0.02 |
| Chrome-ish sphere | UV sphere r=0.85 @ (1.7, 0.85, -1.5) | metal, reflectivity 0.7, roughness 0.12 |
| Diffuse sphere | UV sphere r=0.8 @ (3.0, 0.8, 1.4) | lambertian albedo (0.82,0.24,0.18) |
| Glass sphere | UV sphere r=0.95 @ (-0.1, 0.95, 2.3) | dielectric, IOR 1.52 |
| Mirror cube | cube s=1.5 @ (1.1, 0.78, -0.2), yaw 24° | metal, reflectivity 0.85, roughness 0.05 |
| Diffuse cone | cone r=0.8 h=1.9 @ (-2.8, 0, -1.8) | lambertian albedo (0.20,0.55,0.45) |
| Light A | emissive sphere r=0.32 | orbit r=3.9, y=2.5, ω=0.55 rad/s |
| Light B | emissive sphere r=0.26 | orbit r=4.6, y=1.6, ω=-0.40 rad/s |

* Two emissive light bodies are real geometry, so they show up in reflections and in the
  background; they are also sampled directly for next-event estimation (shadow rays).
* Material model per reflectivity class: Lambertian diffuse, GGX microfacet metal
  (Fresnel-weighted, roughness-controlled), and dielectric refraction with Fresnel reflection —
  Schlick approximation, total internal reflection handled.
* Camera: orbits at radius 11.5, height 3.4 + 0.7·sin(0.23 t), looking at (0, 0.9, 0),
  42° vertical FOV, 16:9-independent (aspect follows the window).
* Renderer: progressive, 1 primary sample/pixel/frame, up to 6 bounces, cosine-weighted
  hemisphere sampling, explicit shadow ray per light per bounce, Russian-roulette-free hard cap,
  PCG hash RNG seeded per pixel per frame. Convergence resets on resize/anim jump so motion
  stays sharp.

## 7. Asset archive (`.pak`)

A ZIP archive read directly from a memory-mapped file:

1. Scan the last 66 KB for the **End Of Central Directory** signature `0x06054b50`.
2. Walk the central directory (signature `0x02014b50`) into a name → {method, offset, size, CRC32} index.
3. `read(name)` seeks the local header (`0x04034b50`), skips the stored/deflate payload, and
   verifies CRC32. Method 0 = stored, method 8 = raw deflate (own inflate implementation, no zlib
   dependency in the exe).

Search order next to the executable: `--pak <file>`, `scene.pak`, `assets.pak`, `*.pak`.

**What the pak actually controls today** (i.e. it is not decorative):

* `scene.json` — camera radius/height/speed/FOV, exposure, background colour, max bounces,
  and the full material table (albedo, emissive, reflectivity, roughness, IOR). Malformed or
  missing → documented defaults, with a console warning.
* `README.txt` — documentation for whoever edits the archive.
* `textures/checker.png`, `textures/emissive_ramp.png` — decoded with a bundled image reader and
  uploaded as `R8G8B8A8_UNORM` sampled images bound at descriptor binding 5, so adding an image
  to the pak changes the render immediately.
* `shaders/<stage>.spv` — optional replacement SPIR-V, loaded verbatim instead of the embedded
  copy. This is the asset-driven hot-patch path, and it is why the shipped exe needs no shader
  compiler.
* `audio/*.ogg` — reserved slot; the archive format already supports it.

## 8. Windowing / presentation

* GLFW 3.4 built from source as a **static** lib, `GLFW_USE_HYBRID_HPG=ON` so the Optimus-style
  switchable GPU path prefers the discrete adapter; `GLFW_EXPOSE_NATIVE_WIN32` supplies the HWND
  for `vkCreateWin32SurfaceKHR`.
* Resizable (`GLFW_RESIZABLE_BIT`), standard Windows chrome, frame-rate capped by `vkAcquireNextImageKHR`
  (FIFO present = VSync, no tearing, no busy loop).
* ESC (`GLFW_KEY_ESCAPE` → press) or window close ends the loop; `glfwWaitEventsTimeout` keeps
  the process responsive while idle.
* Resize: mark swapchain dirty, skip the frame, recreate swapchain + HDR target at the new
  extent, reset accumulation. `VK_SUBOPTIMAL_KHR` also triggers recreation.
* Trace scale: `--scale` / `scene.json` choose the internal resolution (0.4-1.0); the tonemap pass
  upscales to the swap chain, so a slow GPU or a 4K drag-resize stays interactive. Automatic
  feedback-driven scale was *not* implemented — the measured 16 ms GPU budget at 1280x720 did not
  justify the extra state machine, so the knob is manual and documented.

## 9. Non-RT fallback

As built, there are exactly two backends and they share all shading code:

1. **Ray-tracing pipeline** (`VK_KHR_ray_tracing_pipeline`) — the default when the adapter supports
   it. Five shader groups: raygen, two miss records (scene background, shadow), two closest-hit
   records (geometry report, shadow).
2. **Compute + ray query** (`VK_KHR_ray_query`) — a compute shader that traces the *same device
   TLAS* with `rayQueryEXT` and runs the *same* `pathTrace()` from `pt_common.glsl`. Only the two
   host hooks differ (`traceShadingRay` / `occludedRay`).

The earlier draft of this spec promised a third path (an analytic sphere/cone/box intersect
compute tracer for hardware with no ray queries). That was dropped: it would have needed a second
copy of the lighting model to keep the two paths in sync, which is exactly the drift this design is
trying to avoid. Devices with neither extension are pre-Vulkan-1.2 and out of scope; the app prints
a clear diagnostic instead. `--compute` / `--rt` force either path.

The workgroup size of the fallback is `layout(local_size_x = 8, local_size_y = 8)` and must match the
host dispatch. Omitting `local_size` is *not* an error in GLSL — the size becomes implementation
defined (1x1x1 in practice), which silently rendered only the bottom-left 1/144th of the image.

## 10. Verification without a human

`--capture N --accum S --size WxH --out DIR` initialises the full device path with no window or
surface, traces S samples per still into the HDR target, reads it back, tone maps on the CPU
(same curve as the present shader) and writes PNGs. Exit code is non-zero on any Vulkan failure, and
`raytracer.log` beside the exe records adapter choice, per-stage sizes, texture loading and timings —
a GUI-subsystem binary has no console when double-clicked, so the log is the reliable channel.

`--autoexit S` runs the real windowed loop (swap chain, present, frame pacing) and closes itself, so
the interactive path is exercised by a script: 306 frames in 5 s, 61 fps, clean exit.

Both verification modes were run on both backends and the images were inspected rather than trusted:
that is how the camera-basis sign error, the transposed instance inverse, the missing sample-count
division in the readback, and the shadowed stale shader header were found (see §11a).

## 11a. Defects found by running it, kept as a checklist

| Symptom | Cause | Rule |
|---|---|---|
| RT entry points return NULL, first AS call faults | promoted-but-`_KHR`-named commands are gated on the device extension being *enabled* | enable them even on a 1.3 device |
| Image upside down and mirrored | `cross(f, up)` is `(-f.z, 0, f.x)`, not `(f.z, 0, -f.x)`; negating `right` also negates `up` | derive the basis once and check it against a known view |
| Only *rotated* objects shaded black | `A⁻¹ = adj(A)/det`, and `adj` is the **transpose** of the cofactor matrix; the error is invisible for axis-aligned instances | test the rotated case explicitly |
| Stalls in `vkGetAccelerationStructureBuildSizesKHR`-adjacent code | `VkAccelerationStructureBuildSizesInfoKHR.sType` left zero | zero-initialising a struct that has an `sType` is a bug, not a shortcut |
| Compute fallback painted a corner | missing `local_size` → 1x1x1 workgroup | always declare it; dispatch and size must agree |
| Captures blown out in proportion to sample count | the HDR target holds a running **sum**; the window divides in the shader, the readback path must divide too | one owner for the divisor, or none |
| Shader edits appeared to do nothing | a stale copy of the generated header in `src\generated` shadowed the fresh one in `build\generated` because `src` came first on the include path | CMake now refuses to configure if that file exists; generated headers live in the binary dir only |
| **Window black while `--capture` rendered fine and the title bar counted frames** | the render pass was created *before* the swap chain, so its colour attachment format was `VK_FORMAT_UNDEFINED`: the pass still cleared, but every draw into it was silently dropped. Headless capture never touches the render pass, so the whole test suite stayed green | create the swap chain first; `createRenderPass` now fails loudly if the format is still undefined; verify the **presented window** (`tools\window_probe.ps1`), not just the offscreen buffer |
| Window froze a second after starting and Windows closed it as unresponsive | the quality controller drained *both* frame fences before replacing the accumulation target, but the loop had already waited on and reset the current slot's fence, so it waited forever for a submit that had not happened yet. Dropping only that fence, and later clearing the other slot's too, deadlocked one iteration further on instead | wait on the other slot's fence and leave it signalled - the loop owns fence resets, and an already-signalled fence is exactly the truth after a successful wait |
| Quality hunted between adjacent settings, and frame rate sagged | the per-sample cost model was thrown away on every change, so each decision relearned from a single noisy reading, and one reading nudging past the budget line was enough to act on | keep the model across changes (it is per-sample and per-kilopixel by construction), hold inside a deadband, and require a streak of readings before acting |
| Brightness jumped - a flash - when quality changed | the accumulation target stored **radiance x samples**, and the present pass divided by the sample count of *the frame being presented*. Those are only the same frame under normal running, but a frame held over from a previous quality setting is not: after 12 samples dropped to 4, the held image was divided by 4 and came out three times too bright. Holding the previous frame to avoid a blank flash had therefore made a brightness flash instead | the sample mean is now taken where the samples are summed, so the image stores radiance and the present divides only by the number of accumulated frames. Verified with the invariant it should satisfy: the same scene at the same total sample count renders identically at 1 and 4 samples per frame (floor mean 144.0 vs 144.2) |
| Faint spiral / shadow-like band on the floor beneath each light orb | next-event estimation sampled a disc across the light and divided by a `cosLC` measured from the sampled point back toward the sphere's centre - the **zero vector** at the middle of the disc - then discarded any sample where that cosine came out small. The orbs are rendered with uniform radiance in every direction, so there is no light-side cosine to account for at all: the projected disc already carries the foreshortening. The fraction of samples thrown away varied with where on the floor you stood, which is a position-dependent bias, i.e. a ring | removed the cosine from the sampling density and the discard; the diffuse branch now applies the shading cosine explicitly, and the glossy branch carries none because it cancels against the BRDF's own 1/cosine |
| White flash whenever the renderer changed quality | changing quality ran two `vkQueueWaitIdle` calls (to transition and clear the new accumulation targets). A queue drain also waits for the pending present, so the window stopped pumping messages - and Windows paints a stalled window's client area **white**. Holding the previous image did nothing, because the blank was never the render target | one-time uploads submitted mid-change no longer wait (a single queue preserves order; the command buffers are freed at shutdown), and the pump runs around the change. Cost of a change: from a queue drain (tens of ms, vsync-coupled) to 1.2-3.6 ms measured |
| Bright bands on the floor that appeared to be cast by the light orbs | diffuse and glossy surfaces gather every light through next-event estimation, and the transport loop *also* collected an orb's emission whenever a bounce happened to land on it. The orbs are bright enough that the double count saturated, and it switched on and off wherever the shadow test said an orb was hidden behind an object - which is what drew the bands. Emissive was also the one term added before `radianceClamp`, which is why lowering the clamp 34x barely helped | a bounce that leaves a next-event-shaded surface does not collect emission again; metal and glass do no NEE, so they still show the orb, and the camera still sees the orbs on the first hit. Floor near-white pixels went from 0.97% to none, floor mean 139 -> 92 |
| Device lost, a frame after holding the previous image across a quality change | the retired image was freed when the slot's fence completed, but the frame that *displays* it is recorded after that wait, so it sampled a destroyed view | two-phase: mark the hold when a frame is recorded with it, free it only when that frame's own fence has passed, then repoint the descriptor at the live target |
| Stills showed several exposures blended, worse with more samples | between captured stills the HDR accumulator was only reset "in-shader" (`prev = reset ? 0 : imageLoad`); stale samples of earlier stills survived | each captured still starts from a freshly recreated, cleared accumulator (`recreateHdrTargets` + descriptor rewrite) |
| Framing wrong only when maximised, on a DPI-scaled display | **not resolved.** Measured: framebuffer, HDR and swap chain extents all agree (3840x2019), the same resolution rendered headlessly is correct and centred, so the fault is in the windowed present path at that size and is still open |

### How the black-window bug was found

The headless capture path and the window share the tracer but not the present pass, so a green
render-pass clear combined with a hard-coded fragment colour bisected it: the clear appeared, the
draw did not. `tools\window_probe.ps1` screenshots the real desktop while the app runs; that is the
only check that covers the swap chain end to end, and it is now part of the verification list.


## 11. Risks and mitigations

| Risk | Mitigation |
|---|---|
| Driver rejects `shaderRecordEXT` hit-group data | Fallback path: material index via `gl_InstanceCustomIndexEXT` (this is the path actually used: `(kind << 8) \| material`) |
| Sub-pixel motion smear from accumulation | Accumulation reset every frame while animating; temporal reset on resize |
| HDR target allocation churn during resize | Targets recreated only on confirmed extent change; recreate deferred to a frame boundary with fences drained |
| Two GPUs / wrong adapter | Adapters are scored on RT support then `DISCRETE_GPU`; both present adapters are logged at start-up |
| Firefly outliers from glossy grazing samples | Smith `G1` weight clamped to 3 in `sampleSpecular`, plus a radiance clamp (12.0) before accumulation |
| Missing CRT on a clean machine | `/MT` static CRT; link verified to import only OS DLLs |
| Asset archive absent or corrupt | Every loader degrades to compiled-in defaults and logs what it did; the render still happens |


## 12. Explicit assumptions made (no further input available)

1. RTX PRO 5000 Blackwell is the render adapter; the Intel iGPU is ignored unless forced.
2. "Fully ray traced" = path traced with reflection + refraction + shadow rays (not raster hybrid),
   1 spp per frame progressively accumulated at ~60 Hz.
3. VSync'd (FIFO) presentation; no frame limiter UI; no vsync toggle.
4. `scene.pak` is optional at runtime (defaults are compiled in) so the exe alone still runs.
5. Assets are authored outside the app and packed with the provided `tools/make_pak.py`.
6. Console output (stdout) is the only diagnostics channel; no dialogs.
