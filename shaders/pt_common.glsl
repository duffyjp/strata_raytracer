// pt_common.glsl
//
// Shared path-tracing core. Textually included (see tools/build_shaders.py) into:
//   shaders/main.rgen      - ray-generation shader of the VK_KHR_ray_tracing_pipeline path
//   shaders/fallback.comp  - compute shader using VK_KHR_ray_query on the same acceleration
//                            structure, for hardware that exposes ray queries but no RT pipeline
//
// The includer declares its payload variables, includes this file, then implements the two backend
// hooks below and its own main().

#include "sceneblock.glsl"
#include "instances.glsl"
#include "primitives.glsl"
#include "sky.glsl"

struct HitInfo {
    vec3 pos;         // world space
    vec3 posObj;      // object space (analytic normals + UVs)
    vec3 normal;      // world space, geometric
    float t;
    uint material;
    uint kind;
    bool hit;
    bool frontFacing;
};

// Backend hooks implemented by the includer (declared after HitInfo, which they reference).
bool traceShadingRay(vec3 origin, vec3 direction, float tMax, out HitInfo hit);
bool occludedRay(vec3 origin, vec3 direction, float tMax);

// ---------- maths / rng -------------------------------------------------------------------------
const float PI = 3.14159265358979323846;

uint pcgHash(inout uint state) {
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float rnd(inout uint state) { return float(pcgHash(state)) * (1.0 / 4294967296.0); }
vec2 rnd2(inout uint state) { return vec2(rnd(state), rnd(state)); }

vec3 tangentFrameT(vec3 n) {
    vec3 up = abs(n.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    return normalize(cross(up, n));
}

vec3 cosineHemisphere(vec3 n, vec2 u) {
    float phi = 2.0 * PI * u.x;
    float sinTheta = sqrt(max(0.0, u.x));
    float cosTheta = sqrt(max(0.0, 1.0 - u.x));
    vec3 t = tangentFrameT(n);
    vec3 b = cross(n, t);
    return normalize(t * (cos(phi) * sinTheta) + b * (sin(phi) * sinTheta) + n * cosTheta);
}

float fresnelSchlick(float cosTheta, float f0) {
    float m = clamp(cosTheta, 0.0, 1.0);
    return f0 + (1.0 - f0) * pow(1.0 - m, 5.0);
}

vec3 fresnelSchlick3(float cosTheta, vec3 f0) {
    float m = clamp(cosTheta, 0.0, 1.0);
    return f0 + (vec3(1.0) - f0) * pow(1.0 - m, 5.0);
}

float fresnelDielectric(float cosI, float ior) {
    float f0c = (1.0 - ior) / (1.0 + ior);
    return fresnelSchlick(abs(cosI), f0c * f0c);
}

float smithG1(float ncos, float a2) {
    float n = max(1e-5, abs(ncos));
    return 2.0 / (1.0 + sqrt(max(1e-8, 1.0 + a2 * (1.0 - n * n) / (n * n))));
}

// Importance samples the GGX NDF by half-vector; the NDF cancels against the pdf so what is left is
// the geometry term times the Jacobian. Fresnel belongs here rather than at the call site, because
// it has to be evaluated at the half vector: measuring it against the outgoing direction instead
// reports near-grazing Fresnel for lobe samples that are nowhere near grazing, which is how a faint
// sheen turns into a blown-out streak.
//
// The separable Smith term makes the weight blow up at grazing view angles, which is where glossy
// surfaces otherwise throw bright speckles, so it is clamped: a small, standard, deliberate bias.
bool sampleSpecular(vec3 wo, vec3 n, float roughness, vec2 u, vec3 f0, out vec3 wi,
                    out vec3 weight) {
    wi = wo;
    weight = vec3(0.0);
    float a = max(0.004, roughness * roughness);
    float a2 = a * a;
    float phi = 2.0 * PI * u.x;
    float cosThetaH = sqrt(max(1e-6, (1.0 - u.y) / (1.0 + (a2 - 1.0) * u.y)));
    float sinThetaH = sqrt(max(0.0, 1.0 - cosThetaH * cosThetaH));
    vec3 t = tangentFrameT(n);
    vec3 b = cross(n, t);
    mat3 TBN = mat3(t, b, n);
    vec3 h = normalize(TBN * vec3(sinThetaH * cos(phi), sinThetaH * sin(phi), cosThetaH));
    vec3 cand = reflect(-wo, h);
    float noL = dot(n, cand);
    float voH = dot(wo, h);
    float noV = max(1e-4, dot(n, wo));
    if (noL <= 0.0 || voH <= 0.0) return false;
    weight = min(vec3(3.0), fresnelSchlick3(voH, f0) * smithG1(noV, a2) * smithG1(noL, a2) * voH /
                        (noV * max(1e-4, cosThetaH)));
    wi = normalize(cand);
    return true;
}

// ---------- textures supplied by the .pak -------------------------------------------------------
layout(set = 0, binding = 3) uniform texture2D sceneTextures[8];
layout(set = 0, binding = 4) uniform sampler sceneSampler;

vec3 sampleTexture(int slot, vec2 uv) {
    if (slot < 0 || slot > 7) return vec3(1.0);
    return textureLod(sampler2D(nonuniformEXT(sceneTextures[slot]), sceneSampler), fract(uv), 0.0).rgb;
}

vec3 proceduralChecker(vec2 uv, float scale) {
    vec2 g = floor(uv * max(0.01, scale));
    float c = mod(g.x + g.y, 2.0);
    return mix(vec3(0.60, 0.61, 0.65), vec3(0.23, 0.24, 0.28), c);
}

// ---------- direct lighting (next-event estimation over the packed light list) ------------------
vec3 directLighting(HitInfo h, Material m, vec3 wo, vec3 albedo, inout uint state) {
    vec3 accum = vec3(0.0);
    float model = m.flags.x;
    if (model > 1.5) return accum;   // glass is handled purely specularly in the transport loop
    // A near-perfect mirror's specular lobe is a delta: uniform sampling of the light's disc almost
    // never lands on it, so next-event estimation would return almost nothing while still claiming
    // the light has been gathered. Leave those lights to the specular path, which sees them exactly.
    if (model >= 0.5 && m.params.y < 0.06) return accum;

    for (int i = 0; i < LIGHT_COUNT; ++i) {
        Light L = scene.lights[i];
        if (L.color.w <= 0.0) continue;
        vec3 cToL = L.posRadius.xyz - h.pos;
        float distC = length(cToL);
        vec3 dirC = cToL / max(1e-5, distC);
        if (dot(h.normal, dirC) <= 0.0) continue;

        // Sample a point on the light's disc so shadows get a soft penumbra.
        float rL = max(1e-3, L.posRadius.w);
        vec3 lt = tangentFrameT(dirC);
        vec3 lb = cross(dirC, lt);
        vec2 u = rnd2(state);
        float rr = rL * 0.9 * sqrt(u.x);
        float ang = 2.0 * PI * u.y;
        vec3 lp = L.posRadius.xyz + lt * (cos(ang) * rr) + lb * (sin(ang) * rr);
        vec3 sd = lp - h.pos;
        float sdLen = length(sd);
        vec3 sdir = sd / max(1e-5, sdLen);
        float ndots = max(0.0, dot(h.normal, sdir));
        if (ndots <= 1e-5) continue;

        // Offset the shadow-ray origin along the surface normal, and by more the more grazing the
        // light direction is. A fixed epsilon is what makes a big tessellated floor draw bright
        // rings: far from the light the ray runs almost parallel to the surface, intermittently
        // clipping the neighbouring triangles, and the alternating lit/shadow bands clip to white.
        float graze = 1.0 - clamp(ndots, 0.0, 1.0);
        vec3 shadowOrigin = h.pos + h.normal * (0.0015 + 0.055 * graze * graze);
        if (occludedRay(shadowOrigin, sdir, sdLen - 0.004)) continue;

        // The orbs are drawn with uniform radiance in every direction - no limb darkening - so there
        // is no light-side cosine to divide by here: sampling the projected disc already accounts for
        // the foreshortening. The previous version divided by a cosine taken from the sampled point
        // back to the centre, which is the zero vector at the middle of the disc, and threw away any
        // sample where it came out small. How many samples that discarded varied with where on the
        // floor you stood, and a position-dependent bias reads as a faint ring under each light.
        float pdf = sdLen * sdLen / max(1e-4, PI * rL * rL * 0.81);
        float falloff = 1.0 / (1.0 + 0.015 * sdLen * sdLen);
        vec3 incoming = L.color.rgb * (falloff / max(1e-4, pdf));

        if (model < 0.5) {
            // Lambert: the rendering equation's cosine has to appear explicitly now that the sampling
            // density carries only the solid angle.
            accum += albedo * incoming * (ndots / PI);
        } else {
            // Glossy: the cosine at the shading point cancels the 1/cosine inside the specular BRDF,
            // so this term deliberately carries no ndots.
            vec3 hv = normalize(wo + sdir);
            float a = max(0.002, m.params.y * m.params.y);
            float a2 = a * a;
            float noh = max(0.0, dot(h.normal, hv));
            float gh = 1.0 + (a2 - 1.0) * noh * noh;
            float d = a2 / max(1e-8, PI * gh * gh);
            float fres = fresnelSchlick(max(1e-4, dot(wo, hv)), clamp(m.params.x, 0.04, 1.0));
            float g = smithG1(dot(h.normal, wo), a2) * smithG1(dot(h.normal, sdir), a2);
            float noV = max(1e-4, dot(h.normal, wo));
            accum += albedo * incoming * (d * fres * g / (4.0 * noV));
        }
    }
    return accum;
}

// ---------- the shared transport loop ------------------------------------------------------------
vec3 pathTrace(vec3 origin, vec3 direction, uint state) {
    vec3 radiance = vec3(0.0);
    vec3 throughput = vec3(1.0);
    int maxBounces = int(CAM_FWD.w);
    float clampV = CAM_POST.z;
    // The shading mode of the surface this ray left from. Diffuse and glossy surfaces gather every
    // light explicitly through next-event estimation, so a bounce that then lands on one of those
    // lights must not collect its emission as well - see the note at the emissive term below.
    float prevModel = -1.0;

    for (int bounce = 0; bounce < maxBounces; ++bounce) {
        HitInfo h;
        if (!traceShadingRay(origin, direction, 1e5, h)) {
            radiance += throughput * skyColor(direction);
            break;
        }

        Material m = scene.materials[h.material];
        // Double counting an emitter is not a subtle bias: the orb is bright enough that every floor
        // point which can see it, modulated by where the shadow test says it is hidden behind an
        // object, saturates to white. That is the bright banding people read as the lights painting
        // spirals on the floor. Surfaces that do no next-event estimation (metal, glass) still show
        // the orb directly, and the camera still sees the orbs themselves on the first hit.
        // Suppressing the emission is only correct where next-event estimation can actually represent
        // that light. A Lambertian floor can: it gathers the orb explicitly with a shadow ray. A
        // mirror cannot - a near-zero roughness makes the glossy NEE term a spike that uniform disc
        // sampling almost never hits - so suppressing there renders the light as a black hole ringed
        // by the little energy NEE does gather. Only diffuse surfaces suppress.
        bool lightAlreadyCounted = (prevModel >= 0.0 && prevModel < 0.5);
        if (!lightAlreadyCounted) radiance += throughput * m.emissive.rgb;
        float model = m.flags.x;
        prevModel = model;
        if (model > 2.5) break;   // emitter: its shading contribution is the emissive term

        vec3 wo = -normalize(direction);
        float rough = clamp(m.params.y, 0.0, 1.0);
        float refl = clamp(m.params.x, 0.0, 1.0);
        float ior = m.params.z > 1.0001 ? m.params.z : 1.5;

        vec3 albedo = m.albedo.rgb;
        if (m.flags.y >= 0.0) {
            vec2 uv = objectUV(h.kind, h.posObj) * (m.params.w > 0.0 ? m.params.w : 1.0);
            vec3 tex = sampleTexture(int(m.flags.y), uv);
            if (max(max(tex.r, tex.g), tex.b) <= 1e-4) tex = proceduralChecker(uv, m.params.w);
            albedo *= mix(vec3(1.0), tex, 0.85);
        }

        radiance += clamp(throughput * directLighting(h, m, wo, albedo, state), vec3(0.0), vec3(clampV));

        vec3 nextDir = direction;
        vec3 attenuate = vec3(0.0);
        if (model < 0.5) {
            // Rough dielectric: mostly Lambertian with a glossy lobe driven by reflectivity.
            vec3 wgt;
            if (refl > 0.02 && rnd(state) < clamp(refl, 0.0, 0.9) &&
                sampleSpecular(wo, h.normal, max(0.03, rough), rnd2(state), vec3(max(0.02, refl)),
                               nextDir, wgt)) {
                // A dielectric lobe reflects with a neutral tint - the material's colour belongs to
                // the diffuse term only - and Fresnel is already carried inside wgt.
                attenuate = wgt;
            } else {
                nextDir = cosineHemisphere(h.normal, rnd2(state));
                attenuate = albedo;
            }
        } else if (model < 1.5) {
            // Metal: tinted Fresnel + GGX, reflectivity scales the retained energy.
            vec3 wgt;
            if (!sampleSpecular(wo, h.normal, max(0.004, rough), rnd2(state),
                                clamp(albedo, vec3(0.02), vec3(1.0)), nextDir, wgt)) break;
            attenuate = wgt * max(0.05, refl);
        } else {
            // Glass: Fresnel-weighted reflection or refraction, with total internal reflection.
            float f = fresnelDielectric(dot(wo, h.normal), ior);
            if (rnd(state) < f) {
                nextDir = reflect(-wo, h.normal);
                attenuate = vec3(f);
            } else {
                vec3 n = h.normal;
                float eta = ior;
                if (!h.frontFacing) { n = -h.normal; eta = 1.0 / ior; }
                vec3 refr = refract(-wo, n, 1.0 / eta);
                if (dot(refr, refr) < 1e-8) {
                    nextDir = reflect(-wo, n);
                    attenuate = vec3(1.0);
                } else {
                    nextDir = normalize(refr);
                    attenuate = vec3(1.0) - vec3(f);
                }
            }
            attenuate *= mix(vec3(1.0), m.albedo.rgb, 0.2);   // faint tint through the volume
        }

        throughput *= attenuate;
        float lum = max(throughput.r, max(throughput.g, throughput.b));
        if (lum < 0.005) break;
        if (lum > 1.0) throughput /= lum;   // bounded energy; hue is preserved

        origin = h.pos + nextDir * 0.0015;
        direction = normalize(nextDir);
    }
    return radiance;
}
