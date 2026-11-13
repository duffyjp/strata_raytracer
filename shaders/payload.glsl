// payload.glsl - the ray payload shared by every stage of the ray-tracing pipeline.
// The declaration must be byte-identical wherever it is used:
//   main.rgen        declares it as rayPayloadEXT  (the caller / initialiser)
//   hit.rchit        declares it as rayPayloadInEXT
//   miss.rmiss       declares it as rayPayloadInEXT
// Shadow rays use their own single-word payload at location 1 (see shadow_hit/shadow_miss) so that
// each ray type only ever touches the payload variable it was traced with.
struct RayHit {
    vec4 posT;    // xyz = world hit position, w = t along the ray
    vec4 posObj;  // xyz = object-space hit position
    vec4 meta;    // x = materialIndex + 1 (0 = miss), y = primitive kind, z = front facing
    vec4 normal;  // xyz = world-space geometric normal
};
