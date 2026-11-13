// ---- begin shadow_hit.rchit ----
// shadow_hit.rchit - closest-hit for shadow rays: a hit means "occluded", nothing else.
#version 460 core
#extension GL_EXT_ray_tracing : require

layout(location = 1) rayPayloadInEXT uint occludedFlag;

void main() {
    occludedFlag = 1u;
}
// ---- end shadow_hit.rchit ----
