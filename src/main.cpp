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
        "  --target-fps N       frame rate the adaptive controller aims for       [60]\n"
        "  --max-spp N          most samples per pixel per frame it may spend     [8]\n"
        "  --min-scale F        smallest trace resolution it may fall back to     [0.40]\n"
        "  --compute            force the ray-query compute fallback\n"
        "  --rt                 require the ray-tracing pipeline (fail if absent)\n"
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
        else if (a == "--min-scale") { cliMinScale = float(atof(next("--min-scale"))); } else if (a == "--compute") forceCompute = true;
        else if (a == "--rt") requireRt = true;
        else if (a == "--verbose" || a == "-v") app.verbose = true;
        else fail("unknown option '%s' (try --help)", a.c_str());
    }
    if (forceCompute && requireRt) fail("--compute and --rt are mutually exclusive");
    app.wantRt = !forceCompute;
    if (app.captureCount <= 0) app.captureCount = 0;

    // 1. Assets first: the scene file may drive the window size and camera.
    loadScene(&app, pakPath);
    // Command line wins over the archive, so the overrides are applied after it is read.
    if (cliAutoGiven) app.cfg.autoQuality = cliAutoQuality;
    if (cliTargetFps > 0.f) app.cfg.targetFps = cliTargetFps;
    if (cliMaxSpp > 0) app.cfg.maxSamplesPerFrame = cliMaxSpp;
    if (cliMinScale > 0.f) app.cfg.minRenderScale = cliMinScale;

    // 2. Window (skipped entirely in capture mode).
    int winW = 1280, winH = 720;
    if (app.captureCount == 0) {
        if (!glfwInit()) fail("glfwInit failed");
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
        glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
        glfwWindowHint(GLFW_FOCUSED, GLFW_TRUE);
        app.window = glfwCreateWindow(winW, winH, "Vulkan Ray Tracer", nullptr, nullptr);
        if (!app.window) { glfwTerminate(); fail("could not create the window"); }
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
