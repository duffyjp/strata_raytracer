// sky.glsl - analytic environment. Requires sceneblock.glsl to be included first.
vec3 skyColor(vec3 d) {
    float up = clamp(d.y, -1.0, 1.0);
    vec3 horizon = CAM_SKY.xyz;
    vec3 zenith = mix(horizon, vec3(0.13, 0.24, 0.45), 0.92);
    vec3 c = mix(horizon * 0.55, zenith, smoothstep(0.0, 0.55, up));
    c = mix(c, horizon * 0.10, smoothstep(0.0, -0.35, up));
    vec3 lp = scene.lights[0].posRadius.xyz;
    float glow = pow(clamp(dot(d, normalize(lp)), 0.0, 1.0), 28.0);
    c += scene.lights[0].color.rgb * 0.03 * glow;
    return c;
}
