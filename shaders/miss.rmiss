// miss.rmiss - shading rays that hit nothing: mark the payload as a miss so the ray-generation
// shader can substitute the environment colour.
#version 460 core
#extension GL_EXT_ray_tracing : require

#include "payload.glsl"
#include "sceneblock.glsl"

layout(location = 0) rayPayloadInEXT RayHit rayHit;

void main() {
    rayHit.meta = vec4(0.0, 0.0, 0.0, 1.0);
    rayHit.posT = vec4(gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_RayTmaxEXT, gl_RayTmaxEXT);
}
