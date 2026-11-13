// ---- begin present.frag ----
// present.frag - divides the accumulated HDR radiance by the sample count, tone maps (ACES
// approximation) and encodes to the swapchain's UNORM back buffer.
#version 450

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

layout(set = 0, binding = 0) uniform sampler2D hdrTexture;
layout(set = 0, binding = 3) uniform sampler2D denTexture;   // denoiser output, for comparison
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

vec3 acesApprox(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

vec3 encodeSrgb(vec3 c) {
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(max(c, vec3(1e-6)), vec3(1.0 / 2.4)) - 0.055;
    return mix(hi, lo, lessThanEqual(c, vec3(0.0031308)));
}

void main() {
    // skyMode 9 in scene.json paints a UV grid instead of the frame: a built-in check that the
    // present path itself works, independent of the tracer.
    vec2 uv = vec2(vUv.x, mix(vUv.y, 1.0 - vUv.y, CAM_DISPLAY.y));
    if (CAM_POST.w > 8.5) {
        outColor = vec4(fract(uv.x * 4.0), fract(uv.y * 4.0), 0.5, 1.0);
        return;
    }
    if (CAM_POST.w > 8.5) {           // debug hook: skyMode 9 in scene.json proves the present path
        outColor = vec4(fract(uv.x * 4.0), fract(uv.y * 4.0), 0.5, 1.0);
        return;
    }
    float samples = max(1.0, CAM_DISPLAY.x);
    float ssaa = CAM_DISPLAY.w;
    vec3 radiance;
    if (ssaa > 1.05) {
        // The trace target is larger than the back buffer. Take four taps spaced over the footprint
        // this pixel covers: a single bilinear tap would just pick one texel and the extra
        // resolution would shimmer instead of sharpening. At whole-number ratios this is exact.
        vec2 off = 0.25 * ssaa / max(vec2(1.0), CAM_FRAME.xy);
        radiance = textureLod(hdrTexture, uv + vec2(-off.x, -off.y), 0.0).rgb +
                   textureLod(hdrTexture, uv + vec2(off.x, -off.y), 0.0).rgb +
                   textureLod(hdrTexture, uv + vec2(-off.x, off.y), 0.0).rgb +
                   textureLod(hdrTexture, uv + vec2(off.x, off.y), 0.0).rgb;
        radiance *= 0.25;
    } else {
        radiance = textureLod(hdrTexture, uv, 0.0).rgb;
    }
    radiance /= samples;
    if (CAM_FILTER.x > 0.5) {
        // The denoiser's output is already a mean radiance, so it replaces the accumulated value
        // rather than being divided by the frame count a second time.
        radiance = textureLod(denTexture, uv, 0.0).rgb;
    }

    radiance *= CAM_POST.x;                       // exposure, from scene.json
    vec3 colour = acesApprox(radiance);

    vec2 q = vUv - 0.5;                           // gentle vignette
    colour *= 1.0 - 0.45 * dot(q, q);

    colour = encodeSrgb(clamp(colour, 0.0, 1.0));

    float dither = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
    colour += (dither - 0.5) / 255.0;

    outColor = vec4(colour, 1.0);
}
// ---- end present.frag ----
