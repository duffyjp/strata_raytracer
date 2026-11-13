// present.vert - one-triangle fullscreen pass, no vertex buffers.
#version 450
layout(location = 0) out vec2 vUv;

void main() {
    // (0,0) (2,0) (0,2) in UV space -> covers the whole viewport with a single triangle.
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    vUv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
