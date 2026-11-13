// present.frag - divides the accumulated HDR radiance by the sample count, tone maps (ACES
// approximation) and encodes to the swapchain's UNORM back buffer.
#version 450

#include "sceneblock.glsl"

layout(set = 0, binding = 0) uniform sampler2D hdrTexture;
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

    radiance *= CAM_POST.x;                       // exposure, from scene.json
    vec3 colour = acesApprox(radiance);

    vec2 q = vUv - 0.5;                           // gentle vignette
    colour *= 1.0 - 0.45 * dot(q, q);

    colour = encodeSrgb(clamp(colour, 0.0, 1.0));

    float dither = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
    colour += (dither - 0.5) / 255.0;

    outColor = vec4(colour, 1.0);
}
