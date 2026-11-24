; Explicit-LOD gathers on the harness's 8x8 gradient (R = 32x/255, G = 32y/255).
; Texel coordinates (3.75, 4.75) have first tap (3, 5) in gather order.
; Offset (+2, -1) moves that tap to (5, 4). Normalized coordinates (3.75/8, 4.75/8)
; select the same first tap as the unnormalized, unoffset gather.
; Output (3, 5, 4, 3): unnormalized red, offset red, offset green, normalized red.
s_load_dwordx8 s[8:15], s[0:1], 0
s_waitcnt lgkmcnt(0)
s_mov_b32 s16, 0x12
s_mov_b32 s17, 0
s_mov_b32 s18, 0
s_mov_b32 s19, 0
v_mov_b32 v20, 0x3f02
v_mov_b32 v21, 0x40700000
v_mov_b32 v22, 0x40980000
v_mov_b32 v23, 0
v_mov_b32 v24, 0x3ef00000
v_mov_b32 v25, 0x3f180000
v_mov_b32 v26, 0
image_gather4_l v[8:11], v[21:23], s[8:15], s[16:19] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
image_gather4_l_o v[12:15], v[20:23], s[8:15], s[16:19] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
image_gather4_l_o v[16:19], v[20:23], s[8:15], s[16:19] dmask:0x2 dim:SQ_RSRC_IMG_2D unorm
image_gather4_l v[28:31], v[24:26], s[8:15], s[16:19] dmask:0x1 dim:SQ_RSRC_IMG_2D
s_waitcnt vmcnt(0)
v_mul_f32 v4, 0x40ff0000, v8
v_mul_f32 v5, 0x40ff0000, v12
v_mul_f32 v6, 0x40ff0000, v16
v_mul_f32 v7, 0x40ff0000, v28
v_rndne_f32 v4, v4
v_rndne_f32 v5, v5
v_rndne_f32 v6, v6
v_rndne_f32 v7, v7
exp mrt0 v4, v5, v6, v7 done vm
s_endpgm
