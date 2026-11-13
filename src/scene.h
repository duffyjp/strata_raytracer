// scene.h - scene description: authored data, procedural meshes, and the GPU-side block layouts.
//
// The structs here are mirrored 1:1 by the GLSL uniform/storage blocks (see shaders/common.glsl).
// All GPU blocks are std430 (scalar) composed only of vec4 members so the C++ and GLSL layouts are
// identical with no padding surprises.
#ifndef SCENE_H
#define SCENE_H

#include <cstdint>
#include <string>
#include <vector>

struct JsonValue;
class Pak;

struct alignas(16) V4 {
    float x = 0.f, y = 0.f, z = 0.f, w = 0.f;
    V4() = default;
    V4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}
};

static constexpr int kMaxMaterials = 32;
static constexpr int kMaxLights = 4;
static constexpr int kMaxInstances = 64;
static constexpr int kMaxTextures = 8;

// --- GPU material record: 4 vec4 = 64 bytes ---------------------------------------------------
struct GpuMaterial {
    V4 albedo;          // rgb, w = diffuse amount
    V4 emissive;        // rgb * strength, w unused
    V4 params;          // x = reflectivity(0..1) y = roughness z = ior w = texture scale
    V4 flags;           // x = shading model (0 lambert,1 metal,2 glass,3 emissive) y = texture slot-1 (-1 none) z,w unused
};

struct GpuLight {
    V4 posRadius;       // xyz, w = radius
    V4 color;           // rgb premultiplied by intensity, w = on/off
};

struct GpuCamera {
    V4 originFov;       // xyz = position, w = tan(fovY/2)
    V4 rightAspect;     // xyz = right, w = aspect (width/height)
    V4 up;              // xyz = up
    V4 forwardBounces;  // xyz = forward, w = max bounces
    V4 frameSize;       // x = width, y = height, z = 1 if accumulation resets this frame, w = time
    V4 exposureDebug;   // x = exposure, y = frame index, z = radiance clamp, w = sky mode
};

// Storage block written to the GPU: camera/params, then the material table, then the lights.
// Byte layout must match shaders/sceneblock.glsl.
struct SceneGpuData {
    V4 camera[8];
    V4 material[kMaxMaterials * 4];
    V4 light[kMaxLights * 2];
};

// --- Authored (CPU) description ----------------------------------------------------------------
struct MaterialDef {
    std::string name = "mat";
    float albedo[3] = {0.7f, 0.7f, 0.7f};
    float diffuse = 1.0f;
    float emissive[3] = {0.f, 0.f, 0.f};
    float reflectivity = 0.0f;
    float roughness = 0.05f;
    float ior = 1.5f;
    float textureScale = 1.0f;
    int model = 0;      // 0 lambert, 1 metal, 2 glass, 3 emissive
    int textureSlot = -1;
};

struct ObjectDef {
    std::string name = "obj";
    std::string kind = "sphere";    // sphere | cube | cone | disc
    std::string material = "mat";
    float position[3] = {0, 0, 0};
    float scale[3] = {1, 1, 1};
    float rotationY = 0.f;
    bool isLight = false;
    int lightIndex = -1;
};

struct TextureDef {
    std::string name = "texture";
    std::string file;               // path inside the pak
    float scale = 1.0f;
    int slot = -1;
};

struct CameraDef {
    float orbitRadius = 9.5f;
    float orbitHeight = 3.2f;
    float heightBob = 0.6f;
    float speed = 0.24f;            // rad/s for the camera
    float fovDeg = 42.f;
    float lookAt[3] = {0.f, 0.85f, 0.f};
};

struct LightDef {
    std::string name = "light";
    float color[3] = {1, 1, 1};
    float intensity = 60.f;
    float radius = 0.3f;
    float orbitRadius = 4.f;
    float orbitHeight = 2.5f;
    float speed = 0.55f;
    float phase = 0.f;
    bool on = true;
};

struct SceneConfig {
    CameraDef camera;
    SceneConfig() = default;
    float exposure = 1.7f;
    int maxBounces = 6;
    int accumulationFrames = 1;
    int samplesPerFrame = 1;        // primary rays per pixel per dispatched frame (1..4)

    // Adaptive quality: spend the frame budget the display leaves us. autoQuality picks samples per
    // frame (and drops the trace resolution only if one sample cannot fit) to hit targetFps while
    // keeping qualityHeadroom of each frame spare.
    bool autoQuality = true;
    float targetFps = 60.f;
    float qualityHeadroom = 0.26f;   // fraction of the frame left unused, for consistent pacing
    int maxSamplesPerFrame = 16;
    float minRenderScale = 0.4f;
    // Presentation and burden. vsync is on by default (FIFO): without it the loop renders as fast as
    // it can and tears. qualityMode bundles the burden limits so there is one knob to turn when the
    // frame budget cannot be met comfortably: high, balanced, performance or off (no adapting).
    bool vsync = true;
    std::string qualityMode = "high";
    float detectedRefresh = 0.f;   // read from the monitor at startup; 0 means fall back to targetFps
    int baseSamplesPerFrame = 4;
    float maxRenderScale = 2.0f;
    // Hard ceiling on the trace target, so a supersampled 8K window cannot exhaust video memory.
    double maxTracePixels = 8500000.0;
    float radianceClamp = 12.f;
    int skyMode = 0;
    float skyColor[3] = {0.40f, 0.45f, 0.54f};
    std::vector<MaterialDef> materials;
    std::vector<ObjectDef> objects;
    std::vector<LightDef> lights;
    std::vector<TextureDef> textures;

    // Loads defaults, then overlays `scene.json` (from the pak). Returns false only when the file
    // exists but is unusable; missing files are not an error.
    bool loadDefaults();
    bool applyJson(const JsonValue& root, std::string* warn);
    int materialIndexByName(const std::string& n) const;
    int textureIndexByName(const std::string& n) const;
};

// --- Meshes ------------------------------------------------------------------------------------
struct Vec3f { float v[3]; };

struct Mesh {
    std::string name;
    std::vector<Vec3f> positions;
    std::vector<Vec3f> normals;
    std::vector<uint32_t> indices;
};

// Interleaved vertex used for acceleration structures: position(12) + normal(12) = 24 bytes.
struct VertexPN {
    float px, py, pz, nx, ny, nz;
};

Mesh makeSphere(float radius, int segments, int rings);
Mesh makeCube(float size);
Mesh makeCone(float radius, float height, int segments);
Mesh makeDisc(float radius, int sectors);

// A transform in the 3x4 form used by VkAccelerationStructureInstanceKHR.
struct InstanceTransform {
    float m[3][4];
};

InstanceTransform makeTransform(const float position[3], const float scale[3], float rotationY);

// Per-instance matrices uploaded for the shaders (see shaders/instances.glsl): 2 * mat4 = 128 B.
struct GpuInstance {
    V4 col[8];
};

// Fills an object->world matrix (column-major) plus its inverse from an instance transform.
void fillInstanceMatrices(const InstanceTransform& t, GpuInstance* out);

#endif  // SCENE_H
