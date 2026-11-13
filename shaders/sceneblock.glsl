// sceneblock.glsl - the host <-> GPU contract. Mirrored byte-for-byte by SceneGpuData in
// src/scene.h. std140 with vec4-only members means the C++ and GLSL layouts cannot drift.

struct Material {
    vec4 albedo;    // rgb, w = diffuse amount (unused for metal/glass)
    vec4 emissive;  // rgb * strength
    vec4 params;    // x = reflectivity, y = roughness, z = ior, w = texture scale
    vec4 flags;     // x = model (0 lambert, 1 metal, 2 glass, 3 emissive), y = texture slot or -1
};

struct Light {
    vec4 posRadius;  // xyz = centre, w = radius
    vec4 color;      // rgb * intensity, w = on/off
};

layout(set = 0, binding = 2, std140) uniform SceneBlock {
    vec4 camera[8];
    Material materials[32];
    Light lights[4];
} scene;

#define LIGHT_COUNT 4

// camera[] contract (see GpuCamera in src/scene.h)
#define CAM_ORIGIN_FOV scene.camera[0]   // xyz origin, w = tan(fovY/2)
#define CAM_RIGHT      scene.camera[1]   // xyz right, w = aspect
#define CAM_UP         scene.camera[2]   // xyz up
#define CAM_FWD        scene.camera[3]   // xyz forward, w = max bounces
#define CAM_FRAME      scene.camera[4]   // x = width, y = height, z = reset accumulation, w = time
#define CAM_POST       scene.camera[5]   // x = exposure, y = frame index, z = radiance clamp, w = sky mode
#define CAM_DISPLAY    scene.camera[6]   // x = samples accumulated for display, y = flipY
#define CAM_SKY        scene.camera[7]   // xyz horizon colour

// Cull-mask channels for VkAccelerationStructureInstanceKHR.mask.
#define OCCLUDER_MASK  1u
#define LIGHT_MASK     2u
#define ALL_MASK       255u

// Primitive kinds, packed into instanceCustomIndex as (kind << 8) | materialIndex.
#define KIND_SPHERE 0u
#define KIND_CUBE   1u
#define KIND_CONE   2u
#define KIND_DISC   3u
