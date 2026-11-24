; ds_append / ds_consume (LDS) in a wave64: one add / subtract of the wave's active-lane count (both halves) at the
; offset address, the old value to every active lane. Measured natively on RDNA2 gfx1036 (tools/hw_oracle raw
; kernels): EXEC = lanes 0, 32 and 63 appends 3 and returns 100 to those three lanes. The translator has the first
; active lane do it and sends the old value to the others (a subgroup shuffle on a 64-wide subgroup, an exchange
; round under --wave64-emulate). Until this change both raised Unsupported.
; LDS words: [0] = 100, [2] (byte 8) = 200, written by every lane first.
;   EXEC = lanes 0, 32, 63:   ds_append v8          -> v8 = 100 in those lanes;       LDS[0] = 103
;   EXEC = lane 33:           ds_consume v9 offset:8 -> v9 = 200 in lane 33;          LDS[2] = 199
;   EXEC = all (64 lanes):    ds_append v10          -> v10 = 103 everywhere;         LDS[0] = 167
; Lane 0 stores:
;   out[0] v8 of lane 63 = 100     out[1] v8 of lane 32 = 100     out[2] v8 of lane 1 (off) = 0xdead
;   out[3] LDS[0] read by lane 0 after its append = 103   out[4] v9 of lane 33 = 200
;   out[5] LDS[2] read by lane 33 after its consume = 199   out[6] v10 of lane 40 = 103   out[7] LDS[0] = 167
; rsrc2: 0x40088
; Run: VKC_SUBGROUP_SIZE=64 vk_compute_test cs_w64_ds_append.spv 1 dump --require-wave64
;   (or translated with --wave64-emulate, on any subgroup size) -> out[0..7] = 100 100 57005 103 200 199 103 167
; mattr: +wavefrontsize64,-wavefrontsize32
v_mov_b32_e32 v8, 0xdead
v_mov_b32_e32 v9, 0xdead
v_mov_b32_e32 v1, 0
v_mov_b32_e32 v2, 100
v_mov_b32_e32 v3, 200
ds_write_b32 v1, v2
ds_write_b32 v1, v3 offset:8
s_waitcnt lgkmcnt(0)
s_mov_b32 exec_lo, 1
s_mov_b32 exec_hi, 0x80000001
ds_append v8
s_waitcnt lgkmcnt(0)
ds_read_b32 v11, v1
s_waitcnt lgkmcnt(0)
s_mov_b32 exec_lo, 0
s_mov_b32 exec_hi, 2
ds_consume v9 offset:8
s_waitcnt lgkmcnt(0)
ds_read_b32 v12, v1 offset:8
s_waitcnt lgkmcnt(0)
s_mov_b64 exec, -1
ds_append v10
s_waitcnt lgkmcnt(0)
ds_read_b32 v13, v1
s_waitcnt lgkmcnt(0)
v_readlane_b32 s20, v8, 63
v_readlane_b32 s21, v8, 32
v_readlane_b32 s22, v8, 1
v_readlane_b32 s23, v11, 0
v_readlane_b32 s24, v9, 33
v_readlane_b32 s25, v12, 33
v_readlane_b32 s26, v10, 40
v_readlane_b32 s27, v13, 0
v_mov_b32_e32 v14, s20
v_mov_b32_e32 v15, s21
v_mov_b32_e32 v16, s22
v_mov_b32_e32 v17, s23
v_mov_b32_e32 v18, s24
v_mov_b32_e32 v19, s25
v_mov_b32_e32 v20, s26
v_mov_b32_e32 v21, s27
v_cmpx_eq_u32_e32 0, v0
v_mov_b32_e32 v22, 0
buffer_store_dwordx4 v[14:17], v22, s[0:3], 0 offen
v_mov_b32_e32 v22, 16
buffer_store_dwordx4 v[18:21], v22, s[0:3], 0 offen
s_endpgm
