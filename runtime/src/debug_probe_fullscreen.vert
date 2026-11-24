#version 460
// LEONIDA_PROBE's vertex shader (debug_probe.h): vertices 0, 1 and 2 make one triangle that covers the whole viewport,
// (-1,-1), (-1,3), (3,-1), clockwise in D3D screen space. Depth 0.5 passes a LESS or GREATER test against a target
// cleared to 1 or 0. It reads no vertex input and writes no varying.

void main() {
    const vec2 corner = vec2(float(gl_VertexIndex & 2), float((gl_VertexIndex << 1) & 2));
    gl_Position = vec4(corner * 2.0 - 1.0, 0.5, 1.0);
}
