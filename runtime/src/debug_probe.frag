#version 460
// LEONIDA_PROBE's pixel shader (debug_probe.h): opaque green into render target 0. It reads no varying.

layout(location = 0) out vec4 target;

void main() {
    target = vec4(0.0, 1.0, 0.0, 1.0);
}
