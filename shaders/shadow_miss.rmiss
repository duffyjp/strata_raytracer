// shadow_miss.rmiss - a shadow ray that escapes means the light is visible.
#version 460 core
#extension GL_EXT_ray_tracing : require

layout(location = 1) rayPayloadInEXT uint occludedFlag;

void main() {
    occludedFlag = 0u;
}
