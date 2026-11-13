// ---- begin hit.rchit ----
// hit.rchit - closest-hit for shading rays. It only reports geometry; all transport/BRDF work is
// done by the ray-generation shader, which keeps recursion depth at 1 and makes the lighting model
// live in exactly one place.
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
// ---- begin instances.glsl ----
// instances.glsl - per-instance transforms supplied by the host.
//
// The built-in ray-tracing matrices (gl_ObjectToWorldEXT / gl_WorldToObject3x4EXT) have awkward
// GLSL typing (a mat3x4 cannot be multiplied by a vec4), so the host uploads both matrices per TLAS
// instance instead. That keeps the point/normal transforms unambiguous and portable, and lets the
// CPU precompute the world-to-object matrix used for normal mapping.
//
// std430 + mat4 members: 64 bytes per matrix, 128 bytes per instance, matching GpuInstance in
// src/scene.h.
struct InstanceXform {
    mat4 objectToWorld;
    mat4 worldToObject;
};

layout(set = 0, binding = 5, std430) buffer InstanceBlock {
    InstanceXform instances[];
} instanceData;

// World-space point -> object space.
vec3 worldToObjectPoint(int idx, vec3 p) {
    return (instanceData.instances[idx].worldToObject * vec4(p, 1.0)).xyz;
}

// Object-space normal -> world space. Transposing the world-to-object linear part is the correct
// normal transform and stays exact under non-uniform instance scaling.
vec3 objectNormalToWorld(int idx, vec3 nObj) {
    mat4 B = instanceData.instances[idx].worldToObject;
    vec3 n = vec3(dot(B[0].xyz, nObj), dot(B[1].xyz, nObj), dot(B[2].xyz, nObj));
    float l = length(n);
    return l > 1e-8 ? n / l : nObj;
}
// ---- end instances.glsl ----
// ---- begin primitives.glsl ----
// primitives.glsl - analytic per-primitive geometry helpers, shared by the closest-hit shaders and
// the path tracer. All primitives are authored at unit size and scaled by the instance transform:
//   KIND_SPHERE : radius 1 at the origin
//   KIND_CUBE   : half-extent 0.5 at the origin
//   KIND_CONE   : base radius 1 in the y=0 plane, apex at y=1
//   KIND_DISC   : radius 1 in the y=0 plane, facing +y
// Normals and UVs are derived from the object-space hit position, so the meshes need no UV channel
// and the shading is exact rather than interpolated.

vec3 objectNormal(uint kind, vec3 p) {
    if (kind == KIND_SPHERE) {
        return normalize(p + vec3(1e-9));
    } else if (kind == KIND_CUBE) {
        vec3 a = abs(p);
        vec3 axis = (a.x >= a.y && a.x >= a.z) ? vec3(1.0, 0.0, 0.0)
                    : ((a.y >= a.z) ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0));
        return axis * sign(dot(axis, p) + 1e-9);
    } else if (kind == KIND_CONE) {
        float y = clamp(p.y, 0.0, 1.0);
        float rr = length(p.xz);
        if (y < 0.02 && rr > 0.02) return vec3(0.0, -1.0, 0.0);   // base disc
        vec3 g = vec3(2.0 * p.x, 2.0 * (1.0 - y), 2.0 * p.z);     // grad of x^2+z^2-(1-y)^2
        if (length(g.xz) < 1e-6) return vec3(0.0, 1.0, 0.0);      // apex
        return normalize(g);
    }
    return vec3(0.0, 1.0, 0.0);                                   // disc
}

// Object-space normal -> world space. The transpose of the world-to-object linear part is the
// correct normal transform and stays exact under non-uniform instance scaling.
vec3 objectNormalToWorldMat(mat4 worldToObject, vec3 nObj) {
    vec3 n = vec3(dot(worldToObject[0].xyz, nObj),
                  dot(worldToObject[1].xyz, nObj),
                  dot(worldToObject[2].xyz, nObj));
    float l = length(n);
    return l > 1e-8 ? n / l : nObj;
}

vec2 objectUV(uint kind, vec3 p) {
    if (kind == KIND_DISC || kind == KIND_CUBE) return vec2(p.x, p.z);
    return vec2(atan(p.z, p.x) / (2.0 * 3.14159265358979323846) + 0.5, p.y);
}
// ---- end primitives.glsl ----

layout(location = 0) rayPayloadInEXT RayHit rayHit;

// The built-in triangle attribute is declared but intentionally unused: normals are derived
// analytically from the object-space hit position, which is exact and needs no UV channel.
hitAttributeEXT vec3 barycentrics;

void main() {
    uint ci = gl_InstanceCustomIndexEXT;
    int idx = int(gl_InstanceID);
    uint kind = ci >> 8u;
    float t = gl_HitTEXT;
    vec3 world = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * t;
    vec3 obj = worldToObjectPoint(idx, world);

    vec3 n = objectNormalToWorld(idx, objectNormal(kind, obj));

    rayHit.posT = vec4(world, t);
    rayHit.posObj = vec4(obj, 0.0);
    rayHit.normal = vec4(n, 0.0);
    rayHit.meta = vec4(float(ci & 0xFFu) + 1.0,
                       float(kind),
                       dot(gl_WorldRayDirectionEXT, n) < 0.0 ? 1.0 : 0.0,
                       barycentrics.x);
}
// ---- end hit.rchit ----
