; ds_append / ds_consume (LDS): one add (append) or subtract (consume) of the number of lanes on in EXEC, for the whole
; wave, at the byte address in the offset field; every active lane gets the value before the op, inactive lanes keep
; vD. Measured natively on RDNA2 gfx1036 (tools/hw_oracle raw kernels): EXEC 0xf0 appends 4 and returns the old 100
; to lanes 4-7 only; M0 (0x4, 0x100, 0x10000, 0x40000, 0x80000) does not move the address. Until this change both
; raised Unsupported.
; LDS words: [0] = 100, [2] (byte 8) = 200, written by every lane first.
;   EXEC = 0xf0f0 (8 lanes):  ds_append v8          -> v8 = 100 in lanes 4-7, 12-15; LDS[0] = 108
;   EXEC = 0x7 (3 lanes):     ds_consume v9 offset:8 -> v9 = 200 in lanes 0-2;      LDS[2] = 197
;   EXEC = all (32 lanes):    ds_append v10          -> v10 = 108 everywhere;       LDS[0] = 140
; Lane 0 stores:
;   out[0] v8 of lane 4 = 100       out[1] v8 of lane 0 (off) = 0xdead   out[2] v9 of lane 2 = 200
;   out[3] v9 of lane 3 (off) = 0xdead   out[4] LDS[0] read by lane 4 after its append = 108
;   out[5] LDS[2] read by lane 0 after its consume = 197   out[6] v10 of lane 31 = 108   out[7] LDS[0] = 140
; Done once per lane instead of once per wave, the first append alone would leave 100 + 8 * 8.
; rsrc2: 0x40088
; group: 32,1,1
; Run: vk_compute_test cs_w32_ds_append.spv 1 dump -> out[0..7] = 100 57005 200 57005 108 197 108 140
; mattr: +wavefrontsize32,-wavefrontsize64
v_mov_b32_e32 v8, 0xdead
v_mov_b32_e32 v9, 0xdead
v_mov_b32_e32 v1, 0
v_mov_b32_e32 v2, 100
v_mov_b32_e32 v3, 200
ds_write_b32 v1, v2
ds_write_b32 v1, v3 offset:8
s_waitcnt lgkmcnt(0)
s_mov_b32 exec_lo, 0xf0f0
ds_append v8
s_waitcnt lgkmcnt(0)
ds_read_b32 v11, v1
s_waitcnt lgkmcnt(0)
s_mov_b32 exec_lo, 7
ds_consume v9 offset:8
s_waitcnt lgkmcnt(0)
ds_read_b32 v12, v1 offset:8
s_waitcnt lgkmcnt(0)
s_mov_b32 exec_lo, -1
ds_append v10
s_waitcnt lgkmcnt(0)
ds_read_b32 v13, v1
s_waitcnt lgkmcnt(0)
v_readlane_b32 s20, v8, 4
v_readlane_b32 s21, v8, 0
v_readlane_b32 s22, v9, 2
v_readlane_b32 s23, v9, 3
v_readlane_b32 s24, v11, 4
v_readlane_b32 s25, v12, 0
v_readlane_b32 s26, v10, 31
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
