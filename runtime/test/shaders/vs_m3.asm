; M3 vertex shader: a rectangle from the vertex index, placed and set at a depth by root constants. Run:
; d3d12_m3_frame_test (found by the SHA-256 of its FSSM program, as vs_m15 and vs_m2 are).
; User data, from the root signature [0], eight 32-bit constants (floats) = s8..s15 (the console layout starts a
; vertex shader's root arguments at s8, docs/d3d12_vulkan.md section 3.5):
;   s8, s9 = clip-space x, y of the first corner;   s10, s11 = x, y of the opposite corner;   s12 = depth (clip z)
;   s13, s14, s15 = used by the pixel shader
; v0 = VertexIndex, 0..3 (the test draws indices 0 1 2 2 1 3 with DrawIndexedInstanced and base vertex 0):
;   u = v0 & 1, v = (v0 >> 1) & 1;   pos0 = (x0 + u * (x1 - x0), y0 + v * (y1 - y0), z, 1)
; The corners are multiples of 1/32 and the depths are dyadic, so every operation here is exact however it is fused.
; fssm-at: 0x10
; fssm-data: 0x33534d56 0x20535620
v_and_b32_e32 v1, 1, v0
v_and_b32_e32 v2, 2, v0
v_cvt_f32_u32_e32 v1, v1
v_cvt_f32_u32_e32 v2, v2
v_mul_f32_e32 v2, 0.5, v2
v_mov_b32_e32 v7, s8
v_mov_b32_e32 v8, s9
v_mov_b32_e32 v9, s10
v_mov_b32_e32 v10, s11
v_sub_f32_e32 v9, v9, v7
v_sub_f32_e32 v10, v10, v8
v_fma_f32 v3, v1, v9, v7
v_fma_f32 v4, v2, v10, v8
v_mov_b32_e32 v5, s12
v_mov_b32_e32 v6, 1.0
exp pos0 v3, v4, v5, v6 done
s_endpgm
