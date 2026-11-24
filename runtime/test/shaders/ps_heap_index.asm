; Direct heap indexing in a pixel shader (ResourceDescriptorHeap[k], docs/d3d12_vulkan.md sections 3.4 and 3.5), in
; the shape of the game's heap-indexing pixel shaders. Run: d3d12_heap_index_test (a graphics PSO found by the SHA-256
; of its FSSM program, with vs_m2.asm).
; Root signature: [0] one 32-bit constant, flag CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED. The pixel stage's root arguments
; start at s8, and the bound CBV/SRV/UAV heap's base follows them as a 32-bit pointer:
;   s8 = k, the heap index   s9 = console VA of the bound CBV/SRV/UAV heap's first slot (high word 0)
;   heap slot k (offset k * 32, the stride compiled in): a raw buffer view of 16 bytes, four floats
;   mrt0 = those four floats, at every pixel
; fssm-at: 0x10
; fssm-data: 0x50414548 0x58444e49
s_mov_b32 s20, s9
s_mov_b32 s21, 0
s_lshl_b32 s22, s8, 5
s_load_dwordx4 s[24:27], s[20:21], s22
v_mov_b32_e32 v6, 0
s_waitcnt lgkmcnt(0)
buffer_load_dwordx4 v[2:5], v6, s[24:27], 0 offen
s_waitcnt vmcnt(0)
exp mrt0 v2, v3, v4, v5 done vm
s_endpgm
