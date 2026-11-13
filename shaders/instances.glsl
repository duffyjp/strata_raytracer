// instances.glsl - per-instance transforms supplied by the host.
//
// The built-in ray-tracing matrices (gl_ObjectToWorldEXT / gl_WorldToObject3x4EXT) have awkward
// GLSL typing (a mat3x4 cannot be multiplied by a vec4), so the host uploads both matrices per TLAS
// instance instead. That keeps the point/normal transforms unambiguous and portable, and lets the
// CPU precompute the world-to-object matrix used for normal mapping.
//
// std430 + mat4 members: 64 bytes per matrix, 128 bytes per instance, matching GpuInstance in
// src/scene.h.
struct InstanceXform {
    mat4 objectToWorld;
    mat4 worldToObject;
};

layout(set = 0, binding = 5, std430) buffer InstanceBlock {
    InstanceXform instances[];
} instanceData;

// World-space point -> object space.
vec3 worldToObjectPoint(int idx, vec3 p) {
    return (instanceData.instances[idx].worldToObject * vec4(p, 1.0)).xyz;
}

// Object-space normal -> world space. Transposing the world-to-object linear part is the correct
// normal transform and stays exact under non-uniform instance scaling.
vec3 objectNormalToWorld(int idx, vec3 nObj) {
    mat4 B = instanceData.instances[idx].worldToObject;
    vec3 n = vec3(dot(B[0].xyz, nObj), dot(B[1].xyz, nObj), dot(B[2].xyz, nObj));
    float l = length(n);
    return l > 1e-8 ? n / l : nObj;
}
