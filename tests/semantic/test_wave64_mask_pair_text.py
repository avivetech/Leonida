"""A wave64 program's VALU lane masks, printed in wave32 syntax, are SGPR pairs.

tools/shader_disasm.py disassembles every program in llvm-mc's default wave32 mode, so a wave64 program's VOP3 lane
mask that is the pair s[6:7] reads "s6" in the listing (the encoding is the same in both modes). Each test translates
one wave64 program twice, once written with pairs (as llvm-mc prints it with +wavefrontsize64) and once with the
single SGPRs shader_disasm.py prints, and requires the same SPIR-V. Before the translator widened these operands, the
wave32-printed forms read the low SGPR in all 64 lanes. GPU check: runtime/test/shaders/cs_w64_mask_pair_w32text.asm.
"""
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from support import rdna2spirv, translate_lines  # noqa: E402

PROLOGUE = ["s_mov_b64 exec, -1"]   # a 64-bit EXEC write: the program is wave64


def fssm(threads, wave32=False):
    """A compute header: RSRC2 and the group size at 0x138 (as test_andn2_exec_b64.py builds it), and the wave size
    flag the translator reads first (fssm_header: bit 5 of 0xD4, set for wave32)."""
    h = bytearray(0x148)
    struct.pack_into("<4I", h, 0x138, 0x8088, threads, 1, 1)
    if wave32:
        h[0xD4] |= 0x20
    return bytes(h)


def both(body_pairs, body_singles):
    epilogue = ["v_lshlrev_b32_e32 v20, 2, v0", "buffer_store_dword v1, v20, s[0:3], 0 offen", "s_endpgm"]
    a = translate_lines("cs_w64_pairs", PROLOGUE + body_pairs + epilogue, fssm=fssm(64))
    b = translate_lines("cs_w64_pairs", PROLOGUE + body_singles + epilogue, fssm=fssm(64))
    return a, b


class SameAsPairs(unittest.TestCase):
    def assertSame(self, pairs, singles):
        a, b = both(pairs, singles)
        self.assertEqual(a, b, "the wave32-printed listing must translate as the pair form")

    def test_cndmask_condition(self):
        self.assertSame(["s_mov_b32 s6, 0", "s_mov_b32 s7, -1", "v_cndmask_b32_e64 v1, 0, 1, s[6:7]"],
                        ["s_mov_b32 s6, 0", "s_mov_b32 s7, -1", "v_cndmask_b32_e64 v1, 0, 1, s6"])

    def test_cmp_destination_then_cndmask(self):
        self.assertSame(["v_cmp_gt_u32_e64 s[8:9], 40, v0", "v_cndmask_b32_e64 v1, 0, 1, s[8:9]"],
                        ["v_cmp_gt_u32_e64 s8, 40, v0", "v_cndmask_b32_e64 v1, 0, 1, s8"])

    def test_add_carry_out_and_in(self):
        self.assertSame(["v_add_co_u32_e64 v2, s[10:11], v0, -1", "v_add_co_ci_u32_e64 v1, s[12:13], 0, 0, s[10:11]"],
                        ["v_add_co_u32_e64 v2, s10, v0, -1", "v_add_co_ci_u32_e64 v1, s12, 0, 0, s10"])

    def test_mad_u64_carry(self):
        self.assertSame(["v_mov_b32_e32 v4, -1", "v_mov_b32_e32 v5, -1",
                         "v_mad_u64_u32 v[2:3], s[14:15], v0, 1, v[4:5]", "v_cndmask_b32_e64 v1, 0, 1, s[14:15]"],
                        ["v_mov_b32_e32 v4, -1", "v_mov_b32_e32 v5, -1",
                         "v_mad_u64_u32 v[2:3], s14, v0, 1, v[4:5]", "v_cndmask_b32_e64 v1, 0, 1, s14"])


def ins(op, *args):
    return {"op": op, "args": list(args), "mods": {}}


def sgpr(r, n=1):
    return {"k": "sgpr", "r": r, "n": n}


VGPR = {"k": "vgpr", "r": 1, "n": 1}
VCC = {"k": "special", "t": "vcc_lo"}


class Widen(unittest.TestCase):
    """rdna2spirv.widen_wave64_lane_masks, operand by operand."""

    def widened(self, i):
        return [a.get("n") for a in rdna2spirv.widen_wave64_lane_masks([i])[0]["args"]]

    def test_positions(self):
        self.assertEqual(self.widened(ins("v_cndmask_b32_e64", VGPR, VGPR, VGPR, sgpr(6))), [1, 1, 1, 2])
        self.assertEqual(self.widened(ins("v_cmp_eq_u32_e64", sgpr(6), VGPR, VGPR)), [2, 1, 1])
        self.assertEqual(self.widened(ins("v_add_co_ci_u32_e64", VGPR, sgpr(6), VGPR, VGPR, sgpr(8))),
                         [1, 2, 1, 1, 2])
        self.assertEqual(self.widened(ins("v_mad_u64_u32", {"k": "vgpr", "r": 2, "n": 2}, sgpr(6), VGPR, VGPR,
                                          {"k": "vgpr", "r": 4, "n": 2})), [2, 2, 1, 1, 2])

    def test_left_alone(self):
        # an SGPR source that is data, not a mask (v_cndmask's src0/src1, a v_add's value operand)
        self.assertEqual(self.widened(ins("v_cndmask_b32_e64", VGPR, sgpr(4), VGPR, sgpr(6))), [1, 1, 1, 2])
        self.assertEqual(self.widened(ins("v_add_nc_u32_e64", VGPR, sgpr(6), VGPR)), [1, 1, 1])
        # v_cmpx writes EXEC; VCC is already the whole mask; an odd SGPR can't start a pair; a pair stays a pair
        self.assertEqual(self.widened(ins("v_cmpx_eq_u32_e64", sgpr(6), VGPR)), [1, 1])
        self.assertIsNone(rdna2spirv.widen_wave64_lane_masks([ins("v_cndmask_b32_e32", VGPR, VGPR, VGPR, VCC)])
                          [0]["args"][3].get("n"))
        self.assertEqual(self.widened(ins("v_cndmask_b32_e64", VGPR, VGPR, VGPR, sgpr(7))), [1, 1, 1, 1])
        self.assertEqual(self.widened(ins("v_cndmask_b32_e64", VGPR, VGPR, VGPR, sgpr(6, 2))), [1, 1, 1, 2])

    def test_wave32_program_is_not_widened(self):
        """The widening runs for wave64 programs only: a wave32 program's "s6" is a 32-bit mask."""
        a = translate_lines("cs_w32_single", ["s_mov_b32 s6, 0", "v_cndmask_b32_e64 v1, 0, 1, s6",
                                               "v_lshlrev_b32_e32 v20, 2, v0",
                                               "buffer_store_dword v1, v20, s[0:3], 0 offen", "s_endpgm"],
                            fssm=fssm(32, wave32=True))
        self.assertNotIn("OpUGreaterThanEqual", a, "no lane >= 32 split in a wave32 program")


if __name__ == "__main__":
    unittest.main()
