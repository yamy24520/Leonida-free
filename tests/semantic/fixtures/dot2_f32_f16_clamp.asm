; v_dot2_f32_f16 with clamp: the hardware ignores the bit (gfx1031, tools/hw_oracle, wave32 and wave64).
; Expected MRT0 words: 41280000 c1080000 3fa00000 4fffc004 (10.5, -8.5, 1.25, 2 * 65504^2), all unclamped.
	v_mov_b32 v1, 0x42004000
	v_mov_b32 v2, 0x38004400
	v_mov_b32 v3, 1.0
	v_dot2_f32_f16 v4, v1, v2, v3 clamp
	v_dot2_f32_f16 v5, v1, v2, v3 neg_lo:[1,0,0] neg_hi:[0,1,0] clamp
	v_mov_b32 v1, 0x38003400
	v_mov_b32 v2, 0x3c003c00
	v_mov_b32 v3, 0.5
	v_dot2_f32_f16 v6, v1, v2, v3 clamp
	v_mov_b32 v1, 0x7bff7bff
	v_dot2_f32_f16 v7, v1, v1, 0 clamp
	exp mrt0 v4, v5, v6, v7 done vm
	s_endpgm
