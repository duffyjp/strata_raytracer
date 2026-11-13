// main.cpp - entry point, CLI, and start-up sequence.
//
// Run with no arguments for the animated window. See --help for the headless capture mode, which is
// what makes correctness verifiable without anyone watching the window.
#include "app.h"

#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static void printHelp() {
    printf(
        "Vulkan ray tracer - cube / cone / spheres, two orbiting lights, orbiting camera.\n"
        "\n"
        "  raytracer [options]\n"
        "\n"
        "  (no options)         open the window and render; ESC quits; the window is resizable\n"
        "  --capture N          render N stills headless and write them to --out, then exit\n"
        "  --accum S            samples (frames) accumulated per captured still   [48]\n"
        "  --size WxH           capture resolution                                [1280x720]\n"
        "  --out DIR            capture output directory                          [captures]\n"
        "  --pak FILE           asset archive to load instead of searching\n"
        "  --scale F            trace resolution ceiling (0.4 .. 1.0)            [1.0]\n"
        "  --no-auto-quality    pin samples/pixel and scale instead of adapting to the GPU\n"
        "  --quality MODE       burden ceiling: high | balanced | performance | off  [high]\n"
        "                         balanced caps supersampling at 1.25x, performance at 1.0x\n"
        "  --no-vsync           present without waiting for the display (mailbox/immediate)\n"
        "  --denoise            turn the SVGF denoiser on (it is on by default while rendering to the\n"
        "                       window; press N at any time to toggle, the title bar shows which)\n"
        "  --no-denoise         turn it off at start-up instead\n"
        "  --gbuf CH            show what the denoiser sees or produced: albedo | normal | depth |\n"
        "                       denoised | diff (what the filter changed) | variance (its noise estimate)\n"
        "  --filter-z F         denoiser depth edge stopping                  [0.02]\n"
        "  --filter-n F         denoiser normal edge stopping                 [128]\n"
        "  --filter-l F         denoiser brightness edge stopping             [24]\n"
        "  --refresh N          treat the panel as running at N Hz (detected automatically)\n"
        "  --target-fps N       frame rate the adaptive controller aims for       [60]\n"
        "  --test-toggle N      toggle the denoiser after N rendered frames (same path as the N key)\n"
        "  --dump-accum N       during --capture, also write the raw float target after accumulation\n"
        "                       frame N, as <dir>\accum_NNN.bin, for noise analysis\n"
        "  --max-spp N          most samples per pixel per frame it may spend     [8]\n"
        "  --min-scale F        smallest trace resolution it may fall back to     [0.40]\n"
        "  --compute            force the ray-query compute fallback; it fails outright when no adapter\n"
        "                       here offers VK_KHR_ray_query instead of falling back to the other path\n"
        "  --rt                 force the ray-tracing pipeline; it fails outright when no adapter here\n"
        "                       offers VK_KHR_ray_tracing_pipeline. The two flags are mutually\n"
        "                       exclusive, and with neither one the best path the machine actually has\n"
        "                       is picked at start-up - no flag is needed to make it work\n"
        "  --list-adapters      log every adapter, its capabilities and which path each can serve, then\n"
        "                       exit without opening a window: the report to send from a remote machine\n"
        "  --verbose            extra logging (pak contents, buffer sizes)\n"
        "  --help               this text\n");
}

static bool parseSize(const char* s, uint32_t* w, uint32_t* h) {
    return sscanf(s, "%ux%u", w, h) == 2 || sscanf(s, "%ux%u", h, w) == 2;
}

static int realMain(int argc, char** argv) {
    App app{};
    std::string pakPath;
    bool forceCompute = false;
    bool requireRt = false;
    bool cliAutoQuality = true, cliAutoGiven = false;
    float cliTargetFps = 0.f, cliMinScale = 0.f;
    int cliMaxSpp = 0;
    bool cliVsync = true, cliVsyncGiven = false;
    bool cliFullscreen = false;
    bool cliListAdapters = false;  // probe and report every adapter, then exit: no window, no device
    int cliDenoise = 0;
    int cliWinW = 0, cliWinH = 0;  // 0 = not mentioned, 1 = --denoise, -1 = --no-denoise
    int cliFilterDebug = 0;
    float cliRefresh = 0.f;
    std::string cliQuality;

    // A GUI-subsystem process has no console unless it is started from one, so mirror everything
    // to a log file next to the executable.
    {
        char exePath[MAX_PATH] = {0};
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string dir(exePath);
        size_t slash = dir.find_last_of("\\/");
        dir = (slash == std::string::npos) ? std::string(".") : dir.substr(0, slash);
        fopen_s(&g_logFile, (dir + "\\raytracer.log").c_str(), "w");
        if (!g_logFile) fopen_s(&g_logFile, "raytracer.log", "w");
    }

    // A refused command line is answered on stderr, not with a dialog. fail() raises the dialog because
    // this is a GUI-subsystem binary: double-clicked, it has no console, and a start-up failure looked
    // like a window that appeared and vanished. But an argument error can only have been produced by
    // somebody who just typed arguments, and they are looking at a terminal. It also made the flag
    // rules untestable: --rt --compute raised a modal box, so the process sat there until a human
    // dismissed it and never handed back an exit code at all - which is the same trap the headless
    // modes below are excused from. This is turned back off as soon as the arguments have been read, so
    // everything after them - the pak, the device, the window - still answers a failure with the dialog.
    g_headlessMode = true;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) fail("%s needs a value", what);
            return argv[++i];
        };
        if (a == "--help" || a == "-h") { printHelp(); return 0; }
        else if (a == "--capture") app.captureCount = atoi(next("--capture"));
        else if (a == "--autoexit") app.autoExitSeconds = atof(next("--autoexit"));
        else if (a == "--accum") app.captureAccum = atoi(next("--accum"));
        else if (a == "--dump-accum") app.captureDumpFrame = atoi(next("--dump-accum"));  // -2 = every frame
        else if (a == "--size") {
            if (!parseSize(next("--size"), &app.captureWidth, &app.captureHeight))
                fail("--size wants WxH");
        } else if (a == "--out") app.captureDir = next("--out");
        else if (a == "--pak") pakPath = next("--pak");
        else if (a == "--scale") {
            app.renderScale = float(atof(next("--scale")));
            if (app.renderScale < 0.4f) app.renderScale = 0.4f;
            if (app.renderScale > 1.0f) app.renderScale = 1.0f;
        } else if (a == "--no-auto-quality") { cliAutoQuality = false; cliAutoGiven = true; }
        else if (a == "--target-fps") { cliTargetFps = float(atof(next("--target-fps"))); }
        else if (a == "--max-spp") { cliMaxSpp = atoi(next("--max-spp")); }
        else if (a == "--no-vsync") { cliVsync = false; cliVsyncGiven = true; }
        else if (a == "--vsync") { cliVsync = true; cliVsyncGiven = true; }
        else if (a == "--quality") { cliQuality = next("--quality"); }
        else if (a == "--refresh") { cliRefresh = float(atof(next("--refresh"))); }
        else if (a == "--fullscreen") { cliFullscreen = true; }
        else if (a == "--test-camera") { app.testCamera = true; }
        else if (a == "--test-toggle") { app.testToggleFrame = atoi(next("--test-toggle")); }
        else if (a == "--test-give-up") { app.testGiveUpFrame = atoi(next("--test-give-up")); }
        else if (a == "--test-budget") { app.testBudgetMs = atof(next("--test-budget")); }
        else if (a == "--window") { sscanf(next("--window"), "%dx%d", &cliWinW, &cliWinH); }
        else if (a == "--denoise") { cliDenoise = 1; }
        else if (a == "--no-denoise") { cliDenoise = -1; }
        else if (a == "--filter-z") { app.cfg.denoiseSigmaZ = float(atof(next("--filter-z"))); }
        else if (a == "--filter-l") { app.cfg.denoiseSigmaL = float(atof(next("--filter-l"))); }
        else if (a == "--filter-n") { app.cfg.denoiseSigmaN = float(atof(next("--filter-n"))); }
        else if (a == "--gbuf") {
            std::string ch = next("--gbuf");
            cliFilterDebug = 0;
            if (ch == "albedo") cliFilterDebug = 1;
            else if (ch == "normal") cliFilterDebug = 2;
            else if (ch == "depth") cliFilterDebug = 3;
            else if (ch == "denoised") cliFilterDebug = 4;
            else if (ch == "diff") cliFilterDebug = 5;
            else if (ch == "variance") cliFilterDebug = 6;
        }
        else if (a == "--min-scale") { cliMinScale = float(atof(next("--min-scale"))); } else if (a == "--compute") forceCompute = true;
        else if (a == "--rt") requireRt = true;
        else if (a == "--list-adapters") cliListAdapters = true;
        else if (a == "--verbose" || a == "-v") app.verbose = true;
        else fail("unknown option '%s' (try --help)", a.c_str());
    }
    if (forceCompute && requireRt) fail("--compute and --rt are mutually exclusive");
    app.forceRt = requireRt;
    app.forceCompute = forceCompute;
    if (app.captureCount <= 0) app.captureCount = 0;
    // From here on the dialog belongs to the run rather than to the command line: a scripted run (a
    // capture, an adapter report) must never end up sitting behind a box nobody is there to click, and
    // a windowed run that cannot start must never fail silently either.
    g_headlessMode = app.captureCount > 0 || cliListAdapters;

    // --list-adapters is a question about the GPU, not a render, so it runs ahead of the assets and
    // ahead of any window: it then cannot fail for a reason that has nothing to do with the hardware
    // being asked about. Initialise the loader, ask every adapter what it has, write the answer to the
    // log, leave. That ordering is what makes it usable over a network share on a machine nobody is
    // sitting in front of.
    if (cliListAdapters) {
        if (!glfwInit()) fail("glfwInit failed");
        createInstance(&app);
        logAdapterMatrix(&app);
        if (g_logFile) fclose(g_logFile);
        g_logFile = nullptr;  // flushed and released: this file is the whole report
        glfwTerminate();
        return 0;
    }

    // 1. Assets first: the scene file may drive the window size and camera.
    loadScene(&app, pakPath);
    // Command line wins over the archive, so the overrides are applied after it is read.
    if (cliAutoGiven) app.cfg.autoQuality = cliAutoQuality;
    if (cliTargetFps > 0.f) app.cfg.targetFps = cliTargetFps;
    if (cliMaxSpp > 0) app.cfg.maxSamplesPerFrame = cliMaxSpp;
    if (!cliVsync) app.cfg.vsync = false;
    if (cliVsyncGiven) app.cfg.vsync = cliVsync;
    if (!cliQuality.empty()) app.cfg.qualityMode = cliQuality;
    if (cliDenoise > 0) app.cfg.denoise = true;
    if (cliDenoise < 0) app.cfg.denoise = false;
    // A still is the opposite job to a moving image. The filter treats every frame it is handed as
    // an independent estimate and keeps its own history, which is why it turns accumulation off -
    // precisely wrong for a still that is averaging dozens of frames into one clean image. Stills
    // therefore stay unfiltered unless --denoise is named explicitly.
    if (app.captureCount > 0 && cliDenoise == 0 && cliFilterDebug == 0) {
        app.cfg.denoise = false;
        logMsg("stills: denoiser left off (a still averages frames; it does not filter them)\n");
    }
    if (cliFilterDebug > 0) { app.cfg.filterDebug = cliFilterDebug; app.cfg.denoise = true; }
    if (cliRefresh > 0.f) app.cfg.detectedRefresh = cliRefresh;  // overrides the measured panel rate
    if (cliMinScale > 0.f) app.cfg.minRenderScale = cliMinScale;

    // 2. Window (skipped entirely in capture mode).
    int winW = 1280, winH = 720;
    // Debug/repro only: the large-window case cannot otherwise be reproduced from a script, and the
    // per-phase timing that goes with --verbose is the only way to see whether such a frame is slow
    // on the GPU side or on the CPU side.
    if (cliWinW > 0 && cliWinH > 0) { winW = cliWinW; winH = cliWinH; }
    if (app.captureCount == 0) {
        if (!glfwInit()) fail("glfwInit failed");
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
        glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
        glfwWindowHint(GLFW_FOCUSED, GLFW_TRUE);
        // Name a position when the size was named, because leaving it to the window manager lets the
        // desktop choose the size too: with CW_USEDEFAULT, Windows runs its default placement, and
        // default placement shrinks any window larger than the work area down to the work area. Asked
        // for a 3840x2100 window on a 1280x800 desktop - a remote session, a headless box, someone
        // measuring the renderer at a size their panel does not have - the run silently got 1284x781,
        // a ninth of the pixels, and everything downstream read from that: the budget, the cost model,
        // the decisions, the fps. A window that hangs off the edge of the screen is a much better
        // answer to a named size than a window that quietly becomes something else.
        if (cliWinW > 0 && cliWinH > 0)
        {
            glfwWindowHint(GLFW_POSITION_X, app.winRect[0]);
            glfwWindowHint(GLFW_POSITION_Y, app.winRect[1]);
        }
        app.window = glfwCreateWindow(winW, winH, "Vulkan Ray Tracer", nullptr, nullptr);
        if (app.window)
        {
            int gotW = 0, gotH = 0;
            glfwGetWindowSize(app.window, &gotW, &gotH);
            if (gotW != winW || gotH != winH)
            {
                // The window manager sized the window while it was being placed. The maximum track
                // size a window starts with is its monitor's work area, so anything wider or taller
                // than the screen came back shortened no matter what was asked for: 3840x2100 on this
                // machine's 1280x800 remote session came back as 1284x781, an eighth of the pixels,
                // and every number taken through that window - the frame budget, the pixel counts the
                // cost model divides by, the decisions, the fps the run reports - described a render
                // nobody asked for. Pin the track size to the size that was named, resize under the
                // pin, then release it so dragging the border still works. A window hanging off the
                // edge of the screen is the honest answer to a size the screen does not have; a window
                // that quietly becomes something else is not an answer.
                glfwSetWindowSizeLimits(app.window, winW, winH, winW, winH);
                glfwSetWindowSize(app.window, winW, winH);
                glfwSetWindowSizeLimits(app.window, GLFW_DONT_CARE, GLFW_DONT_CARE,
                                        GLFW_DONT_CARE, GLFW_DONT_CARE);
                glfwGetWindowSize(app.window, &gotW, &gotH);
            }
            if (gotW != winW || gotH != winH)
                logMsg("window: asked for %dx%d, given %dx%d\n", winW, winH, gotW, gotH);
        }
        if (!app.window) { glfwTerminate(); fail("could not create the window"); }
        glfwSwapInterval(app.cfg.vsync ? 1 : 0);  // only meaningful if GLFW ever owns the swap chain
        if (cliFullscreen) toggleFullscreen(&app);  // starts on the monitor's own mode
        glfwSwapInterval(1);  // FIFO presentation; the loop is VSync-paced
    } else {
        if (!glfwInit()) fail("glfwInit failed");  // volk/windowing shared init; no window created
    }

    // 3. Vulkan.
    createInstance(&app);
    if (app.window) {
        if (glfwCreateWindowSurface(app.instance, app.window, nullptr, &app.surface) != VK_SUCCESS)
            fail("glfwCreateWindowSurface failed");
    }
    pickPhysicalDevice(&app);
    createDevice(&app);
    logMsg("step: device + queues created\n");
    createFrameSync(&app);
    logMsg("step: command pool, sync objects, sampler\n");

    // 4. Scene contents on the GPU.
    createSceneGpuResources(&app);
    logMsg("step: geometry and acceleration structures\n");
    loadPakTextures(&app);
    logMsg("step: textures\n");
    createDescriptorLayouts(&app);
    createShaderModulesAndPipelines(&app);
    if (app.cfg.denoise && app.usingComputeFallback) {
        // The fallback path never writes the feature images, so filtering there would run on
        // inputs that were never filled in. Disabled rather than silently wrong.
        app.cfg.denoise = false;
        logMsg("denoise: unavailable on the ray-query fallback, disabled\n");
    }
    logMsg("step: %s pipeline ready\n", app.usingComputeFallback ? "compute fallback" : "ray-tracing");

    int rc = 0;
    if (app.captureCount > 0) {
        rc = runCapture(&app);
    } else {
        // Order matters: the render pass and the present pipeline both take the swap chain's
        // format, so the swap chain has to exist first. Creating the pass earlier built it against
        // VK_FORMAT_UNDEFINED, which silently discarded every draw while still clearing.
        int fbw = 0, fbh = 0;
        glfwGetFramebufferSize(app.window, &fbw, &fbh);
        if (fbw < 16) fbw = winW;
        if (fbh < 16) fbh = winH;
        if (!createSwapchain(&app, uint32_t(fbw), uint32_t(fbh)))
            fail("could not create the swapchain");
        createRenderPass(&app);
        createFramebuffers(&app);
        createPresentPipeline(&app);
        logMsg("window %ux%u - drag the border to resize, ESC to quit\n", app.swapExtent.width,
               app.swapExtent.height);
        rc = runWindowed(&app);
    }

    destroyApp(&app);
    glfwTerminate();
    return rc;
}

// Built as a GUI-subsystem binary so double-clicking launches no console window; when started from
// a terminal the parent console is attached, which is what makes --capture usable in scripts.
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* tmp = nullptr;
        if (freopen_s(&tmp, "CONOUT$", "w", stdout) == 0) setvbuf(stdout, nullptr, _IONBF, 0);
        freopen_s(&tmp, "CONOUT$", "w", stderr);
    }

    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> store;
    std::vector<char*> argv;
    if (wargv) {
        for (int i = 0; i < argc; ++i) {
            int len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
            std::string s(len > 1 ? size_t(len - 1) : 0, '\0');
            if (len > 1) WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, &s[0], len, nullptr, nullptr);
            store.push_back(std::move(s));
        }
        LocalFree(wargv);
        for (auto& s : store) argv.push_back(s.data());
    }
    return realMain(int(argv.size()), argv.empty() ? nullptr : argv.data());
}
