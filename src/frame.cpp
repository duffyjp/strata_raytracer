// frame.cpp - swapchain, present pass, the windowed loop, and headless PNG capture.
#include "app.h"
#include "generated/shaders_spv.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <chrono>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <direct.h>
#include <thread>
#include <algorithm>

// -------------------------------------------------------------------------------------------------
// Present pipeline
static VkShaderModule makeModule(App* app, const uint32_t* code, size_t words, const char* overrideName) {
    static std::vector<uint8_t> keep;
    keep.clear();
    if (overrideName && app->pakLoaded && app->pak.read(overrideName, &keep) && keep.size() > 4) {
        code = reinterpret_cast<const uint32_t*>(keep.data());
        words = keep.size() / 4;
    }
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = words * 4;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(app->device, &ci, nullptr, &m));
    return m;
}

void createPresentPipeline(App* app) {
    VkShaderModule vs = makeModule(app, shaders::kPresentVert, shaders::kPresentVertWords,
                                   "shaders/present.vert.spv");
    VkShaderModule fs = makeModule(app, shaders::kPresentFrag, shaders::kPresentFragWords,
                                   "shaders/present.frag.spv");

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_FALSE;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &ds;
    ci.layout = app->presentLayout;
    ci.renderPass = app->renderPass;
    ci.subpass = 0;
    VK_CHECK(vkCreateGraphicsPipelines(app->device, app->pipelineCache, 1, &ci, nullptr,
                                       &app->presentPipeline));
    vkDestroyShaderModule(app->device, vs, nullptr);
    vkDestroyShaderModule(app->device, fs, nullptr);
}

// -------------------------------------------------------------------------------------------------
void createRenderPass(App* app) {
    // The attachment format must be the swap chain's real format: with VK_FORMAT_UNDEFINED the pass
    // still clears, but every draw into it is dropped, which looks exactly like a black window.
    if (app->swapFormat == VK_FORMAT_UNDEFINED)
        fail("createRenderPass before the swap chain chose a format");
    VkAttachmentDescription color{};
    color.format = app->swapFormat;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference ref{};
    ref.attachment = 0;
    ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = 1;
    ci.pAttachments = &color;
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    ci.dependencyCount = 1;
    ci.pDependencies = &dep;
    VK_CHECK(vkCreateRenderPass(app->device, &ci, nullptr, &app->renderPass));
}

bool createSwapchain(App* app, uint32_t width, uint32_t height) {
    VkSurfaceCapabilitiesKHR caps{};
    VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(app->physicalDevice, app->surface, &caps);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) return false;
    VK_CHECK(r);

    if (caps.currentExtent.width != 0xFFFFFFFFu) {
        width = caps.currentExtent.width;
        height = caps.currentExtent.height;
    }
    width = std::min(std::max(width, 16u), caps.maxImageExtent.width);
    height = std::min(std::max(height, 16u), caps.maxImageExtent.height);

    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(app->physicalDevice, app->surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(app->physicalDevice, app->surface, &n, fmts.data());
    if (fmts.empty()) fail("surface reports no formats");

    VkSurfaceFormatKHR chosen = fmts[0];
    const VkFormat wanted[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                               VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB};
    bool found = false;
    for (VkFormat wf : wanted) {
        for (const auto& sf : fmts)
            if (sf.format == wf) { chosen = sf; found = true; break; }
        if (found) break;
    }
    app->swapFormat = chosen.format;

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = app->surface;
    ci.minImageCount = imageCount;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = {width, height};
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                          ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                          : caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    // FIFO is vsync: the loop waits for the display, which is what makes motion even. When asked to
    // run without it, mailbox is preferred over immediate because it still never tears.
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (!app->cfg.vsync) {
        uint32_t pmN = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(app->physicalDevice, app->surface, &pmN, nullptr);
        std::vector<VkPresentModeKHR> pms(pmN);
        vkGetPhysicalDeviceSurfacePresentModesKHR(app->physicalDevice, app->surface, &pmN,
                                                  pms.data());
        for (VkPresentModeKHR m : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR})
            if (std::find(pms.begin(), pms.end(), m) != pms.end()) { ci.presentMode = m; break; }
    }
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = VK_NULL_HANDLE;

    VkSwapchainKHR sc = VK_NULL_HANDLE;
    r = vkCreateSwapchainKHR(app->device, &ci, nullptr, &sc);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) return false;
    VK_CHECK(r);
    app->swapchain = sc;
    app->swapExtent = {width, height};

    n = 0;
    vkGetSwapchainImagesKHR(app->device, sc, &n, nullptr);
    app->swapImages.resize(n);
    vkGetSwapchainImagesKHR(app->device, sc, &n, app->swapImages.data());
    app->swapViews.resize(n);
    for (size_t i = 0; i < n; ++i) {
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = app->swapImages[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = app->swapFormat;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(app->device, &vi, nullptr, &app->swapViews[i]));
    }
    return true;
}

void createFramebuffers(App* app) {
    app->framebuffers.assign(app->swapViews.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < app->swapViews.size(); ++i) {
        VkFramebufferCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        ci.renderPass = app->renderPass;
        ci.attachmentCount = 1;
        ci.pAttachments = &app->swapViews[i];
        ci.width = app->swapExtent.width;
        ci.height = app->swapExtent.height;
        ci.layers = 1;
        VK_CHECK(vkCreateFramebuffer(app->device, &ci, nullptr, &app->framebuffers[i]));
    }
}

void destroySwapchainObjects(App* app) {
    vkDeviceWaitIdle(app->device);
    for (VkFramebuffer fb : app->framebuffers)
        if (fb) vkDestroyFramebuffer(app->device, fb, nullptr);
    app->framebuffers.clear();
    for (VkImageView v : app->swapViews)
        if (v) vkDestroyImageView(app->device, v, nullptr);
    app->swapViews.clear();
    app->swapImages.clear();
    if (app->swapchain) {
        vkDestroySwapchainKHR(app->device, app->swapchain, nullptr);
        app->swapchain = VK_NULL_HANDLE;
    }
}

// -------------------------------------------------------------------------------------------------
static void applyHdrSize(App* app, uint32_t swapW, uint32_t swapH) {
    float s = app->cfg.autoQuality ? app->liveScale : app->renderScale;
    recreateHdrTargets(app, uint32_t(swapW * s), uint32_t(swapH * s));
    for (int i = 0; i < kFramesInFlight; ++i) writeTraceDescriptors(app, i);
    app->samplesAccumulated = 0;
}

// -------------------------------------------------------------------------------------------------
// Adaptive quality.
//
// The CPU cannot see how long the GPU spent: while the loop is vsync-paced every frame looks like
// 16.7 ms. Timestamp queries bracketing the command buffer give the real number without stalling,
// and cost is close to proportional to (pixels x samples per frame), which makes the next choice a
// prediction rather than a guess. Resolution is held at the window size whenever one sample fits,
// and the leftover budget goes into samples per pixel; the trace target only shrinks if a single
// sample cannot make the frame.
static const int kStaleFramesAfterChange = 3;  // measurements still reflecting the old settings
static const int kAdjustEveryFrames = 12;      // re-decide about five times a second
static const double kDeadband = 0.72;          // between 72% and 104% of budget, change nothing

// What the display will actually deliver. Sizing the frame budget for 60 Hz when the panel runs at
// 58 or 59 makes it slightly too big on every single frame: the number in the title bar still looks
// fine, but the delivery is consistently on the edge, and that is felt as roughness.
static double effectiveFps(const SceneConfig& c)
{
    double want = double(c.targetFps);
    if (c.detectedRefresh >= 20.0 && c.detectedRefresh <= 400.0) want = std::min(want, double(c.detectedRefresh));
    return want > 0.0 ? want : 60.0;
}

// One knob for "this machine cannot hold the frame": the bundles only move the ceiling and how much
// of each frame is left spare, so samples and supersampling keep adapting inside a smaller box.
static void applyQualityMode(SceneConfig& c)
{
    if (c.qualityMode == "balanced") {
        c.qualityHeadroom = 0.34f; c.maxRenderScale = 1.25f;
        c.baseSamplesPerFrame = 2; c.maxSamplesPerFrame = 8;
    } else if (c.qualityMode == "performance") {
        c.qualityHeadroom = 0.40f; c.maxRenderScale = 1.0f; c.minRenderScale = 0.5f;
        c.baseSamplesPerFrame = 1; c.maxSamplesPerFrame = 4;
    } else if (c.qualityMode == "off") {
        c.autoQuality = false;
    }
}

void createTimingQueries(App* app) {
    if (app->props.limits.timestampPeriod <= 0.f) {
        logMsg("  timing: this device reports no timestamp period; quality stays fixed\n");
        return;
    }
    VkQueryPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    ci.queryCount = kFramesInFlight * 2;
    if (vkCreateQueryPool(app->device, &ci, nullptr, &app->timeQueries) != VK_SUCCESS) {
        app->timeQueries = VK_NULL_HANDLE;
        logMsg("  timing: query pool unavailable; quality stays fixed\n");
        return;
    }
    app->timingSupported = true;
}

void invalidateQuality(App* app) {
    // Only the smoothed total is thrown away. The per-sample cost stays: it is defined per kilopixel
    // per sample, so it is still valid after a sample-count change or a resize, and keeping it is
    // what lets the controller predict instead of relearning from scratch every time.
    app->gpuMsAvg = 0.0;
    app->settleFrames = 0;
    app->overStreak = 0;
    app->underStreak = 0;
    app->lateFramesWindow = 0;
    app->framesWindow = 0;
    memset(app->dtBins, 0, sizeof(app->dtBins));
    app->qualityChanged = true;
}

// No in-flight frame may still hold a view of the accumulation target being replaced. Only the
// *other* slot can be in flight: the loop has already waited on this slot's fence and reset it, so
// waiting for both fences here would block forever on a submit that has not happened yet.
// The fence is deliberately left signalled - the loop resets fences itself, and clearing one here
// would strand its next iteration waiting for a frame that has not been submitted yet.
static void drainInFlight(App* app) {
    const VkFence other = app->fence[1 - app->slot];
    vkWaitForFences(app->device, 1, &other, VK_TRUE, ~0ull);
}

static void setLiveScale(App* app, float s) {
    if (fabsf(s - app->liveScale) < 0.02f) return;
    app->liveScale = s;
    drainInFlight(app);

    // Retire the current accumulation targets by moving them aside instead of destroying them. The
    // frame that follows the change traces into a new target while the screen keeps showing the last
    // complete image, so adjusting quality cannot present a blank frame.
    for (int i = 0; i < kFramesInFlight; ++i) {
        if (app->holdPending[i]) destroyImage(app, &app->hold[i]);
        app->hold[i] = app->hdr[i];
        app->holdPending[i] = app->hold[i].image != VK_NULL_HANDLE;
        app->holdShown[i] = false;
        memset(&app->hdr[i], 0, sizeof(GpuImage));
    }

    const double t0 = glfwGetTime();
    glfwPollEvents();
    applyHdrSize(app, app->swapExtent.width, app->swapExtent.height);
    glfwPollEvents();
    app->lastChangeCostMs = (glfwGetTime() - t0) * 1000.0;
    invalidateQuality(app);
    ++app->qualityChanges;
    app->qualityChanged = true;
}

// Alt+Enter, as Windows users expect: fullscreen on the primary monitor's own mode, and back to the
// window exactly where it was.
void toggleFullscreen(App* app) {
    GLFWmonitor* mon = glfwGetPrimaryMonitor();
    const GLFWvidmode* vm = mon ? glfwGetVideoMode(mon) : nullptr;
    if (!mon || !vm) return;
    if (app->fullscreen) {
        glfwSetWindowMonitor(app->window, nullptr, app->winRect[0], app->winRect[1],
                             app->winRect[2], app->winRect[3], 0);
        app->fullscreen = false;
    } else {
        glfwGetWindowPos(app->window, &app->winRect[0], &app->winRect[1]);
        glfwGetWindowSize(app->window, &app->winRect[2], &app->winRect[3]);
        glfwSetWindowMonitor(app->window, mon, 0, 0, vm->width, vm->height, vm->refreshRate);
        app->fullscreen = true;
    }
    app->resizePending = true;  // the swap chain follows the new extent
    logMsg("window: %s (%dx%d)\n", app->fullscreen ? "fullscreen" : "windowed",
           vm->width, vm->height);
}

// Taking the camera over must start exactly where the automated one is, or the first frame of free
// look reads as a jump.
static void enterFreeCam(App* app) {
    const float* f = app->orbitF;
    app->pitch = asinf(std::max(-0.999f, std::min(0.999f, f[1])));
    app->yaw = atan2f(f[0], f[2]);
    app->camPos[0] = app->orbitO[0];
    app->camPos[1] = app->orbitO[1];
    app->camPos[2] = app->orbitO[2];
    app->freeCam = true;
    app->cursorTracked = false;
    glfwSetInputMode(app->window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    logMsg("camera: free look (WASD to move, mouse to look; idle 10 s hands control back)\n");
}

void reportGpuFrameTime(App* app, double ms) {
    if (!app->cfg.autoQuality || !app->timingSupported || ms <= 0.0) return;
    if (app->settleFrames < kStaleFramesAfterChange) { ++app->settleFrames; return; }

    const double a = 0.2;
    app->gpuMsAvg = (app->gpuMsAvg <= 0.0) ? ms : (a * ms + (1.0 - a) * app->gpuMsAvg);
    if (++app->settleFrames < kAdjustEveryFrames) return;

    const double frameMs = 1000.0 / effectiveFps(app->cfg);
    // Read the display's cadence and the lateness from one histogram: the refresh period is the
    // interval that occurs most often (smoothed over neighbouring milliseconds, since 16 and 17 ms
    // are the same period), and a frame counts as late once it is past a third of a period beyond
    // that. Frames arriving a whole vsync late are what the eye reads as stutter, and they are
    // entirely invisible in an average frame rate.
    uint32_t histTotal = 0, modeCount = 0, modeMs = 0;
    for (uint32_t i = 2; i < 72; ++i) {
        histTotal += app->dtBins[i];
        const uint32_t pair = app->dtBins[i] + (i + 1 < 72 ? app->dtBins[i + 1] : 0);
        if (pair > modeCount) { modeCount = pair; modeMs = i; }
    }
    // The budget is the requested frame period, never a measured one: the histogram is cleared at
    // each change, so for its first frames the mode can be a short interval, which collapses the
    // budget, cuts the sample count, then recovers and climbs again - a self-sustaining cycle at
    // about the decision rate. The mode is therefore kept only as a diagnostic.
    const double periodMs = frameMs;
    (void)modeMs;
    uint32_t lateCount = 0;
    const uint32_t lateAbove = uint32_t(periodMs * 1.35);
    if (modeCount >= 10)
        for (uint32_t i = lateAbove; i < 72; ++i) lateCount += app->dtBins[i];
    const double lateFrac = histTotal >= 30 ? double(lateCount) / double(histTotal) : 0.0;
    const double kCpuReserveMs = 2.5;  // the loop's own stages, which the GPU timestamp does not see
    const double budget =
        std::max(1.5, periodMs * (1.0 - double(app->cfg.qualityHeadroom)) - kCpuReserveMs);
    // Watchdog. The command buffer can report that it fits while the display still misses vsync, so
    // the real interval gets a vote too: aim lower until the pace recovers, and refuse to climb
    // again until the pace has been clean for three quarters of a second - otherwise the two
    // signals argue and the image flickers in quality.
    // Blame the workload only when the GPU is also near its budget. Missing a 120 fps target on a
    // 60 Hz panel is the display talking, and cutting quality for that would just give a worse
    // picture at the same frame rate.
    // Measured between the CPU loop's own iterations, "lateness" is not what the display shows: with
    // the workload well under the refresh period the loop alternates short and long intervals while
    // presents still land on vsync boundaries. So the histogram informs the target period, but does
    // not drive quality decisions - the average frame time and the GPU's own measurement do.
    const bool nearBudget = app->gpuMsAvg > kDeadband * budget;
    const bool missingVsync = app->cpuMsAvg > periodMs * 1.10 && nearBudget;
    if (missingVsync) app->cleanPaceStreak = 0; else ++app->cleanPaceStreak;
    const double budgetUse = missingVsync ? budget * 0.85 : budget;
    // Kilopixels the trace target would cover at scale 1.0 for this window.
    const double kiloPx1 =
        double(app->swapExtent.width) * double(app->swapExtent.height) / 1000.0;
    if (kiloPx1 <= 0.0 || app->liveSpp <= 0) return;

    const double scale2 = double(app->liveScale) * double(app->liveScale);
    const double unit = app->gpuMsAvg / (kiloPx1 * double(app->liveSpp) * scale2);
    app->msPerSampleKiloPx = (app->msPerSampleKiloPx <= 0.0)
                                 ? unit
                                 : (0.3 * unit + 0.7 * app->msPerSampleKiloPx);
    if (app->msPerSampleKiloPx <= 1e-9) return;

    // Decide on streaks, not on single readings: a frame can be slow for reasons unrelated to the
    // scene (a background process, the compositor), and reacting to one such reading is exactly how
    // a controller ends up oscillating. Inside the band, hold and stop counting.
    if (app->gpuMsAvg > budgetUse * 1.04) { ++app->overStreak; app->underStreak = 0; }
    else if (app->gpuMsAvg < kDeadband * budgetUse) { ++app->underStreak; app->overStreak = 0; }
    else {
        app->overStreak = app->underStreak = 0;
        app->settleFrames = kAdjustEveryFrames;
        return;
    }
    const bool mayReduce = app->overStreak >= 2;
    const bool mayClimb = app->underStreak >= 3;

    // Resolution first: keep the window's native size unless even one sample per pixel is too slow.
    const double sNative = std::max(0.1, double(app->renderScale));
    const double sMin = double(app->cfg.minRenderScale) * sNative;
    // Supersampling is bounded both by the configured multiple and by an absolute pixel count:
    // the accumulation target is RGBA16F, so two slots at 8K would cost hundreds of megabytes.
    const double pixelsNative = double(app->swapExtent.width) * double(app->swapExtent.height);
    double sMax = sNative * std::max(1.0, double(app->cfg.maxRenderScale));
    if (pixelsNative > 0.0) {
        double sPixels = std::sqrt(app->cfg.maxTracePixels / pixelsNative);
        if (sPixels < sMax) sMax = sPixels;
    }
    if (sMax < sMin) sMax = sMin;

    // "Sample units" the budget buys at scale 1.0: one unit is one kilopixel traced with one sample.
    const double S = budgetUse / (app->msPerSampleKiloPx * kiloPx1);

    // Spend them as: samples at the window's own resolution, then supersampling, then samples again.
    int k = int(S / (sNative * sNative));
    if (k < 1) k = 1;
    if (k > app->cfg.baseSamplesPerFrame) k = app->cfg.baseSamplesPerFrame;
    double s = std::sqrt(S / double(k));
    if (s < sMin) s = sMin;
    if (s > sMax) s = sMax;
    int wantSpp = int(S / (s * s));  // floor: never over-promise
    if (wantSpp < 1) wantSpp = 1;
    if (wantSpp > app->cfg.maxSamplesPerFrame) wantSpp = app->cfg.maxSamplesPerFrame;
    float wantScale = float(s);

    const int prevSpp = app->liveSpp;
    const float prevScale = app->liveScale;

    // A change is not free - replacing the accumulation target costs a frame - so require that the
    // one being considered is worth it. Over budget, dropping is mandatory however small the step;
    // under budget, climb only on a difference of two samples or more. That asymmetry is what stops
    // the controller from flapping by one sample on either side of the line.
    // Hysteresis on delivery: cut above 6% late, climb only below 3%. A desktop run of about 2% late
    // is ordinary system noise and must not be mistaken for the workload being too heavy, otherwise
    // the controller refuses to spend anything at all.
    const bool settledPace = !missingVsync && app->cleanPaceStreak >= 45;
    // A scale change is the expensive one - it reallocates the trace target and replays a frame - so
    // it has to be worth it: a fifth of the current scale, not a nudge. Sample count, which costs
    // nothing, still moves in steps of two. A resize is unaffected because it discards the model and
    // the difference it produces is far larger than this.
    const bool scaleDiffers = fabs(double(wantScale) - double(app->liveScale)) >
                              0.2 * double(app->liveScale);
    const bool scaleMove = scaleDiffers && ((wantScale < app->liveScale && mayReduce) ||
                                            (wantScale > app->liveScale && mayClimb && settledPace));
    const bool sppMove = mayReduce
                             ? (wantSpp < app->liveSpp)
                             : (mayClimb && settledPace && wantSpp - app->liveSpp >= 2);

    if (scaleMove) {
        app->liveSpp = wantSpp;
        setLiveScale(app, wantScale);
    }
    if (sppMove && wantSpp != app->liveSpp) {
        app->liveSpp = wantSpp;
        // The displayed image divides by the sample count, so changing it must restart accumulation.
        app->samplesAccumulated = 0;
        invalidateQuality(app);
        ++app->qualityChanges;
        app->qualityChanged = true;
    } else if (!scaleMove) {
        app->settleFrames = kAdjustEveryFrames;  // settled: keep evaluating, change nothing
    }

    if (app->verbose && (prevSpp != app->liveSpp || prevScale != app->liveScale))
        logMsg("  quality: %.1f ms measured -> %d spp at scale %.2f (budget %.1f ms of %.1f, "
               "change cost %.1f ms)\n",
               app->gpuMsAvg, app->liveSpp, app->liveScale, budget, frameMs,
               app->lastChangeCostMs);
}

static void readAndReportGpuTime(App* app, int slotIdx) {
    if (!app->timingSupported) return;
    uint64_t ticks[2] = {0, 0};
    VkResult r = vkGetQueryPoolResults(app->device, app->timeQueries, slotIdx * 2, 2,
                                       sizeof(ticks), ticks, 8, VK_QUERY_RESULT_64_BIT);
    if (r != VK_SUCCESS || ticks[1] <= ticks[0]) return;
    reportGpuFrameTime(app, double(ticks[1] - ticks[0]) *
                                double(app->props.limits.timestampPeriod) / 1e6);
}

static void rebuildAfterResize(App* app) {
    int w = 0, h = 0;
    glfwGetFramebufferSize(app->window, &w, &h);
    if (w < 16) w = 16;
    if (h < 16) h = 16;
    destroySwapchainObjects(app);
    if (!createSwapchain(app, uint32_t(w), uint32_t(h))) return;  // try again next frame
    createFramebuffers(app);
    applyHdrSize(app, app->swapExtent.width, app->swapExtent.height);
    // A different extent means a different pixel count, so yesterday's cost model is wrong here.
    // Re-measure immediately; the controller converges within a few frames.
    invalidateQuality(app);
    app->resizePending = false;
    if (app->verbose) logMsg("resize -> %ux%u (trace %ux%u)\n", app->swapExtent.width,
                             app->swapExtent.height, app->hdrWidth, app->hdrHeight);
}

void createFrameSync(App* app) {
    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = app->queueFamily;
    VK_CHECK(vkCreateCommandPool(app->device, &cpi, nullptr, &app->cmdPool));

    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = app->cmdPool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = kFramesInFlight;
    VK_CHECK(vkAllocateCommandBuffers(app->device, &cai, app->cmd));

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (int i = 0; i < kFramesInFlight; ++i) {
        VK_CHECK(vkCreateFence(app->device, &fci, nullptr, &app->fence[i]));
        VK_CHECK(vkCreateSemaphore(app->device, &sci, nullptr, &app->imageReady[i]));
        VK_CHECK(vkCreateSemaphore(app->device, &sci, nullptr, &app->drawDone[i]));
    }

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.maxLod = 1000.f;
    VK_CHECK(vkCreateSampler(app->device, &si, nullptr, &app->sampler));

    createTimingQueries(app);
}

// -------------------------------------------------------------------------------------------------
// HDR readback (capture mode)
static float halfToFloat(uint16_t h) {
    uint32_t sign = uint32_t(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) bits = sign;
        else {  // subnormal
            exp = 127 - 15 + 1;
            while (!(mant & 0x400u)) { mant <<= 1; --exp; }
            mant &= 0x3FFu;
            bits = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

void readHdrToCpu(App* app, int slotIdx, std::vector<float>* out) {
    const GpuImage& im = app->hdr[slotIdx];
    uint32_t pxBytes = (im.format == VK_FORMAT_R32G32B32A32_SFLOAT) ? 16 : 8;
    VkDeviceSize bytes = VkDeviceSize(im.width) * im.height * pxBytes;

    GpuBuffer dst{};
    createBuffer(app, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, false,
                 &dst);

    VkCommandBuffer cmd = beginOneTime(app);
    transitionImage(app, cmd, const_cast<GpuImage*>(&im), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT);
    VkBufferImageCopy region{};
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {im.width, im.height, 1};
    vkCmdCopyImageToBuffer(cmd, im.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.buf, 1, &region);
    transitionImage(app, cmd, const_cast<GpuImage*>(&im), VK_IMAGE_LAYOUT_GENERAL,
                    VK_ACCESS_TRANSFER_READ_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                    VK_IMAGE_ASPECT_COLOR_BIT);
    endOneTime(app, cmd);

    out->resize(size_t(im.width) * im.height * 4);
    const uint8_t* p = static_cast<const uint8_t*>(dst.mapped);
    for (size_t i = 0; i < size_t(im.width) * im.height; ++i) {
        for (int c = 0; c < 4; ++c) {
            float v = 0.f;
            if (im.format == VK_FORMAT_R32G32B32A32_SFLOAT) {
                memcpy(&v, p + (i * 4 + c) * 4, 4);
            } else if (im.format == VK_FORMAT_R16G16B16A16_SFLOAT) {
                uint16_t h;
                memcpy(&h, p + (i * 4 + c) * 2, 2);
                v = halfToFloat(h);
            } else {
                uint16_t h;
                memcpy(&h, p + (i * 4 + c) * 2, 2);
                v = float(h) / 65535.f;
            }
            (*out)[i * 4 + c] = v;
        }
    }
    destroyBuffer(app, &dst);
}

static float tonemap(float x) {
    const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    float v = (x * (a * x + b)) / (x * (c * x + d) + e);
    return std::min(std::max(v, 0.f), 1.f);
}

static float encodeSrgb(float c) {
    return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(std::max(c, 1e-6f), 1.f / 2.4f) - 0.055f;
}

void writePng(const std::string& path, uint32_t w, uint32_t h, const std::vector<float>& hdr,
              float exposure, float invSamples) {
    std::vector<uint8_t> rgb(size_t(w) * h * 3);
    for (uint32_t y = 0; y < h; ++y) {
        // The ray origin's row 0 is the bottom of the frame; PNG wants the top row first.
        uint32_t srcRow = h - 1 - y;
        for (uint32_t x = 0; x < w; ++x) {
            size_t s = (size_t(srcRow) * w + x) * 4;
            size_t d0 = (size_t(y) * w + x) * 3;
            float vx = float(x) / float(w) - 0.5f;
            float vy = float(y) / float(h) - 0.5f;
            float vign = 1.f - 0.45f * (vx * vx + vy * vy);
            for (int c = 0; c < 3; ++c) {
                // The HDR target holds a running SUM of samples; the window divides by the sample
                // count in the present shader, so the readback path has to do the same.
                float v = hdr[s + c] * invSamples * exposure * vign;
                rgb[d0 + c] = uint8_t(std::min(std::max(encodeSrgb(tonemap(v)), 0.f), 1.f) * 255.f + 0.5f);
            }
        }
    }
    if (!stbi_write_png(path.c_str(), int(w), int(h), 3, rgb.data(), int(w) * 3))
        fail("cannot write %s", path.c_str());
}

// -------------------------------------------------------------------------------------------------
int runWindowed(App* app) {
    std::vector<CpuInstance> instances;
    double last = glfwGetTime();
    double fpsClock = last;
    uint64_t fpsFrames = 0;

    bool escWasDown = false;
    const double startT = glfwGetTime();
    app->liveScale = app->renderScale;
    app->userScaleCap = app->renderScale;
    app->liveSpp = std::max(1, app->cfg.samplesPerFrame);
    applyQualityMode(app->cfg);
    GLFWmonitor* mon = glfwGetPrimaryMonitor();
    const GLFWvidmode* vm = mon ? glfwGetVideoMode(mon) : nullptr;
    if (vm && vm->refreshRate > 0) app->cfg.detectedRefresh = float(vm->refreshRate);
    logMsg("display: %d Hz panel, targeting %.0f fps, vsync %s, quality mode %s\n",
           vm ? vm->refreshRate : 0, effectiveFps(app->cfg), app->cfg.vsync ? "on" : "off",
           app->cfg.qualityMode.c_str());
    invalidateQuality(app);
    // Where the frame time actually goes; makes "is it us or the compositor?" an answered question.
    double accPoll = 0, accPrep = 0;
    double accWait = 0, accAcquire = 0, accRecord = 0, accSubmit = 0, accPresent = 0;
    uint64_t idleSkips = 0, resizeSkips = 0, totalFrames = 0, dtLate = 0;
    float dtMin = 1e9f, dtMax = 0.f, dtSum = 0.f;
    using clk = std::chrono::high_resolution_clock;
    clk::time_point t0 = clk::now();
    auto lap = [&t0]() {
        auto n = clk::now();
        double ms = std::chrono::duration<double, std::milli>(n - t0).count();
        t0 = n;
        return ms;
    };
    app->samplesAccumulated = 0;
    applyHdrSize(app, app->swapExtent.width, app->swapExtent.height);
    double lastLoopT = 0.0;
    bool altEnterWasDown = false, cursorNormal = true;

    while (!glfwWindowShouldClose(app->window)) {
        glfwPollEvents();
        accPoll += lap();
        // The pace the user actually sees, measured separately from the command buffer: if the gap
        // between frames exceeds the deadline, a frame is being missed whatever the timestamps say,
        // because present latency and other processes are invisible inside a command buffer.
        const double nowT = glfwGetTime();
        if (lastLoopT > 0.0 && !glfwGetWindowAttrib(app->window, GLFW_ICONIFIED)) {
            const double itv = (nowT - lastLoopT) * 1000.0;
            if (itv > 0.5 && itv < 100.0)
                app->cpuMsAvg =
                    app->cpuMsAvg <= 0.0 ? itv : (0.1 * itv + 0.9 * app->cpuMsAvg);
        }
        lastLoopT = nowT;
        bool esc = glfwGetKey(app->window, GLFW_KEY_ESCAPE) == GLFW_PRESS;
        if (esc && !escWasDown) break;  // ESC exits, as specified
        escWasDown = esc;
        if (app->autoExitSeconds > 0.0 && glfwGetTime() - startT > app->autoExitSeconds) {
            double el = glfwGetTime() - startT;
            logMsg("auto-exit after %.1f s: %llu frames (%.1f fps average) | frame interval "
                   "%.1f..%.1f ms, mean %.1f, %llu over 20 ms\n",
                   el, (unsigned long long)totalFrames, double(totalFrames) / el, dtMin * 1000.0,
                   dtMax * 1000.0, totalFrames ? 1000.0 * dtSum / double(totalFrames) : 0.0,
                   (unsigned long long)dtLate);
            break;
        }

        int w = 0, h = 0;
        glfwGetFramebufferSize(app->window, &w, &h);
        if (w == 0 || h == 0) {  // minimised: idle without burning the CPU
            ++idleSkips;
            glfwWaitEventsTimeout(0.1);
            continue;
        }
        if (uint32_t(w) != app->swapExtent.width || uint32_t(h) != app->swapExtent.height)
            app->resizePending = true;
        if (app->resizePending) {
            rebuildAfterResize(app);
            continue;
        }

        double now = glfwGetTime();
        float dt = float(now - last);
        last = now;
        if (dt > 0.1f) dt = 0.1f;
        app->simTime += dt;
        // Smooth motion needs more than a good average: a stray 33 ms interval between two 16 ms
        // ones is exactly what the eye reads as a stutter, so the spread is tracked, not the mean.
        if (dt < dtMin) dtMin = dt;
        if (dt > dtMax) dtMax = dt;
        if (dt > 0.020f) ++dtLate;

        // ---- Alt+Enter: fullscreen, as expected on Windows ------------------------------------
        const bool altDown = glfwGetKey(app->window, GLFW_KEY_LEFT_ALT) == GLFW_PRESS ||
                             glfwGetKey(app->window, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS;
        const bool enterDown = glfwGetKey(app->window, GLFW_KEY_ENTER) == GLFW_PRESS ||
                               glfwGetKey(app->window, GLFW_KEY_KP_ENTER) == GLFW_PRESS;
        if (altDown && enterDown && !altEnterWasDown) toggleFullscreen(app);
        altEnterWasDown = altDown && enterDown;

        // ---- WASD takes the camera; ten idle seconds hands it back -----------------------------
        const int fwdKey = (glfwGetKey(app->window, GLFW_KEY_W) == GLFW_PRESS) -
                           (glfwGetKey(app->window, GLFW_KEY_S) == GLFW_PRESS);
        const int sideKey = (glfwGetKey(app->window, GLFW_KEY_D) == GLFW_PRESS) -
                            (glfwGetKey(app->window, GLFW_KEY_A) == GLFW_PRESS);
        if ((fwdKey || sideKey) && !app->freeCam) enterFreeCam(app);
        if (fwdKey || sideKey) app->lastWasd = glfwGetTime();
        if (app->testCamera) {
            // Drives the same state machine from the clock, so the handover can be verified from the
            // log rather than by a human holding keys: free at 2 s, released at 6 s, back at ~16 s.
            const double el = glfwGetTime() - startT;
            if (el > 2.0 && el < 6.0) {
                if (!app->freeCam) enterFreeCam(app);
                app->lastWasd = glfwGetTime();
                app->yaw += 0.5f * float(dt);
                app->camPos[2] += 2.0f * float(dt);
            }
        }
        if (app->freeCam) {
            // Look by re-centring the pointer and reading how far it moved: no callback, and no
            // run-away when the pointer reaches an edge. Centred on the window size, because cursor
            // coordinates are not framebuffer coordinates on a DPI-scaled display.
            int ww = 0, wh = 0;
            glfwGetWindowSize(app->window, &ww, &wh);
            double mx = 0.0, my = 0.0;
            glfwGetCursorPos(app->window, &mx, &my);
            if (app->cursorTracked && ww > 0 && wh > 0) {
                app->yaw -= float(mx - ww * 0.5) * 0.0022f;
                app->pitch -= float(my - wh * 0.5) * 0.0022f;
                app->pitch = std::max(-1.45f, std::min(1.45f, app->pitch));
            }
            if (ww > 0 && wh > 0) glfwSetCursorPos(app->window, ww * 0.5, wh * 0.5);
            app->cursorTracked = true;
            // Movement stays in the horizontal plane - no vertical control yet.
            const float speed = glfwGetKey(app->window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ? 12.f : 4.f;
            const float fx = sinf(app->yaw), fz = cosf(app->yaw);
            app->camPos[0] += (fx * fwdKey + -fz * sideKey) * speed * float(dt);
            app->camPos[2] += (fz * fwdKey + fx * sideKey) * speed * float(dt);
            if (glfwGetTime() - app->lastWasd > 10.0) {
                app->freeCam = false;
                logMsg("camera: idle for 10 s, easing back to the orbit\n");
            }
        }
        // Taking control must feel immediate, handing it back must not be noticeable, so the two
        // directions ease at different rates.
        const float wantBlend = app->freeCam ? 1.f : 0.f;
        const float rate = app->freeCam ? 8.f : 0.7f;
        if (app->camBlend < wantBlend)
            app->camBlend = std::min(wantBlend, app->camBlend + rate * float(dt));
        else if (app->camBlend > wantBlend)
            app->camBlend = std::max(wantBlend, app->camBlend - rate * float(dt));
        if (app->freeCam) cursorNormal = false;
        else if (app->camBlend <= 0.f && !cursorNormal) {
            glfwSetInputMode(app->window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            cursorNormal = true;
            app->cursorTracked = false;
        }
        if (app->verbose && (app->freeCam || app->camBlend > 0.f) && app->frameIndex % 30 == 0)
            logMsg("  cam: %s blend %.2f pos %.1f %.1f %.1f yaw %.2f pitch %.2f\n",
                   app->freeCam ? "free  " : "handing", app->camBlend, app->camPos[0],
                   app->camPos[1], app->camPos[2], app->yaw, app->pitch);

        const double dtMs = dt * 1000.0;
        if (dtMs >= 1.0 && dtMs < 72.0) ++app->dtBins[uint32_t(dtMs)];
        ++app->framesWindow;
        dtSum += dt;

        int cap = std::max(1, app->cfg.accumulationFrames);
        // The camera orbits, so every presented frame is a new image: we trace each frame and only
        // keep accumulating when the scene explicitly asks for several frames per image.
        bool reset = app->samplesAccumulated <= 0 || app->samplesAccumulated >= cap;
        app->samplesAccumulated = reset ? 1 : app->samplesAccumulated + 1;
        bool traceIt = true;

        computeInstances(app, app->simTime, &instances);
        uploadFrameInstances(app, app->slot, instances);

        accPrep += lap();
        VK_CHECK(vkWaitForFences(app->device, 1, &app->fence[app->slot], VK_TRUE, ~0ull));
        VK_CHECK(vkResetFences(app->device, 1, &app->fence[app->slot]));
        // Release the retired target for this slot only now: its fence just completed, so the frame
        // that was displaying it has finished sampling it. This runs before the controller, which
        // may install a fresh one during this same iteration.
        if (app->holdPending[app->slot] && app->holdShown[app->slot]) {
            // A frame that displayed the retired image has now completed: free it and let this slot
            // sample its own live target again.
            destroyImage(app, &app->hold[app->slot]);
            app->holdPending[app->slot] = false;
            app->holdShown[app->slot] = false;
            writeTraceDescriptors(app, app->slot);
        }
        readAndReportGpuTime(app, app->slot);  // this slot's previous frame is finished
        accWait += lap();

        uint32_t imageIndex = 0;
        VkResult ar = vkAcquireNextImageKHR(app->device, app->swapchain, ~0ull,
                                            app->imageReady[app->slot], VK_NULL_HANDLE, &imageIndex);
        if (ar != VK_SUCCESS && app->verbose && app->frameIndex < 8)
            logMsg("  acquire returned %d\n", int(ar));
        if (ar == VK_ERROR_OUT_OF_DATE_KHR || ar == VK_SUBOPTIMAL_KHR) {
            app->resizePending = true;
            VK_CHECK(vkResetFences(app->device, 1, &app->fence[app->slot]));
            continue;
        }
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
            app->resizePending = true;
            continue;
        }
        accAcquire += lap();

        buildFrameCommand(app, app->slot, imageIndex, true, instances, app->simTime, reset,
                          app->samplesAccumulated, traceIt);
        // This frame is the one that displays the retired image for this slot, if there is one; it
        // may only be freed once its fence has passed.
        if (app->holdPending[app->slot]) app->holdShown[app->slot] = true;
        accRecord += lap();

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &app->imageReady[app->slot];
        si.pWaitDstStageMask = &waitStage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &app->cmd[app->slot];
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &app->drawDone[app->slot];
        VK_CHECK(vkQueueSubmit(app->queue, 1, &si, app->fence[app->slot]));
        accSubmit += lap();

        VkPresentInfoKHR pi{};
        pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &app->drawDone[app->slot];
        pi.swapchainCount = 1;
        pi.pSwapchains = &app->swapchain;
        pi.pImageIndices = &imageIndex;
        VkResult pr = vkQueuePresentKHR(app->queue, &pi);
        accPresent += lap();
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) app->resizePending = true;

        // Frame limiter, used when presentation does not pace us. Rendering 112 fps into a 60 Hz
        // display means several frames are produced per refresh and only the last one is ever shown,
        // so each displayed frame is on screen for an uneven number of refreshes - which reads as
        // rough motion even though the fps number looks great. Holding the loop to the panel rate
        // with a high-resolution timer keeps the interval steady without FIFO's queue of pending
        // presents, which is where the extra latency comes from.
        if (!app->cfg.vsync) {
            static HANDLE paceTimer = CreateWaitableTimerExW(
                nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            const double spent = glfwGetTime() - now;
            const double remain = 1.0 / effectiveFps(app->cfg) - spent;
            if (remain > 0.0008) {
                if (paceTimer) {
                    LARGE_INTEGER due;
                    due.QuadPart = -(LONGLONG)(remain * 1e7);  // relative, 100 ns units
                    SetWaitableTimer(paceTimer, &due, 0, nullptr, nullptr, FALSE);
                    WaitForSingleObject(paceTimer, 100);
                } else {
                    std::this_thread::sleep_for(std::chrono::microseconds(int64_t(remain * 1e6)));
                }
            }
        }

        if (app->verbose && app->frameIndex % 60 == 0) {
            double n = double(app->frameIndex + 1);
            logMsg("  per-frame ms: poll %.1f  prep %.1f  wait %.1f  acquire %.1f  record %.1f"
                   "  submit %.1f  present %.1f"
                   "  | fb %dx%d hdr %ux%u swap %ux%u gpu %.1f ms - %d spp @ scale %.2f (changes %llu)\n",
                   accPoll / n, accPrep / n,
                   accWait / n, accAcquire / n, accRecord / n, accSubmit / n, accPresent / n, w, h,
                   app->hdrWidth, app->hdrHeight, app->swapExtent.width, app->swapExtent.height,
                   app->gpuMsAvg, app->liveSpp, app->liveScale,
                   (unsigned long long)app->qualityChanges);
        }

        ++app->frameIndex;
        ++fpsFrames;
        ++totalFrames;
        if (now - fpsClock > 1.0) {
            double fps = double(fpsFrames) / (now - fpsClock);
            char title[192];
            if (app->cfg.autoQuality)
                snprintf(title, sizeof(title),
                         "Vulkan Ray Tracer - %.0f fps - %ux%u trace %ux%u - %d spp - %s - ESC quits",
                         fps, app->swapExtent.width, app->swapExtent.height, app->hdrWidth,
                         app->hdrHeight, app->liveSpp,
                         app->usingComputeFallback ? "ray-query" : "RT pipeline");
            else
                snprintf(title, sizeof(title),
                         "Vulkan Ray Tracer - %.0f fps - %ux%u trace %ux%u - %d spp - %s%s - ESC quits",
                         fps, app->swapExtent.width, app->swapExtent.height,
                         app->hdr[app->slot].width, app->hdr[app->slot].height,
                         app->cfg.autoQuality ? app->liveSpp : app->cfg.samplesPerFrame,
                         app->usingComputeFallback ? "ray-query" : "RT pipeline",
                         app->camBlend > 0.5f ? " - free look" : "");
            glfwSetWindowTitle(app->window, title);
            fpsClock = now;
            fpsFrames = 0;
        }
        app->slot = (app->slot + 1) % kFramesInFlight;
    }
    vkDeviceWaitIdle(app->device);
    return 0;
}

// -------------------------------------------------------------------------------------------------
int runCapture(App* app) {
    // Stills are deterministic: the controller is not involved, and each accumulated dispatch
    // contributes exactly cfg.samplesPerFrame samples, so the two must agree.
    app->liveSpp = std::max(1, app->cfg.samplesPerFrame);
    app->liveScale = app->renderScale;
    std::vector<CpuInstance> instances;
    recreateHdrTargets(app, app->captureWidth, app->captureHeight);
    for (int i = 0; i < kFramesInFlight; ++i) writeTraceDescriptors(app, i);

    _mkdir(app->captureDir.c_str());
    logMsg("capturing %d image(s) x %d samples into %s\\\n", app->captureCount, app->captureAccum,
           app->captureDir.c_str());

    for (int img = 0; img < app->captureCount; ++img) {
        float t = 0.55f + float(img) * 2.35f;  // spread the orbit so the shots differ
        // Start every still from a guaranteed-zero accumulator. Zeroing it in-shader left stale
        // samples of previous stills behind, which appeared as several exposures in one image.
        recreateHdrTargets(app, app->captureWidth, app->captureHeight);
        writeTraceDescriptors(app, 0);
        computeInstances(app, t, &instances);
        uploadFrameInstances(app, 0, instances);

        int samples = 0;
        for (int f = 0; f < app->captureAccum; ++f) {
            bool reset = (f == 0);
            ++samples;
            app->frameIndex = uint64_t(f) + 1;
            buildFrameCommand(app, 0, 0, false, instances, t, reset, samples, true);
            VkSubmitInfo si{};
            si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.commandBufferCount = 1;
            si.pCommandBuffers = &app->cmd[0];
            VK_CHECK(vkQueueSubmit(app->queue, 1, &si, app->fence[0]));
            VK_CHECK(vkWaitForFences(app->device, 1, &app->fence[0], VK_TRUE, ~0ull));
            VK_CHECK(vkResetFences(app->device, 1, &app->fence[0]));
        }

        std::vector<float> hdr;
        readHdrToCpu(app, 0, &hdr);
        char name[256];
        snprintf(name, sizeof(name), "%s\\frame_%03d_t%.2f.png", app->captureDir.c_str(), img, t);
        // The target holds means per frame, so only the frame count is divided out here.
        writePng(name, app->captureWidth, app->captureHeight, hdr, app->cfg.exposure,
                 1.0f / float(std::max(1, samples)));
        logMsg("  wrote %s (%d spp)\n", name, samples);
    }
    vkDeviceWaitIdle(app->device);
    return 0;
}
