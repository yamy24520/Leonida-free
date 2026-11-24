; M2 pixel shader for a 2D array view: ps_m2.asm with dim:SQ_RSRC_IMG_2D_ARRAY and slice coordinate 1.0. Run:
; d3d12_m2_formats_test. User data as in ps_m2.asm ([0] a table holding one SRV, [1] a table holding one sampler).
; The slice is relative to the view: a view of slices 0..2 reads slice 1, a view of slices 1..2 reads slice 2.
; attr0.xy = (u, v), 0..1 over the quad.   mrt0 = the texel under the pixel.
; fssm-at: 0x10
; fssm-data: 0x324d5350 0x59525241
s_mov_b32 s20, s8
s_mov_b32 s21, 0
s_mov_b32 s22, s9
s_mov_b32 s23, 0
s_load_dwordx8 s[24:31], s[20:21], 0x0
s_load_dwordx4 s[32:35], s[22:23], 0x0
s_waitcnt lgkmcnt(0)
v_interp_p1_f32_e32 v2, v0, attr0.x
v_interp_p2_f32_e32 v2, v1, attr0.x
v_interp_p1_f32_e32 v3, v0, attr0.y
v_interp_p2_f32_e32 v3, v1, attr0.y
v_mov_b32 v4, 1.0
image_sample v[5:8], v[2:4], s[24:31], s[32:35] dmask:0xf dim:SQ_RSRC_IMG_2D_ARRAY
s_waitcnt vmcnt(0)
exp mrt0 v5, v6, v7, v8 done vm
s_endpgm
