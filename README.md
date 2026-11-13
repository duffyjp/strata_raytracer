# Vulkan Ray Tracer - cube, cone, spheres, two lights, orbiting camera

A real-time GPU path tracer for Windows. Everything is rendered by tracing rays on the GPU with
Vulkan's standard ray-tracing extensions - no software rasterisation of the scene, no NVIDIA-only
APIs. The application is a single self-contained executable that loads its art from one `.pak`
(next, a plain ZIP file).

```
build\bin\raytracer.exe     <- the whole application, 745 KB, imports only OS DLLs
build\bin\scene.pak         <- scene description + textures, editable, no rebuild needed
```

![Path-traced scene](captures/frame_000_t0.55.png)

## Running it

Double-click `build\bin\raytracer.exe`, or from a terminal:

```powershell
build\bin\raytracer.exe
```

* The window is a standard resizable Windows window - drag any border or maximise it and the render
  target follows.
* **ESC quits.** **Alt+Enter toggles fullscreen** (on the monitor's own mode, returning the window to
  exactly where it was).
* **WASD takes the camera.** Any of those keys switches from the automated orbit to a free camera:
  the mouse looks, WASD walks in the horizontal plane, Shift moves faster. There is no vertical
  control. Take your hands off for **ten seconds** and the view eases back to the orbit over about a
  second and a half - taking control is immediate, handing it back is not meant to be noticeable.
  Mouse capture is released when control is handed back.
* The title bar shows the live frame rate, the window size, the size actually being traced, the
  samples per pixel and which backend is active - for example
  `1280x720 trace 2560x1440 - 12 spp`.
* **N toggles the denoiser**, which is **on by default**. The title bar states the state - 
  `(N) Denoise ON` or `(N) Denoise OFF` - so there is nothing to guess. Toggling rebuilds the
  filter's targets on the spot and drops its history, so the first denoised frame after a toggle is
  a fresh estimate; give it a second before judging. A press of N is also treated as final: the
  quality controller stops second-guessing it.
* **Stills are not denoised** unless `--denoise` is named explicitly. A still averages dozens of
  frames into one clean image, and the filter works by assuming each frame is independent and
  keeping its own history - the two are opposite jobs.
* **Quality adapts itself to the GPU** (see below), so it holds 60 fps instead of rendering cheaply.
  On the machine it was built on that means tracing 2560x1440 with 11-12 samples per pixel for a
  1280x720 window, using about 12 ms of each 16.7 ms frame.

### Adaptive quality

The loop measures the GPU's own opinion of how long a frame took (two timestamp queries bracket the
command buffer, which costs nothing and never stalls), then spends the frame budget on quality:
first samples per pixel, then supersampling above the window's own resolution, then samples again.
The measured cost is close to proportional to pixels x samples, so the next setting is a prediction
rather than a search. Details that matter in practice:

* **Headroom is deliberate.** By default it targets 74% of the frame (`qualityHeadroom` 0.26), which
  is why the frame rate is flat rather than occasionally dropping a frame.
* A CPU-side pace watchdog also watches the real interval between frames: if vsync is being missed
  even though the command buffer fits, quality steps back, because present latency and background
  processes are invisible inside a command buffer.
* Inside a deadband (72%-104% of budget) it changes nothing at all, and outside it the controller
  waits for a streak of readings before acting, so a single slow frame from an unrelated process
  cannot make the image flicker in quality.
* It reacts to a resize or maximise within a few frames, and to a scene becoming more expensive the
  same way, since both simply change the measured cost.
* The window's own resolution is protected: supersampling is capped by `maxRenderScale` and by
  `maxTracePixels`, so a 4K window cannot exhaust video memory. Below the window's size it will only
  go if a single sample per pixel cannot make the frame; then it shrinks the trace target instead of
  dropping frames.

Options (all optional):

| Option | Meaning |
| --- | --- |
| `--capture N` | render N stills headless, write PNGs, exit (this is how correctness is verified) |
| `--accum S` | samples accumulated per still, default 48 |
| `--size WxH` | capture resolution, default 1280x720 |
| `--out DIR` | capture output directory, default `captures` |
| `--fullscreen` | start in fullscreen; Alt+Enter toggles it at any time |
| `--quality MODE` | burden ceiling: `high` (2x supersampling), `balanced` (1.25x), `performance` (window size), `off` |
| `--no-vsync` | the default - present as soon as a frame is ready (mailbox, else immediate), with the loop paced to the panel rate by a high-resolution timer |
| `--vsync` | wait for the display before presenting (FIFO). Off by default: FIFO queues pending presents and delivers unevenly once the loop runs ahead of the panel |
| `--refresh N` | treat the panel as running at N Hz. Detected from the monitor at startup and logged |
| `--scale F` | baseline trace resolution 0.4-1.0; adaptive quality measures against this (1.0 = the window's own size) |
| `--no-auto-quality` | pin samples and scale instead of adapting to the GPU |
| `--target-fps N` | frame rate the controller aims for, default 60 |
| `--max-spp N` | most samples per pixel it may spend, default 16 |
| `--min-scale F` | smallest trace resolution it may fall back to, default 0.40 |
| `--compute` | force the compute + ray-query fallback instead of the ray-tracing pipeline |
| `--rt` | require the ray-tracing pipeline and fail if it is missing |
| `--pak FILE` | load a specific asset archive |
| `--denoise` | filter the image with SVGF. **On by default when rendering to the window**; press **N** at any time |
| `--no-denoise` | start without it (stills are never filtered unless `--denoise` is named) |
| `--gbuf NAME` | show a filter input or output instead of the render: `albedo`, `normal`, `depth`, `denoised`, `diff`, `variance` |
| `--filter-z F` | depth edge stopping, default 0.02 (reciprocal-depth units) |
| `--filter-n F` | normal edge stopping, default 128 |
| `--filter-l F` | luminance edge stopping, default 24 |
| `--autoexit S` | close the window by itself after S seconds (used by the automated smoke test) |
| `--dump-accum N` | during `--capture`, also write the raw float target after accumulation frame N
  (`-2` for every frame) as `<dir>\accum_NNN.bin`, for noise analysis |
| `--test-toggle N` | toggle the denoiser after N rendered frames - the same code path the N key uses |
| `--verbose` | extra logging, including a per-phase frame-time breakdown |
| `--help` | usage |


### Denoising

The renderer denoises by default, so one sample per pixel can be enough. It is SVGF
(Spatiotemporal Variance-Guided Filtering, Schied et al. 2017) written as ordinary GLSL compute passes:
one temporal pass that reprojects the previous frame into this one and keeps a capped running average,
then five a-trous wavelet iterations that widen 1, 2, 4, 8, 16 pixels, each weighted by how similar the
neighbour is in depth, normal and brightness, and by how noisy each pixel is. The filter runs on the GPU
like everything else, needs no third-party library, and ships inside the same executable.

What it costs and what it buys, measured on the machine it was built on (RTX PRO 5000, 1280x720,
error quoted as RMSE against a reference converged to 512 frames):

| one sample per pixel, frames spent | accumulating | with `--denoise` |
| --- | --- | --- |
| 1 frame | 22.45 | **10.29** |
| 4 frames | 16.14 | 10.03 |
| 16 frames | 11.23 | about 10 |

So one denoised frame is worth roughly eight accumulated ones, which is the whole point: a moving camera
cannot wait for sixteen frames. The GPU cost, measured with the same timestamps the quality controller
uses and averaged over steady-state frames in interleaved runs: **0.31 ms** tracing one sample per
pixel at 1280x720 without the filter, **1.10 ms** with it, so the filter costs about **0.8 ms** there.
It runs over trace pixels, so expect roughly four times that when the controller is supersampling at
2x - that figure is expected rather than measured.

It gives itself up when it has to. The frame budget is spent on resolution and samples first, and
only when both are already at their floor and frames are still arriving late does the controller
surrender the filter, because that is the one case where the fraction of a millisecond it costs is
worth more than the image quality it adds. Pressing N at any point overrides the controller
permanently. It is worth knowing, though, that the filter has two real limits. Past about one frame of budget the improvement stops -
one denoised frame and sixteen score the same - which means the remaining error is systematic (the
filter smooths detail away) rather than noise, and the fixes for that are a normal-history test and a
direct/indirect split, neither of which is implemented yet. And the weights are tuned on this scene:
the paper's own constants score 12.91 where these score 10.29, because a checkerboard floor has a flat
normal and a hard colour edge, so the geometric tests have almost nothing to work with and the
brightness term has to carry them. `--filter-z`, `--filter-n` and `--filter-l` let you retune for a
different scene; `--gbuf` shows every input the filter sees, plus what it changed (`diff`) and how noisy
it thinks it is (`variance`), which is how the weights were found.

Deviations from the paper, all deliberate: motion is handled by reprojecting with the previous frame's
camera basis rather than per-pixel motion vectors, because nothing in this scene moves except the
camera; direct and indirect light are not separated into two channels, so the variance estimate is
coarser than the paper's; and there is no second normal history, so a surface that swaps identity
between frames without changing depth can keep its history for one frame too long.
Everything it says also goes to `build\bin\raytracer.log`, because a GUI-subsystem program has no
console when launched from Explorer.

## Building it

Requirements: Windows, Visual Studio Build Tools (the C++ workload), Python 3. Nothing else to
install - CMake, Ninja, the Vulkan headers and glslang are either already inside Build Tools or
fetched into `tools\`.

```powershell
.\build.ps1              # configure, compile, pack the assets
.\build.ps1 -Capture     # ... and then render verification stills
.\build.ps1 -Clean       # throw away build\ first
```

What the build does:

1. Imports the MSVC environment from `vcvars64.bat` and uses the CMake and Ninja that ship with
   Build Tools.
2. Runs `tools\build_shaders.py`, which flattens the `#include` graph in `shaders\`, compiles all
   eight stages to SPIR-V with `tools\glslang\bin\glslang.exe`, and emits a C++ header. The SPIR-V is
   embedded in the executable, so the exe needs no shader files.
3. Builds GLFW as a static library against the same static CRT (`/MT`), links volk, and produces one
   exe with no runtime dependencies beyond the OS.
4. Runs `tools\make_pak.py`, which (re)generates the textures and `scene.json` and writes
   `build\bin\scene.pak`.

The link is verified to be self-contained:

```
exe imports: SHELL32.dll, KERNEL32.dll, USER32.dll, GDI32.dll
```

## How it renders

One path-traced sample per pixel per frame, accumulated in an RGBA16F image, tone-mapped (ACES) and
blitted to the swap chain by a full-screen triangle. The transport loop is iterative inside the ray
generation shader - closest-hit shaders only report geometry - so `maxPipelineRayRecursionDepth` is
2 and the same loop is reused verbatim by the compute fallback through a host hook pair
(`traceShadingRay` / `occludedRay`) in `shaders\pt_common.glsl`.

Materials are Lambert, GGX metal, dielectric glass with Fresnel-weighted reflect/refract, and
emissive. Lighting is next-event estimated against two disc-sampled area lights with shadow rays, so
shadows are soft and the light spheres are visible in mirrors. The ground albedo comes from a PNG in
the `.pak`; if the archive is missing, a procedural checker is substituted and the render still works.

`scene.pak` and every file inside it are documented in `assets\README.txt`, which is itself packed,
so the instructions travel with the executable. Editing `scene.json` changes lights, materials,
camera speed, exposure and sample counts without recompiling.

## Hardware and fallback

Two backends, chosen at start-up and reported in the window title:

| | Path | Requires |
| --- | --- | --- |
| 1 | `VK_KHR_ray_tracing_pipeline` + `VK_KHR_acceleration_structure`, tracing a device TLAS | any Vulkan 1.2/1.3+ GPU with hardware RT |
| 2 | compute shader + `VK_KHR_ray_query` over the *same* TLAS and the same shading code | no RT pipeline, e.g. Intel UHD 770 |

Verified on both adapters present in the build machine: the NVIDIA card takes path 1, and forcing
`--compute` renders the identical scene through path 2 (see `captures\compute-fallback.png`).

## What was actually checked

* **The window itself, on screen.** `--capture` shares the tracer with the window but not the
  present pass, so a headless green run does not prove the window works. `tools\window_probe.ps1`
  screenshots the desktop while the app runs, and that is what caught the black-window bug (the
  render pass had been built before the swap chain existed, so it had no colour format and quietly
  dropped every draw while still clearing).
* `--capture 2 --accum 128 --size 1280x720` on the ray-tracing pipeline, and the result inspected:
  the cube, cone, four spheres, both light spheres, their reflections in the metal, their refraction
  through the glass, soft shadows and the floor caustics are all there.
* The same scene through the compute fallback (`captures\compute-fallback.png`).
* A windowed run: 60 fps sustained; the render also verified at 540x380 and at maximised 3840x2019.
* **The adaptive controller, measured rather than assumed.** 20 s untouched: 59.8-60.0 fps, settling
  at 2x supersampling with 11-12 samples per pixel and about 12 ms of GPU work per frame. Maximising
  mid-run (8x the pixels) made it drop to 1.05x scale and 4-5 samples within about a second, and
  restoring the window returned it to 2x and 9-12 samples, with roughly five quality changes in the
  whole 26 s rather than a continuous flutter. The numbers are logged with `--verbose`.
* The asset path: `scene.json` read from the `.pak` drives the render; deleting the archive falls
  back to compiled-in defaults and says so in the log.

Known issue: on a DPI-scaled display, maximising leaves the framing zoomed and off-centre. The
measured extents all agree and the same resolution renders correctly headlessly, so it is isolated to
the windowed present path at that size and is still open; at ordinary window sizes the framing is
correct. Everything else above is verified.


## Repository layout

```
CMakeLists.txt            build definition; also refuses to build against a stale generated header
build.ps1                 one-command build (finds Build Tools, glslang, Python itself)
shaders/                  GLSL: rt entry points, present pass, and the shared path-tracing core
src/                      Vulkan setup (core*.cpp), windowing + frame loop (frame.cpp), scene model,
                          pak reader (zip + inflate + crc32), tolerant JSON parser
tools/build_shaders.py    include flattening + glslang invocation + SPIR-V header emitter
tools/make_pak.py         generates textures/scene.json and writes scene.pak
tools/window_probe.ps1    screenshots the real window while it renders - the present-path check
assets/                   source files that go into the pak
third_party/              volk, GLFW 3.4, stb, Vulkan-Headers (all MIT/PD/zlib licensed, see LICENSE
                          files alongside each)
captures/                 verification stills produced by the runs above
DESIGN.md                 the design spec, including the API gotchas found while implementing it
```
