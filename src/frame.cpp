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
// The filter is the first thing this controller gives up, and the last thing it takes back.
//
// It used to go last, on the theory that a clean image is worth more than a couple of milliseconds.
// Measured on this machine that theory is wrong by an order of magnitude: at 3840x2100 a frame costs
// 2.1 ms without the filter and 163 ms with it, so the choice is 58.8 fps or 5.6. The controller
// never found that out, because its cost model had a single term for the whole frame and the
// filter's price hid inside the per-sample constant, and because the escape hatch asked for forty
// consecutive late readings - a streak the oscillation it was meant to cure restarted every time it
// changed anything.
//
// So giving up the filter is no longer conditional on samples and resolution having already been
// cut. Cutting them cannot pay for the filter anyway, and it spends the image to postpone the only
// decision that works. What remains is the user's veto - see toggleDenoise - and a cap on how many
// times the controller may change its mind.
static const int kMaxDenoiseFlips = 3;
// Consecutive late readings before the filter goes. Lateness means the frame arrived late against
// the target the user asked for, so a machine running at 22 fps cannot report itself on time just
// because 45 ms has become the most common frame interval.
static const int kDenoiseGiveUpFrames = 8;

// A frame needs several readings before it means anything: the timestamp query measures one command
// buffer, and one command buffer can be slow because something else on the machine woke up.
static const int kStaleFramesAfterChange = 3;  // measurements still reflecting the old settings
// A quality change rebuilds the accumulation target, the three feature images, the filter's three
// history images, its three output images and its wavelet scratch for both slots - some thirty
// Vulkan allocations, measured at 20 to 140 ms of wall time. None of that is inside the timestamps,
// but it lands on the frames straight afterwards, and a model learned from those describes the
// allocator rather than the renderer. This is how many readings are thrown away for that reason.
static const int kStaleFramesAfterRealloc = 6;
static const int kAdjustEveryFrames = 12;      // weigh the evidence this many readings apart
// Spending up leaves less than the whole budget on the table, because a climb that lands exactly on
// the budget lands on top of the measurement noise too and the next reading then says it is over.
static const double kClimbMargin = 0.86;
static const double kDeadband = 0.72;          // between 72% and 104% of budget, change nothing
// A flip's own cost is invisible from inside the frame it lands in, because the rebuild that comes
// with it happens outside the timestamps. Its price is read this many frames later, once the
// settings on both sides of it have been still for a while.
static const int kFilterCalibFrames = 15;

static void applyHdrSize(App* app, uint32_t swapW, uint32_t swapH) {
    float s = app->cfg.autoQuality ? app->liveScale : app->renderScale;
    // Timed, because it is the one thing the controller must not learn from: this is where thirty
    // Vulkan images are destroyed and rebuilt, none of it inside the GPU timestamps but all of it
    // landing on the frames that follow. Recorded even when the caller times a wider window itself,
    // so that a resize - which reaches here without going through the quality change paths - is
    // accounted for too.
    const double t0 = glfwGetTime();
    recreateHdrTargets(app, uint32_t(swapW * s), uint32_t(swapH * s));
    for (int i = 0; i < kFramesInFlight; ++i) writeTraceDescriptors(app, i);
    app->samplesAccumulated = 0;
    const double cost = (glfwGetTime() - t0) * 1000.0;
    app->lastChangeCostMs = cost;
}

// Turning the filter on or off mid-run is a size change in disguise: the feature images and the
// filter's own history exist only while it is on, so the targets have to be rebuilt. What comes
// back has no history and no accumulated samples, which is the right answer when the pipeline
// underneath the image has just changed - the alternative is showing a blend of two different
// filters as if it were one of them.
// Megapixels the trace target covers right now - what the filter is charged against, since it runs
// over the traced image rather than over the window.
static double tracedMegaPx(const App* app) {
    const double s = double(app->liveScale);
    return double(app->swapExtent.width) * double(app->swapExtent.height) * s * s / 1.0e6;
}

// Turning the filter on or off mid-run is a size change in disguise: the feature images and the
// filter's own history exist only while it is on, so the targets have to be rebuilt. What comes
// back has no history and no accumulated samples, which is the right answer when the pipeline
// underneath the image has just changed - the alternative is showing a blend of two different
// filters as if it were one of them.
static void setDenoiseEnabled(App* app, bool on, const char* why) {
    if (app->usingComputeFallback || app->cfg.denoise == on) return;
    // A flip is the one moment the filter's own cost can be seen: everything else about the frame
    // stays exactly as it was, so the difference between a settled reading on one side and a settled
    // reading on the other is the filter and nothing else. Take the first half of that measurement
    // here, while the old state is still what the smoothed average describes.
    //
    // It only counts if the reading was actually settled. Sampling through an oscillation is how the
    // constant used to be dragged to a value that described neither state.
    app->filterCalArmed = false;
    // Settled means past the readings that still describe the previous settings, not past the whole
    // decision window: the emergency path that gives the filter up runs sooner than that, and a flip
    // nobody measured is a flip whose cost has to be guessed at again next time.
    if (app->timingSupported && app->settleFrames >= kStaleFramesAfterChange + 3 &&
        app->gpuMsAvg > 0.0 && !app->resizePending) {
        app->filterCalBeforeMs = app->gpuMsAvg;
        app->filterCalMegaPx = tracedMegaPx(app);
        app->filterCalBeforeScale = app->liveScale;
        app->filterCalBeforeSpp = app->liveSpp;
        app->filterCalTurningOn = on;
        app->filterCalArmed = app->filterCalMegaPx > 1.0e-4;
    }
    app->cfg.denoise = on;
    const double t0 = glfwGetTime();
    vkDeviceWaitIdle(app->device);
    applyHdrSize(app, app->swapExtent.width, app->swapExtent.height);
    app->lastChangeCostMs = (glfwGetTime() - t0) * 1000.0;
    // The rebuild is the reallocation the controller must not learn from: it spends tens of
    // milliseconds making images that were never measured, and until it is done every reading
    // describes a half-built frame.
    invalidateQuality(app);
    ++app->qualityChanges;
    app->qualityChanged = true;
    logMsg("denoise %s (%s)\n", on ? "ON" : "OFF", why);
}

static void toggleDenoise(App* app) {
    if (app->usingComputeFallback) {
        logMsg("denoise: unavailable on the ray-query fallback\n");
        return;
    }
    // A press of N is a decision, not a suggestion: the controller is told to stop second-guessing
    // it, otherwise a machine that cannot hold the frame rate would immediately switch the filter
    // back off and the key would appear to do nothing. It is final for the rest of the run in both
    // directions, so a user who turned the filter on deliberately cannot be argued with by a
    // controller that has decided the frame is late.
    app->denoiseAutoOff = false;
    app->denoiseUserToggled = true;
    app->denoiseFlips = kMaxDenoiseFlips;
    setDenoiseEnabled(app, !app->cfg.denoise, "pressed N");
}

// -------------------------------------------------------------------------------------------------
// Adaptive quality.
//
// The CPU cannot see how long the GPU spent: while the loop is vsync-paced every frame looks like
// 16.7 ms. Timestamp queries bracketing the command buffer give the real number without stalling,
// and cost is close to proportional to (pixels x samples per frame), which makes the next choice a
// prediction rather than a guess. Resolution is held at the window size whenever one sample fits,
// and the leftover budget goes into samples per pixel; the trace target only shrinks if a single
// sample cannot make the frame. That account is missing one line: the filter does not scale with the
// sample count at all, so it is carried as its own term - see the cost model below.

// The frame rate the run is held to. It is the rate that was asked for, and the panel gets exactly
// one concession: a display running a little below its nominal rate (58 or 59.94 for a "60 Hz" mode)
// makes a budget sized for exactly 60 slightly too big on every single frame - the number in the title
// bar still looks fine, but the delivery is consistently on the edge, and that is felt as roughness -
// so a detected rate close to the target is allowed to trim it toward the panel.
//
// What the panel may not do is redefine the target. What stood here was a std::min with the detected
// rate, and on a machine whose primary display is a virtual one, a min hands the target to whatever
// that device happens to be running. Measured on this one: the session's primary display is the
// Microsoft Remote Display Adapter at 1280x800 @ 32 Hz, so a 60 fps target silently became a 31.25 ms
// frame period and a 19.4 ms budget. Two things followed from that alone. The limiter at the foot of
// the frame loop paces the renderer to this number, so a machine making a 1 ms frame delivered 31.5 fps
// and every frame of the run was late against the target it had been given; and the controller, holding
// more than twice the frame it was entitled to, spent the surplus on supersampling (scale 1.00 -> 2.00)
// and never once concluded that anything had to be given up. Inferring the deadline from the environment
// is the same defect as inferring it from the most common frame interval, one layer up, and it hides in
// the log because the number looks plausible. A panel further away than a fifth of the target is a
// different device, not a slower edition of the one that was asked for, and it does not get to set the
// clock.
static double effectiveFps(const SceneConfig& c)
{
    const double want = c.targetFps > 0.0 ? double(c.targetFps) : 60.0;
    const double panel = double(c.detectedRefresh);
    if (panel >= 20.0 && panel <= 400.0 && panel < want && panel > 0.8 * want) return panel;
    return want;
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
    app->decideFrames = 0;
    app->overStreak = 0;
    app->underStreak = 0;
    app->lateFramesWindow = 0;
    app->framesWindow = 0;
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

// What a frame would cost at a given trace scale and sample count, under the two-term model: the
// trace pays per sample per traced pixel, and the filter pays per traced pixel whatever the sample
// count is. kiloPx1 is the window's own size in kilopixels, so the traced area is kiloPx1*s*s.
// Everything the controller decides is a comparison between one of these and the frame budget.
struct CostModel {
    double msPerSampleKiloPx = 0.00026;  // trace: ms per sample per traced kilopixel
    double filterMsPerMegaPx = 0.86;     // filter: ms per traced megapixel
    double kiloPx1 = 0.0;                // window kilopixels at scale 1.0
    bool denoise = false;

    double msAt(double scale, int spp) const {
        const double area = kiloPx1 * scale * scale;               // kilopixels traced
        double ms = msPerSampleKiloPx * area * double(std::max(1, spp));
        if (denoise) ms += filterMsPerMegaPx * area / 1000.0;      // area/1000 = megapixels
        return ms;
    }
    // The filter's share on its own, which is what has to come off a measurement before the
    // per-sample constant is learned from it.
    double filterOnly(double scale) const {
        return denoise ? filterMsPerMegaPx * kiloPx1 * scale * scale / 1000.0 : 0.0;
    }
};

void reportGpuFrameTime(App* app, double ms) {
    if (!app->timingSupported || ms <= 0.0) return;

    // A reading taken while the frame is still settling describes settings that are no longer in
    // effect, and one taken just after a reallocation describes a frame that spent part of its time
    // being built. Neither belongs in the average: that blending is exactly how the same setting came
    // to read 0.7 ms at one moment and 11.2 ms at the next, and every prediction made from a number
    // like that swings by the difference between them.
    const int staleWanted = app->lastChangeCostMs > 25.0 ? kStaleFramesAfterRealloc
                                                         : kStaleFramesAfterChange;
    if (app->cfg.autoQuality && app->settleFrames < staleWanted) {
        ++app->settleFrames;
        return;
    }
    // The running average is the only readout of GPU cost, so it keeps updating even when adaptation
    // is off. Gating it on autoQuality made every pinned-quality run report 0.0 ms, which is how a
    // cost comparison came back looking like the filter was free.
    const double a = 0.2;
    app->gpuMsAvg = (app->gpuMsAvg <= 0.0) ? ms : (a * ms + (1.0 - a) * app->gpuMsAvg);
    if (!app->cfg.autoQuality) return;
    ++app->settleFrames;
    ++app->decideFrames;

    const double frameMs = 1000.0 / effectiveFps(app->cfg);
    const double kCpuReserveMs = 2.5;  // the loop's own stages, which the GPU timestamp does not see
    double budget =
        std::max(1.5, frameMs * (1.0 - double(app->cfg.qualityHeadroom)) - kCpuReserveMs);
    if (app->testBudgetMs > 0.0) budget = app->testBudgetMs;

    // ---- is this machine missing the frame rate it was asked for? --------------------------------
    // Against the target the user set, never against the interval the loop happens to be delivering.
    // The histogram this used to consult took the display period from the most common frame interval,
    // so a renderer stuck at 22 fps found 45 ms to be the period, compared itself with 45 ms, and
    // reported a late fraction of zero forever. The counts themselves live in the frame loop, where
    // the interval is actually known: a frame that arrived late arrived late whatever its timestamps
    // say, and the controller throws readings away while a change settles.
    const bool lateNow = app->lastFrameLate || app->testLateForced;
    if (lateNow) ++app->denoiseLateStreak; else app->denoiseLateStreak = 0;
    const double lateFrac = app->framesWindow >= 12
                                ? double(app->lateFramesWindow) / double(app->framesWindow)
                                : 0.0;

    // ---- price the filter, from a flip seen from both sides --------------------------------------
    // One flip at settings that did not change is the only thing that separates the filter's cost from
    // the trace's: on every other axis the frame is identical either side of it. The second reading is
    // taken a while after the flip rather than in the frame the flip landed in, because that frame's
    // numbers are about a rebuild of thirty images and not about the pipeline underneath it.
    if (app->filterCalArmed) {
        if (app->liveScale != app->filterCalBeforeScale || app->liveSpp != app->filterCalBeforeSpp) {
            app->filterCalArmed = false;   // settings moved, so the two readings no longer describe
                                           // the same frame and their difference means nothing
        } else if (++app->filterCalWait >= kFilterCalibFrames) {
            app->filterCalArmed = false;
            const double delta = app->filterCalTurningOn ? app->gpuMsAvg - app->filterCalBeforeMs
                                                         : app->filterCalBeforeMs - app->gpuMsAvg;
            // Only a frame that got dearer when the filter went on, or cheaper when it went off, says
            // anything about its price. Anything else means the readings were still settling.
            if (delta > 0.0 && app->filterCalMegaPx > 1.0e-4) {
                double perMega = delta / app->filterCalMegaPx;
                // A first measurement replaces the seeded guess outright; after that one reading may
                // not move the constant by more than a factor of two, so a single bad pair cannot
                // reprice the filter out of existence.
                if (app->filterCalibrated)
                    perMega = std::min(std::max(perMega, app->filterMsPerMegaPx / 2.0),
                                       app->filterMsPerMegaPx * 2.0);
                perMega = std::min(std::max(perMega, 0.02), 80.0);
                app->filterMsPerMegaPx = perMega;
                app->filterCalibrated = true;
                logMsg("  denoise cost: %.2f ms per megapixel traced (%.1f ms for %.2f MP, measured "
                       "across the filter going %s)\n",
                       app->filterMsPerMegaPx, app->filterMsPerMegaPx * app->filterCalMegaPx,
                       app->filterCalMegaPx, app->filterCalTurningOn ? "on" : "off");
            }
        }
    }

    // ---- the model, and what it says about the frame just measured -------------------------------
    const double kiloPx1 =
        double(app->swapExtent.width) * double(app->swapExtent.height) / 1000.0;
    if (kiloPx1 <= 0.0 || app->liveSpp <= 0) return;
    const double sNative = std::max(0.1, double(app->renderScale));
    const double tracedKilo = kiloPx1 * double(app->liveScale) * double(app->liveScale);
    CostModel model;
    model.msPerSampleKiloPx = app->msPerSampleKiloPx;
    model.filterMsPerMegaPx = app->filterMsPerMegaPx;
    model.kiloPx1 = kiloPx1;
    model.denoise = app->cfg.denoise;

    // Learn the trace's own unit cost from the measurement with the filter's share taken out - the
    // step that was missing, and the reason one constant had to stand in for two unrelated things.
    //
    // The filter's share is bounded by the frame it is charged against, because a part of a frame
    // cannot cost more than the whole of it, and until a real flip has priced the filter the number
    // being subtracted is a seeded starting value rather than a measurement. So a frame that comes in
    // cheaper than the modelled price contradicts the seed, not itself. What stood here refused those
    // readings outright, and on a fast machine that meant refusing almost all of them: measured here at
    // one megapixel, where a whole frame costs 0.9 ms and the seed prices the filter alone at 0.86 ms,
    // 192 readings were thrown away and the trace term learned nothing from any of the frames that were
    // actually measured - the state this two-term model exists to avoid. Refusing a reading is
    // legitimate while it is still settling or just after a reallocation, and both are dealt with above,
    // before this point; it is also legitimate when what is left of the frame is too small for the
    // timestamps to resolve. It is not legitimate merely because one of the model's own guesses does
    // not fit. Capping the share here costs the trace term nothing it can prove wrong: the cap only
    // ever applies to a filter share the frame itself has already contradicted.
    double filterShare = model.filterOnly(app->liveScale);
    if (filterShare > 0.5 * app->gpuMsAvg)
    {
        filterShare = 0.5 * app->gpuMsAvg;
        // Counted rather than silent: a run capped on nearly every frame is a run whose filter has never
        // been priced, and the number belongs next to the refusals in the log for exactly that reason.
        ++app->modelShareCaps;
    }
    const double traceMs = app->gpuMsAvg - filterShare;
    // A tenth of a millisecond is the floor below which the remainder is inside the resolution of the
    // query and of the smoothing applied to it, and a unit cost divided off that is invented.
    if (traceMs > 0.1 && tracedKilo > 0.0) {
        double unit = traceMs / (tracedKilo * double(app->liveSpp));
        if (app->msPerSampleKiloPx > 0.0) {
            unit = std::min(std::max(unit, app->msPerSampleKiloPx / 2.0),
                            app->msPerSampleKiloPx * 2.0);
            app->msPerSampleKiloPx = 0.3 * unit + 0.7 * app->msPerSampleKiloPx;
        } else {
            app->msPerSampleKiloPx = unit;
        }
    } else {
        ++app->modelRejects;
    }
    if (app->msPerSampleKiloPx <= 1e-9) return;
    model.msPerSampleKiloPx = app->msPerSampleKiloPx;

    // ---- 1. the filter is the first thing given up -----------------------------------------------
    // Before any reduction of resolution below the window's own size, and before any cut in samples,
    // because where it hurts nothing else can pay for it: at 3840x2100 the filter costs more than the
    // rest of the frame put together, so cutting pixels to make room for it throws away the image and
    // still misses the frame. The streak is short and nothing the controller does restarts it, which
    // is the whole reason it now fires: the version this replaces asked for forty consecutive late
    // readings at the quality floor and reset its own count every time it changed anything, so on a
    // machine that was oscillating it could never arrive anywhere. A frame that costs more than three
    // times the budget is not a borderline case, and every further frame spent measuring it is a
    // frame watched at a fraction of the frame rate, so that shortcut needs only two late readings.
    const bool filterIsOursToGive = app->cfg.denoise && !app->denoiseUserToggled &&
                                    app->denoiseFlips < kMaxDenoiseFlips;
    // The cost gate is here so the filter is not sacrificed on a machine whose GPU cost is nowhere
    // near the frame budget - a late frame there is something else on the system. --test-give-up
    // means the opposite of that on purpose: it says to treat the run as missing its target
    // whatever the numbers, so it overrides the gate as well as the lateness.
    if (filterIsOursToGive && (app->testLateForced || app->gpuMsAvg > budget * 1.04)) {
        const bool gross = app->gpuMsAvg > budget * 3.0 && app->denoiseLateStreak >= 2;
        if (gross || lateFrac >= 0.25 || app->denoiseLateStreak >= kDenoiseGiveUpFrames) {
            logMsg("quality decision: denoise off (late fraction %.2f, scale %.2f, spp %d)\n",
                   lateFrac, double(app->liveScale), app->liveSpp);
            app->denoiseAutoOff = true;
            ++app->denoiseFlips;
            setDenoiseEnabled(app, false, "frames late: the filter is the first thing given up");
            return;
        }
    }
    // While the filter is still ours to give up, the controller may spend spare budget but may not
    // hand any of it back. Ordering is the whole point: if the frame is being missed and the filter
    // is on, the filter is what goes, not the pixels - and a reduction taken in the meantime both
    // costs the image and delays the thing that would actually have fixed it. Nothing is frozen
    // though: with money to spare the controller still buys quality, exactly as before.

    // And while a flip is being priced, nothing moves either. The measurement is a difference between
    // two readings of the same frame, so a sample-count change halfway through it voids the only
    // number that says what the filter costs - and without that number the controller is back to
    // guessing, which is the state this whole rewrite was meant to end.
    if (app->filterCalArmed && app->settleFrames < kFilterCalibFrames + kStaleFramesAfterChange)
        return;

    // ---- weigh it once per window, not once per frame --------------------------------------------
    // The streaks below only mean something if each of them is an independent look at a settled
    // frame. Left running on every frame they filled inside a single settling period, so the
    // controller acted on one observation four times over - which is how it came to alternate
    // between seven different trace scales in half a minute.
    if (app->decideFrames < kAdjustEveryFrames) return;
    app->decideFrames = 0;

    // Watchdog on the delivery pace. The command buffer can report that it fits while the display
    // still misses vsync, so the real interval gets a vote on how much of the budget may be spent -
    // but not on lateness, which is measured above against the requested frame rate.
    const bool nearBudget = app->gpuMsAvg > kDeadband * budget;
    const bool missingVsync = app->cpuMsAvg > frameMs * 1.10 && nearBudget;
    if (missingVsync) app->cleanPaceStreak = 0; else ++app->cleanPaceStreak;
    const double spend = missingVsync ? budget * 0.85 : budget;
    // Whether the frames are arriving on time, which is a separate question from whether the GPU is
    // near its share of them: a machine can be cheap and deliver late anyway, and then the cheapness
    // says nothing at all about what the next frame will feel like. Cheap-but-late is exactly the
    // situation the ordering rule exists for, and the version this guards did not look at it: with 788
    // frames in 789 arriving at twice the interval they were asked for, and with the filter still
    // switched on and still the controller's to give up, it raised supersampling from scale 1.00 to
    // 2.00 - the opposite of surrendering something. Nothing is bought while the delivery is late, and
    // in the restore branch below, nothing is handed back either.
    const bool deliveringLate = lateNow || lateFrac >= 0.25;
    // Spending up is not the same as spending down: a climb that lands exactly on the budget lands on
    // top of the measurement noise as well, and the next reading then says it is over. Leave a margin
    // when going up so the settle lands inside the deadband rather than on its edge.
    const double climbSpend = budget * kClimbMargin;

    // Hysteresis on delivery: spend up only when the pace has been clean for three quarters of a
    // second, otherwise the measured cost and the delivered pace argue and the image flickers - and
    // never at all while the frames are arriving late, whatever the timestamps say they cost. Buying
    // quality is a decision made for the frames that come next, and if those are already missing their
    // target the next thing has to be given up, not bought. See deliveringLate above.
    const bool settledPace =
        !missingVsync && !deliveringLate && app->cleanPaceStreak >= 45;
    // Decide on streaks, not on single readings: a frame can be slow for reasons unrelated to the
    // scene (a background process, the compositor), and reacting to one such reading is exactly how
    // a controller ends up oscillating. Inside the band both streaks are cleared but control is not
    // handed back: a frame sitting in the middle of the band is precisely the one that can afford the
    // filter back, and returning early here used to make that branch unreachable - the reading never
    // stayed low long enough to build the streak the restore was waiting for.
    const bool overBudget = app->gpuMsAvg > spend * 1.04;
    if (overBudget) { ++app->overStreak; app->underStreak = 0; }
    else if (app->gpuMsAvg < kDeadband * spend) { ++app->underStreak; app->overStreak = 0; }
    else { app->overStreak = app->underStreak = 0; }
    const bool mayReduce = app->overStreak >= 2 && !filterIsOursToGive;
    const bool mayClimb = app->underStreak >= 3 && !overBudget;

    // Resolution floor and ceiling. The floor is the configured fraction of the window's own size;
    // the ceiling is bounded both by the configured multiple and by an absolute pixel count, because
    // the accumulation target is RGBA16F and two slots at 8K cost hundreds of megabytes.
    const double sMin = double(app->cfg.minRenderScale) * sNative;
    const double pixelsNative = double(app->swapExtent.width) * double(app->swapExtent.height);
    double sMax = sNative * std::max(1.0, double(app->cfg.maxRenderScale));
    if (pixelsNative > 0.0) {
        double sPixels = std::sqrt(app->cfg.maxTracePixels / pixelsNative);
        if (sPixels < sMax) sMax = sPixels;
    }
    if (sMax < sMin) sMax = sMin;

    // What the budget buys, spent in the order quality is worth it: the window's own resolution
    // first, then samples up to the ceiling, then supersampling with whatever is spare. The filter's
    // charge is paid before any of it, because it is levied per traced pixel whatever the sample
    // count is. Solving in closed form rather than stepping means the controller goes where it thinks
    // it should in one move, instead of walking there through six rebuilds of thirty images each.
    const double spareAtNative = spend - model.filterOnly(sNative);
    const double oneSampleAtNative = model.msAt(sNative, 1);
    const int cap = app->cfg.denoise ? std::max(1, app->cfg.baseSamplesPerFrame)
                                     : app->cfg.maxSamplesPerFrame;
    int wantSpp = 1;
    float wantScale = float(sNative);
    if (spareAtNative < oneSampleAtNative) {
        // Not even one sample at full size fits. The filter is already off by the time this can be
        // true, so shrinking the image is the only lever left, and it goes to wherever one sample
        // pays for itself rather than straight to the floor.
        const double perSq = model.msAt(1.0, 1);
        const double s = perSq > 0.0 ? std::sqrt(spend / perSq) : sMin;
        wantScale = float(std::min(std::max(s, sMin), sMax));
    } else {
        // While the filter is running the extra samples are worth less than the extra pixels, because
        // the filter is what removes the noise; without it, samples are the whole quality story.
        const double limit = mayReduce ? spend : climbSpend;
        const double spareClimb = limit - model.filterOnly(sNative);
        int k = int(spareClimb / oneSampleAtNative);  // floor: never overpromise
        if (k > cap) k = cap;
        if (k < 1) k = 1;
        wantSpp = k;
        // Whatever the samples did not spend goes into supersampling, and never into a smaller image:
        // dropping below the window's own size is the decision made above, not a side effect of
        // having money left over.
        const double s = std::sqrt(limit / model.msAt(1.0, k));
        wantScale = float(std::min(std::max(s, sNative), sMax));
    }

    const int prevSpp = app->liveSpp;
    const float prevScale = app->liveScale;
    // The measurement this decision is being reported against. It has to be saved here: a scale change
    // below runs through invalidateQuality, which zeroes the smoothed average, and printing the live
    // value afterwards logged "0.0 ms measured" for a frame that had just read 2.4 ms.
    const double measuredMs = app->gpuMsAvg;

    // A change is not free - replacing the accumulation target costs a frame - so require that the
    // one being considered is worth it. Over budget, dropping is mandatory however small the step;
    // under budget, climb only on a difference of two samples or more. That asymmetry is what stops
    // the controller from flapping by one sample on either side of the line.
    // A scale change is the expensive one - it reallocates the trace target and replays a frame - so
    // it has to be worth it: a fifth of the current scale, not a nudge. A resize is unaffected because
    // it discards the model and the difference it produces is far larger than this.
    const bool scaleDiffers = fabs(double(wantScale) - double(app->liveScale)) >
                              0.2 * double(app->liveScale);
    const bool scaleMove = scaleDiffers && ((wantScale < app->liveScale && mayReduce) ||
                                            (wantScale > app->liveScale && mayClimb && settledPace));
    const bool sppMove = mayReduce
                             ? (wantSpp < app->liveSpp)
                             : (mayClimb && settledPace && wantSpp - app->liveSpp >= 2);

    // ---- 2. then samples, and resolution only once they are where the model wants them -----------
    // Samples cost a frame to change and resolution costs thirty images, so samples move first and
    // the trace target only shrinks once there is nothing left to save that way.
    if (sppMove && wantSpp != app->liveSpp) {
        logMsg("quality decision: samples %d -> %d (gpu %.1f ms, budget %.1f ms)\n",
               app->liveSpp, wantSpp, app->gpuMsAvg, spend);
        app->liveSpp = wantSpp;
        // The displayed image divides by the sample count, so changing it must restart accumulation.
        app->samplesAccumulated = 0;
        // Nothing was reallocated to change the sample count, so the longer allowance for a rebuild
        // must not carry over from whatever change happened before this one.
        app->lastChangeCostMs = 0.0;
        invalidateQuality(app);
        ++app->qualityChanges;
        app->qualityChanged = true;
    } else if (scaleMove) {
        logMsg("quality decision: scale %.2f -> %.2f (gpu %.1f ms, budget %.1f ms)\n",
               double(app->liveScale), double(wantScale), app->gpuMsAvg, spend);
        app->liveSpp = wantSpp;
        setLiveScale(app, wantScale);
    }

    // ---- 3. and the filter is the last thing taken back ------------------------------------------
    // Only once samples and resolution have climbed as far as the budget lets them and there is room
    // left over for the filter on top of that: the same cost model read in the other direction. It is
    // checked after the climbs above so that a frame with spare money spends it on pixels first, and
    // it asks for room for the whole of the filter's price, not merely some of it - putting the
    // filter back and taking it again a moment later costs two rebuilds and the noise in between.
    // What is asked is not a wish for more samples or a bigger target: both of those are looked at
    // before this, and the frame has to pay for the filter on top of wherever it has climbed to.
    // The old version also demanded that the model want no more scale than it already had, and that
    // deadlocked on a real machine: the model wanted a fifth more scale, the hysteresis said a fifth
    // was not worth a rebuild, and so neither the climb nor the restore ever happened.
    const bool nothingMoved = prevSpp == app->liveSpp && prevScale == app->liveScale;
    // Frames have to be arriving on time, not merely cheap. The filter was given up because the
    // machine was missing its target, and taking it back while it is still missing that target -
    // however much the timestamps say the frame could afford it - is the flip that would be reversed
    // on the next late reading, which is the behaviour this ordering exists to remove.
    // ...and priced. The flip that gave the filter up armed the only measurement of what the filter
    // costs, and that pair completes fifteen readings later - three readings after the first decision
    // window of the new state would otherwise open. On a machine whose frame turns cheap the instant the
    // filter goes, the restore branch used to win that race: measured here at four megapixels, 24 ms with
    // the filter and 1.6 ms without, and it took the filter back on the seeded 0.86 ms per megapixel
    // while the truth at that size was 5.58 - the emergency decision undone a quarter of a second after
    // it, at the price of two rebuilds and noise at both ends. That is the flap this whole ordering was
    // written to end. Asking whether there is room for the whole of the filter's price is not a question
    // that can be answered while the price is a guess, so it is not asked until one flip has answered it.
    if (!app->cfg.denoise && app->denoiseAutoOff && !app->denoiseUserToggled &&
        app->filterCalibrated &&
        app->denoiseFlips < kMaxDenoiseFlips && !lateNow && !overBudget && settledPace &&
        nothingMoved) {
        const double withFilter = model.msAt(app->liveScale, app->liveSpp) +
                                  app->filterMsPerMegaPx * tracedKilo / 1000.0;
        const double headroom = spend - withFilter;
        // Room for the whole of it and then some. Putting the filter back and taking it again a
        // moment later costs two rebuilds and shows noise at both ends of it.
        if (headroom > 0.25 * spend) {
            logMsg("quality decision: denoise on (headroom %.1f ms, scale %.2f, spp %d)\n",
                   headroom, double(app->liveScale), app->liveSpp);
            app->denoiseAutoOff = false;
            ++app->denoiseFlips;
            setDenoiseEnabled(app, true, "budget has room for the filter again");
        }
    }

    if (app->verbose && (prevSpp != app->liveSpp || prevScale != app->liveScale))
        // The period is spelled out next to the budget, and the reallocation's cost is labelled as wall
        // time, because it is none of the GPU's: thirty images destroyed and rebuilt outside the
        // timestamps, timed only so that the readings which follow it can be recognised as describing a
        // half-built frame. Printed as "change cost" beside a GPU figure, it reads as part of the frame.
        logMsg("  quality: %.1f ms measured -> %d spp at scale %.2f (budget %.1f ms of a %.1f ms "
               "period, reallocation %.1f ms of wall time, filter %.2f ms/MP, model refused %d, model "
               "share capped %d)\n",
               measuredMs, app->liveSpp, app->liveScale, spend, frameMs, app->lastChangeCostMs,
               app->filterMsPerMegaPx, app->modelRejects, app->modelShareCaps);
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
    // While the filter is on screen, that is the image worth saving. The accumulation target holds
    // only one frame when filtering, on purpose, so reading it back would show a single sample.
    const GpuImage& im = app->cfg.denoise
                             ? app->denPP[slotIdx][1][0]
                             : app->hdr[slotIdx];
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
    // While filtering, extra samples are worth less than extra resolution: the filter is what
    // removes the noise, so the budget should buy pixels and let one sample per frame stand.
    if (app->cfg.denoise && app->cfg.autoQuality) app->cfg.baseSamplesPerFrame = 1;
    GLFWmonitor* mon = glfwGetPrimaryMonitor();
    const GLFWvidmode* vm = mon ? glfwGetVideoMode(mon) : nullptr;
    // ...and only when nothing else supplied one. --refresh is applied before the frame loop is
    // entered, so reading the monitor unconditionally here overwrote it: the flag whose whole job is
    // to pin the panel rate for a measurement did nothing at all.
    if (vm && vm->refreshRate > 0 && app->cfg.detectedRefresh <= 0.f)
        app->cfg.detectedRefresh = float(vm->refreshRate);
    logMsg("display: %d Hz panel, targeting %.0f fps, vsync %s, quality mode %s\n",
           vm ? vm->refreshRate : 0, effectiveFps(app->cfg), app->cfg.vsync ? "on" : "off",
           app->cfg.qualityMode.c_str());
    // Said out loud, because the number is easy to mistake for the target. A panel reporting a rate
    // far from the one that was asked for is a different device - a virtual display adapter on a
    // remote session, in the case this was found on - and it does not get to set the frame deadline.
    const double panelHz = double(app->cfg.detectedRefresh);
    if (panelHz >= 20.0 && panelHz <= 400.0 && effectiveFps(app->cfg) > panelHz + 0.5)
        logMsg("  panel rate %.0f Hz is not the frame target: this run is held to %.0f fps, "
               "the rate that was asked for (see effectiveFps)\n",
               panelHz, effectiveFps(app->cfg));
    // Where the run starts from, stated rather than inferred: the controller's first move is
    // measured against this, and a test of the decisions needs to know what was on before any of
    // them were made.
    logMsg("quality start: denoise %s, scale %.2f, spp %d, filter %.2f ms per megapixel\n",
           app->cfg.denoise ? "on" : "off", double(app->liveScale), app->liveSpp,
           app->filterMsPerMegaPx);
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
    bool altEnterWasDown = false, cursorNormal = true, denoiseWasDown = false;

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
        app->lastFrameLate = dt > 1.25 / double(std::max(10, app->cfg.targetFps > 0 ? int(app->cfg.targetFps) : 60));
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

        // ---- N toggles the denoiser, so its effect can be judged side by side -----------------
        const bool denoiseDown = glfwGetKey(app->window, GLFW_KEY_N) == GLFW_PRESS;
        if (denoiseDown && !denoiseWasDown) toggleDenoise(app);
        denoiseWasDown = denoiseDown;

        // ---- WASD takes the camera; ten idle seconds hands it back -----------------------------
        const int fwdKey = (glfwGetKey(app->window, GLFW_KEY_W) == GLFW_PRESS) -
                           (glfwGetKey(app->window, GLFW_KEY_S) == GLFW_PRESS);
        const int sideKey = (glfwGetKey(app->window, GLFW_KEY_D) == GLFW_PRESS) -
                            (glfwGetKey(app->window, GLFW_KEY_A) == GLFW_PRESS);
        if ((fwdKey || sideKey) && !app->freeCam) enterFreeCam(app);
        if (fwdKey || sideKey) app->lastWasd = glfwGetTime();
        // Drives the same code path the N key does: scripted keystrokes never reach this window, so
        // the toggle is exercised by frame number instead.
        if (app->testToggleFrame >= 0 && int(app->frameIndex) == app->testToggleFrame)
            toggleDenoise(app);

        if (app->testGiveUpFrame >= 0 && int(app->frameIndex) == app->testGiveUpFrame) {
            // Force the condition the controller now acts on: frames that are missing the target
            // while the filter is still on. Pinning quality to the floor is no longer part of it -
            // the filter goes before the floor is ever reached - and a machine fast enough to hold
            // 60 fps would otherwise never report a late frame, which is exactly the case this flag
            // exists to exercise.
            app->denoiseLateStreak = kDenoiseGiveUpFrames;
            app->testLateForced = true;
            logMsg("test: forced a frame rate that is missing its target\n");
        }
        if (app->testLateForced && !app->cfg.denoise) {
            // The forcing asked for one thing: that a run which cannot hold its frame rate give the
            // filter up. Once that has happened, keeping the lie going would stop the controller
            // from ever doing the rest of its job - it would spend the remaining run refusing to take
            // the filter back on a machine that is comfortably making its frames, which is the
            // opposite of what is left to test.
            app->testLateForced = false;
            logMsg("test: forcing stopped, the give-up it was asking for has happened\n");
        }

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

        // Lateness is counted here rather than in the controller, because the controller throws away
        // readings while a change settles and a frame that arrived late arrived late whatever its
        // timestamps say. The window is reset at each quality change, so the fraction always
        // describes the settings currently in effect and nothing else.
        if (app->lastFrameLate) ++app->lateFramesWindow;
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
                         "Vulkan Ray Tracer - %.0f fps - %ux%u trace %ux%u - %d spp - %s%s - (ESC) Quit",
                         fps, app->swapExtent.width, app->swapExtent.height, app->hdrWidth,
                         app->hdrHeight, app->liveSpp,
                         app->usingComputeFallback ? "ray-query" : "RT pipeline",
                         app->cfg.denoise ? " - (N) Denoise ON" : " - (N) Denoise OFF");
            else
                snprintf(title, sizeof(title),
                         "Vulkan Ray Tracer - %.0f fps - %ux%u trace %ux%u - %d spp - %s%s%s - (ESC) Quit",
                         fps, app->swapExtent.width, app->swapExtent.height,
                         app->hdr[app->slot].width, app->hdr[app->slot].height,
                         app->cfg.autoQuality ? app->liveSpp : app->cfg.samplesPerFrame,
                         app->usingComputeFallback ? "ray-query" : "RT pipeline",
                         app->camBlend > 0.5f ? " - free look" : "",
                         app->cfg.denoise ? " - (N) Denoise ON" : " - (N) Denoise OFF");
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
        // Stills must be reproducible. The clock position was taken from wall time and only
        // rounded in the filename, so two runs of the same build rendered slightly different
        // cameras while claiming the same name, and no before/after pixel comparison could be
        // trusted. Fixed schedule instead: same image, same time, every run.
        t = 0.55f + float(img) * 2.35f;
        app->simTime = t;
        recreateHdrTargets(app, app->captureWidth, app->captureHeight);
        writeTraceDescriptors(app, 0);
        computeInstances(app, t, &instances);
        uploadFrameInstances(app, 0, instances);

        int samples = 0;
        for (int f = 0; f < app->captureAccum; ++f) {
            bool reset = (f == 0);
            ++samples;
            app->frameIndex = uint64_t(f) + 1;
            // Verbose only, and only while capturing stills: the accumulation behaviour of the still
            // path was measured to converge at half the expected rate, and this line shows whether a
            // frame was handed to the tracer as a fresh start or as an addition to the target.
            if (app->verbose)
                logMsg("  accum frame %d reset=%d frameIndex=%llu samples=%d\n", f, reset ? 1 : 0,
                       (unsigned long long)app->frameIndex, samples);
            buildFrameCommand(app, 0, 0, false, instances, t, reset, samples, true);
            VkSubmitInfo si{};
            si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.commandBufferCount = 1;
            si.pCommandBuffers = &app->cmd[0];
            VK_CHECK(vkQueueSubmit(app->queue, 1, &si, app->fence[0]));
            VK_CHECK(vkWaitForFences(app->device, 1, &app->fence[0], VK_TRUE, ~0ull));
            VK_CHECK(vkResetFences(app->device, 1, &app->fence[0]));

            // Raw-sample dump for the convergence investigation: the PNG path quantises to 8 bits
            // and divides by the frame count, which is exactly the information needed to tell two
            // similar noise patterns apart. This is the buffer before any of that.
            // -2 means every frame: comparing frames against each other only means something if they
                        if (app->captureDumpFrame == -2 || f == app->captureDumpFrame) {
                std::vector<float> raw;
                readHdrToCpu(app, 0, &raw);
                char dn[280];
                snprintf(dn, sizeof(dn), "%s\\accum_%03d.bin", app->captureDir.c_str(), f);
                FILE* fp = nullptr;
                if (fopen_s(&fp, dn, "wb") == 0 && fp) {
                    const uint32_t dims[2] = { app->captureWidth, app->captureHeight };
                    fwrite(dims, sizeof(dims), 1, fp);
                    fwrite(raw.data(), sizeof(float) * raw.size(), 1, fp);
                    fclose(fp);
                    logMsg("  dumped raw target after frame %d (%zu floats)\n", f, raw.size());
                }
            }
        }

        std::vector<float> hdr;
        readHdrToCpu(app, 0, &hdr);
        char name[256];
        snprintf(name, sizeof(name), "%s\\frame_%03d_t%.2f.png", app->captureDir.c_str(), img, t);
        // The target holds means per frame, so only the frame count is divided out here.
        writePng(name, app->captureWidth, app->captureHeight, hdr, app->cfg.exposure,
                 app->cfg.denoise ? 1.0f : 1.0f / float(std::max(1, samples)));
        logMsg("  wrote %s (%d spp)\n", name, samples);
    }
    vkDeviceWaitIdle(app->device);
    return 0;
}
