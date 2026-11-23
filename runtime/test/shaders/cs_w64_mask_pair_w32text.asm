; group: 64,1,1
; rsrc2: 0x8090
; A wave64 program as tools/shader_disasm.py prints it. llvm-mc disassembles in its default wave32 mode, so a VOP3 lane
; mask that is the SGPR pair s[6:7] in wave64 reads "s6" (the encoding is the same in both modes; RDNA2's wave64
; VOP3 SSRC2 and VOPC/VOP3b SDST are 64-bit pairs). The 64-bit EXEC write marks the program as wave64. Until the
; translator widened these operands, lanes 32-63 read s6's bits instead of s7's.
;   reads:  s6 = 0, s7 = -1; v1 = v_cndmask_b32_e64(0, 1, s6): lanes 0-31 take 0, lanes 32-63 take 1.
;           out[0..3] = v1 of lanes 0, 31, 32, 63                                  = 0 0 1 1
;   writes: v_cmp_gt_u32_e64 s8, 40, v0 sets the mask of lanes 0-39 in s[8:9]; v3 = v_cndmask_b32_e64(0, 1, s8).
;           out[4..7] = v3 of lanes 0, 39, 40, 63                                  = 1 1 0 0
; Before the fix the reads gave 0 0 0 0 (every lane took s6). The write-then-read case already passed: the translator
; keeps a mask a v_cmp writes as each lane's own bit, so that round trip never needs the pair; it guards the widening.
; Run: VKC_SUBGROUP_SIZE=64 vk_compute_test cs_w64_mask_pair_w32text.spv 1 dump --require-wave64
s_mov_b64 exec, -1
s_mov_b32 s6, 0
s_mov_b32 s7, -1
v_cndmask_b32_e64 v1, 0, 1, s6
v_cmp_gt_u32_e64 s8, 40, v0
v_cndmask_b32_e64 v3, 0, 1, s8
v_readlane_b32 s10, v1, 0
v_readlane_b32 s11, v1, 31
v_readlane_b32 s12, v1, 32
v_readlane_b32 s13, v1, 63
v_readlane_b32 s14, v3, 0
v_readlane_b32 s15, v3, 39
v_readlane_b32 s16, v3, 40
v_readlane_b32 s17, v3, 63
v_mov_b32_e32 v4, s10
v_mov_b32_e32 v5, s11
v_mov_b32_e32 v6, s12
v_mov_b32_e32 v7, s13
v_mov_b32_e32 v8, s14
v_mov_b32_e32 v9, s15
v_mov_b32_e32 v10, s16
v_mov_b32_e32 v11, s17
v_mov_b32_e32 v12, 0
buffer_store_dwordx4 v[4:7], v12, s[0:3], 0 offen
buffer_store_dwordx4 v[8:11], v12, s[0:3], 0 offen offset:16
s_endpgm
