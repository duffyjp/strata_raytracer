// ---- begin miss.rmiss ----
// miss.rmiss - shading rays that hit nothing: mark the payload as a miss so the ray-generation
// shader can substitute the environment colour.
#version 460 core
#extension GL_EXT_ray_tracing : require

// ---- begin payload.glsl ----
// payload.glsl - the ray payload shared by every stage of the ray-tracing pipeline.
// The declaration must be byte-identical wherever it is used:
//   main.rgen        declares it as rayPayloadEXT  (the caller / initialiser)
//   hit.rchit        declares it as rayPayloadInEXT
//   miss.rmiss       declares it as rayPayloadInEXT
// Shadow rays use their own single-word payload at location 1 (see shadow_hit/shadow_miss) so that
// each ray type only ever touches the payload variable it was traced with.
struct RayHit {
    vec4 posT;    // xyz = world hit position, w = t along the ray
    vec4 posObj;  // xyz = object-space hit position
    vec4 meta;    // x = materialIndex + 1 (0 = miss), y = primitive kind, z = front facing
    vec4 normal;  // xyz = world-space geometric normal
};
// ---- end payload.glsl ----
// ---- begin sceneblock.glsl ----
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
    vec4 camera[16];
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
// camera[8..11] hold the *previous* frame's basis, in the same layout as [0..3], so a temporal
// filter can reproject without the shader having to know how the camera is animated. camera[11].w
// is 1 when the previous frame is usable (the first frame after startup or a resize is not).
#define CAM_PREV_ORIGIN scene.camera[8]   // xyz origin, w = tan(fovY/2)
#define CAM_PREV_RIGHT  scene.camera[9]   // xyz right, w = aspect
#define CAM_PREV_UP     scene.camera[10]  // xyz up
#define CAM_PREV_FWD    scene.camera[11]  // xyz forward, w = previous frame valid
// Denoiser control. .x enables the feature writes, .y selects what the screen shows while the
// filter is being built (0 nothing, 1 albedo, 2 normal, 3 depth), .z the history cap.
#define CAM_FILTER      scene.camera[12]
#define GBUF_ALBEDO  scene.camera[13]     // rgb albedo of the feature surface, w = 1 if found
#define GBUF_NORMAL  scene.camera[14]     // xyz shading normal, w = linear depth of first hit
#define GBUF_DEPTH   scene.camera[15]     // x = first hit depth (1e5 for sky), yzw unused

// Cull-mask channels for VkAccelerationStructureInstanceKHR.mask.
#define OCCLUDER_MASK  1u
#define LIGHT_MASK     2u
#define ALL_MASK       255u

// Primitive kinds, packed into instanceCustomIndex as (kind << 8) | materialIndex.
#define KIND_SPHERE 0u
#define KIND_CUBE   1u
#define KIND_CONE   2u
#define KIND_DISC   3u
// ---- end sceneblock.glsl ----

layout(location = 0) rayPayloadInEXT RayHit rayHit;

void main() {
    rayHit.meta = vec4(0.0, 0.0, 0.0, 1.0);
    rayHit.posT = vec4(gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_RayTmaxEXT, gl_RayTmaxEXT);
}
// ---- end miss.rmiss ----
