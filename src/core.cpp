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

void fail(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "fatal: ");
    vfprintf(stderr, fmt, ap);
    if (g_logFile) {
        va_end(ap);
        va_start(ap, fmt);
        fprintf(g_logFile, "fatal: ");
        vfprintf(g_logFile, fmt, ap);
        fflush(g_logFile);
    }
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
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
// Device selection
static bool deviceRtCaps(App* app, VkPhysicalDevice dev, bool* rt, bool* rq) {
    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(dev, &p);

    VkPhysicalDeviceAccelerationStructureFeaturesKHR asf{};
    asf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtf{};
    rtf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
    rtf.pNext = &asf;
    VkPhysicalDeviceRayQueryFeaturesKHR rqf{};
    rqf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
    rqf.pNext = &rtf;
    VkPhysicalDeviceVulkan12Features f12{};
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    f12.pNext = &rqf;
    VkPhysicalDeviceFeatures2 f2{};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(dev, &f2);

    bool core13 = p.apiVersion >= VK_API_VERSION_1_3;
    *rt = core13 && rtf.rayTracingPipeline && asf.accelerationStructure && f12.bufferDeviceAddress;
    *rq = core13 && rqf.rayQuery && f12.bufferDeviceAddress;
    return *rt || *rq;
}

void pickPhysicalDevice(App* app) {
    uint32_t count = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(app->instance, &count, nullptr));
    if (!count) fail("no Vulkan-capable GPU found");
    std::vector<VkPhysicalDevice> devices(count);
    VK_CHECK(vkEnumeratePhysicalDevices(app->instance, &count, devices.data()));

    int best = -1, bestScore = -1000;
    bool bestRt = false, bestRq = false;

    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(devices[i], &p);

        bool rt = false, rq = false;
        deviceRtCaps(app, devices[i], &rt, &rq);
        logMsg("adapter %u: %-40s vulkan %u.%u  rayTracing=%s rayQuery=%s\n", i, p.deviceName,
               VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion), rt ? "yes" : "no",
               rq ? "yes" : "no");

        if (app->surface != VK_NULL_HANDLE) {
            uint32_t qc = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &qc, nullptr);
            std::vector<VkQueueFamilyProperties> qs(qc);
            vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &qc, qs.data());
            bool presentable = false;
            for (uint32_t q = 0; q < qc; ++q) {
                VkBool32 ok = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(devices[i], q, app->surface, &ok);
                if (ok && (qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { presentable = true; break; }
            }
            if (!presentable) continue;
        }

        int score = 0;
        if (app->wantRt && rt) score += 1000;
        if (!app->wantRt && rq) score += 1000;
        if (rq) score += 100;
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += 50;
        else if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score += 10;
        if (rt) score += 20;
        if (score > bestScore) { bestScore = score; best = int(i); bestRt = rt; bestRq = rq; }
    }
    if (best < 0) fail("no usable Vulkan device found");

    app->physicalDevice = devices[best];
    vkGetPhysicalDeviceProperties(app->physicalDevice, &app->props);

    app->asProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
    app->rtProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
    app->asProps.pNext = &app->rtProps;
    VkPhysicalDeviceProperties2 p2{};
    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    p2.pNext = &app->asProps;
    vkGetPhysicalDeviceProperties2(app->physicalDevice, &p2);

    app->usingComputeFallback = app->wantRt ? !bestRt : true;
    if (app->usingComputeFallback && !bestRq)
        fail("%s supports neither VK_KHR_ray_tracing_pipeline nor VK_KHR_ray_query",
             app->props.deviceName);

    logMsg("rendering on: %s  [%s]\n", app->props.deviceName,
           app->usingComputeFallback ? "ray-query compute path" : "ray-tracing pipeline");
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

    VkPhysicalDeviceVulkan12Features f12{};
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    f12.bufferDeviceAddress = VK_TRUE;
    f12.scalarBlockLayout = VK_TRUE;
    f12.descriptorIndexing = VK_TRUE;
    f12.uniformBufferStandardLayout = VK_TRUE;
    f12.hostQueryReset = VK_TRUE;

    VkPhysicalDeviceAccelerationStructureFeaturesKHR asf{};
    asf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    asf.accelerationStructure = VK_TRUE;
    asf.pNext = &f12;

    VkPhysicalDeviceRayQueryFeaturesKHR rqf{};
    rqf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
    rqf.rayQuery = VK_TRUE;
    rqf.pNext = &asf;

    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtf{};
    rtf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
    rtf.rayTracingPipeline = VK_TRUE;
    rtf.pNext = &rqf;

    void* chain = app->usingComputeFallback ? (void*)&rqf : (void*)&rtf;

    // The acceleration-structure / ray-tracing commands keep their _KHR spelling after promotion
    // into Vulkan 1.3, and the loader only hands out those entry points when the extension is
    // enabled - so enable them explicitly (verified: without this the pointers come back NULL).
    uint32_t devExtCount = 0;
    vkEnumerateDeviceExtensionProperties(app->physicalDevice, nullptr, &devExtCount, nullptr);
    std::vector<VkExtensionProperties> devExts(devExtCount);
    vkEnumerateDeviceExtensionProperties(app->physicalDevice, nullptr, &devExtCount, devExts.data());
    auto supported = [&](const char* name) {
        for (const auto& e : devExts)
            if (strcmp(e.extensionName, name) == 0) return true;
        return false;
    };

    std::vector<const char*> exts;
    exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    const char* rtExts[] = {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
                            VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
                            VK_KHR_RAY_QUERY_EXTENSION_NAME,
                            VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
                            VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME};
    for (const char* e : rtExts)
        if (supported(e)) exts.push_back(e);

    VkDeviceCreateInfo di{};
    di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    di.pNext = chain;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = uint32_t(exts.size());
    di.ppEnabledExtensionNames = exts.data();

    VK_CHECK(vkCreateDevice(app->physicalDevice, &di, nullptr, &app->device));
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
    VkShaderStageFlags stages = app->usingComputeFallback
                                    ? VK_SHADER_STAGE_COMPUTE_BIT
                                    : (VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                                       VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                                       VK_SHADER_STAGE_MISS_BIT_KHR);

    VkDescriptorSetLayoutBinding b[6] = {};
    const VkDescriptorType types[6] = {
        VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        VK_DESCRIPTOR_TYPE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    const uint32_t counts[6] = {1, 1, 1, kMaxTextures, 1, 1};
    for (int i = 0; i < 6; ++i) {
        b[i].binding = uint32_t(i);
        b[i].descriptorType = types[i];
        b[i].descriptorCount = counts[i];
        b[i].stageFlags = stages;
    }
    VkDescriptorSetLayoutCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.bindingCount = 6;
    ci.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(app->device, &ci, nullptr, &app->traceSetLayout));

    VkDescriptorSetLayoutBinding pb[2] = {};
    pb[0].binding = 0;
    pb[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pb[0].descriptorCount = 1;
    pb[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pb[1].binding = 2;
    pb[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pb[1].descriptorCount = 1;
    pb[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    pci.bindingCount = 2;
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

    VkDescriptorPoolSize pool[] = {
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, kFramesInFlight},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kFramesInFlight},
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
    }
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
    VkDescriptorImageInfo presentInfo{};
    presentInfo.imageView = app->hdr[slotIdx].view;
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

    const void* extra[6] = {&asInfo, nullptr, nullptr, nullptr, nullptr, nullptr};
    VkDescriptorType types[6] = {
        VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        VK_DESCRIPTOR_TYPE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    uint32_t counts[6] = {1, 1, 1, kMaxTextures, 1, 1};
    const VkDescriptorImageInfo* imgPtrs[6] = {nullptr, &hdrInfo, nullptr, texInfos, &samplerInfo, nullptr};
    const VkDescriptorBufferInfo* bufPtrs[6] = {nullptr, nullptr, &uboInfo, nullptr, nullptr, &xformInfo};

    VkWriteDescriptorSet writes[6];
    for (int i = 0; i < 6; ++i) {
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
    vkUpdateDescriptorSets(app->device, 6, writes, 0, nullptr);

    VkWriteDescriptorSet pw[2] = {};
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
    vkUpdateDescriptorSets(app->device, 2, pw, 0, nullptr);
}
