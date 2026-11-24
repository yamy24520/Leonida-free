; group: 64,1,1
; rsrc2: 0x8084
; fssm-at: 0x150
; fssm-data: 0x58444948 0x3d504148
; Direct heap indexing (ResourceDescriptorHeap[k], docs/d3d12_vulkan.md section 3.4): hand-written RDNA2 in the shape
; the game's heap-indexing compute shaders have. Run: d3d12_heap_index_test (a compute PSO found by the SHA-256 of its
; synthetic FSSM header, as for the other cs_ shaders).
; Root signature: [0] one 32-bit constant, flag CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED. The console puts the bound
; CBV/SRV/UAV heap's base right after the root arguments, as a 32-bit pointer; the heap stride (32 bytes) is compiled
; into the shader. RSRC2 0x8084 = two user SGPRs and TGID_X, so the group index is s2:
;   s0 = k, the heap index   s1 = console VA of the bound CBV/SRV/UAV heap's first slot (high word 0)
; D3D12 build: runtime/CMakeLists.txt builds this with LEONIDA_USER_DATA_SHIFT=4, so in the PSO every SGPR named here
; is 4 higher (the root constant at s4, the heap base at s5).
;   heap slot k (dynamic offset k * 32): a raw buffer UAV; lane g writes 0x1000 * k + g at byte 4 * g
;   heap slot 1 (static offset 0x20):    a raw buffer UAV; lane g writes 0xa5000000 + g at byte 4 * g
s_mov_b32 s4, s1
s_mov_b32 s5, 0
s_lshl_b32 s6, s0, 5
s_load_dwordx4 s[8:11], s[4:5], s6
s_load_dwordx4 s[12:15], s[4:5], 0x20
s_lshl_b32 s7, s2, 6
v_add_nc_u32_e32 v1, s7, v0
v_lshlrev_b32_e32 v2, 2, v1
s_lshl_b32 s3, s0, 12
v_add_nc_u32_e32 v3, s3, v1
v_add_nc_u32_e32 v4, 0xa5000000, v1
s_waitcnt lgkmcnt(0)
buffer_store_dword v3, v2, s[8:11], 0 offen
buffer_store_dword v4, v2, s[12:15], 0 offen
s_endpgm
