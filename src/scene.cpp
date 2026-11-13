// scene.cpp - procedural geometry + the authored scene description and its scene.json overlay.
#include "scene.h"
#include "json.h"
#include "pak.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

void pushVert(Mesh& m, const Vec3f& p, const Vec3f& n) {
    m.positions.push_back(p);
    m.normals.push_back(n);
}

void addTri(Mesh& m, const Vec3f& a, const Vec3f& b, const Vec3f& c, const Vec3f& n) {
    size_t base = m.positions.size();
    pushVert(m, a, n);
    pushVert(m, b, n);
    pushVert(m, c, n);
    m.indices.push_back(uint32_t(base));
    m.indices.push_back(uint32_t(base + 1));
    m.indices.push_back(uint32_t(base + 2));
}

Vec3f normalize(Vec3f v) {
    float l = sqrtf(v.v[0] * v.v[0] + v.v[1] * v.v[1] + v.v[2] * v.v[2]);
    if (l < 1e-8f) l = 1.f;
    v.v[0] /= l; v.v[1] /= l; v.v[2] /= l;
    return v;
}

}  // namespace

Mesh makeSphere(float radius, int segments, int rings) {
    Mesh m;
    m.name = "sphere";
    const int stride = segments + 1;
    for (int j = 0; j <= rings; ++j) {
        float phi = float(j) / float(rings) * float(M_PI);
        for (int i = 0; i <= segments; ++i) {
            float th = float(i) / float(segments) * 2.f * float(M_PI);
            Vec3f n{{sinf(phi) * cosf(th), cosf(phi), sinf(phi) * sinf(th)}};
            Vec3f p{{n.v[0] * radius, n.v[1] * radius, n.v[2] * radius}};
            pushVert(m, p, n);
        }
    }
    for (int j = 0; j < rings; ++j) {
        for (int i = 0; i < segments; ++i) {
            uint32_t a = uint32_t(j * stride + i);
            uint32_t b = a + uint32_t(stride);
            m.indices.insert(m.indices.end(), {a, b, a + 1, b, b + 1, a + 1});
        }
    }
    return m;
}

Mesh makeCube(float size) {
    Mesh m;
    m.name = "cube";
    const float h = size * 0.5f;
    const struct {
        Vec3f n;
        Vec3f v[4];
    } faces[6] = {
        {{{0, 1, 0}}, {{-h, h, -h}, {-h, h, h}, {h, h, h}, {h, h, -h}}},
        {{{0, -1, 0}}, {{-h, -h, -h}, {h, -h, -h}, {h, -h, h}, {-h, -h, h}}},
        {{{1, 0, 0}}, {{h, -h, -h}, {h, h, -h}, {h, h, h}, {h, -h, h}}},
        {{{-1, 0, 0}}, {{-h, -h, -h}, {-h, -h, h}, {-h, h, h}, {-h, h, -h}}},
        {{{0, 0, 1}}, {{-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h}}},
        {{{0, 0, -1}}, {{-h, -h, -h}, {-h, h, -h}, {h, h, -h}, {h, -h, -h}}},
    };
    for (const auto& f : faces) {
        addTri(m, f.v[0], f.v[1], f.v[2], f.n);
        addTri(m, f.v[0], f.v[2], f.v[3], f.n);
    }
    return m;
}

Mesh makeCone(float radius, float height, int segments) {
    Mesh m;
    m.name = "cone";
    const Vec3f apex{{0.f, height, 0.f}};
    for (int i = 0; i < segments; ++i) {
        float t0 = float(i) / float(segments) * 2.f * float(M_PI);
        float t1 = float(i + 1) / float(segments) * 2.f * float(M_PI);
        Vec3f b0{{cosf(t0) * radius, 0.f, sinf(t0) * radius}};
        Vec3f b1{{cosf(t1) * radius, 0.f, sinf(t1) * radius}};
        // Side normals are perpendicular to the slant so the cone shades smoothly.
        Vec3f n0 = normalize(Vec3f{{cosf(t0) * height, radius, sinf(t0) * height}});
        Vec3f n1 = normalize(Vec3f{{cosf(t1) * height, radius, sinf(t1) * height}});
        Vec3f nApex = normalize(Vec3f{{(n0.v[0] + n1.v[0]) * 0.5f, (n0.v[1] + n1.v[1]) * 0.5f,
                                       (n0.v[2] + n1.v[2]) * 0.5f}});
        size_t base = m.positions.size();
        pushVert(m, apex, nApex);
        pushVert(m, b0, n0);
        pushVert(m, b1, n1);
        m.indices.push_back(uint32_t(base));
        m.indices.push_back(uint32_t(base + 1));
        m.indices.push_back(uint32_t(base + 2));
    }
    const Vec3f dn{{0.f, -1.f, 0.f}};
    const Vec3f c{{0.f, 0.f, 0.f}};
    for (int i = 0; i < segments; ++i) {
        float t0 = float(i) / float(segments) * 2.f * float(M_PI);
        float t1 = float(i + 1) / float(segments) * 2.f * float(M_PI);
        Vec3f b0{{cosf(t0) * radius, 0.f, sinf(t0) * radius}};
        Vec3f b1{{cosf(t1) * radius, 0.f, sinf(t1) * radius}};
        addTri(m, c, b1, b0, dn);
    }
    return m;
}

Mesh makeDisc(float radius, int sectors) {
    Mesh m;
    m.name = "disc";
    const Vec3f up{{0.f, 1.f, 0.f}};
    const Vec3f c{{0.f, 0.f, 0.f}};
    for (int i = 0; i < sectors; ++i) {
        float t0 = float(i) / float(sectors) * 2.f * float(M_PI);
        float t1 = float(i + 1) / float(sectors) * 2.f * float(M_PI);
        Vec3f b0{{cosf(t0) * radius, 0.f, sinf(t0) * radius}};
        Vec3f b1{{cosf(t1) * radius, 0.f, sinf(t1) * radius}};
        addTri(m, c, b0, b1, up);
    }
    return m;
}

InstanceTransform makeTransform(const float position[3], const float scale[3], float rotationY) {    InstanceTransform t{};
    float c = cosf(rotationY), s = sinf(rotationY);
    t.m[0][0] = c * scale[0]; t.m[0][1] = 0.f;      t.m[0][2] = -s * scale[2]; t.m[0][3] = position[0];
    t.m[1][0] = 0.f;          t.m[1][1] = scale[1]; t.m[1][2] = 0.f;           t.m[1][3] = position[1];
    t.m[2][0] = s * scale[0]; t.m[2][1] = 0.f;      t.m[2][2] = c * scale[2];  t.m[2][3] = position[2];
    return t;
}

void fillInstanceMatrices(const InstanceTransform& t, GpuInstance* out) {
    // Row-major 3x4 object->world becomes a column-major mat4, plus the inverse affine transform
    // that the shaders use for object-space positions and normals.
    const float* m = &t.m[0][0];
    const float a00 = m[0], a01 = m[1], a02 = m[2], tx = m[3];
    const float a10 = m[4], a11 = m[5], a12 = m[6], ty = m[7];
    const float a20 = m[8], a21 = m[9], a22 = m[10], tz = m[11];

    float* o = &out->col[0].x;
    o[0] = a00; o[1] = a10; o[2] = a20; o[3] = 0.f;
    o[4] = a01; o[5] = a11; o[6] = a21; o[7] = 0.f;
    o[8] = a02; o[9] = a12; o[10] = a22; o[11] = 0.f;
    o[12] = tx; o[13] = ty; o[14] = tz; o[15] = 1.f;

    // Cofactors of A.
    const float c00 = a11 * a22 - a12 * a21;
    const float c01 = -(a10 * a22 - a12 * a20);
    const float c02 = a10 * a21 - a11 * a20;
    const float c10 = -(a01 * a22 - a02 * a21);
    const float c11 = a00 * a22 - a02 * a20;
    const float c12 = -(a00 * a21 - a01 * a20);
    const float c20 = a01 * a12 - a02 * a11;
    const float c21 = -(a00 * a12 - a02 * a10);
    const float c22 = a00 * a11 - a01 * a10;

    float det = a00 * c00 + a01 * c01 + a02 * c02;
    if (fabsf(det) < 1e-12f) det = (det < 0.f) ? -1e-12f : 1e-12f;
    const float invDet = 1.f / det;

    // A^-1 = adj(A)/det, and adj is the TRANSPOSE of the cofactor matrix. Getting that transposition
    // wrong is invisible for axis-aligned instances and breaks any rotated one.
    const float i00 = c00 * invDet, i01 = c10 * invDet, i02 = c20 * invDet;
    const float i10 = c01 * invDet, i11 = c11 * invDet, i12 = c21 * invDet;
    const float i20 = c02 * invDet, i21 = c12 * invDet, i22 = c22 * invDet;

    const float ex = -(i00 * tx + i01 * ty + i02 * tz);
    const float ey = -(i10 * tx + i11 * ty + i12 * tz);
    const float ez = -(i20 * tx + i21 * ty + i22 * tz);

    float* r = &out->col[4].x;  // column-major world->object
    r[0] = i00; r[1] = i10; r[2] = i20; r[3] = 0.f;
    r[4] = i01; r[5] = i11; r[6] = i21; r[7] = 0.f;
    r[8] = i02; r[9] = i12; r[10] = i22; r[11] = 0.f;
    r[12] = ex; r[13] = ey; r[14] = ez; r[15] = 1.f;
}

int SceneConfig::materialIndexByName(const std::string& n) const {
    for (size_t i = 0; i < materials.size(); ++i)
        if (materials[i].name == n) return int(i);
    return -1;
}

int SceneConfig::textureIndexByName(const std::string& n) const {
    for (size_t i = 0; i < textures.size(); ++i)
        if (textures[i].name == n) return int(i);
    return -1;
}

bool SceneConfig::loadDefaults() {
    materials.clear();
    objects.clear();
    lights.clear();
    textures.clear();

    auto addMat = [&](const char* name, int model, float ar, float ag, float ab, float refl,
                      float rough, float ior, float er = 0.f, float eg = 0.f, float eb = 0.f) {
        MaterialDef md;
        md.name = name;
        md.model = model;
        md.albedo[0] = ar; md.albedo[1] = ag; md.albedo[2] = ab;
        md.emissive[0] = er; md.emissive[1] = eg; md.emissive[2] = eb;
        md.reflectivity = refl;
        md.roughness = rough;
        md.ior = ior;
        md.diffuse = (model == 1 || model == 2 || model == 3) ? 0.f : 1.f;
        materials.push_back(md);
    };

    addMat("ground", 0, 0.52f, 0.53f, 0.56f, 0.12f, 0.35f, 1.4f);
    addMat("mirror", 1, 0.95f, 0.96f, 0.97f, 0.95f, 0.02f, 1.0f);
    addMat("satin", 1, 0.86f, 0.87f, 0.90f, 0.70f, 0.16f, 1.0f);
    addMat("red_dull", 0, 0.82f, 0.20f, 0.16f, 0.05f, 0.60f, 1.0f);
    addMat("glass", 2, 1.f, 1.f, 1.f, 0.05f, 0.01f, 1.52f);
    addMat("mirror_cube", 1, 0.90f, 0.92f, 0.95f, 0.85f, 0.05f, 1.0f);
    addMat("teal", 0, 0.18f, 0.55f, 0.47f, 0.06f, 0.55f, 1.0f);
    addMat("light_warm", 3, 0.10f, 0.08f, 0.06f, 0.f, 0.f, 1.f, 13.f, 8.6f, 4.8f);
    addMat("light_cool", 3, 0.06f, 0.08f, 0.10f, 0.f, 0.f, 1.f, 5.2f, 8.4f, 13.f);

    auto addObject = [&](const char* name, const char* kind, const char* mat,
                         float px, float py, float pz, float sx, float sy, float sz, float rotY) {
        ObjectDef od;
        od.name = name;
        od.kind = kind;
        od.material = mat;
        od.position[0] = px; od.position[1] = py; od.position[2] = pz;
        od.scale[0] = sx; od.scale[1] = sy; od.scale[2] = sz;
        od.rotationY = rotY;
        objects.push_back(od);
    };

    addObject("ground", "disc", "ground", 0, 0, 0, 12, 1, 12, 0.f);
    addObject("mirror_sphere", "sphere", "mirror", -1.6f, 1.15f, 0.3f, 1.15f, 1.15f, 1.15f, 0.f);
    addObject("satin_sphere", "sphere", "satin", 1.7f, 0.85f, -1.5f, 0.85f, 0.85f, 0.85f, 0.f);
    addObject("red_sphere", "sphere", "red_dull", 3.0f, 0.8f, 1.4f, 0.8f, 0.8f, 0.8f, 0.f);
    addObject("glass_sphere", "sphere", "glass", -0.1f, 0.95f, 2.3f, 0.95f, 0.95f, 0.95f, 0.f);
    addObject("mirror_cube", "cube", "mirror_cube", 1.15f, 0.72f, -0.25f, 1.44f, 1.44f, 1.44f, 0.42f);
    addObject("teal_cone", "cone", "teal", -2.8f, 0.f, -1.8f, 0.8f, 1.9f, 0.8f, 0.f);

    LightDef la;
    la.name = "key";
    la.color[0] = 1.0f; la.color[1] = 0.78f; la.color[2] = 0.55f;
    la.intensity = 16.f;
    la.radius = 0.34f;
    la.orbitRadius = 3.9f;
    la.orbitHeight = 2.6f;
    la.speed = 0.55f;
    lights.push_back(la);

    LightDef lb;
    lb.name = "fill";
    lb.color[0] = 0.45f; lb.color[1] = 0.66f; lb.color[2] = 1.0f;
    lb.intensity = 10.f;
    lb.radius = 0.27f;
    lb.orbitRadius = 4.7f;
    lb.orbitHeight = 1.7f;
    lb.speed = -0.40f;
    lb.phase = 2.1f;
    lights.push_back(lb);

    auto addLightObject = [&](const char* name, const char* mat, int lightIdx, float radius) {
        ObjectDef od;
        od.name = name;
        od.kind = "sphere";
        od.material = mat;
        od.isLight = true;
        od.lightIndex = lightIdx;
        od.scale[0] = od.scale[1] = od.scale[2] = radius;
        objects.push_back(od);
    };
    addLightObject("light_key", "light_warm", 0, 0.34f);
    addLightObject("light_fill", "light_cool", 1, 0.27f);

    TextureDef td;
    td.name = "checker";
    td.file = "textures/checker.png";
    td.scale = 6.0f;
    textures.push_back(td);
    TextureDef td2;
    td2.name = "ramp";
    td2.file = "textures/emissive_ramp.png";
    td2.scale = 1.0f;
    textures.push_back(td2);

    if (!materials.empty()) materials[0].textureSlot = 0;  // ground uses the packed checker
    return true;
}

bool SceneConfig::applyJson(const JsonValue& root, std::string* warn) {
    if (root.type != JsonValue::Type::Object) {
        if (warn) *warn = "scene.json root is not an object";
        return false;
    }
    exposure = float(root.num("exposure", exposure));
    maxBounces = int(root.num("maxBounces", maxBounces));
    accumulationFrames = int(root.num("accumulationFrames", accumulationFrames));
    samplesPerFrame = int(root.num("samplesPerFrame", samplesPerFrame));
    if (samplesPerFrame < 1) samplesPerFrame = 1;
    if (samplesPerFrame > 4) samplesPerFrame = 4;
    autoQuality = root.booleanAt("autoQuality", autoQuality);
    targetFps = float(root.num("targetFps", targetFps));
    if (targetFps < 20.f) targetFps = 20.f;
    if (targetFps > 240.f) targetFps = 240.f;
    qualityHeadroom = float(root.num("qualityHeadroom", qualityHeadroom));
    if (qualityHeadroom < 0.f) qualityHeadroom = 0.f;
    if (qualityHeadroom > 0.5f) qualityHeadroom = 0.5f;
    maxSamplesPerFrame = int(root.num("maxSamplesPerFrame", maxSamplesPerFrame));
    if (maxSamplesPerFrame < 1) maxSamplesPerFrame = 1;
    if (maxSamplesPerFrame > 32) maxSamplesPerFrame = 32;
    minRenderScale = float(root.num("minRenderScale", minRenderScale));
    if (minRenderScale < 0.25f) minRenderScale = 0.25f;
    if (minRenderScale > 1.f) minRenderScale = 1.f;
    baseSamplesPerFrame = int(root.num("baseSamplesPerFrame", baseSamplesPerFrame));
    if (baseSamplesPerFrame < 1) baseSamplesPerFrame = 1;
    if (baseSamplesPerFrame > maxSamplesPerFrame) baseSamplesPerFrame = maxSamplesPerFrame;
    maxRenderScale = float(root.num("maxRenderScale", maxRenderScale));
    if (maxRenderScale < 1.f) maxRenderScale = 1.f;
    if (maxRenderScale > 3.f) maxRenderScale = 3.f;
    maxTracePixels = root.num("maxTracePixels", maxTracePixels);
    if (maxTracePixels < 300000.0) maxTracePixels = 300000.0;
    radianceClamp = float(root.num("radianceClamp", radianceClamp));
    skyMode = int(root.num("skyMode", skyMode));
    root.vecN("skyColor", skyColor, 3);

    const JsonValue* cam = root.find("camera");
    if (cam) {
        camera.orbitRadius = float(cam->num("orbitRadius", camera.orbitRadius));
        camera.orbitHeight = float(cam->num("orbitHeight", camera.orbitHeight));
        camera.heightBob = float(cam->num("heightBob", camera.heightBob));
        camera.speed = float(cam->num("speed", camera.speed));
        camera.fovDeg = float(cam->num("fovDeg", camera.fovDeg));
        cam->vecN("lookAt", camera.lookAt, 3);
    }

    const JsonValue* mats = root.find("materials");
    if (mats && mats->type == JsonValue::Type::Array) {
        for (const auto& mv : mats->array) {
            std::string nm = mv.text("name", "");
            int idx = nm.empty() ? -1 : materialIndexByName(nm);
            if (idx < 0) {
                if ((int)materials.size() >= kMaxMaterials) continue;
                MaterialDef md;
                md.name = nm.empty() ? std::string("mat") : nm;
                md.diffuse = 1.f;
                materials.push_back(md);
                idx = int(materials.size() - 1);
            }
            MaterialDef& md = materials[idx];
            mv.vecN("albedo", md.albedo, 3);
            mv.vecN("emissive", md.emissive, 3);
            md.reflectivity = float(mv.num("reflectivity", md.reflectivity));
            md.roughness = float(mv.num("roughness", md.roughness));
            md.ior = float(mv.num("ior", md.ior));
            md.diffuse = float(mv.num("diffuse", md.diffuse));
            md.model = int(mv.num("model", md.model));
            md.textureScale = float(mv.num("textureScale", md.textureScale));
            std::string tex = mv.text("texture", "");
            if (!tex.empty()) md.textureSlot = textureIndexByName(tex);
        }
    }

    const JsonValue* lts = root.find("lights");
    if (lts && lts->type == JsonValue::Type::Array) {
        for (size_t i = 0; i < lts->array.size() && i < (size_t)kMaxLights; ++i) {
            const JsonValue& lv = lts->array[i];
            if (i >= lights.size()) {
                if ((int)lights.size() >= kMaxLights) break;
                lights.push_back(LightDef());
            }
            LightDef& ld = lights[i];
            std::string nm = lv.text("name", "");
            if (!nm.empty()) ld.name = nm;
            lv.vecN("color", ld.color, 3);
            ld.intensity = float(lv.num("intensity", ld.intensity));
            ld.radius = float(lv.num("radius", ld.radius));
            ld.orbitRadius = float(lv.num("orbitRadius", ld.orbitRadius));
            ld.orbitHeight = float(lv.num("orbitHeight", ld.orbitHeight));
            ld.speed = float(lv.num("speed", ld.speed));
            ld.phase = float(lv.num("phase", ld.phase));
            ld.on = lv.booleanAt("on", ld.on);
        }
    }

    const JsonValue* tex = root.find("textures");
    if (tex && tex->type == JsonValue::Type::Array) {
        for (const auto& tv : tex->array) {
            std::string nm = tv.text("name", "");
            if (nm.empty()) continue;
            int idx = textureIndexByName(nm);
            if (idx < 0) {
                if ((int)textures.size() >= kMaxTextures) continue;
                TextureDef td;
                td.name = nm;
                textures.push_back(td);
                idx = int(textures.size() - 1);
            }
            textures[idx].file = tv.text("file", textures[idx].file);
            textures[idx].scale = float(tv.num("scale", textures[idx].scale));
        }
    }
    (void)warn;
    return true;
}
