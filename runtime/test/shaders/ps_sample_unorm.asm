; image_sample_lz, image_sample_l, image_sample and image_gather4_lz with the MIMG unorm bit: the coordinates are in
; texels, and the translator divides them by the image's level-0 size before the Vulkan sample or gather.
; Run: vk_texture_test <vs_ids.spv> <ps_sample_unorm.spv> out.raw 0 92 c00000 14500000 0 -> RGBA32F pixels, all the same.
; The T# is loaded from the heap as in ps_texture_sample; the harness image is the 8x8 gradient R = 32x/255,
; G = 32y/255 (x, y = 0..7). The S# is the harness's: clamp to the last texel, bilinear. Every sample is at a texel
; centre, so bilinear filtering returns that texel exactly; a channel is scaled back by 255/32 and rounded to its index.
;   out.r: image_sample_lz (5.5, 2.5)          -> texel (5, 2): 5*8 + 2 = 42
;   out.g: image_sample_l (1.5, 6.5), LOD 0     -> texel (1, 6): 1*8 + 6 = 14
;   out.b: image_sample (3.5, 4.5), implicit LOD (the coordinates are the same in every pixel: derivatives 0, mip 0)
;                                               -> texel (3, 4): 3*8 + 4 = 28
;   out.a: image_gather4_lz (4.0, 4.0), R       -> footprint x = 3, 4; gather order (x0,y1) (x1,y1) (x1,y0) (x0,y0)
;                                                  gives R = 3 4 4 3: ((3*8 + 4)*8 + 4)*8 + 3 = 1827
; Read as normalized (the unorm bit ignored), every coordinate is past the far edge and clamps to texel 7:
; 63, 63, 63 and 4095.
v_mov_b32_e32 v23, 0
s_load_dwordx8 s[8:15], s[0:1], 0x0
s_waitcnt lgkmcnt(0)
s_mov_b32 s16, 0x92
s_mov_b32 s17, 0xc00000
s_mov_b32 s18, 0x14500000
s_mov_b32 s19, 0
v_mov_b32_e32 v20, 0x40b00000
v_mov_b32_e32 v21, 0x40200000
image_sample_lz v[8:9], v[20:21], s[8:15], s[16:19] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm
v_mov_b32_e32 v20, 0x3fc00000
v_mov_b32_e32 v21, 0x40d00000
v_mov_b32_e32 v22, 0
image_sample_l v[10:11], v[20:22], s[8:15], s[16:19] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm
v_mov_b32_e32 v24, 0x40600000
v_mov_b32_e32 v25, 0x40900000
image_sample v[12:13], v[24:25], s[8:15], s[16:19] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm
v_mov_b32_e32 v26, 4.0
v_mov_b32_e32 v27, 4.0
image_gather4_lz v[14:17], v[26:27], s[8:15], s[16:19] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
s_waitcnt vmcnt(0)
v_mul_f32 v8, 0x40ff0000, v8
v_rndne_f32 v8, v8
v_mul_f32 v9, 0x40ff0000, v9
v_rndne_f32 v9, v9
v_mul_f32 v10, 0x40ff0000, v10
v_rndne_f32 v10, v10
v_mul_f32 v11, 0x40ff0000, v11
v_rndne_f32 v11, v11
v_mul_f32 v12, 0x40ff0000, v12
v_rndne_f32 v12, v12
v_mul_f32 v13, 0x40ff0000, v13
v_rndne_f32 v13, v13
v_mul_f32 v14, 0x40ff0000, v14
v_rndne_f32 v14, v14
v_mul_f32 v15, 0x40ff0000, v15
v_rndne_f32 v15, v15
v_mul_f32 v16, 0x40ff0000, v16
v_rndne_f32 v16, v16
v_mul_f32 v17, 0x40ff0000, v17
v_rndne_f32 v17, v17
v_fma_f32 v4, v8, 8.0, v9
v_fma_f32 v5, v10, 8.0, v11
v_fma_f32 v6, v12, 8.0, v13
v_fma_f32 v7, v14, 8.0, v15
v_fma_f32 v7, v7, 8.0, v16
v_fma_f32 v7, v7, 8.0, v17
exp mrt0 v4, v5, v6, v7 done vm
s_endpgm
