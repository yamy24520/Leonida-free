#version 460
// LEONIDA_PROBE_POS's vertex shader (debug_probe.h): the POSITION input's x and y, times a scale plus an offset, as
// clip-space x and y. The runtime moves the input to POSITION's register (its Location decoration) and sets the four
// specialization constants in the words: scale 1 and offset 0 read POSITION as NDC, scale (2/W, -2/H) and offset
// (-1, 1) as pixels of a W x H target (y down).

layout(location = 0) in vec4 position;

layout(constant_id = 0) const float scale_x = 1.0;
layout(constant_id = 1) const float scale_y = 1.0;
layout(constant_id = 2) const float offset_x = 0.0;
layout(constant_id = 3) const float offset_y = 0.0;

void main() {
    gl_Position = vec4(position.xy * vec2(scale_x, scale_y) + vec2(offset_x, offset_y), 0.5, 1.0);
}
