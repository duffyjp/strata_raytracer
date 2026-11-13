// hit.rchit - closest-hit for shading rays. It only reports geometry; all transport/BRDF work is
// done by the ray-generation shader, which keeps recursion depth at 1 and makes the lighting model
// live in exactly one place.
#version 460 core
#extension GL_EXT_ray_tracing : require

#include "payload.glsl"
#include "sceneblock.glsl"
#include "instances.glsl"
#include "primitives.glsl"

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
