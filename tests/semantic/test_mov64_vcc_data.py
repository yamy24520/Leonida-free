"""Regression tests for SHADER-1263: s_mov_b64 to/from VCC when VCC holds 64-bit scalar data."""
import os
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
import sys
sys.path.insert(0, HERE)

from support import translate_lines  # noqa: E402


class Mov64VccDataTests(unittest.TestCase):
    def test_s_mov_b64_data_pair_through_vcc_preserves_both_halves(self):
        lines = [
            "s_mov_b32 s2, 0x11223344",
            "s_mov_b32 s3, 0x55667788",
            "s_mov_b64 vcc, s[2:3]",
            "s_mov_b64 s[4:5], vcc",
            "v_mov_b32 v0, s4",
            "v_mov_b32 v1, s5",
            "exp mrt0 v0, v1, v0, v1 done vm",
            "s_endpgm",
        ]
        spv = translate_lines("ps_mov64_vcc_data", lines)
        self.assertIn("OpStore %vcc_hi", spv)
        self.assertIn("OpLoad %uint %vcc_hi", spv)
        self.assertNotIn("OpShiftRightLogical", spv)

    def test_s_mov_b64_lane_mask_through_vcc_keeps_mask_semantics(self):
        lines = [
            "v_cmp_lt_f32 vcc, v0, v1",
            "s_mov_b64 s[2:3], vcc",
            "s_mov_b64 vcc, s[2:3]",
            "v_cndmask_b32 v2, v0, v1, vcc",
            "exp mrt0 v2, v2, v2, v2 done vm",
            "s_endpgm",
        ]
        spv = translate_lines("ps_mov64_vcc_mask", lines)
        self.assertNotIn("OpStore %vcc_hi", spv)
        self.assertNotIn("OpLoad %uint %vcc_hi", spv)


if __name__ == "__main__":
    unittest.main()
