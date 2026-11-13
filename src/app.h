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
    // Denoiser feature targets, same lifetime and resolution as the HDR accumulation image:
    // 0 = albedo (w: a feature surface was found), 1 = normal (w: roughness),
    // 2 = reciprocal depth (y: the primary ray hit something, z: material index).
    GpuImage gbuf[kFramesInFlight][3];
    // The camera basis actually presented last frame, so reprojection works for the automated orbit
    // and the free camera alike.
    V4 prevCam[4] = {};
    bool prevCamValid = false;
    // SVGF temporal state. History belongs to a slot, not to the frame before: two frames are in
    // flight and the loop only waits on its own slot's fence, so reading the other slot's output
    // would be a race. A slot's own history was written two frames ago, which the fence does cover.
    GpuImage denHist[kFramesInFlight][3];    // 0 colour, 1 moments, 2 depth + material
    GpuImage denOut[kFramesInFlight][3];
    // Wavelet scratch: iterations ping-pong between index 0 and 1, each holding filtered
    // colour and the variance propagated with it. Five iterations from denOut land the result
    // in index 1, which is what present and the capture read.
    GpuImage denPP[kFramesInFlight][2][2];
    V4 prevCamSlot[kFramesInFlight][4];
    bool prevCamSlotValid[kFramesInFlight] = {};
    // Denoiser plumbing, created only when denoising is enabled.
    VkPipeline denPipeline = VK_NULL_HANDLE;
    VkPipelineLayout denLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout denSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool denPool = VK_NULL_HANDLE;
    VkDescriptorSet denSet[kFramesInFlight] = {};
    // Wavelet pass: three sets per slot cover all five iterations, because the src/dst pair
    // only takes three combinations while the iteration varies by push constant.
    VkPipeline atrousPipeline = VK_NULL_HANDLE;
    VkPipelineLayout atrousLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout atrousSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool atrousPool = VK_NULL_HANDLE;
    VkDescriptorSet atrousSet[kFramesInFlight][3] = {};
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
    // Window and camera control.
    bool fullscreen = false;
    int winRect[4] = {80, 60, 1280, 720};   // remembered so Alt+Enter can restore the window
    bool freeCam = false;                   // WASD has the camera (false also covers "handing back")
    float camBlend = 0.f;                   // 0 = automated orbit, 1 = free camera, eased between
    float yaw = 0.f, pitch = 0.f;
    float camPos[3] = {0.f, 0.f, 0.f};
    double lastWasd = -1e9;
    bool cursorTracked = false;
    bool testCamera = false;                // drives the same state machine from the clock, for tests
    float orbitO[3] = {0.f, 0.f, 0.f};      // last automated camera, so taking over does not jump
    float orbitF[3] = {0.f, 0.f, -1.f};
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
    uint32_t lateFramesWindow = 0;  // frames since the last quality change that missed the target
    uint32_t framesWindow = 0;      // ...out of how many
    int overStreak = 0;             // consecutive decision windows over budget
    int underStreak = 0;            // ...and comfortably under it
    int decideFrames = 0;           // readings since the controller last weighed anything
    double msPerSampleKiloPx = 0.0;  // smoothed unit cost, used to predict the next choice
    // The cost model has two terms, because the work has two terms. Tracing costs per sample per
    // traced pixel; the SVGF filter costs a fixed charge per traced pixel whatever the sample count
    // is. Folding the second into the first is what made one setting read 0.7 ms at one moment and
    // 11.2 ms at another - the filter's price was inside the per-sample constant whenever the filter
    // was on and absent whenever it was not, so the constant moved by the filter's cost every time
    // anyone pressed N.
    double filterMsPerMegaPx = 0.86;   // ms the filter costs per megapixel traced
    // That number can only be seen across a real flip at settings that did not change, so a flip
    // arms a measurement which completes once the frame has settled again.
    bool filterCalArmed = false;
    bool filterCalTurningOn = false;
    double filterCalBeforeMs = 0.0;
    double filterCalMegaPx = 0.0;
    float filterCalBeforeScale = 0.0f;
    int filterCalBeforeSpp = 0;
    int filterCalWait = 0;
    int modelRejects = 0;             // readings refused as unsettled or as outliers
    bool filterCalibrated = false;    // at least one two-sided measurement backs the constant
    bool denoiseUserToggled = false;  // N was pressed: the user owns the switch for this run
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
    // Debug only: dump the raw accumulation buffer, as floats, after this accumulation frame
    // (-1 = never). Convergence was measured at half the rate independent samples should give, and
    // aggregate statistics cannot tell whether consecutive frames repeat the same noise pattern or
    // shift it; correlating the raw samples can.
    int captureDumpFrame = -1;
    // Debug only: toggle denoising after this many rendered frames, so the mid-run toggle can be
    // exercised without a keyboard (-1 = never).
    int testToggleFrame = -1;
    // Debug only: after this many frames, force the condition the controller treats as 'the frame
    // rate cannot be reached' - samples and resolution already at their floor, streak over budget -
    // so the automatic decision can be exercised on a GPU that is comfortably fast enough.
    int testGiveUpFrame = -1;
    // Set along with it. The controller's condition for surrendering the filter is lateness against
    // the frame rate the user asked for, and on a GPU that comfortably holds 60 fps there is no way
    // to reach that state from inside a run, so this says the frames are late whether they are.
    bool testLateForced = false;
    // Debug only: replace the frame budget the controller works against (0 = leave it alone). The
    // real budget has a 1.5 ms floor, so on a GPU that clears it easily there is no other way to
    // reach the state where the frame rate genuinely cannot be held.
    double testBudgetMs = 0.0;
    // Consecutive readings in which frames arrived late or the GPU ran over budget. Feeds the
    // decision to give up the denoiser, and deliberately separate from the controller's own streaks,
    // which are reset whenever a reading falls inside the deadband.
    int denoiseLateStreak = 0;
    // Whether the frame just delivered missed the configured target. Measured against the target the
    // user asked for, not against the most common frame interval: the histogram-based signal decides
    // lateness relative to whatever cadence the loop has settled into, so a machine running at 24 fps
    // for sixteen seconds reports itself perfectly on time.
    bool lastFrameLate = false;
    // Denoiser bookkeeping for the automatic decision: whether the controller (rather than the
    // user) switched it off, and how many times it has been allowed to change its mind.
    bool denoiseAutoOff = false;
    int denoiseFlips = 0;
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
VkImageView featureView(App* app, int slotIdx, int c);
void fillSceneData(App* app, SceneGpuData* d, float t, uint32_t w, uint32_t h, bool reset,
                   int samplesForDisplay, int slotIdx);
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
void toggleFullscreen(App* app);   // Alt+Enter at runtime, or --fullscreen at startup
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
