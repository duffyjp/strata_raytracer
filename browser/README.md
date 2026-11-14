# WebGPU port, second attempt: `browser/`

Entry point: **`browser/index.html`**, served from the repository root:
**http://localhost:8731/browser/index.html** (`web/demo_server.py --port 8731 --root .`).

The interface is the render window and nothing else: one canvas filling the viewport, no report
page, no results panel, no buttons. The document title carries the exe's title-bar text.
Everything the exe writes to `raytracer.log` is written with `console.log`.

This file is the **contract**: the exe's actual behaviour, the web equivalent, and where the two
cannot meet, the specific thing in WebGPU or the browser that prevents it. It was written by reading
`src/main.cpp`, `src/app.h`, `src/frame.cpp`, `src/core.cpp`, `src/core2.cpp`, `src/scene.cpp`,
`src/scene.h` and the GLSL, before any of the app below was written. Line references are to the tree
at the commit this was written against (`e0e3b92`).

The first attempt (`web/`) is kept, untouched, as the reference its proven code came from.
Everything under this directory that is not new (`zip.js`, `scene.js`, `bvh.js`, `geometry.js`,
`renderer.js`, `quality.js`, `imageio.js`, `shaders/*.wgsl`) is a copy of `web/`'s working renderer
at that same commit; the new files are `index.html`, `app.js`, `exe.js` (log/title parity layer)
and this README.

---

## Where it runs: GitHub Pages, and locally

**Published site: <https://duffyjp.github.io/strata_raytracer/>** — deployed by
`.github/workflows/pages.yml`, which copies this directory to `dist/` unchanged, adds `assets/scene.pak`
built by `tools/make_pak.py` (standard library, no compiler), and uploads it as the Pages artifact.
Pages serves a repository site **under a path prefix**, so every URL the page asks for is resolved
relative to the module that asks for it (`app.js`: `PAK_CANDIDATES`, `ASSET_DIRS`, `REF_CANDIDATES`) —
never against the server root. That is also why the same build still works from a deeper path locally.

**Locally**, serve the repository root and open the deeper path, as before:

```
python web/demo_server.py --port 8731 --root .      # or: python -m http.server 8731
start http://localhost:8731/browser/index.html
```

The page then finds `scene.pak` at `../bin/scene.pak` (that is `build/bin/scene.pak`, which `build.ps1`
writes) and falls back to the loose `../../assets/` files if it is not there. To make the served
directory carry its own archive exactly like the published site does, run
`python tools/make_pak.py --out browser/assets/scene.pak`.

Two things differ on the published site, both on purpose. The upload sink (`/results`, `/upload`) is
`demo_server.py`, so a static host has none and the page says `not uploaded (static host...)` instead of
POSTing into a 404. And the still comparison is **opt-in**: `?cmp=1` needs a native reference PNG, which
lives in the repository (`web/refs/`) and is not published, so on Pages it logs
`still comparison SKIPPED - no reference image (tried ...)` and records `cmp_verdict=skipped`. It never
counts a skipped comparison as a pass.

**Browser: Edge or Chrome**, as the section below measures — Firefox was 42x slower on the same compute
kernel and cannot name its adapter, so nothing run there is attributable.

**Keys:** `ESC` quit (a page cannot close its tab: it stops rendering and says so), `Alt+Enter`
fullscreen, `N` denoiser, `P` pause the scene clock, `Up`/`Down` samples per pixel, `WASD` free camera
with `Shift` faster, mouse look once the canvas has pointer lock, and 10 s of idle hands the camera
back. `?help=1` prints the whole flag list in the console.

---

---

## Which browser to run it in: Edge or Chrome, not Firefox

**This port targets Edge or Chrome.** Firefox runs the page - it renders, it reports no errors, and it
is 10.6x slower doing it. Measured on this machine, in the same session, at the same 1944x1240 backing
store:

| browser | frame time | calibration kernel (2147483648 FLOP) | adapter identity |
|---|---|---|---|
| Edge (Chromium, RTX PRO 5000 Blackwell) | **9.32 ms** | **894.78 GFLOP/s** | `vendor=nvidia`, `architecture=blackwell` |
| Firefox, same machine, same session | **99.11 ms** | **21.47 GFLOP/s** - 42x slower | reads back **empty** |

Two separate problems, and the second is the reason the first cannot even be pinned down. The 42x on an
identical compute kernel is not a tuning gap in this port; it is the browser underneath it. And because
Firefox's WebGPU adapter identity fields come back empty, a Firefox run cannot be attributed to a GPU at
all - nothing in its own log says which adapter did the work, so nothing measured there is comparable
with anything measured here. Section 9.5 documents the same empty-field problem on Chromium's
`adapter.device` / `adapter.description`, which is why the kernel price is printed next to the identity
in every run; on Firefox there is neither half.

Run it in Edge or Chrome. The headless driver in `tools/run_page.mjs` launches Edge
(`--headless=new --enable-unsafe-webgpu`), and every number in this file came from that route.

**One Chromium warning in the log is expected and is not a fault.** On Windows Chromium prints that
`powerPreference is currently ignored when calling requestAdapter()`
([crbug.com/369219127](https://crbug.com/369219127)). The page does hand `powerPreference:
'high-performance'` to `requestAdapter()`; the warning means the browser picks the adapter anyway and
the hint is documentation. `app.js` now says so in its own startup log, next to the adapter identity and
the kernel price, so the line stops reading as a problem in a run log.

---

## 0. How to read the verdict column

| mark | meaning |
|---|---|
| **yes** | the web build behaves the same way, driven the same way |
| **adapted** | same behaviour through the mapped input (flag → URL parameter, log file → console) |
| **partial** | the behaviour exists, one observable difference is documented below it |
| **no** | not achievable; the reason names the specific browser/WebGPU limit |

## 1. Command-line flags → URL parameters

Every flag of `src/main.cpp:102-155` and its default, mapped to `?name=value` on the page URL
(`browser/app.js`, `parseArgs`). Values are the exe's defaults, from `src/scene.h:119-171` and
`src/main.cpp:204`.

| exe flag | default | web equivalent | verdict |
|---|---|---|---|
| *(no options)* | window 1280x720, animating | no parameters | yes |
| `--capture N` | 0 (off) | `?capture=N` — accumulation stills, no animation, uploaded as PNGs through the server's `/upload` and also left on `window.__ray.still` | adapted |
| `--accum S` | 48 | `?accum=S` | yes |
| `--size WxH` | 1280x720 | `?size=WxH` (capture size). In the animated path the window size is the browser viewport, which the page does not own; see `?window=` below | adapted |
| `--window WxH` | 1280x720 (undocumented in `--help`) | `?window=WxH` sizes the canvas element in CSS pixels and the drawing buffer in device pixels. It cannot resize the browser window: a page has no API for that (`window.resizeTo` is refused for a tab the script did not open, and is refused outright in headless). The requested size is honoured inside the page and the difference is logged exactly as the exe logs `window: asked for %dx%d, given %dx%d` (`main.cpp:252`) | partial |
| `--out DIR` | `captures` | `?out=<subdir>` — the POST target directory for uploaded stills (server writes under the repo root); `?out=0` disables uploading | adapted |
| `--pak FILE` | search next to the exe | `?pak=/path.pak` — fetched over HTTP; default `/build/bin/scene.pak`, falling back to `/assets/scene.json` + loose textures | adapted |
| `--scale F` | 1.0, clamped 0.4..1.0 | `?scale=F`, same clamp, same meaning (trace-resolution ceiling; auto never exceeds it) | yes |
| `--no-auto-quality` | auto on | `?autoq=0` — pins `?spp=` and `?scale=` | yes |
| `--quality MODE` | `high` | `?quality=high\|balanced\|performance\|off`, same four bundles (`frame.cpp:430-441`) | yes |
| `--denoise` / `--no-denoise` | on while animating | `?denoise=1\|0`. In `?capture` mode the exe turns it off unless it is named explicitly, and logs `stills: denoiser left off (a still averages frames; it does not filter them)` (`main.cpp:191-198`); the page does the same, same sentence | yes |
| `--gbuf CH` | off | `?gbuf=albedo\|normal\|depth\|denoised\|diff\|variance`; forces the feature writes on, as `main.cpp:199` does. Only albedo/normal/depth are ported in `shaders/trace.wgsl:749-759`; `denoised`/`diff`/`variance` are **no** for now: those three are not shader-channel switches in the trace kernel but presentation- and à-trous-side modes (`src/core2.cpp:589-590` passes `mode` 1 and 2 to the à-trous dispatches, `shaders/present.frag:52-56` swaps in the denoiser texture), and porting them means new WGSL variants, which is listed in §7 rather than pretended | partial |
| `--filter-z/-n/-l F` | 0.02 / 128 / 24 | `?filterz=`, `?filtern=`, `?filterl=` | yes |
| `--refresh N` | measured from the monitor | `?refresh=N` overrides the measured rate | yes |
| *(panel rate detection)* | `glfwGetVideoMode` | **no** equivalent: a page cannot read the display's mode. The rate is *estimated* from the median `requestAnimationFrame` interval over the first 60 callbacks, logged with `~`, and `?refresh=` overrides it — same role, weaker evidence. `effectiveFps()`'s "panel within 20 % below the target trims the target" rule then applies to the estimate (`frame.cpp:420-426`) | partial |
| `--target-fps N` | 60 | `?targetfps=N` | yes |
| `--max-spp N` | `--help` says 8; `scene.h:133` says 16, and `quality=high` keeps 16 | `?maxspp=N` | adapted (the exe's own help text is stale; the web default follows `scene.h`) |
| `--min-scale F` | 0.40 | `?minscale=F` | yes |
| `--spp` | *not an exe flag* — `samplesPerFrame` comes from `scene.json` (1..4) | `?spp=N`: the brief asked for it; with `?autoq=1` it is a starting value the controller may move, with `?autoq=0` it is pinned | adapted (new knob, documented as such) |
| `--compute` / `--rt` | pick the best the adapter has | **no**: WebGPU has no ray-tracing pipeline and no ray-query. There is one path — a compute kernel walking a CPU-built two-level BVH — so there is nothing to select. `--rt --compute` mutual exclusion has no analogue | no |
| `--list-adapters` | — | **partial**: the page logs the one adapter the browser handed it, plus the feature/limit surface. Web content cannot enumerate adapters: `navigator.gpu.requestAdapter()` returns a single implementation-selected adapter (`powerPreference` is a hint and Dawn ignores it on Windows), and there is no `enumerateAdapters` in the spec | partial |
| `--verbose` | off | `?verbose=1` — per-frame phase breakdown, camera state, accumulation-frame lines | adapted |
| `--help` | — | `?help=1` prints the same text to the console | adapted |
| `--autoexit S` | 0 (undocumented in `--help`) | `?autoexit=S`, then the same `auto-exit after %.1f s: ...` summary line | yes |
| `--test-toggle N` | -1 | `?testtoggle=N` — same code path as the `N` key | yes |
| `--test-give-up N` | -1 | `?testgiveup=N` — forces "the frame rate is being missed", including the cost gate (`frame.cpp:1250-1268`) | yes |
| `--test-budget MS` | 0 | `?testbudget=MS` — replaces the controller's frame budget | yes |
| `--test-camera` | off | `?testcamera=1` — drives free-look from the clock (free at 2 s, released at 6 s) so the handover is verifiable without a keyboard (`frame.cpp:1270-1280`) | yes |
| `--dump-accum N` | -1 | `?dumpaccum=N` — uploads the raw accumulated float target after frame N as a PNG-encoded float dump; the exe writes `accum_NNN.bin`, a browser cannot write a file, so the bytes are POSTed | adapted |

## 2. Key bindings (`src/frame.cpp:1186-1320`)

| key | exe behaviour | web equivalent | verdict |
|---|---|---|---|
| `ESC` | leaves the loop, tears down, process exits | stops the frame loop, `device.destroy()`, releases every texture, stops updating the title and leaves the page titled `Vulkan Ray Tracer - quit (ESC)` with the final frame frozen on the canvas | partial, deliberately: a web page cannot close its own tab (browsers forbid `window.close()` for a tab the script did not open, and forbid it outright in headless). Parity here is the exe's meaning — *stop rendering, tear down, leave the title saying quit* — minus what the browser forbids |
| `Alt+Enter` | fullscreen on the primary monitor's **native mode**, restoring the remembered window rect; logs `window: fullscreen (WxH)` (`frame.cpp:512-529`) | `document.documentElement.requestFullscreen()` on the fullscreen *element* canvas, which on a real desktop browser is the page-filling mode of the monitor's current mode; the log line is the exe's | partial: a page cannot select a display mode. There is no monitor-mode API in the browser, so "the monitor's native mode" is whatever mode the desktop is already in; entering fullscreen is also only allowed inside a user gesture, so a scripted/headless dispatch of the key logs what the browser permitted instead |
| `N` | `toggleDenoise()`: rebuilds the HDR + feature + history targets, resets accumulation and history, invalidates the cost model, logs `denoise ON (pressed N)`. Sets `denoiseUserToggled`, which hands the switch to the user **for the rest of the run in both directions** (`frame.cpp:374-388`) | identical, including the user-owns-it rule: after one `N` the controller never flips the filter again (`quality.js` `denoiseUser`), and the title changes | yes |
| `W`/`A`/`S`/`D` | first press enters free look (`enterFreeCam`, `frame.cpp:533-544`): camera starts exactly at the orbit's position/orientation, cursor is hidden, `camera: free look (WASD to move, mouse to look; idle 10 s hands control back)` | identical: same enter-free-look message, blend eased at the same rates (8/s in, 0.7/s out), same ` - free look` title suffix in the pinned-quality title variant | yes |
| *(10 s without WASD)* | hands control back: `camera: idle for 10 s, easing back to the orbit`, cursor restored | identical, same 10 s, same easing | yes |
| `LeftShift` | move speed 12 units/s instead of 4 (`frame.cpp:1297`) | identical | yes |
| *(vertical movement)* | none: motion is locked to the horizontal plane; `pitch` is look-only (`frame.cpp:1296`) | identical — no `Q`/`E`, no vertical control | yes |
| mouse look | re-centre the cursor every frame and read the offset: `yaw -= dx*0.0022`, `pitch -= dy*0.0022`, pitch clamped ±1.45 rad | Pointer Lock (`canvas.requestPointerLock()`), same 0.0022 sensitivity and same ±1.45 clamp, reading `movementX/movementY` — the browser's version of "re-centre and read the offset" | partial: pointer lock is granted only in response to a user gesture and the browser may refuse or show a prompt; when it is refused, mouse look stops and the refusal is logged instead of being faked |
| *(mouse look before any click)* | GLFW cursor is disabled at the moment free look starts, so the very first free frame already has relative motion | the page must ask for pointer lock inside a gesture, so the first free-look frames after `W` may have no motion until the canvas is clicked; `camera: pointer lock pending - click the window` is logged. Everything else (yaw/pitch state) is live | partial |

### 2.1 Keys the exe does not have: `P` and `Up`/`Down`

Two bindings exist on this page because the page needs them, not because `frame.cpp:1186-1320` has
them. Both are stated here with their semantics decided rather than guessed at, and both appear in the
overlay's hint line so they are discoverable.

| key | what it does | why this and not the obvious other thing |
|---|---|---|
| `P` | **pauses scene time and keeps accumulating.** The orbit angle, the two light orbits and the orb refit all read one clock (`run.simTime`), and that clock stops; the idle hand-back eases on the same clock, so it stops too. The frame loop, the frame counter, the fps window, the sample count and the accumulation counter all keep running, and while paused the accumulation window is opened (`cfg.accumulationFrames` no longer caps it), so every frame adds its samples to the same target. Pressing `P` again restarts the clock **and restarts accumulation**. The state is in the overlay (`PAUSED` / `LIVE`) and in the title (` - PAUSED`, before `(ESC) Quit`). | Freezing the frame loop is what "pause" means in a video player and it is the wrong thing here: the frame you stopped on is a 1-spp frame, so pausing would freeze the picture at its noisiest instant. The exe has the better answer in its stills - a clean image is the same camera averaged over many frames - so pause averages while the camera holds still, and the pause becomes the moment to look. Resuming has to reset accumulation for the same reason a resize or a denoiser flip does: samples gathered at a stationary camera are not samples of a moving one. With the filter on, what the eye sees is the filter's output (`present.wgsl:75-81`), which converges on its own history; the accumulated sum underneath keeps growing either way and the counter says so. `?capture` stills never run the loop, so they are unaffected. |
| `ArrowUp` / `ArrowDown` | **samples per pixel per frame, +/-1, clamped to 1..16** - the exe's own range: `scene.h:125` is the floor, `scene.h:133` (`maxSamplesPerFrame`, the controller's ceiling) is 16, and `?maxspp=` moves the ceiling exactly as `--max-spp` does. | The exe has no key for it because its sample count is the controller's. The web build had `?spp=` already, which is a start value, not a control. |
| *(after one `Up`/`Down`)* | **the manual change is authoritative for the rest of the run.** `quality.sppUser` takes samples out of the controller's hands in both directions - the model's climb, the model's cut, and the measured-step fallback that cuts samples when nothing else will move (`quality.js`) - and leaves it the resolution and the filter. For "is there anything left to give", pinned samples count as already at the floor, so a run with pinned samples still surrenders the denoiser when it must and still says `exhausted` when the floor is the floor. The log says the value and that it is the user's now: `quality decision: samples 4 -> 5 (pressed ArrowUp) - samples are the user's from here on; the controller keeps the resolution and the filter`, and at a clamp `quality decision: samples 16 held (pressed ArrowUp: 16 is the most this app allows, 1..16) - samples are the user's from here on`. | This is the exe's own convention, copied: `N` sets `denoiseUserToggled` and the controller never flips the filter again (`frame.cpp:374-388`, section 6). A quality knob the machine is allowed to overrule five seconds later is not a setting the user made. On the pinned-quality path (`?autoq=0`) the same keypress moves `cfg.samplesPerFrame`, which is where that path reads its value, as `core2.cpp:384` does. |

## 3. Camera and scene (`src/core2.cpp:294-431`, `src/scene.cpp:201-298`)

| item | exe | web | verdict |
|---|---|---|---|
| orbit | `ang = t*0.24`, `y = 3.2 + 0.6*sin(t*0.23)`, radius 9.5, look-at (0,0.85,0), fov 42, `right = normalize(cross(f,(0,1,0)))` | same formulae, same constants (`scene.js` `cameraBasis`) | yes |
| free-cam blend | origin and forward interpolated, then the basis re-derived from the blended forward (never lerping a basis) (`core2.cpp:351-375`) | same, ported row for row | yes |
| objects | disc ground r=12 s=12, mirror sphere, satin sphere, red sphere, glass sphere, mirror cube (rot 0.42), teal cone + 2 emissive light orbs | same 9 instances, same materials by name (`scene.js` `defaultScene`) | yes |
| triangle counts | sphere 1225 v/6912 i (2304 tri), cube 36/36 (12), cone 576/576 (192), disc 384/384 (128) | same, printed in the same sentence shape (`  mesh sphere: uploading 1225 verts / 6912 indices`), and the instanced total is the same 14156 world triangles | yes |
| lights | key: warm (1,0.78,0.55) x16, r 0.34, orbit 3.9 @ 2.6, speed 0.55; fill: cool (0.45,0.66,1) x10, r 0.27, orbit 4.7 @ 1.7, speed -0.40, phase 2.1 — both analytic lights *and* an emissive orb riding the same orbit | same table; orbs re-transformed and the BVH refit per frame like `uploadFrameInstances` + BLAS rebuild | yes |
| materials | 9, models lambert/GGX-metal/glass/emissive; ground textured with the pak's 128x128 checker at scale 6 | same table and same UNORM byte handling (`trace.wgsl` transliterated from the GLSL) | yes |
| sky, exposure, clamp, bounces | sky 0.4/0.45/0.54, exposure 1.7, radiance clamp 12, 6 bounces, `scene.json` overlay from the pak | same, `scene.json` applied from the pak with the same padding trim (`zip.js` + `scene.js`) | yes |
| next-event estimation, G-buffer prepass, SVGF temporal + 5 à-trous, ACES, vignette | `main.rgen`, `svgf_*.comp`, `present.frag` | `trace.wgsl`, `svgf_temporal.wgsl`, `svgf_atrous.wgsl`, `present.wgsl` — proven against the exe's stills in `web/` | yes |
| accumulation | windowed: `accumulationFrames` 1, so every frame is traced fresh and the temporal filter carries the smoothing; the displayed image divides by frames accumulated | same (`samplesForDisplay`), and stills accumulate `--accum` dispatches with `reset` only on the first | yes |
| still path | `t = 0.55 + img*2.35` fixed schedule, PNG via `writePng` (linear vignette, per-channel ACES, no dither) | `?capture=` renders the same t, and `present.wgsl` mode 0 reproduces `writePng`'s order; verified against `web/refs/native_*_t0.55.png` in §6 | yes |

## 4. Title bar → `document.title` (`src/frame.cpp:1447-1464`)

Updated once per second, same cadence, same format. Two variants, exactly as the exe has two:

* adaptive (default): `Vulkan Ray Tracer - %.0f fps - %ux%u trace %ux%u - %d spp - WebGPU pipeline - (N) Denoise ON|OFF - (ESC) Quit`
* pinned quality (`?autoq=0`): the same with ` - free look` inserted when the blend is past 0.5.

Notes, because they are contract items:

* **`Vulkan Ray Tracer` is kept verbatim** as the product prefix. The brief permits one substitution,
  the pipeline name, and silently renaming the product is the kind of substitution it forbids. So the
  string reads `... - WebGPU pipeline - ...` while the prefix still names the desktop app it is a port
  of. Change it here and nowhere else.
* The exe's `%.0f fps` is frames/second over the last second of the loop; the page counts presented
  frames over the same 1 s window.
* `trace %ux%u` is the real trace target (`renderer.resize` size), so a supersampled or downscaled
  frame shows what was traced, not what is displayed — the exe's behaviour.
* The exe's adaptive title variant has **no** ` - free look` clause (only the pinned variant has it,
  `frame.cpp:1448-1463`). That quirk is reproduced, not "fixed", so a side-by-side run matches.
* `(ESC) Quit` stays in the string even though the browser will not close the tab; what ESC does is
  in §2.

### 4.1 The status overlay: a deviation, on purpose

This page has exactly one element that is not the canvas: `#status`, top-left. It is a **deliberate
break from "the interface is the render window and nothing else"** stated at the top of this file, and it
was asked for explicitly (2026-11-13). The excuse, if it needs one, is that the exe's live numbers are
printed in its *window decoration*, which a page has no equivalent of: `document.title` is the closest
thing and it is not visible while the canvas has the screen. So the same values are echoed on screen.

What "does not disturb the layout" means concretely, all of it asserted in the `features` scenario
(`tools/run_page.mjs`) and not just intended:

| property | why |
|---|---|
| `position: fixed` | out of the flow: nothing the overlay says can move, resize or scroll the canvas. Asserted by measuring the canvas's own bounding box with the element shown and hidden - both `1246x616`, and `document.documentElement.scrollWidth/Height` equal to the viewport. |
| `pointer-events: none` | a click falls through to the canvas, so the pointer-lock gesture still works. Asserted by taking the lock with a plain click *after* the overlay is on screen: `locked false -> true`. |
| not focusable, no handlers | it cannot take focus from the window or swallow a key. Asserted by `document.activeElement.tagName === 'BODY'` after clicks and keypresses. |
| monospace, `white-space: pre` | the live line is a readout; it must not reflow. |
| translucent plate (`rgba(0,0,0,.26)`) plus a dark text shadow with a 1 px light edge | one line has to be legible over the bright sky *and* over the dark floor, which is what the frame actually contains at the top-left. No solid box: the picture is the interface and the plate is 26 % of one channel. |

Two kinds of lines: the live one, built from the same values `document.title` carries and refreshed on
the title's own once-a-second cadence - `60 fps | 1246x616 trace 1005x497 | 1 spp | WebGPU pipeline |
denoise ON | LIVE` - and the keyboard line, `N denoise | P pause | Up/Down samples | WASD free camera
(10 s idle returns to the orbit) | Alt+Enter fullscreen | ESC quit`. The last field of the live line is
`PAUSED` or `LIVE`, so the picture is never ambiguous about whether it is moving (section 2.1).

Whether it is *drawn* is checked by pixels, not by the DOM: two full-page screenshots of one and the
same frame (the frame loop is stopped for the pair with the driver's `__ray.freeze`, which is not `P`),
taken with the element shown and hidden, decoded **outside the browser** by `tools/png_read.mjs` -
Node's `zlib`, no dependency, the same independence `web/png_check.py` gives the stills. The bounding box
of the differing pixels is where the overlay is.

## 5. Log file → `console.log`

Same messages, same wording, same order — a diff of the two logs should show only the lines this
section lists as impossible. Info goes to `console.log`, warnings to `console.warn`, and the exe's
`fatal:` lines to `console.error` (the exe has no per-line level prefix: it writes plain text with
conventional prefixes such as `step: `, `quality decision: `, `fatal: `, `  timing: `; those are kept
exactly and nothing is added).

Startup sequence, exe first, web second:

```
pak: <path> (4 entries)                                    pak: <url> (4 entries)
scene.json applied                                         scene.json applied
adapter 0: NVIDIA RTX PRO 5000 Blackwell ...               rendering on: nvidia/blackwell  [WebGPU pipeline]
adapter 1: Intel(R) UHD Graphics 770 ...                   adapter surface: <named fields>, isFallbackAdapter=...,
rendering on: <name>  [ray-tracing pipeline]               features=N [...] limits=...
pipeline chosen by the adapter probe: ...                  pipeline chosen by the browser: WebGPU pipeline
device request: rayTracingPipeline=...                      device request: (browser-owned) ...
step: device + queues created                              step: device + queues created
step: command pool, sync objects, sampler                  step: command pool, sync objects, sampler
  mesh sphere: uploading 1225 verts / 6912 indices           mesh sphere: uploading 1225 verts / 6912 indices
step: geometry and acceleration structures                 step: geometry and acceleration structures
  texture from pak: textures/checker.png ...                 texture from pak: textures/checker.png ...
step: textures                                             step: textures
step: ray-tracing pipeline ready                           step: WebGPU pipeline ready
window 1280x720 - drag the border to resize, ESC to quit   window 1280x720 - drag the border to resize, ESC to quit
display: 60 Hz panel, targeting 60 fps, ...                display: ~60 Hz panel (estimated from rAF), targeting 60 fps, ...
quality start: denoise on, scale 1.00, spp 1, ...          quality start: denoise on, scale 1.00, spp 1, ...
```

Then, in the same words: `denoise ON|OFF (<why>)`, `quality decision: denoise off (late fraction ...)`,
`quality decision: samples %d -> %d (...)`, `quality decision: scale %.2f -> %.2f (...)`,
`quality decision: denoise on (headroom ...)`, `  denoise cost: %.2f ms per megapixel traced (...)`,
`camera: free look (...)`, `camera: idle for 10 s, easing back to the orbit`, `window: fullscreen
(WxH)`, `stills: denoiser left off (...)`, `capturing %d image(s) x %d samples into ...`,
`  wrote %s (%d spp)`, `auto-exit after %.1f s: %llu frames (...)`.

**The sentences that cannot be identical, and why.** Everything above is byte-for-byte the exe's text.
These are the places where the exe's words would state something the web build does not know, so the
word changes and the reason is the thing listed in §7:

| exe | web | why it cannot be the same sentence |
|---|---|---|
| `adapter %u: %-40s vulkan %u.%u ...` | `web: adapter identity for this run: {...} (source adapter.info, isFallbackAdapter=...)` | one adapter, no enumeration (§7); the fields are printed as read, with the route that produced them |
| `pipeline chosen by the adapter probe: ray-tracing pipeline` | `pipeline chosen by the browser: WebGPU pipeline (...)` | the browser picked it, and there is only one path |
| `device request: rayTracingPipeline=...` | `device: %d optional features enabled on it, the adapter offers %d; max...` | a page cannot ask for Vulkan features; what it can state is what the device came with |
| `display: %d Hz panel` | `display: ~%d Hz panel (estimated from requestAnimationFrame: the browser exposes no display mode)` | §1: no refresh-rate API |
| `step: ray-tracing pipeline ready` | `step: WebGPU pipeline ready` | the pipeline name slot, as the brief directs |
| `quality decision: scale %.2f -> %.2f (gpu %.1f ms, budget %.1f ms)` | `... (frame %.1f ms measured, budget %.1f ms)` | the number is a wall-clock submission time, not a GPU timestamp; calling it `gpu` would be a false claim |
| `  denoise cost: ... measured across the filter going off` | `... (learned from split-probe frames)` | the web price comes from frames measured as two submissions, not from a flip |
| `window %ux%u - drag the border to resize, ESC to quit` | same, then `web: the window is live ...` | the extra line is web-only and marked with `web: ` |
| *(the exe has no pause)* | `web: pressed P - the scene clock is stopped and accumulation continues: ...` and `web: pressed P - the scene clock runs again and accumulation restarts, ...` | a new control (section 2.1), so it is marked `web: ` like every other sentence the exe cannot have |
| `quality decision: samples %d -> %d (gpu %.1f ms, budget %.1f ms)` | `quality decision: samples %d -> %d (pressed ArrowUp) - samples are the user's from here on; the controller keeps the resolution and the filter`, and at a clamp `samples %d held (pressed %s: ...)` | the exe's decision sentence, with the key press in the reason slot instead of a measurement: the reason a manual change happened is the key, and printing a frame time as the cause of it would be false |

Lines the exe has no equivalent for are prefixed `web: ` and warnings use `console.warn`, so they are
identifiable as ours rather than as translations: the pass-price banner, the calibration kernel, the
`CONTROLLER EXHAUSTED` line, the canvas-copy refusals, and the quit explanation.


**First-run console banner** (web-only addition the brief asks for): adapter identity, then the price
of every pass measured in this run — trace, temporal, à-trous x5, present, accum copy — each next to
the empty-dispatch floor it was measured against, plus a fixed **calibration kernel** price (§7).

## 6. Adaptive quality controller (`src/frame.cpp:569-931`)

Same policy, ported in `quality.js`: two-term cost model (trace per sample per traced kilopixel,
filter per traced megapixel), budget `max(1.5, frameMs*(1-headroom) - 2.5)`, deadband 0.72..1.04,
settle 3 readings after a change and 6 after a reallocation, evidence weighed every 12 readings,
over-streak 2 / under-streak 3, climb margin 0.86, samples capped at `baseSamplesPerFrame` while the
filter is on (and `baseSamplesPerFrame` is forced to 1 while animating with denoise on,
`frame.cpp:1128`), scale floor `minRenderScale`, ceiling `min(maxRenderScale, sqrt(maxTracePixels/pixels))`,
filter restored only once it has been priced, at most 3 flips, and an explicit `exhausted` state so a
run that missed its target cannot be read as a success.

Two **documented divergences**, both from measurement, both selectable:

1. **Input.** The exe decides on Vulkan GPU timestamps. Web content has no trustworthy GPU clock:
   `timestamp-query` exists on this adapter but resolving a query costs a submit plus a `mapAsync`
   round trip measured at 0.5-3.0 ms — the size of the budget itself. So the page decides on
   wall-clock milliseconds around submissions, and periodically measures one frame as two
   submissions (trace, then filter+present) to price the two terms on the same workload.
2. **Order.** The exe gives up the **denoiser first** (`frame.cpp:699-725`), which on its machine is
   nearly free: measured 0.86 ms/MP, 0.79 ms of a 16.9 ms frame. Through the browser the same passes
   measured 3.6 ms/MP on nvidia/blackwell and 37.8 ms/MP on Intel UHD 770, so on this path the filter
   is a first-order term and the page gives up **pixels first**, then samples, then the filter.
   `?priority=denoise` reproduces the shipped native order for comparison; the default is
   `?priority=pixels`.

Also logged, same wording, from the same rule: **the manual toggle wins.** Pressing `N` sets
`denoiseUser`, which ends the controller's authority over the filter for the rest of the run in both
directions. `ArrowUp`/`ArrowDown` set `sppUser` for the same reason and with the same effect on the
sample count (section 2.1): the controller keeps the resolution and the filter, and nothing else. When
the samples are pinned and the resolution floor has been reached, `atSppFloor` is what lets the filter
still be surrendered and `exhausted` still be said, with the why-line reading `... at 6 spp (the user's,
pinned), scale 0.40 (the floor)` instead of claiming 1 spp.

## 7. Not achievable, and exactly what prevents it

| item | what stops it |
|---|---|
| ESC closing the tab | Browsers forbid a script from closing a tab it did not open (`window.close()` is a no-op for a tab opened by navigation, and headless refuses it). The page therefore stops rendering, destroys the device and titles itself as quit |
| `Alt+Enter` choosing the monitor's native mode | No browser API selects a display mode. Fullscreen is the desktop's current mode, and can only be requested from inside a real user gesture |
| Resizing the OS window (`--window`) | No API. A page can size its canvas, not its window; `window.resizeTo` is refused for tabs it did not open |
| Reading the panel's refresh rate | No API. Estimated from `requestAnimationFrame` intervals and marked `~` in the log; `?refresh=` pins it |
| Enumerating adapters (`--list-adapters`) | `navigator.gpu.requestAdapter()` hands out one implementation-chosen adapter; there is no enumeration in the spec, and `powerPreference` is ignored by Dawn on Windows (crbug.com/369219127), so the page cannot even ask for the dGPU reliably |
| Hardware ray tracing (`--rt`) | WebGPU has no ray-tracing pipeline or ray-query object. The port traces by walking a CPU-built two-level BVH in a compute kernel |
| Honest GPU frame timestamps | `timestamp-query` resolution needs a submit + `mapAsync` round trip measured at 0.5-3.0 ms, larger than the thing being measured |
| Naming the GPU with certainty | `GPUAdapterInfo` fields are prototype getters (`Object.keys()` on it is empty) and are deliberately coarse; some browsers leave `device`/`description` blank, and a fallback (software) adapter can look plausible. The page logs the named fields **and** a fixed calibration-kernel price on every run, and never attributes a run to a card from timing alone |
| `--gbuf denoised\|diff\|variance` | Needs the à-trous `mode` variants and the present-pass denoiser swap the port has no WGSL for yet (§1) |
| Writing files (`--out`, `--dump-accum`) | A page writes nothing to disk; PNG bytes are POSTed to the local server, which is what `web/demo_server.py` is for |
| Process exit codes | A page has none. `window.__ray.verdict` and the POSTed block carry what an exit code would have said |
| Raw `accum_NNN.bin` float dump | Float32 bytes cannot be POSTed to `/upload` (the server accepts PNG or a `data:` URL only, and `web/` may not be edited), so the float dump is carried as a PNG-encoded float buffer and said so in the log |

## 8. Resize

`resize` reallocates every texture and resets accumulation and history — the exe does the same in
`rebuildAfterResize` (`frame.cpp:943-958`): destroy, recreate swapchain/framebuffers, `applyHdrSize`
(which zeroes `samplesAccumulated`), `invalidateQuality`. The page's path is
`renderer.resize(w,h)` (destroys 16 textures and every bind group holding a view of one, rebuilds
them, clears `historyValid`/`prevBasis`) plus the app's own reset of `samplesAccumulated` and the
cost model. The canvas is sized in CSS pixels and the drawing buffer in `devicePixelRatio` device
pixels; `?dpr=1` pins it for reproducible measurements.

This path is **tested explicitly**, not assumed, because of the bug it once hid: the blit sampled the
presentation texture with pixel-based coordinates, so once the trace scale dropped below 1 the
source coordinates left 0..1, clamp-to-edge replicated the last column, and the right half of the
canvas was one smeared column while every "is there a picture" statistic still passed
(`docs/web-port-notes.md` §11). The blit now derives `uv` from the clip-space corner, and the proof
is three-way: three successive canvas readbacks whose SHA-256 hashes differ (frames really
advance), the last of them byte-compared against the presentation texture, and `web/png_check.py`
run outside the browser on both PNGs — its `--pair` mode plus the run-length test that separates a
correctly magnified frame from a corrupted one.

## 9. Verification of this build

1. **Frames advance, and the screen shows the frame that was rendered** — headless run,
   `console.log` captured to a file; three canvas-readback hashes; `png_check.py --pair`.
2. **Keys** — driven through the DevTools protocol with real input events, asserting the observable
   effect: `N` flips the denoiser *and* the title, WASD moves the camera (frames stop matching, a
   fixed pixel changes), `Alt+Enter` requests fullscreen (what the browser permitted is logged),
   `ESC` freezes the frame counter and titles the page as quit.
3. **Visual parity** — `?capture=1&accum=48&size=1280x720&t=0.55` against
   `raytracer.exe --capture 1 --accum 48 --size 1280x720`, with the criteria written down *before*
   the comparison (§9.3 below) and `web/png_check.py` as the independent decoder.
4. **Adaptive behaviour** — a run whose frames genuinely miss the target, with the decision trace in
   the console and the resulting stable state, adapter named.
5. **Native regression** — `.\build.ps1`, then `build\bin\raytracer.exe --autoexit 6`, an empty
   `git diff --name-only bc2c422..HEAD -- src shaders tools .github build.ps1 CMakeLists.txt assets`,
   and `git log --oneline -5`.

### 9.3 The still criteria, fixed before measuring

Inherited unchanged from the run that passed them (`correctness_failures=0`,
`correctness_verdict=pipeline_matches_native`), so the new location is held to the same bar rather
than to a new, easier one. Reference: `web/refs/native_still48_t0.55.png` and the three native
feature dumps, all taken at the exe's own capture time t=0.55.

| id | criterion |
|---|---|
| G1 | depth within 4/255 on >= 99.0 % of pixels; no-feature-surface (sky) mask agreement >= 98.0 % |
| G2 | albedo within 12/255 on >= 95 %; albedo mean abs difference <= 6 |
| G3 | normal within 12/255 on >= 95 % |
| S1 | floor band (x 0.30-0.70, y 0.84-0.96) mean luminance within 6 % |
| S2 | whole frame within 16/255 on >= 70 % |
| S3 | floor band within 16/255 on >= 85 % |
| S4 | whole-frame luminance within 8 % |

Recorded baseline to beat or match, from `web/demo-results.txt`:
`cmp_still_mean_abs_diff=0.66`, `cmp_still_within_16=0.9976`,
`cmp_still_band_rel_luma=0.0004`, `cmp_still_band_within=0.9834`, `cmp_sky_mask_agree=1.0000`,
albedo mean abs diff 0.05, normal mean abs diff 0.08. A worse number here is a bug to explain, not a
new baseline.

### 9.4 Measured against, on 2026-11-13: the exe's own accumulated still is not reproducible

That baseline is kept as the comparison because the alternative is meaningless. Two runs of the
identical command `raytracer.exe --capture 1 --accum 48 --size 1280x720` on the same machine produce
PNGs that are **not** the same bytes, and they differ from each other by more than this build differs
from either:

| pair compared | mean worst-channel diff | within 16/255 | floor band within | floor band luma |
|---|---|---|---|---|
| native run A vs native run B | 2.73 | 0.9753 | 0.8369 (fails S3 on its own) | 0.51 % |
| native run C vs native run D | 3.12 | 0.9732 | 0.8334 (fails S3 on its own) | 0.00 % |
| native run A vs the stored reference | 0.94 | 0.9952 | 0.9660 | 0.06 % |
| **this build vs the stored reference** | **0.66** | **0.9976** | **0.9833** | **0.04 %** |
| this build vs native run A | 0.98 | 0.9953 | 0.9668 | 0.10 % |

Every row is `browser/tools/imgcmp.py`, which decodes both files with `web/png_check.py`'s
standard-library PNG reader, so no part of these numbers comes from the browser that drew one of them.
The mean is the statistic the recorded baseline used - the mean over pixels of the worst of the three
channel differences, which is what `imageio.js compareImages` computes - and on the pair the baseline
covers, the page's own number and this tool's agree: 0.66 against 0.6573, 0.9976 against 0.9976, 0.04 %
against 0.04 %.

Single-frame dumps *are* reproducible. Fresh `--gbuf depth`, `--gbuf albedo` and `--gbuf normal`
captures from this run hash identically to the stored ones (`AA61E466…`, `F382A545…`, `0B356FCD…`) and
compare at mean difference 0.0000. Four runs of the 48-frame beauty still produced four different files
(`CA51B9D3…`, `706D1562…`, `C9B46144…`, `90BAA168…`). This build's own 48-frame still is byte-identical
between runs (`45FFAF48…` = `45FFAF48…`), which is the one place where the web build is *more*
reproducible than the exe.

| channel | this build vs the stored native dump | criterion | verdict |
|---|---|---|---|
| depth | 0.0703 mean worst-channel, 1.0000 within 4/255, no-feature-surface mask agreement 1.0000 | G1: >= 99 % within 4, sky agreement >= 98 % | PASS |
| albedo | 0.0525 mean worst-channel, 1.0000 within 12/255 | G2: >= 95 % within 12, mean <= 6 | PASS |
| normal | 0.0776 mean worst-channel, 1.0000 within 12/255 | G3: >= 95 % within 12 | PASS |
| still | 0.6573 mean worst-channel, 0.9976 within 16/255, floor band 0.9833 within, 0.04 % luminance | S1-S4 | PASS |

So the accumulated-still comparison is made against the stored reference - the same reference the
previous attempt's numbers were made against - and against nothing else, because any other choice
measures the exe's run-to-run variation rather than the port. Judged that way the numbers are unchanged
from the baseline in §9.3, which is the result the brief asked for: the property survived the move, and
the numbers did not drift. Where a number *did* move - the 0.98 against fresh native run A, against the
0.66 against the stored reference - the reason is the row above it: that same fresh run differs from
the stored reference by 0.94.


## 10. What this build has been measured to do

All of it headless Edge 157 on `--headless=new --enable-unsafe-webgpu --no-sandbox`, driven by
`browser/tools/run_page.mjs` over the DevTools protocol, with the browser's own console messages
captured to a file. The adapter for every run below: `{"vendor":"nvidia","architecture":"blackwell",
"isFallbackAdapter":false,"subgroupMinSize":32,"subgroupMaxSize":32}`, read through `adapter.info`; the
calibration kernel of §7 put its 2147483648 FLOP at **2.50 / 3.00 / 3.10 / 3.30 ms in four runs of the
same build = 859 / 716 / 693 / 651 GFLOP/s**, a 1.36x spread on one card in one browser, which is worth
knowing before anyone reads a timing signature as an identity. `adapter.device` and
`adapter.description` come back empty from every route this browser offers, which is why the kernel
price is printed next to them.

**The filter cannot be timed at start-up on this machine, and the banner says so.** The price block
measures each pass against an empty-grid dispatch floor - 0.90 ms at 1280x720 - and the SVGF chain's net
came out at 0.03 ms for the temporal pass plus the five a-trous iterations together, which is the round
trip, not the work:

```
BOUND, NOT A MEASUREMENT: denoise_temporal, denoise_atrous_x5, present, accum_copy each came out under
twice the 0.90 ms dispatch floor, so all that is known about any of them is that it costs less than
about 1.80 ms; the controller learns their real prices from split-probe frames while it runs
filter (temporal + 5 a-trous) 0.03 ms = 0.04 ms per megapixel traced - which is the floor showing
through, not a price: this machine filters inside one round trip at this size
web: controller seeded with trace 0.00637 ms per kilopixel per sample (measured on this machine) and
filter 1.00 ms per megapixel (kept from the probe, unmeasurable at this size); the probe seed was
0.00350 / 1.00
```

So the trace price quoted at start-up is a measurement and the filter price is not, and the lines say
which is which rather than printing a default as though it had been measured here. What the filter
actually costs on this card only shows up in the controller's own in-run probes, and those readings move
around: the start-of-run line read `filter 1.84 ms per megapixel` in the 240 fps run and the model
carried 3.58 rising to 19.45 as it repriced on probe frames with the filter off, while a 60 fps run sat
at 4.36-4.56. Treat the recorded historical figure of 4.1-5.3 ms per megapixel as being inside that
spread on this card, and remember that every one of those numbers includes a CPU-GPU round trip the
desktop app's GPU timestamps do not.

| what | measured |
|---|---|
| frames advance | four canvas-readback SHA-256s about 0.73 s apart, all different: frame 57 `1063123447db5b6f…`, frame 100 `76477f277f0dbbc0…`, frame 142 `07510b1a9eea8ef1…`, frame 184 `b90444340e1d1127…`; the pixel at (640,360) read `[68,43,62]`, `[163,81,88]`, `[190,107,115]`, `[38,57,95]` in those four |
| the screen shows the frame that was rendered | the pair taken inside frame 186: `smoke_present.png` and `smoke_canvas.png`, both 1246x616, mean luminance 137.4 and 137.4; `png_check.py --pair` -> 85696 of 85696 sampled canvas pixels equal the presentation texel they were scaled from (**100.00 %**), `PNG_CHECK result=content`; on the canvas alone, 0 duplicated columns of 1245 and a longest run of 0 px, so nothing is being clamped or smeared |
| still parity | §9.3 criteria all PASS: still 0.66 mean / 99.76 % within 16 / floor band 0.04 % apart and 98.34 % within, depth 0.07 and 100 % within 4, albedo 0.05 and 100 % within 12, normal 0.08 and 100 % within 12, no-feature-surface mask agreement 100 %, `correctness_verdict_* = pipeline_matches_native`, 0 failures (§9.4) |
| `N` | denoiser `true -> false`, title `…1246x616 - 1 spp - WebGPU pipeline - (N) Denoise OFF - (ESC) Quit`, console `denoise OFF (pressed N)`, `denoiseUserToggled=true` so the controller cannot take it back |
| `W` | `freeCam=true` with blend 1, camera `[0,0,0] -> [3.373,3.354,0.935]`, canvas hash `fe6942409991… -> 33317ae53d6d…`, the pixel at (700,420) `(32,41,63) -> (51,80,125)`, console `camera: free look (WASD to move, mouse to look; idle 10 s hands control back)` |
| 10 s idle | `freeCam=false`, blend eased down through 0.44, console `camera: idle for 10 s, easing back to the orbit` |
| `Alt+Enter` | the browser granted it here: `fullscreen=entered`, `document.fullscreenElement=true`, and the page printed `window: fullscreen (800x600)` then `resize -> 800x558 (trace 736x513)` with the size the browser actually gave it - headless Edge reports a virtual 800x600 screen, which is exactly why the line prints what happened rather than what was asked for |
| `ESC` | frame counter 805 -> 806 -> 806 (frozen, `quit=true`), title `Vulkan Ray Tracer - quit (ESC)` |
| mouse look | three routes, all measured. **Granted lock** (what this browser does, even headless): 40 px of net horizontal travel turned the camera 0.08800 rad and 50 px of vertical travel pitched it 0.11000 rad - 0.0022 per pixel, the exe's gain to the digit (`frame.cpp:1290-1291`) - with pitch inside the exe's ±1.45 clamp and the frame counter moving throughout (65 -> 109). **Refusal rehearsed** with `?testnolock=1`, which logs that the page forced it: `locked=false refused=true` and the same 0.08800 / 0.11000 through the movement fallback. **Refusal with `?mouselook=strict`**: `dyaw=0.000000`, the mouse deliberately inert, and the page logged `web: pointer lock refused (forced by ?testnolock=1) - and ?mouselook=strict means the mouse will not look at all; WASD still moves` |
| resize | 1024x640, 700x500, 1280x720: the canvas becomes exactly that size, the frame after each one differs from the frame before it (`c13ebb6f49fd -> f985117817c2`, `-> 92626f6caabd`, `-> c23ecc3c89c6`), and the title names the new size each time (`1024x640 trace 1024x640`, `700x500 trace 700x500`, `1280x720 trace 1280x720` at scale 1.00) |
| adaptive | `?targetfps=240` on this adapter: `quality decision: scale 1.00 -> 0.40 (frame 10.0 ms measured, budget 1.5 ms)`, `quality decision: denoise off (late fraction 1.00, scale 0.40, spp 1)`, `web: CONTROLLER EXHAUSTED: cannot reach 240 fps: 1.5 ms budget against 4.3 ms at 1 spp, scale 0.40 (the floor), no denoiser`, then spp 1 / scale 0.40 / denoise off unchanged over 6 s with the frame counter still moving |
| native regression | `build.ps1` rebuilt, `raytracer.exe --autoexit 6` -> `auto-exit after 6.0 s: 354 frames (58.9 fps average) \| frame interval 9.1..17.5 ms, mean 16.9, 0 over 20 ms`, `git diff --name-only bc2c422..HEAD -- src shaders tools .github build.ps1 CMakeLists.txt assets` empty |


### 10.2 Measured 2026-11-13 for `P`, `Up`/`Down` and the status overlay

| item | evidence |
|---|---|
| `P` / resume | `tools/run_page.mjs --scenario features` (trusted key events): title `Vulkan Ray Tracer - 60 fps - 1246x616 trace 523x259 - 1 spp - WebGPU pipeline - (N) Denoise ON - PAUSED - (ESC) Quit`; **the scene clock 1.02 -> 1.02 -> 1.02 s across 2 s of frames** while the **accumulation counter climbed 1 -> 24 -> 172** and the frame counter went 131 -> 279 at 58 fps. Second `P`: clock 2.62 -> 3.82, `PAUSED` gone, accumulation 173 -> 1 -> 1 (the windowed cap restored) |
| `ArrowUp`/`ArrowDown` | `1 -> 3` from two presses with `sppUser=true` and the title following (` - 3 spp - `); clamped at both ends - `spp=1` after six `ArrowDown`, `spp=16` of max 16 after 22 `ArrowUp` - the clamp logged (`samples 16 held`) and the overlay naming it `16 spp (user)` |
| samples after a manual change | `--scenario pin --query '?targetfps=8&denoise=0'`, a budget the frames sit well inside: unasked the controller moved **spp 1 -> 7** (52.8 ms against a 90 ms budget); one `ArrowDown` pinned it at **6** and over the next 25 s / 363 frames it stayed **6 -> 6** at 73.2 ms with the ceiling still 16 and `withinBudget=true` - the same budget, now refused. `scale 1` and the filter stayed its to move (`denoiseUser=false`), `gpu_errors=0` |
| the overlay | `browser/evidence/overlay_page.png` and `overlay_page_no_status.png`: two full-page screenshots of **one frame** (loop stopped with `__ray.freeze`, unfrozen after: frame 8 -> 35, both still showing the render at mean luma 139.8 / 137.5), decoded outside the browser by `tools/png_read.mjs` - **37460 pixels differ, bounding box x 8..899, y 8..49 of a 1246x616 page, mean channel delta 56.2**: text in the top-left and nowhere else. Canvas box identical shown and hidden (1246x616) and document scroll 1246x616 = the viewport; `position: fixed`, `pointer-events: none`, monospace, `rgba(0,0,0,0.26)` plus text shadow, `activeElement=BODY` after keys and clicks, and a plain click still takes pointer lock (`locked false -> true` at 623,308) |
| nothing regressed | `--scenario smoke`: four canvas readbacks all different (`f4db15ff…`, `e09e442a…`, `202c66b4…`, `633fecdc…`), frame 13 -> 191, canvas 137.41 against presentation texture 137.49 mean luma, `gpu_errors=0`, `console_errors=0`, title in the exe's format, controller holding **60 fps at scale 0.841, 1 spp, denoiser on, not exhausted, no flips** (10.42 ms measured against a 9.83 ms budget). `--scenario still`: `correctness_failures_still=0`, `correctness_verdict_still=pipeline_matches_native`, `run_verdict=clean`, mean 0.66, within-16 0.9976, floor band 0.0004 relative luminance and 0.9834 within - the §9.3 baseline unchanged |
| a bug the still run caught | the comparison used to end `fatal: start-up: ReferenceError: failures is not defined` (`app.js` assigned to an undeclared name inside a module, which is strict): the S1-S4 lines printed and passed, but `correctness_failures_still` and `correctness_verdict_still` were never posted and the run titled its own verdict `NOT_CLEAN`. Fixed here, which is why the two metrics in the row above exist at all |

### 10.1 Things the browser did that the checklist now records

* **`ESC` inside browser fullscreen never reaches the page.** The browser consumes it to leave
  fullscreen. Measured: with `document.fullscreenElement` set, the frame counter stopped (the tab went
  hidden, see next) and `quit` stayed `false`. So the key test runs ESC before any fullscreen test, and
  the fullscreen test is a separate run. §2's ESC row is the behaviour when the page is not in the
  browser's own fullscreen.
* **A hidden page stops the frame loop.** `requestAnimationFrame` is the loop's clock and the browser
  stops calling it for a page it considers not visible; the desktop app keeps rendering an unfocused
  window. The page logs `web: the page is now hidden - the browser stops calling requestAnimationFrame
  …` on `visibilitychange` so a frozen frame counter is attributable instead of mysterious.
* **`copyExternalImageToTexture` from a WebGPU canvas is not dependable.** Taken between frames it
  sometimes returns an all-zero buffer, and on some flag sets it fails outright with
  `[Invalid Texture] … CopyTextureForBrowser` - with `--use-angle=d3d11` added to the launch flags it
  returned empty *every* time, which is a fact about the flags, not about the renderer. So: the copy is
  taken inside a validation error scope and counted apart from the run's error total; an empty copy is
  retried and the retry count is reported; and the pair for `png_check.py --pair` is read **inside** the
  frame that was just blitted, because a frame boundary between the two reads, or pausing the loop to
  make the reads, both produce a canvas that is not the frame. Pausing is measurably the wrong answer:
  paused, the copy came back cleared while the same frame copied from inside the loop agreed with the
  presentation texture at 100 %.
* **The trace target is not rounded up to a whole number of 8x8 workgroups**, as the first attempt did.
  Every kernel bounds-checks its thread (`trace.wgsl:650`, `svgf_temporal.wgsl:65`,
  `svgf_atrous.wgsl:85`, `present.wgsl:68`), so `trace %ux%u` in the title is the window at the live
  scale - 1246x616 on a 1246x616 canvas, not 1248x616.
* **The controller's filter price wanders while the filter is off.** In the exhausted run above
  `filterPerMegaPx` moved 3.58 -> 19.45 ms/MP across probe frames with the denoiser off at scale 0.40.
  It changes nothing while the controller is exhausted (no further moves are made), but it is the
  ported controller's own behaviour and it is recorded here rather than smoothed out of the report.

