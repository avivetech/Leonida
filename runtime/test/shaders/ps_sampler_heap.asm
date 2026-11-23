; Direct sampler-heap indexing in a pixel shader (SamplerDescriptorHeap[k], docs/d3d12_vulkan.md section 3.4), the way
; the game's pixel shaders index the sampler heap: index << 4, the 16-byte step compiled in. Run: d3d12_heap_index_test
; (a graphics PSO found by the SHA-256 of its FSSM program, with vs_m2.asm).
; Root signature: [0] one 32-bit constant, flags CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED, so
; from s8:   s8 = k   s9 = the bound CBV/SRV/UAV heap's base   s10 = the bound sampler heap's base (32-bit VAs)
;   resource heap slot 0: the T# of a 2x1 texture (texel 0 red, texel 1 green)
;   sampler heap slot k (offset k * 16): an S#, point filtering; U is WRAP or CLAMP
;   mrt0 = the texel at (u, v) = (1.25, 0.5): green through a CLAMP sampler, red through a WRAP one
; fssm-at: 0x10
; fssm-data: 0x50534d50 0x50414548
s_mov_b32 s20, s9
s_mov_b32 s21, 0
s_mov_b32 s22, s10
s_mov_b32 s23, 0
s_lshl_b32 s36, s8, 4
s_load_dwordx8 s[24:31], s[20:21], 0x0
s_load_dwordx4 s[32:35], s[22:23], s36
v_mov_b32_e32 v2, 0x3fa00000
v_mov_b32_e32 v3, 0.5
s_waitcnt lgkmcnt(0)
image_sample v[4:7], v[2:3], s[24:31], s[32:35] dmask:0xf dim:SQ_RSRC_IMG_2D
s_waitcnt vmcnt(0)
exp mrt0 v4, v5, v6, v7 done vm
s_endpgm
