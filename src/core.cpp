// core.cpp - Vulkan bootstrap, scene resources, pipelines, and per-frame command recording.
#include "app.h"
#include "json.h"
#include "generated/shaders_spv.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// -------------------------------------------------------------------------------------------------
// Logging and error checking
FILE* g_logFile = nullptr;

void logMsg(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    if (g_logFile) {
        fflush(stdout);
        va_end(ap);
        va_start(ap, fmt);
        vfprintf(g_logFile, fmt, ap);
        fflush(g_logFile);
    }
    va_end(ap);
    fflush(stdout);
}

// Set by the headless modes (--capture, --list-adapters), which must never end up waiting on a
// dialog nobody is there to click.
bool g_headlessMode = false;

// One formatted message, three destinations. The dialog is the point of the rewrite: this is a
// GUI-subsystem binary, so a double-click has no console, stderr goes nowhere, and a start-up failure
// looked like a window that appeared and then vanished - which is exactly how the refused
// vkCreateDevice on the second machine presented itself. The log gets the line either way; a
// scripted run (--capture, --list-adapters) gets no dialog so it can never hang.
void fail(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[2048];
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    buf[sizeof buf - 1] = '\0';

    fprintf(stderr, "fatal: %s\n", buf);
    fflush(stderr);
    if (g_logFile) {
        fprintf(g_logFile, "fatal: %s\n", buf);
        fflush(g_logFile);
    }
    if (!g_headlessMode) {
        std::string msg(buf);
        std::vector<wchar_t> wide(msg.size() + 2, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, msg.c_str(), -1, wide.data(), int(wide.size()));
        MessageBoxW(nullptr, wide.data(), L"Vulkan Raytracer - start-up failed",
                    MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    }
    exit(1);
}

const char* vkResultName(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_INCOMPLETE: return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        default: return "VkResult?";
    }
}

VkResult vkCheck(const char* expr, VkResult r) {
    if (r != VK_SUCCESS && r != VK_NOT_READY) fail("%s failed: %s (%d)", expr, vkResultName(r), (int)r);
    return r;
}

// -------------------------------------------------------------------------------------------------
// Allocation helpers
uint32_t findMemoryType(App* app, uint32_t typeBits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(app->physicalDevice, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    fail("no memory type satisfies 0x%x", (unsigned)want);
}

void createBuffer(App* app, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                  bool deviceAddress, GpuBuffer* out) {
    if (size == 0) size = 16;
    if (deviceAddress) usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    VkBufferCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(app->device, &ci, nullptr, &out->buf));

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(app->device, out->buf, &req);

    VkMemoryAllocateFlagsInfo afi{};
    afi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    if (deviceAddress) afi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.pNext = deviceAddress ? &afi : nullptr;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(app, req.memoryTypeBits, props);

    VkResult r = vkAllocateMemory(app->device, &ai, nullptr, &out->mem);
    if (r != VK_SUCCESS && deviceAddress) {
        ai.pNext = nullptr;  // retry: a few drivers reject the address flag on host memory
        ai.memoryTypeIndex = findMemoryType(app, req.memoryTypeBits, props);
        VK_CHECK(vkAllocateMemory(app->device, &ai, nullptr, &out->mem));
        out->addr = 0;
    } else if (r != VK_SUCCESS) {
        fail("vkAllocateMemory: %s", vkResultName(r));
    }
    VK_CHECK(vkBindBufferMemory(app->device, out->buf, out->mem, 0));
    out->size = req.size;

    if (deviceAddress && out->addr == 0) {
        VkBufferDeviceAddressInfo dai{};
        dai.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        dai.buffer = out->buf;
        out->addr = vkGetBufferDeviceAddress(app->device, &dai);
        if (!out->addr) fail("vkGetBufferDeviceAddress returned 0 (bufferDeviceAddress not enabled?)");
    }
    if (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        VK_CHECK(vkMapMemory(app->device, out->mem, 0, VK_WHOLE_SIZE, 0, &out->mapped));
}

void destroyBuffer(App* app, GpuBuffer* b) {
    if (b->mapped) { vkUnmapMemory(app->device, b->mem); b->mapped = nullptr; }
    if (b->buf) { vkDestroyBuffer(app->device, b->buf, nullptr); b->buf = VK_NULL_HANDLE; }
    if (b->mem) { vkFreeMemory(app->device, b->mem, nullptr); b->mem = VK_NULL_HANDLE; }
    b->addr = 0;
    b->size = 0;
}

void createImage(App* app, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
                 VkImageAspectFlags aspect, GpuImage* out) {
    out->width = w;
    out->height = h;
    out->format = fmt;
    out->layout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(app->device, &ci, nullptr, &out->image));

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(app->device, out->image, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(app, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(app->device, &ai, nullptr, &out->mem));
    VK_CHECK(vkBindImageMemory(app->device, out->image, out->mem, 0));

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = out->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {aspect, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(app->device, &vi, nullptr, &out->view));
}

void destroyImage(App* app, GpuImage* im) {
    if (im->view) { vkDestroyImageView(app->device, im->view, nullptr); im->view = VK_NULL_HANDLE; }
    if (im->image) { vkDestroyImage(app->device, im->image, nullptr); im->image = VK_NULL_HANDLE; }
    if (im->mem) { vkFreeMemory(app->device, im->mem, nullptr); im->mem = VK_NULL_HANDLE; }
    memset(im, 0, sizeof(*im));
}

void transitionImage(App* app, VkCommandBuffer cmd, GpuImage* im, VkImageLayout dst,
                     VkAccessFlags srcAccess, VkAccessFlags dstAccess, VkPipelineStageFlags srcStage,
                     VkPipelineStageFlags dstStage, VkImageAspectFlags aspect) {
    (void)app;
    if (im->layout == dst) return;
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = im->layout;
    b.newLayout = dst;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im->image;
    b.subresourceRange.aspectMask = aspect;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = (im->layout == VK_IMAGE_LAYOUT_UNDEFINED) ? VkAccessFlags(0) : srcAccess;
    b.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
    im->layout = dst;
}

void memBarrier(VkCommandBuffer cmd, VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) {
    VkMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 1, &b, 0, nullptr, 0, nullptr);
}

VkCommandBuffer beginOneTime(App* app) {
    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = app->cmdPool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(app->device, &cai, &cmd));
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    return cmd;
}

void endOneTime(App* app, VkCommandBuffer cmd) {
    VK_CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VK_CHECK(vkQueueSubmit(app->queue, 1, &si, VK_NULL_HANDLE));
    // Always drain before returning. Submitting this kind of work and carrying on without an
    // observable completion looked harmless - one queue keeps the order - but the driver faulted at
    // teardown with the buffers still unreferenced-but-unretired, so the wait stays.
    VK_CHECK(vkQueueWaitIdle(app->queue));
    vkFreeCommandBuffers(app->device, app->cmdPool, 1, &cmd);
}

static void uploadBytes(App* app, const void* data, VkDeviceSize size, VkBufferUsageFlags usage,
                        bool deviceAddress, GpuBuffer* out) {
    VkDeviceSize padded = (size + 15) & ~VkDeviceSize(15);
    GpuBuffer stage{};
    createBuffer(app, padded, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, false,
                 &stage);
    memcpy(stage.mapped, data, size_t(size));
    createBuffer(app, padded, VK_BUFFER_USAGE_TRANSFER_DST_BIT | usage,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, deviceAddress, out);
    VkCommandBuffer cmd = beginOneTime(app);
    VkBufferCopy region{};  // legacy struct: no sType member
    region.size = size;
    vkCmdCopyBuffer(cmd, stage.buf, out->buf, 1, &region);
    memBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_READ_BIT);
    endOneTime(app, cmd);
    destroyBuffer(app, &stage);
}

// -------------------------------------------------------------------------------------------------
// Instance
void createInstance(App* app) {
    if (volkInitialize() != VK_SUCCESS) fail("volkInitialize failed (is the Vulkan runtime installed?)");

    uint32_t extCount = 0;
    const char** glfwExts = glfwGetRequiredInstanceExtensions(&extCount);
    std::vector<const char*> exts;
    for (uint32_t i = 0; i < extCount; ++i) exts.push_back(glfwExts[i]);

    bool debugAvailable = false;
    uint32_t avail = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &avail, nullptr);
    std::vector<VkExtensionProperties> availProps(avail);
    vkEnumerateInstanceExtensionProperties(nullptr, &avail, availProps.data());
    for (const auto& p : availProps) {
        if (strcmp(p.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) debugAvailable = true;
    }
    if (debugAvailable) exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    // The validation layer is only used when it is actually present (no SDK is required to run).
    bool useValidation = false;
    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> layers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
    for (const auto& l : layers) {
        if (strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) useValidation = true;
    }
    std::vector<const char*> enabledLayers;
    if (useValidation && app->verbose) {
        enabledLayers.push_back("VK_LAYER_KHRONOS_validation");
        logMsg("validation layer enabled\n");
    }

    VkApplicationInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.apiVersion = VK_API_VERSION_1_3;
    ai.pApplicationName = "Vulkan Raytracer";
    ai.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    ai.pEngineName = "vkt";
    ai.engineVersion = VK_MAKE_VERSION(1, 0, 0);

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &ai;
    ci.enabledExtensionCount = uint32_t(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    ci.enabledLayerCount = uint32_t(enabledLayers.size());
    ci.ppEnabledLayerNames = enabledLayers.empty() ? nullptr : enabledLayers.data();

    VkDebugUtilsMessengerCreateInfoEXT dbg{};
    dbg.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    dbg.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    auto cb = +[](VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT,
                  const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) -> VkBool32 {
        if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
            fprintf(stderr, "[vulkan] %s\n", data->pMessage);
        else if (user && sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
            printf("[vulkan warn] %s\n", data->pMessage);
        return VK_FALSE;
    };
    dbg.pfnUserCallback = cb;
    dbg.pUserData = app->verbose ? app : nullptr;
    if (debugAvailable) ci.pNext = &dbg;

    VK_CHECK(vkCreateInstance(&ci, nullptr, &app->instance));
    volkLoadInstance(app->instance);

    if (debugAvailable) {
        PFN_vkCreateDebugUtilsMessengerEXT createFn =
            reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(app->instance, "vkCreateDebugUtilsMessengerEXT"));
        if (createFn) createFn(app->instance, &dbg, nullptr, &app->debugMessenger);
    }
}

// -------------------------------------------------------------------------------------------------
// Two render paths, decided per adapter at start-up:
//
//   ray-tracing pipeline   VK_KHR_acceleration_structure + VK_KHR_ray_tracing_pipeline
//   ray-query fallback     VK_KHR_acceleration_structure + VK_KHR_ray_query
//
// Both need Vulkan 1.3 - the accelerated commands keep their _KHR spelling but only exist once 1.3 is
// core - an acceleration structure, and bufferDeviceAddress. A driver is free to advertise one path
// and not the other: a GTX 1080 Ti reports the ray-tracing pipeline without ray queries. So every
// capability is queried per adapter and turned into two booleans, and what the device is later asked
// to enable comes out of the winning adapter's own answers. Nothing downstream may assume more than
// the probe confirmed.
static const char* yesNo(bool b) { return b ? "yes" : "no"; }

// What the ray-tracing pipeline still needs from this adapter, or nullptr when it needs nothing.
static const char* rtMissing(const AdapterCaps& c) {
    if (!c.core13) return "needs Vulkan 1.3";
    if (!c.bufferDeviceAddress) return "needs bufferDeviceAddress";
    if (!c.extAccelStruct) return "needs the VK_KHR_acceleration_structure extension";
    if (!c.asf.accelerationStructure) return "needs the accelerationStructure feature";
    if (!c.extRtPipeline) return "needs the VK_KHR_ray_tracing_pipeline extension";
    if (!c.rtf.rayTracingPipeline) return "needs the rayTracingPipeline feature";
    return nullptr;
}

// The same question for the ray-query compute fallback.
static const char* rqMissing(const AdapterCaps& c) {
    if (!c.core13) return "needs Vulkan 1.3";
    if (!c.bufferDeviceAddress) return "needs bufferDeviceAddress";
    if (!c.extAccelStruct) return "needs the VK_KHR_acceleration_structure extension";
    if (!c.asf.accelerationStructure) return "needs the accelerationStructure feature";
    if (!c.extRayQuery) return "needs the VK_KHR_ray_query extension";
    if (!c.rqf.rayQuery) return "needs the rayQuery feature";
    return nullptr;
}

static const char* deviceTypeName(VkPhysicalDeviceType t) {
    switch (t) {
        case VK_PHYSICAL_DEVICE_TYPE_OTHER: return "other";
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated GPU";
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete GPU";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual GPU";
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return "CPU";
        default: return "unknown";
    }
}

static void addWhy(AdapterCaps* c, const char* text) {
    if (!c->whyNot.empty()) c->whyNot += " | ";
    c->whyNot += text;
}

// Ask one adapter everything it knows. The chain hangs the promoted 1.2 and 1.3 aggregates off
// VkPhysicalDeviceFeatures2 and then the three KHR feature structs, because the accelerated features
// were never folded into the aggregates and have to be asked for by name.
static void queryAdapterCaps(VkPhysicalDevice dev, uint32_t index, AdapterCaps* c) {
    *c = AdapterCaps{};
    c->dev = dev;
    c->index = index;
    vkGetPhysicalDeviceProperties(dev, &c->props);

    c->f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    c->f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    c->f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    c->rtf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
    c->rqf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
    c->asf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    c->f2.pNext = &c->f13;
    c->f13.pNext = &c->f12;
    c->f12.pNext = &c->rtf;
    c->rtf.pNext = &c->rqf;
    c->rqf.pNext = &c->asf;
    vkGetPhysicalDeviceFeatures2(dev, &c->f2);

    // TEMPORARY SIMULATION - DELETE: hides ray queries so this machine can stand in for a GTX 1080 Ti
    // (ray-tracing pipeline yes, ray queries no).
    if (getenv("RT_SIMULATE_NO_RAYQUERY")) c->rqf.rayQuery = VK_FALSE;

    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &n, exts.data());
    auto has = [&](const char* name) {
        for (const auto& e : exts)
            if (strcmp(e.extensionName, name) == 0) return true;
        return false;
    };
    c->extSwapchain = has(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    c->extAccelStruct = has(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
    c->extRtPipeline = has(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
    c->extRayQuery = has(VK_KHR_RAY_QUERY_EXTENSION_NAME);
    c->extDeferredHostOps = has(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    c->extPipelineLibrary = has(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);

    c->core13 = c->props.apiVersion >= VK_API_VERSION_1_3;
    c->bufferDeviceAddress = c->f12.bufferDeviceAddress != VK_FALSE;
    c->accelStruct = c->extAccelStruct && c->asf.accelerationStructure != VK_FALSE;
    c->canRt = rtMissing(*c) == nullptr;
    c->canRq = rqMissing(*c) == nullptr;

    // Say what is missing rather than only "no": a log carried in from another machine has to be
    // enough on its own.
    if (const char* w = rtMissing(*c)) {
        std::string s = "ray-tracing pipeline: ";
        s += w;
        addWhy(c, s.c_str());
    }
    if (const char* w = rqMissing(*c)) {
        std::string s = "ray-query fallback: ";
        s += w;
        addWhy(c, s.c_str());
    }
}

// The one line per adapter the start-up log has always printed, with the two path verdicts added at
// the end. In this line rayTracing and rayQuery name the paths, as they always have; the raw feature
// bits are in the --list-adapters matrix below.
static void logAdapterRow(const AdapterCaps& c) {
    logMsg("adapter %u: %-40s vulkan %u.%u  rayTracing=%s rayQuery=%s  bufferDeviceAddress=%s  usable=%s\n",
           c.index, c.props.deviceName, VK_VERSION_MAJOR(c.props.apiVersion),
           VK_VERSION_MINOR(c.props.apiVersion), yesNo(c.canRt), yesNo(c.canRq),
           yesNo(c.bufferDeviceAddress),
           (c.canRt && c.canRq)   ? "both paths"
                                  : c.canRt ? "ray-tracing pipeline"
                                            : c.canRq ? "ray-query fallback" : "neither path");
    if (!c.whyNot.empty()) logMsg("    missing: %s\n", c.whyNot.c_str());
}

// Enumerate and probe every adapter the loader knows about. Presentability only means something when a
// window surface exists, so --list-adapters does not test it.
static void queryAllAdapters(App* app, std::vector<AdapterCaps>* out) {
    out->clear();
    uint32_t count = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(app->instance, &count, nullptr));
    if (!count) fail("no Vulkan-capable GPU found");
    std::vector<VkPhysicalDevice> devices(count);
    VK_CHECK(vkEnumeratePhysicalDevices(app->instance, &count, devices.data()));
    out->resize(count);

    for (uint32_t i = 0; i < count; ++i) {
        AdapterCaps& c = (*out)[i];
        queryAdapterCaps(devices[i], i, &c);
        if (app->surface != VK_NULL_HANDLE) {
            c.presentable = false;
            if (!c.extSwapchain) {
                addWhy(&c, "VK_KHR_swapchain is absent, so it cannot present the window");
            } else {
                uint32_t qc = 0;
                vkGetPhysicalDeviceQueueFamilyProperties(c.dev, &qc, nullptr);
                std::vector<VkQueueFamilyProperties> qs(qc);
                vkGetPhysicalDeviceQueueFamilyProperties(c.dev, &qc, qs.data());
                for (uint32_t q = 0; q < qc; ++q) {
                    VkBool32 ok = VK_FALSE;
                    vkGetPhysicalDeviceSurfaceSupportKHR(c.dev, q, app->surface, &ok);
                    if (ok && (qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { c.presentable = true; break; }
                }
                if (!c.presentable) addWhy(&c, "no queue family can present the window surface");
            }
        }
        logAdapterRow(c);
    }
}

// The best adapter that can serve one path, considering all of them. A discrete GPU wins when more
// than one adapter can render the path, and equal scores keep the lower-numbered adapter, because a
// score is only replaced when it is beaten outright.
static int bestAdapter(const std::vector<AdapterCaps>& caps, bool wantRt) {
    int best = -1, bestScore = 0;
    for (const AdapterCaps& c : caps) {
        if (!c.presentable || !(wantRt ? c.canRt : c.canRq)) continue;
        int score = 0;
        if (c.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score = 50;
        else if (c.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score = 10;
        if (best < 0 || score > bestScore) { bestScore = score; best = int(c.index); }
    }
    return best;
}

// Decide which adapter renders, and by which path. Automatic: the ray-tracing pipeline wherever some
// adapter here has it, otherwise the ray-query fallback wherever some adapter has that, and only then
// a refusal naming what each adapter lacks. --rt and --compute narrow the choice to one path, and
// then fail loudly instead of falling back to the other one.
static bool chooseAdapter(App* app, const std::vector<AdapterCaps>& caps, int* out, bool* useRt,
                          std::string* why) {
    if (app->forceRt || app->forceCompute) {
        const bool wantRt = app->forceRt;
        const int i = bestAdapter(caps, wantRt);
        if (i < 0) {
            *why = wantRt
                       ? "the ray-tracing pipeline needs VK_KHR_ray_tracing_pipeline and "
                         "VK_KHR_acceleration_structure, which this adapter does not support (--rt)"
                       : "the compute fallback needs VK_KHR_ray_query, which this adapter does not "
                         "support (--compute)";
            return false;
        }
        *out = i;
        *useRt = wantRt;
        return true;
    }

    int i = bestAdapter(caps, true);
    if (i < 0) i = bestAdapter(caps, false);
    if (i < 0) {
        *why = "no adapter here can render this scene: neither VK_KHR_ray_tracing_pipeline nor "
               "VK_KHR_ray_query is available (the adapter lines above say what each one lacks)";
        return false;
    }
    *out = i;
    *useRt = caps[size_t(i)].canRt;
    return true;
}

void pickPhysicalDevice(App* app) {
    std::vector<AdapterCaps> caps;
    queryAllAdapters(app, &caps);

    int chosen = -1;
    bool useRt = false;
    std::string why;
    if (!chooseAdapter(app, caps, &chosen, &useRt, &why)) fail("%s", why.c_str());

    app->chosen = caps[size_t(chosen)];
    app->physicalDevice = app->chosen.dev;
    vkGetPhysicalDeviceProperties(app->physicalDevice, &app->props);

    app->asProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
    app->rtProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
    app->asProps.pNext = &app->rtProps;
    VkPhysicalDeviceProperties2 p2{};
    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    p2.pNext = &app->asProps;
    vkGetPhysicalDeviceProperties2(app->physicalDevice, &p2);

    app->usingComputeFallback = !useRt;

    logMsg("rendering on: %s  [%s]\n", app->props.deviceName,
           app->usingComputeFallback ? "ray-query compute path" : "ray-tracing pipeline");
    logMsg("pipeline chosen by %s: %s\n",
           (app->forceRt || app->forceCompute) ? "the command line" : "the adapter probe",
           app->usingComputeFallback ? "ray-query compute fallback" : "ray-tracing pipeline");
}

// --list-adapters: everything these drivers say about themselves, in the log, with no window and no
// device created. It is the tool for a machine nobody is sitting in front of - one small text file
// comes back and it already says which path that machine would pick, and what a card that cannot
// serve either path is missing.
void logAdapterMatrix(App* app) {
    std::vector<AdapterCaps> caps;
    queryAllAdapters(app, &caps);

    int chosen = -1;
    bool useRt = false;
    std::string why;
    const bool ok = chooseAdapter(app, caps, &chosen, &useRt, &why);

    logMsg("adapter matrix: %u adapter(s); no window is opened, so presentation is not tested\n",
           uint32_t(caps.size()));
    if (app->forceRt || app->forceCompute)
        logMsg("  (%s is in effect, so the choice below reflects the flag, not the default)\n",
               app->forceRt ? "--rt" : "--compute");
    for (const AdapterCaps& c : caps) {
        logMsg("adapter %u: %s\n", c.index, c.props.deviceName);
        logMsg("    type                 : %s\n", deviceTypeName(c.props.deviceType));
        logMsg("    vulkan               : %u.%u.%u\n", VK_VERSION_MAJOR(c.props.apiVersion),
               VK_VERSION_MINOR(c.props.apiVersion), VK_VERSION_PATCH(c.props.apiVersion));
        logMsg("    extensions           : accel_structure=%s ray_tracing_pipeline=%s ray_query=%s\n",
               yesNo(c.extAccelStruct), yesNo(c.extRtPipeline), yesNo(c.extRayQuery));
        logMsg("                           deferred_host_ops=%s pipeline_library=%s swapchain=%s\n",
               yesNo(c.extDeferredHostOps), yesNo(c.extPipelineLibrary), yesNo(c.extSwapchain));
        logMsg("    features             : bufferDeviceAddress=%s accelerationStructure=%s\n",
               yesNo(c.bufferDeviceAddress), yesNo(c.asf.accelerationStructure));
        logMsg("                           rayTracingPipeline=%s rayQuery=%s\n",
               yesNo(c.rtf.rayTracingPipeline), yesNo(c.rqf.rayQuery));
        logMsg("                           descriptorIndexing=%s scalarBlockLayout=%s\n",
               yesNo(c.f12.descriptorIndexing), yesNo(c.f12.scalarBlockLayout));
        logMsg("    path ray-tracing     : %s\n", c.canRt ? "usable" : "not usable");
        logMsg("    path ray-query       : %s\n", c.canRq ? "usable" : "not usable");
        if (!c.whyNot.empty()) logMsg("    missing              : %s\n", c.whyNot.c_str());
        logMsg("    selected             : %s\n",
               (ok && chosen == int(c.index))
                   ? (useRt ? "yes - ray-tracing pipeline" : "yes - ray-query compute fallback")
                   : "no");
    }
    if (ok)
        logMsg("result: adapter %d (%s)  [%s]\n", chosen, caps[size_t(chosen)].props.deviceName,
               useRt ? "ray-tracing pipeline" : "ray-query compute path");
    else
        logMsg("result: nothing here can render the scene - %s\n", why.c_str());
}

void createDevice(App* app) {
    uint32_t qc = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(app->physicalDevice, &qc, nullptr);
    std::vector<VkQueueFamilyProperties> qs(qc);
    vkGetPhysicalDeviceQueueFamilyProperties(app->physicalDevice, &qc, qs.data());

    // The KHR ray-tracing API adds no queue flag of its own (unlike the earlier VK_NV extension):
    // a family with graphics + compute is what vkCmdTraceRaysKHR needs.
    uint32_t need = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    int family = -1;
    for (uint32_t q = 0; q < qc; ++q) {
        if ((qs[q].queueFlags & need) != need) continue;
        VkBool32 ok = VK_TRUE;
        if (app->surface != VK_NULL_HANDLE)
            vkGetPhysicalDeviceSurfaceSupportKHR(app->physicalDevice, q, app->surface, &ok);
        if (ok) { family = int(q); break; }
    }
    if (family < 0 && !app->usingComputeFallback) {
        logMsg("note: no queue family advertises VK_QUEUE_RAY_TRACING_BIT; retrying without it\n");
        for (uint32_t q = 0; q < qc; ++q) {
            if (!(qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            VkBool32 ok = VK_TRUE;
            if (app->surface != VK_NULL_HANDLE)
                vkGetPhysicalDeviceSurfaceSupportKHR(app->physicalDevice, q, app->surface, &ok);
            if (ok) { family = int(q); break; }
        }
    }
    if (family < 0) fail("no suitable queue family");
    app->queueFamily = uint32_t(family);

    float prio = 1.f;
    VkDeviceQueueCreateInfo qi{};
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = app->queueFamily;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;

    // vkCreateDevice treats the pNext chain as a list of demands. The rule the spec states is that the
    // call fails with VK_ERROR_FEATURE_NOT_PRESENT if any member of any feature struct in the chain is
    // VK_TRUE and the adapter does not report that feature, while VK_FALSE is always accepted. So the
    // chain is built out of what the probe of THIS adapter answered, never out of a fixed wish list,
    // and a struct for a capability the adapter does not offer is left out of the chain entirely:
    // clearing its members would already be accepted, but sending a driver a feature struct for an
    // extension we are not enabling either only invites it to be strict about something that should
    // never have been sent.
    //
    // This is the whole of the failure being fixed. The ray-tracing path used to chain
    // VkPhysicalDeviceRayQueryFeaturesKHR{rayQuery = VK_TRUE} in front of the acceleration-structure
    // struct whatever the hardware said, so an adapter that offers the ray-tracing pipeline but not
    // ray queries - a GTX 1080 Ti - had its device refused over a feature the path it was picked for
    // does not even use. VkPhysicalDeviceVulkan12Features{descriptorIndexing = VK_TRUE} sat in the
    // same chain and was the second candidate: nothing here binds a descriptor update-after-bind, so
    // there was no need to ask for it at all.
    const AdapterCaps& cap = app->chosen;

    // TEMPORARY SELF-TEST - DELETE. What does a driver do with a feature struct in a vkCreateDevice
    // chain that asks for something the adapter does not offer? That is the GTX 1080 Ti question. Two
    // probes:
    //   (a) an EXTENSION feature the Intel UHD 770 lacks, asked of it while the extension is not
    //       enabled;
    //   (b) a CORE feature this machine genuinely lacks, asked of the adapter in use. No extension is
    //       involved, so a refusal there can only be the feature request itself being rejected.
    if (getenv("RT_SELFTEST_FNP")) {
        uint32_t tn = 0;
        vkEnumeratePhysicalDevices(app->instance, &tn, nullptr);
        std::vector<VkPhysicalDevice> devs(tn);
        vkEnumeratePhysicalDevices(app->instance, &tn, devs.data());
        for (uint32_t i = 0; i < tn; ++i) {
            AdapterCaps t{};
            queryAdapterCaps(devs[i], i, &t);
            if (t.asf.accelerationStructure) continue;
            logMsg("selftest a: adapter %u (%s) reports accelerationStructure=no\n", i, t.props.deviceName);
            for (int attempt = 0; attempt < 2; ++attempt) {
                VkPhysicalDeviceAccelerationStructureFeaturesKHR probe{};
                probe.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
                probe.accelerationStructure = attempt ? VK_TRUE : VK_FALSE;
                VkDeviceCreateInfo d{};
                d.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
                d.pNext = &probe;
                VkDevice out = VK_NULL_HANDLE;
                VkResult r = vkCreateDevice(devs[i], &d, nullptr, &out);
                logMsg("selftest a: %s accelerationStructure -> %s (%d)\n",
                       attempt ? "requesting   " : "not requesting", vkResultName(r), int(r));
                if (out != VK_NULL_HANDLE) vkDestroyDevice(out, nullptr);
            }
        }

        VkPhysicalDeviceFeatures2 base{};
        base.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        vkGetPhysicalDeviceFeatures2(app->physicalDevice, &base);
        auto coreFeatureOff = [&](int i) -> const char* {
            switch (i) {
                case 0: return base.features.textureCompressionETC2 == VK_FALSE ? "textureCompressionETC2" : nullptr;
                case 1: return base.features.textureCompressionASTC_LDR == VK_FALSE ? "textureCompressionASTC_LDR" : nullptr;
                case 2: return base.features.alphaToOne == VK_FALSE ? "alphaToOne" : nullptr;
                case 3: return base.features.shaderStorageImageReadWithoutFormat == VK_FALSE
                               ? "shaderStorageImageReadWithoutFormat" : nullptr;
                case 4: return base.features.geometryShader == VK_FALSE ? "geometryShader" : nullptr;
                case 5: return base.features.tessellationShader == VK_FALSE ? "tessellationShader" : nullptr;
                default: return nullptr;
            }
        };
        auto forceCoreFeature = [&](VkPhysicalDeviceFeatures& f, const char* name) {
            if (!strcmp(name, "textureCompressionETC2")) f.textureCompressionETC2 = VK_TRUE;
            else if (!strcmp(name, "textureCompressionASTC_LDR")) f.textureCompressionASTC_LDR = VK_TRUE;
            else if (!strcmp(name, "alphaToOne")) f.alphaToOne = VK_TRUE;
            else if (!strcmp(name, "shaderStorageImageReadWithoutFormat")) f.shaderStorageImageReadWithoutFormat = VK_TRUE;
            else if (!strcmp(name, "geometryShader")) f.geometryShader = VK_TRUE;
            else f.tessellationShader = VK_TRUE;
        };
        const char* coreName = nullptr;
        for (int i = 0; i < 6 && !coreName; ++i) coreName = coreFeatureOff(i);
        if (!coreName) {
            logMsg("selftest b: every probed core feature is supported, nothing to probe\n");
        } else {
            logMsg("selftest b: %s is reported unsupported on %s; a throwaway device asks for it anyway\n",
                   coreName, cap.props.deviceName);
            for (int attempt = 0; attempt < 2; ++attempt) {
                VkPhysicalDeviceFeatures2 want{};
                want.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
                want.features = base.features;
                if (attempt) forceCoreFeature(want.features, coreName);
                VkDeviceCreateInfo d{};
                d.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
                d.pNext = &want;
                d.queueCreateInfoCount = 1;
                d.pQueueCreateInfos = &qi;
                VkDevice out = VK_NULL_HANDLE;
                VkResult r = vkCreateDevice(app->physicalDevice, &d, nullptr, &out);
                logMsg("selftest b: attempt %d %s %s -> %s (%d)\n", attempt,
                       attempt ? "(requesting)   " : "(not requesting)", coreName, vkResultName(r), int(r));
                if (out != VK_NULL_HANDLE) vkDestroyDevice(out, nullptr);
            }
        }
    }

    VkPhysicalDeviceVulkan12Features f12{};
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    f12.bufferDeviceAddress = cap.f12.bufferDeviceAddress;  // both paths address buffers this way
    f12.scalarBlockLayout = cap.f12.scalarBlockLayout;
    f12.uniformBufferStandardLayout = cap.f12.uniformBufferStandardLayout;
    f12.hostQueryReset = cap.f12.hostQueryReset;
    // Asked for only where it is offered, for the reason above: it is a member an older card is
    // likeliest to lack, and requesting it regardless would reproduce the ray-query failure one
    // feature over.
    f12.descriptorIndexing = cap.f12.descriptorIndexing;

    VkPhysicalDeviceAccelerationStructureFeaturesKHR asf{};
    asf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    // Both paths build acceleration structures on the device; host builds are never used, and
    // descriptors are written normally, so the other members of this struct stay FALSE.
    asf.accelerationStructure = cap.asf.accelerationStructure;

    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtf{};
    rtf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
    rtf.rayTracingPipeline = cap.rtf.rayTracingPipeline;

    VkPhysicalDeviceRayQueryFeaturesKHR rqf{};
    rqf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
    rqf.rayQuery = cap.rqf.rayQuery;

    // Only the struct the path in use needs goes in, plus the acceleration structure and the 1.2
    // aggregate. No VkPhysicalDeviceVulkan13Features struct is passed at all, because the renderer uses
    // no feature gated by 1.3; if that ever changes, its members come from cap.f13 exactly the same
    // way. The per-extension structs for features that 1.2 promoted must not accompany the aggregate
    // struct, which is why bufferDeviceAddress and the rest are requested through f12 and nowhere
    // else.
    //
    // VkBaseOutStructure is what the spec provides for walking a chain: every struct in one starts with
    // sType and then pNext, so the shared prefix can be addressed through it.
    VkBaseOutStructure* chain[3];
    size_t chainCount = 0;
    if (app->usingComputeFallback)
        chain[chainCount++] = reinterpret_cast<VkBaseOutStructure*>(&rqf);
    else
        chain[chainCount++] = reinterpret_cast<VkBaseOutStructure*>(&rtf);
    chain[chainCount++] = reinterpret_cast<VkBaseOutStructure*>(&asf);
    chain[chainCount++] = reinterpret_cast<VkBaseOutStructure*>(&f12);
    for (size_t i = 0; i + 1 < chainCount; ++i) chain[i]->pNext = chain[i + 1];
    chain[chainCount - 1]->pNext = nullptr;

    // The extensions are gated the same way as the features: only what the path in use traces, and
    // only what this adapter offers. The KHR ray-tracing API adds no queue flag of its own, but its
    // commands keep their _KHR spelling after the promotion into 1.3 and the loader only hands those
    // entry points out when the extension is enabled, so the ones the chosen path needs have to be
    // here (verified: without this the pointers come back NULL).
    std::vector<const char*> exts;
    if (cap.extSwapchain) exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    exts.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
    if (app->usingComputeFallback) {
        if (cap.extRayQuery) exts.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
    } else {
        if (cap.extRtPipeline) exts.push_back(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
    }
    if (cap.extDeferredHostOps)
        exts.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);  // required by the AS extension
    if (cap.extPipelineLibrary) exts.push_back(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);

    // The request, in the log before it is made: if a driver still refuses, the machine's own file
    // says which features were asked of it.
    logMsg("device request: rayTracingPipeline=%s rayQuery=%s accelerationStructure=%s bufferDeviceAddress=%s\n",
           yesNo(!app->usingComputeFallback && rtf.rayTracingPipeline != VK_FALSE),
           yesNo(app->usingComputeFallback && rqf.rayQuery != VK_FALSE),
           yesNo(asf.accelerationStructure != VK_FALSE), yesNo(f12.bufferDeviceAddress != VK_FALSE));
    logMsg("                plus: scalarBlockLayout=%s uniformBufferStandardLayout=%s hostQueryReset=%s descriptorIndexing=%s\n",
           yesNo(f12.scalarBlockLayout != VK_FALSE),
           yesNo(f12.uniformBufferStandardLayout != VK_FALSE), yesNo(f12.hostQueryReset != VK_FALSE),
           yesNo(f12.descriptorIndexing != VK_FALSE));

    VkDeviceCreateInfo di{};
    di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    di.pNext = chain[0];
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = uint32_t(exts.size());
    di.ppEnabledExtensionNames = exts.data();

    const VkResult created = vkCreateDevice(app->physicalDevice, &di, nullptr, &app->device);
    if (created != VK_SUCCESS)
        // Every member of that chain came back from this adapter's own feature query, so a refusal now
        // means the combination is what the driver objects to rather than a capability it never had.
        // The two lines above are the request, which is what a remote diagnosis needs.
        fail("vkCreateDevice on %s failed: %s (%d) - the request logged above contains only features this adapter reported",
             cap.props.deviceName, vkResultName(created), int(created));
    volkLoadDevice(app->device);
    vkGetDeviceQueue(app->device, app->queueFamily, 0, &app->queue);

    VkPipelineCacheCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    vkCreatePipelineCache(app->device, &pci, nullptr, &app->pipelineCache);
}

// -------------------------------------------------------------------------------------------------
// Textures from the .pak (stb_image does the decoding)
#define STBI_NO_LINEAR
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

static void uploadTextureBytes(App* app, const uint8_t* rgba, uint32_t w, uint32_t h, GpuImage* out) {
    createImage(app, w, h, VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, out);

    VkDeviceSize bytes = VkDeviceSize(w) * h * 4;
    GpuBuffer stage{};
    createBuffer(app, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, false,
                 &stage);
    memcpy(stage.mapped, rgba, size_t(bytes));

    VkCommandBuffer cmd = beginOneTime(app);
    transitionImage(app, cmd, out, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    VkBufferImageCopy region{};
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, stage.buf, out->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &region);
    transitionImage(app, cmd, out, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT);
    endOneTime(app, cmd);
    destroyBuffer(app, &stage);
}

void loadPakTextures(App* app) {
    const uint8_t white[4] = {255, 255, 255, 255};
    uploadTextureBytes(app, white, 1, 1, &app->whiteTex);   // filler for unused slots

    int slot = 0;
    for (const TextureDef& td : app->cfg.textures) {
        if ((int)app->textures.size() >= kMaxTextures) break;
        std::vector<uint8_t> blob;
        if (!app->pakLoaded || !app->pak.read(td.file, &blob)) {
            logMsg("  texture absent from pak (shader uses its procedural fallback): %s\n",
                   td.file.c_str());
            continue;
        }
        int w = 0, h = 0, n = 0;
        stbi_set_flip_vertically_on_load(0);
        uint8_t* px = stbi_load_from_memory(blob.data(), int(blob.size()), &w, &h, &n, 4);
        if (!px) {
            logMsg("  texture decode failed: %s (%s)\n", td.file.c_str(), stbi_failure_reason());
            continue;
        }
        GpuImage im{};
        uploadTextureBytes(app, px, uint32_t(w), uint32_t(h), &im);
        stbi_image_free(px);
        app->textures.push_back(im);
        logMsg("  texture from pak: %s (%dx%d) -> slot %d\n", td.file.c_str(), w, h, slot++);
    }
    // A material may only point at a slot that actually exists.
    for (MaterialDef& m : app->cfg.materials)
        if (m.textureSlot >= (int)app->textures.size()) m.textureSlot = -1;
}

// -------------------------------------------------------------------------------------------------
// Scene loading
void loadScene(App* app, const std::string& explicitPak) {
    app->cfg.loadDefaults();
    app->cfg.maxBounces = 6;
    app->cfg.exposure = 1.0f;

    std::vector<std::string> candidates;
    char exePath[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string exeDir = exePath;
    size_t slash = exeDir.find_last_of("\\/");
    exeDir = (slash == std::string::npos) ? "." : exeDir.substr(0, slash);

    if (!explicitPak.empty()) candidates.push_back(explicitPak);
    candidates.push_back(exeDir + "\\scene.pak");
    candidates.push_back(exeDir + "\\assets.pak");
    candidates.push_back("scene.pak");
    candidates.push_back("assets.pak");
    candidates.push_back(exeDir + "\\pak\\scene.pak");

    for (const auto& c : candidates) {
        std::string err;
        if (app->pak.open(c, &err)) {
            app->pakLoaded = true;
            logMsg("pak: %s (%zu entries)\n", c.c_str(), app->pak.entryCount());
            break;
        }
        if (app->verbose) logMsg("  pak candidate %s: %s\n", c.c_str(), err.c_str());
    }
    if (!app->pakLoaded) {
        logMsg("pak: none found; using built-in defaults (window still renders)\n");
        return;
    }

    std::vector<uint8_t> blob;
    if (app->pak.read("scene.json", &blob)) {
        std::string text(blob.begin(), blob.end());
        JsonValue root;
        std::string jerr;
        if (jsonParse(text, &root, &jerr)) {
            std::string warn;
            if (app->cfg.applyJson(root, &warn))
                logMsg("scene.json applied\n");
            else
                logMsg("scene.json partially applied: %s\n", warn.c_str());
        } else {
            logMsg("scene.json parse error (%s); defaults kept\n", jerr.c_str());
        }
    } else {
        logMsg("scene.json not in pak; defaults kept\n");
    }

    std::vector<uint8_t> readme;
    if (app->pak.read("README.txt", &readme)) {
        readme.push_back(0);
        if (app->verbose) logMsg("pak README: %s\n", reinterpret_cast<const char*>(readme.data()));
    }
}

// -------------------------------------------------------------------------------------------------
// Geometry + acceleration structures
static void interleaveMesh(const Mesh& m, std::vector<VertexPN>* out) {
    out->resize(m.positions.size());
    for (size_t i = 0; i < m.positions.size(); ++i) {
        VertexPN& v = (*out)[i];
        v.px = m.positions[i].v[0]; v.py = m.positions[i].v[1]; v.pz = m.positions[i].v[2];
        v.nx = m.normals[i].v[0]; v.ny = m.normals[i].v[1]; v.nz = m.normals[i].v[2];
    }
}

static VkDeviceSize alignUp(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) / a * a; }

void createSceneGpuResources(App* app) {
    Mesh meshes[4];
    meshes[kMeshSphere] = makeSphere(1.0f, 48, 24);
    meshes[kMeshCube] = makeCube(1.0f);
    meshes[kMeshCone] = makeCone(1.0f, 1.0f, 96);
    meshes[kMeshDisc] = makeDisc(1.0f, 128);

    VkDeviceSize scratchAlign =
        std::max<VkDeviceSize>(256, app->asProps.minAccelerationStructureScratchOffsetAlignment);
    VkDeviceSize maxScratch = 0;

    for (int k = 0; k < 4; ++k) {
        MeshGpu& m = app->meshes[k];
        std::vector<VertexPN> verts;
        interleaveMesh(meshes[k], &verts);
        m.vertexCount = uint32_t(verts.size());
        m.indexCount = uint32_t(meshes[k].indices.size());
        logMsg("  mesh %s: uploading %u verts / %u indices\n", meshes[k].name.c_str(),
               m.vertexCount, m.indexCount);

        uploadBytes(app, verts.data(), VkDeviceSize(verts.size()) * sizeof(VertexPN),
                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                    true, &m.vb);
        uploadBytes(app, meshes[k].indices.data(), VkDeviceSize(m.indexCount) * sizeof(uint32_t),
                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                    true, &m.ib);

        // Query the build sizes, allocate the AS, then build it in a one-time command buffer.
        VkAccelerationStructureGeometryKHR geo{};
        geo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        geo.geometry.triangles.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        geo.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        geo.geometry.triangles.vertexData.deviceAddress = m.vb.addr;
        geo.geometry.triangles.vertexStride = sizeof(VertexPN);
        geo.geometry.triangles.maxVertex = m.vertexCount;
        geo.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
        geo.geometry.triangles.indexData.deviceAddress = m.ib.addr;

        VkAccelerationStructureBuildGeometryInfoKHR bi{};
        bi.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
        bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        bi.geometryCount = 1;
        bi.pGeometries = &geo;

        uint32_t prim = m.indexCount / 3;
        VkAccelerationStructureBuildSizesInfoKHR sizes{};
        sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        vkGetAccelerationStructureBuildSizesKHR(app->device,
                                                VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi,
                                                &prim, &sizes);
        m.asSize = sizes.accelerationStructureSize;
        m.buildScratch = sizes.buildScratchSize;
        maxScratch = std::max(maxScratch, alignUp(m.buildScratch, scratchAlign));

        createBuffer(app, m.asSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, &m.asBuffer);
        VkAccelerationStructureCreateInfoKHR aci{};
        aci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        aci.buffer = m.asBuffer.buf;
        aci.offset = 0;
        aci.size = m.asSize;
        aci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        VK_CHECK(vkCreateAccelerationStructureKHR(app->device, &aci, nullptr, &m.blas));

        GpuBuffer scratch{};
        createBuffer(app, std::max<VkDeviceSize>(16, alignUp(m.buildScratch, scratchAlign)),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true,
                     &scratch);
        bi.dstAccelerationStructure = m.blas;
        bi.scratchData.deviceAddress = scratch.addr;

        VkCommandBuffer cmd = beginOneTime(app);
        // The uploaded vertex/index data must be visible to the build.
        memBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_SHADER_READ_BIT);
        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount = prim;
        const VkAccelerationStructureBuildRangeInfoKHR* ranges[1] = {&range};
        vkCmdBuildAccelerationStructuresKHR(cmd, 1, &bi, ranges);
        // BLAS device addresses are consumed by later TLAS builds.
        memBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                   VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                   VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
        endOneTime(app, cmd);
        destroyBuffer(app, &scratch);

        if (app->verbose)
            logMsg("  mesh %s: %u verts, %u indices, as=%llu B, scratch=%llu B\n",
                   meshes[k].name.c_str(), m.vertexCount, m.indexCount,
                   (unsigned long long)m.asSize, (unsigned long long)m.buildScratch);
    }

    // Instance buffers and the TLAS per frame slot.
    VkDeviceSize instBytes = VkDeviceSize(kMaxInstances) * sizeof(VkAccelerationStructureInstanceKHR);
    VkDeviceSize xformBytes = VkDeviceSize(kMaxInstances) * sizeof(GpuInstance);
    for (int i = 0; i < kFramesInFlight; ++i) {
        createBuffer(app, instBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, false,
                     &app->instanceStage[i]);
        createBuffer(app, xformBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, false,
                     &app->xformStage[i]);
        createBuffer(app, instBytes,
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                         VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true, &app->instanceDev[i]);
    }
    createBuffer(app, xformBytes,
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, &app->xformDev);

    // TLAS size: query with an instances geometry description.
    VkAccelerationStructureGeometryKHR tlasGeo{};
    tlasGeo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    tlasGeo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tlasGeo.geometry.instances.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    tlasGeo.geometry.instances.arrayOfPointers = VK_FALSE;
    tlasGeo.geometry.instances.data.deviceAddress = 0;

    VkAccelerationStructureBuildGeometryInfoKHR tlasBi{};
    tlasBi.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    tlasBi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tlasBi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    tlasBi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tlasBi.geometryCount = 1;
    tlasBi.pGeometries = &tlasGeo;
    uint32_t tlasPrim = kMaxInstances;
    VkAccelerationStructureBuildSizesInfoKHR tlasSizes{};
    tlasSizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetAccelerationStructureBuildSizesKHR(app->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                            &tlasBi, &tlasPrim, &tlasSizes);
    maxScratch = std::max(maxScratch, alignUp(tlasSizes.buildScratchSize, scratchAlign));
    app->scratchSize = std::max<VkDeviceSize>(16, maxScratch);

    for (int i = 0; i < kFramesInFlight; ++i) {
        createBuffer(app, app->scratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true, &app->scratch[i]);
        createBuffer(app, tlasSizes.accelerationStructureSize,
                     VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, &app->tlasBuffer[i]);
        VkAccelerationStructureCreateInfoKHR aci{};
        aci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        aci.buffer = app->tlasBuffer[i].buf;
        aci.size = tlasSizes.accelerationStructureSize;
        aci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        VK_CHECK(vkCreateAccelerationStructureKHR(app->device, &aci, nullptr, &app->tlas[i]));
    }
    if (app->verbose)
        logMsg("  tlas: %llu B, scratch %llu B (x%d slots)\n",
               (unsigned long long)tlasSizes.accelerationStructureSize,
               (unsigned long long)app->scratchSize, kFramesInFlight);

    createBuffer(app, sizeof(SceneGpuData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, false,
                 &app->sceneUbo[0]);
    createBuffer(app, sizeof(SceneGpuData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, false,
                 &app->sceneUbo[1]);
}

void uploadFrameInstances(App* app, int slotIdx, const std::vector<CpuInstance>& inst) {
    VkAccelerationStructureInstanceKHR* asInst =
        reinterpret_cast<VkAccelerationStructureInstanceKHR*>(app->instanceStage[slotIdx].mapped);
    GpuInstance* mats = reinterpret_cast<GpuInstance*>(app->xformStage[slotIdx].mapped);
    memset(asInst, 0, sizeof(VkAccelerationStructureInstanceKHR) * inst.size());

    for (size_t i = 0; i < inst.size(); ++i) {
        const CpuInstance& ci = inst[i];
        VkAccelerationStructureInstanceKHR& v = asInst[i];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) v.transform.matrix[r][c] = ci.xf.m[r][c];
        v.instanceCustomIndex = uint32_t((ci.meshKind << 8) | ci.material) & 0x00FFFFFFu;
        v.mask = ci.isLight ? kMaskLight : kMaskOccluder;
        v.instanceShaderBindingTableRecordOffset = 0;
        v.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        VkAccelerationStructureDeviceAddressInfoKHR dai{};
        dai.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
        dai.accelerationStructure = app->meshes[ci.meshKind].blas;
        v.accelerationStructureReference = vkGetAccelerationStructureDeviceAddressKHR(app->device, &dai);
        if (v.accelerationStructureReference == 0) fail("BLAS device address is 0");
        fillInstanceMatrices(ci.xf, &mats[i]);
    }
}

// -------------------------------------------------------------------------------------------------
void createDescriptorLayouts(App* app) {
    VkShaderStageFlags stages =
        VK_SHADER_STAGE_COMPUTE_BIT |   // the denoiser reads the scene block from this same set
        (app->usingComputeFallback
             ? VkShaderStageFlags(0)
             : (VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                VK_SHADER_STAGE_MISS_BIT_KHR));

    VkDescriptorSetLayoutBinding b[7] = {};
    const VkDescriptorType types[7] = {
        VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        VK_DESCRIPTOR_TYPE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    // Binding 6 is the denoiser's feature array: albedo, normal, depth. It is always bound, even
    // when denoising is off, because a descriptor holding a null view is only safe if nothing ever
    // reads it - and "nothing ever reads it" is a claim about a uniform value, not the type system.
    const uint32_t counts[7] = {1, 1, 1, kMaxTextures, 1, 1, 3};
    for (int i = 0; i < 7; ++i) {
        b[i].binding = uint32_t(i);
        b[i].descriptorType = types[i];
        b[i].descriptorCount = counts[i];
        b[i].stageFlags = stages;
    }
    VkDescriptorSetLayoutCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.bindingCount = 7;
    ci.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(app->device, &ci, nullptr, &app->traceSetLayout));

    VkDescriptorSetLayoutBinding pb[3] = {};
    pb[0].binding = 0;
    pb[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pb[0].descriptorCount = 1;
    pb[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pb[1].binding = 2;
    pb[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pb[1].descriptorCount = 1;
    pb[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pb[2].binding = 3;
    pb[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pb[2].descriptorCount = 1;
    pb[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    pci.bindingCount = 3;
    pci.pBindings = pb;
    VK_CHECK(vkCreateDescriptorSetLayout(app->device, &pci, nullptr, &app->presentSetLayout));

    VkPipelineLayoutCreateInfo tli{};
    tli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    tli.setLayoutCount = 1;
    tli.pSetLayouts = &app->traceSetLayout;
    VK_CHECK(vkCreatePipelineLayout(app->device, &tli, nullptr, &app->traceLayout));

    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &app->presentSetLayout;
    VK_CHECK(vkCreatePipelineLayout(app->device, &pli, nullptr, &app->presentLayout));

    // Denoiser set (descriptor set 1). Bindings 0..3 are this frame's inputs - noisy radiance and
    // the three feature images - 4..6 the previous filtered state and 7..9 this frame's. All storage
    // images, all resident in GENERAL, so no layout work is needed per frame; ordering is done with
    // memory barriers at the dispatch.
    VkDescriptorSetLayoutBinding db[10] = {};
    for (int i = 0; i < 10; ++i) {
        db[i].binding = uint32_t(i);
        db[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        db[i].descriptorCount = 1;
        db[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dci.bindingCount = 10;
    dci.pBindings = db;
    VK_CHECK(vkCreateDescriptorSetLayout(app->device, &dci, nullptr, &app->denSetLayout));

    // Set 0 is reused from the trace pipeline so the filter can read the same scene block.
    const VkDescriptorSetLayout denPipelineSets[2] = {app->traceSetLayout, app->denSetLayout};
    VkPipelineLayoutCreateInfo dli{};
    dli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    dli.setLayoutCount = 2;
    dli.pSetLayouts = denPipelineSets;
    VK_CHECK(vkCreatePipelineLayout(app->device, &dli, nullptr, &app->denLayout));

    VkDescriptorPoolSize denPoolSizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 10 * kFramesInFlight}};
    VkDescriptorPoolCreateInfo dpci2{};
    dpci2.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci2.maxSets = kFramesInFlight;
    dpci2.poolSizeCount = 1;
    dpci2.pPoolSizes = denPoolSizes;
    VK_CHECK(vkCreateDescriptorPool(app->device, &dpci2, nullptr, &app->denPool));

    // Wavelet pass. Seven storage images (source colour and variance, the three feature images, the
    // two outputs) plus a push constant block carrying the dilation step and the iteration flags.
    VkDescriptorSetLayoutBinding ab[7] = {};
    for (int i = 0; i < 7; ++i) {
        ab[i].binding = uint32_t(i);
        ab[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        ab[i].descriptorCount = 1;
        ab[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo aci{};
    aci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    aci.bindingCount = 7;
    aci.pBindings = ab;
    VK_CHECK(vkCreateDescriptorSetLayout(app->device, &aci, nullptr, &app->atrousSetLayout));

    const VkDescriptorSetLayout atrousSets[2] = {app->traceSetLayout, app->atrousSetLayout};
    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.size = 28;   // mode, step, first, last, then sigmaN, sigmaL, sigmaZ
    VkPipelineLayoutCreateInfo ali{};
    ali.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    ali.setLayoutCount = 2;
    ali.pSetLayouts = atrousSets;
    ali.pushConstantRangeCount = 1;
    ali.pPushConstantRanges = &pcr;
    VK_CHECK(vkCreatePipelineLayout(app->device, &ali, nullptr, &app->atrousLayout));

    VkDescriptorPoolSize atrousPoolSizes[] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 7 * kFramesInFlight * 3}};
    VkDescriptorPoolCreateInfo apci{};
    apci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    apci.maxSets = kFramesInFlight * 3;
    apci.poolSizeCount = 1;
    apci.pPoolSizes = atrousPoolSizes;
    VK_CHECK(vkCreateDescriptorPool(app->device, &apci, nullptr, &app->atrousPool));
    VkDescriptorPoolSize pool[] = {
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, kFramesInFlight},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kFramesInFlight * 4},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight * 2},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kFramesInFlight * kMaxTextures},
        {VK_DESCRIPTOR_TYPE_SAMPLER, kFramesInFlight},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kFramesInFlight},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight},
    };
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = kFramesInFlight * 2;
    dpci.poolSizeCount = uint32_t(sizeof(pool) / sizeof(pool[0]));
    dpci.pPoolSizes = pool;
    VK_CHECK(vkCreateDescriptorPool(app->device, &dpci, nullptr, &app->descPool));

    for (int i = 0; i < kFramesInFlight; ++i) {
        VkDescriptorSetAllocateInfo dai{};
        dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dai.descriptorPool = app->descPool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &app->traceSetLayout;
        VK_CHECK(vkAllocateDescriptorSets(app->device, &dai, &app->traceSet[i]));
        dai.pSetLayouts = &app->presentSetLayout;
        VK_CHECK(vkAllocateDescriptorSets(app->device, &dai, &app->presentSet[i]));
        dai.descriptorPool = app->denPool;
        dai.pSetLayouts = &app->denSetLayout;
        VK_CHECK(vkAllocateDescriptorSets(app->device, &dai, &app->denSet[i]));
        dai.descriptorPool = app->atrousPool;
        dai.descriptorSetCount = 3;
        // An array of layouts, not the address of one: the driver reads descriptorSetCount
        // handles from here, so pointing at a single member made it walk into the rest of the
        // struct and fault before anything was rendered.
        const VkDescriptorSetLayout atrousLayouts[3] = {app->atrousSetLayout, app->atrousSetLayout,
                                                        app->atrousSetLayout};
        dai.pSetLayouts = atrousLayouts;
        VK_CHECK(vkAllocateDescriptorSets(app->device, &dai, app->atrousSet[i]));
    }
}

VkImageView featureView(App* app, int slotIdx, int c) {
    // The feature images exist only while the denoiser or a feature view needs them. Binding the
    // accumulation target in their place keeps every descriptor pointing at a real GENERAL image;
    // the shader gates its writes on the same condition, so a placeholder is never touched.
    return app->gbuf[slotIdx][c].view ? app->gbuf[slotIdx][c].view : app->hdr[slotIdx].view;
}

void writeTraceDescriptors(App* app, int slotIdx) {
    VkDescriptorImageInfo texInfos[kMaxTextures];
    for (int i = 0; i < kMaxTextures; ++i) {
        memset(&texInfos[i], 0, sizeof(texInfos[i]));
        const GpuImage* im = (i < (int)app->textures.size()) ? &app->textures[i] : &app->whiteTex;
        texInfos[i].imageView = im->view;
        texInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    VkDescriptorImageInfo samplerInfo{};
    samplerInfo.sampler = app->sampler;
    VkDescriptorImageInfo hdrInfo{};
    hdrInfo.imageView = app->hdr[slotIdx].view;
    hdrInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo gbufInfo[3];
    for (int i = 0; i < 3; ++i) {
        memset(&gbufInfo[i], 0, sizeof(gbufInfo[i]));
        gbufInfo[i].imageView = featureView(app, slotIdx, i);
        gbufInfo[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    VkDescriptorImageInfo presentInfo{};
    // While a quality change is taking effect, show the previous complete frame rather than the
    // newly allocated (and not yet traced) target.
    presentInfo.imageView = app->holdPending[slotIdx] ? app->hold[slotIdx].view
                                                      : app->hdr[slotIdx].view;
    presentInfo.sampler = app->sampler;
    presentInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorBufferInfo uboInfo{};
    uboInfo.buffer = app->sceneUbo[slotIdx].buf;
    uboInfo.offset = 0;
    uboInfo.range = VK_WHOLE_SIZE;
    VkDescriptorBufferInfo xformInfo{};
    xformInfo.buffer = app->xformDev.buf;
    xformInfo.offset = 0;
    xformInfo.range = VK_WHOLE_SIZE;

    VkWriteDescriptorSetAccelerationStructureKHR asInfo{};
    asInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    asInfo.accelerationStructureCount = 1;
    asInfo.pAccelerationStructures = &app->tlas[slotIdx];

    const void* extra[7] = {&asInfo, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    VkDescriptorType types[7] = {
        VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        VK_DESCRIPTOR_TYPE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    uint32_t counts[7] = {1, 1, 1, kMaxTextures, 1, 1, 3};
    const VkDescriptorImageInfo* imgPtrs[7] = {nullptr, &hdrInfo, nullptr, texInfos, &samplerInfo,
                                               nullptr, gbufInfo};
    const VkDescriptorBufferInfo* bufPtrs[7] = {nullptr, nullptr, &uboInfo, nullptr, nullptr,
                                                &xformInfo, nullptr};

    VkWriteDescriptorSet writes[7];
    for (int i = 0; i < 7; ++i) {
        memset(&writes[i], 0, sizeof(writes[i]));
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].pNext = extra[i];
        writes[i].dstSet = app->traceSet[slotIdx];
        writes[i].dstBinding = uint32_t(i);
        writes[i].descriptorCount = counts[i];
        writes[i].descriptorType = types[i];
        writes[i].pImageInfo = imgPtrs[i];
        writes[i].pBufferInfo = bufPtrs[i];
    }
    vkUpdateDescriptorSets(app->device, 7, writes, 0, nullptr);

    VkWriteDescriptorSet pw[3] = {};
    pw[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    pw[0].dstSet = app->presentSet[slotIdx];
    pw[0].dstBinding = 0;
    pw[0].descriptorCount = 1;
    pw[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pw[0].pImageInfo = &presentInfo;
    pw[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    pw[1].dstSet = app->presentSet[slotIdx];
    pw[1].dstBinding = 2;
    pw[1].descriptorCount = 1;
    pw[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pw[1].pBufferInfo = &uboInfo;
    // The denoiser's filtered image, so the present pass can show it. Before the filter exists the
    // binding points at the HDR target, which is a real sampled image in the layout the sampler
    // expects - a descriptor is allowed to be unused but not to name nothing.
    VkDescriptorImageInfo denPresentInfo{};
    denPresentInfo.imageView = app->cfg.denoise ? app->denPP[slotIdx][1][0].view
                                                : app->hdr[slotIdx].view;
    denPresentInfo.sampler = app->sampler;
    denPresentInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    pw[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    pw[2].dstSet = app->presentSet[slotIdx];
    pw[2].dstBinding = 3;
    pw[2].descriptorCount = 1;
    pw[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pw[2].pImageInfo = &denPresentInfo;
    vkUpdateDescriptorSets(app->device, 3, pw, 0, nullptr);

    // Denoiser set. Bindings 0..3 read this frame, 4..6 the history, 7..9 the outputs.
    VkDescriptorImageInfo denImg[10];
    const VkImageView denViews[10] = {
        app->hdr[slotIdx].view,
        app->gbuf[slotIdx][0].view,
        app->gbuf[slotIdx][1].view,
        app->gbuf[slotIdx][2].view,
        app->cfg.denoise ? app->denHist[slotIdx][0].view : app->hdr[slotIdx].view,
        app->cfg.denoise ? app->denHist[slotIdx][1].view : app->hdr[slotIdx].view,
        app->cfg.denoise ? app->denHist[slotIdx][2].view : app->hdr[slotIdx].view,
        app->cfg.denoise ? app->denOut[slotIdx][0].view : app->hdr[slotIdx].view,
        app->cfg.denoise ? app->denOut[slotIdx][1].view : app->hdr[slotIdx].view,
        app->cfg.denoise ? app->denOut[slotIdx][2].view : app->hdr[slotIdx].view};
    for (int i = 0; i < 10; ++i) {
        memset(&denImg[i], 0, sizeof(denImg[i]));
        denImg[i].imageView = denViews[i];
        denImg[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    VkWriteDescriptorSet dw[10];
    for (int i = 0; i < 10; ++i) {
        memset(&dw[i], 0, sizeof(dw[i]));
        dw[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        dw[i].dstSet = app->denSet[slotIdx];
        dw[i].dstBinding = uint32_t(i);
        dw[i].descriptorCount = 1;
        dw[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        dw[i].pImageInfo = &denImg[i];
    }
    vkUpdateDescriptorSets(app->device, 10, dw, 0, nullptr);

    // Wavelet sets. Iteration 0 reads the temporal output and its moments; the others read each
    // other's scratch. Five iterations across the three sets leave the result in denPP[slot][1][0].
    {
        const VkImageView hdrFallback = app->hdr[slotIdx].view;
        const VkImageView srcCol[3] = {app->denOut[slotIdx][0].view, app->denPP[slotIdx][1][0].view,
                                       app->denPP[slotIdx][0][0].view};
        const VkImageView srcVar[3] = {app->denOut[slotIdx][1].view, app->denPP[slotIdx][1][1].view,
                                       app->denPP[slotIdx][0][1].view};
        const VkImageView dstCol[3] = {app->denPP[slotIdx][1][0].view, app->denPP[slotIdx][0][0].view,
                                       app->denPP[slotIdx][1][0].view};
        const VkImageView dstVar[3] = {app->denPP[slotIdx][1][1].view,
                                       app->denPP[slotIdx][0][1].view,
                                       app->denPP[slotIdx][1][1].view};
        for (int k = 0; k < 3; ++k) {
            const VkImageView views[7] = {
                srcCol[k] ? srcCol[k] : hdrFallback, srcVar[k] ? srcVar[k] : hdrFallback,
                featureView(app, slotIdx, 0), featureView(app, slotIdx, 1),
                featureView(app, slotIdx, 2),
                dstCol[k] ? dstCol[k] : hdrFallback, dstVar[k] ? dstVar[k] : hdrFallback};
            VkDescriptorImageInfo info[7];
            VkWriteDescriptorSet aw[7];
            for (int i = 0; i < 7; ++i) {
                memset(&info[i], 0, sizeof(info[i]));
                info[i].imageView = views[i];
                info[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                memset(&aw[i], 0, sizeof(aw[i]));
                aw[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                aw[i].dstSet = app->atrousSet[slotIdx][k];
                aw[i].dstBinding = uint32_t(i);
                aw[i].descriptorCount = 1;
                aw[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                aw[i].pImageInfo = &info[i];
            }
            vkUpdateDescriptorSets(app->device, 7, aw, 0, nullptr);
        }
    }
}
