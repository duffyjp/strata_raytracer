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
