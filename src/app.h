// app.h - shared state for the ray tracer's translation unit set (core + frame + capture).
#pragma once

#include "volk.h"

#include <GLFW/glfw3.h>

#include "pak.h"
#include "scene.h"

#include <string>
#include <vector>

static const int kFramesInFlight = 2;
enum MeshKind { kMeshSphere = 0, kMeshCube = 1, kMeshCone = 2, kMeshDisc = 3 };

// Cull-mask channels, mirroring OCCLUDER_MASK / LIGHT_MASK in shaders/sceneblock.glsl.
static const uint8_t kMaskOccluder = 1;
static const uint8_t kMaskLight = 2;

struct GpuBuffer {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceAddress addr = 0;
    VkDeviceSize size = 0;
    void* mapped = nullptr;
};

struct GpuImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct MeshGpu {
    GpuBuffer vb, ib, asBuffer;
    VkAccelerationStructureKHR blas = VK_NULL_HANDLE;
    uint32_t indexCount = 0;
    uint32_t vertexCount = 0;
    VkDeviceSize asSize = 0;
    VkDeviceSize buildScratch = 0;
};

struct CpuInstance {
    int meshKind = 0;
    int material = 0;
    bool isLight = false;
    InstanceTransform xf{};
};

struct App {
    // core
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps{};
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps{};

    // window / swapchain
    GLFWwindow* window = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D swapExtent{};
    std::vector<VkImage> swapImages;
    std::vector<VkImageView> swapViews;
    std::vector<VkFramebuffer> framebuffers;
    VkRenderPass renderPass = VK_NULL_HANDLE;

    // capability choice
    bool usingComputeFallback = false;
    bool wantRt = true;

    // scene
    MeshGpu meshes[4];
    GpuBuffer xformDev;
    GpuBuffer instanceStage[kFramesInFlight];
    GpuBuffer xformStage[kFramesInFlight];
    GpuBuffer instanceDev[kFramesInFlight];
    GpuBuffer scratch[kFramesInFlight];
    GpuBuffer tlasBuffer[kFramesInFlight];
    VkAccelerationStructureKHR tlas[kFramesInFlight] = {};
    VkDeviceSize scratchSize = 0;

    GpuBuffer sceneUbo[kFramesInFlight];
    GpuBuffer sbt;
    GpuImage hdr[kFramesInFlight];
    // The last complete image, kept alive across a quality change so the window never shows a
    // freshly cleared (blank) target: the next frame is traced into the new one while the screen
    // still displays this. A small hitch is preferable to a flash.
    GpuImage hold[kFramesInFlight];
    bool holdPending[kFramesInFlight] = {};
    // Two phases, because "installed" and "displayed" are different frames: the present descriptor
    // still points at the retired image until a frame has actually been recorded that showed it, and
    // the image must outlive that frame by one more fence. Releasing it a frame early means sampling
    // a destroyed view, which takes the device down with it.
    bool holdShown[kFramesInFlight] = {};
    GpuImage whiteTex;
    std::vector<GpuImage> textures;
    VkSampler sampler = VK_NULL_HANDLE;

    // descriptors / pipelines
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout traceSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout presentSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet traceSet[kFramesInFlight] = {};
    VkDescriptorSet presentSet[kFramesInFlight] = {};
    VkPipelineLayout traceLayout = VK_NULL_HANDLE;
    VkPipelineLayout presentLayout = VK_NULL_HANDLE;
    VkPipeline tracePipeline = VK_NULL_HANDLE;
    VkPipeline presentPipeline = VK_NULL_HANDLE;

    // per frame
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd[kFramesInFlight] = {};
    VkFence fence[kFramesInFlight] = {};
    VkSemaphore imageReady[kFramesInFlight] = {};
    VkSemaphore drawDone[kFramesInFlight] = {};
    int slot = 0;

    // state
    SceneConfig cfg;
    Pak pak;
    bool pakLoaded = false;
    uint64_t frameIndex = 0;
    float simTime = 0.f;
    uint32_t hdrWidth = 0, hdrHeight = 0;
    float renderScale = 1.0f;
    VkFormat hdrFormat = VK_FORMAT_UNDEFINED;
    int samplesAccumulated = 1;
    bool resizePending = false;
    bool verbose = false;
    // One-time uploads normally submit and wait. Mid-frame that wait is a full queue drain, which
    // also waits for the display - long enough for Windows to consider the window unresponsive and
    // paint it white. While this is set, one-time work is submitted without waiting (a single queue
    // preserves order) and its command buffers are freed at shutdown.
    bool deferOneTime = false;
    std::vector<VkCommandBuffer> retiredCmd;
    double lastChangeCostMs = 0.0;

    // adaptive quality: measure the GPU's own frame time, then spend the budget on samples/pixel
    VkQueryPool timeQueries = VK_NULL_HANDLE;
    bool timingSupported = false;
    double gpuMsAvg = 0.0;          // smoothed full-frame GPU time at the current settings
    double cpuMsAvg = 0.0;          // smoothed real interval between frames: did we make vsync?
    int cleanPaceStreak = 0;        // consecutive frames that met the deadline
    int overStreak = 0;             // consecutive decision windows over budget
    int underStreak = 0;            // ...and comfortably under it
    double msPerSampleKiloPx = 0.0;  // smoothed unit cost, used to predict the next choice
    int liveSpp = 1;                // samples per pixel per frame in effect right now
    float liveScale = 1.0f;         // trace resolution scale in effect right now
    float userScaleCap = 1.0f;      // ceiling from --scale / scene.json; auto never exceeds it
    int settleFrames = 0;           // frames since quality last changed
    bool qualityChanged = true;
    uint64_t qualityChanges = 0;

    // headless capture
    int captureCount = 0;
    double autoExitSeconds = 0.0;  // >0: close the window by itself (lets the windowed path be tested)
    int captureAccum = 48;
    std::string captureDir = "captures";
    uint32_t captureWidth = 1280, captureHeight = 720;
};

// core.cpp
extern FILE* g_logFile;  // always mirrored to a file: a GUI-subsystem binary has no console
void logMsg(const char* fmt, ...);
[[noreturn]] void fail(const char* fmt, ...);
const char* vkResultName(VkResult r);
VkResult vkCheck(const char* expr, VkResult r);
#define VK_CHECK(x) vkCheck(#x, (x))

uint32_t findMemoryType(App* app, uint32_t typeBits, VkMemoryPropertyFlags want);
void createBuffer(App* app, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                  bool deviceAddress, GpuBuffer* out);
void destroyBuffer(App* app, GpuBuffer* b);
void createImage(App* app, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
                 VkImageAspectFlags aspect, GpuImage* out);
void destroyImage(App* app, GpuImage* im);
void transitionImage(App* app, VkCommandBuffer cmd, GpuImage* im, VkImageLayout dst,
                     VkAccessFlags srcAccess, VkAccessFlags dstAccess, VkPipelineStageFlags srcStage,
                     VkPipelineStageFlags dstStage, VkImageAspectFlags aspect);
void memBarrier(VkCommandBuffer cmd, VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                VkPipelineStageFlags dstStage, VkAccessFlags dstAccess);
VkCommandBuffer beginOneTime(App* app);
void endOneTime(App* app, VkCommandBuffer cmd);

void pickPhysicalDevice(App* app);
void createDevice(App* app);
void createInstance(App* app);
void loadScene(App* app, const std::string& explicitPak);
void createSceneGpuResources(App* app);
void loadPakTextures(App* app);
void createShaderModulesAndPipelines(App* app);
void createDescriptorLayouts(App* app);
void writeTraceDescriptors(App* app, int slotIdx);
void fillSceneData(App* app, SceneGpuData* d, float t, uint32_t w, uint32_t h, bool reset,
                   int samplesForDisplay);
void computeInstances(App* app, float t, std::vector<CpuInstance>* out);
void uploadFrameInstances(App* app, int slotIdx, const std::vector<CpuInstance>& inst);
uint32_t sbtStrideFor(App* app);
void createShaderBindingTable(App* app);
void recreateHdrTargets(App* app, uint32_t width, uint32_t height);
void buildFrameCommand(App* app, int slotIdx, uint32_t imageIndex, bool includePresent,
                       const std::vector<CpuInstance>& instances, float t, bool reset,
                       int samplesForDisplay, bool traceIt);

// frame.cpp
bool createSwapchain(App* app, uint32_t width, uint32_t height);
void destroySwapchainObjects(App* app);
void createRenderPass(App* app);
void createFramebuffers(App* app);
void createPresentPipeline(App* app);
void createFrameSync(App* app);
int runWindowed(App* app);
int runCapture(App* app);
void createTimingQueries(App* app);
// Feed one measured GPU frame time (ms) to the controller; it re-picks quality when warranted.
void reportGpuFrameTime(App* app, double ms);
// Called after a resize so the controller re-measures at the new size instead of trusting stale data.
void invalidateQuality(App* app);
void readHdrToCpu(App* app, int slotIdx, std::vector<float>* out);
void writePng(const std::string& path, uint32_t w, uint32_t h, const std::vector<float>& hdr,
              float exposure, float invSamples);
void destroyApp(App* app);
