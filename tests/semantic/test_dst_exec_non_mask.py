"""Regression tests for SHADER-1264: Gen.dst writing a computed non-mask value to exec_lo / exec_hi."""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from support import translate_lines  # noqa: E402


class DstExecNonMaskTests(unittest.TestCase):
    def test_wave32_computed_exec_lo_extracts_per_lane_bit(self):
        lines = [
            "s_mov_b32 s2, 2",
            "s_lshl_b32 exec_lo, 1, s2",
            "v_mov_b32 v0, 1",
            "exp mrt0 v0, v0, v0, v0 done vm",
            "s_endpgm",
        ]
        spv = translate_lines("ps_dst_exec_lo_w32", lines)
        self.assertIn("OpShiftRightLogical %uint", spv)

    def test_wave64_computed_exec_halves_extract_per_lane_bit(self):
        lines = [
            "s_mov_b32 s2, 5",
            "s_lshl_b32 exec_lo, 1, s2",
            "s_add_u32 exec_hi, s2, 0",
            "v_mov_b32 v0, 1",
            "exp mrt0 v0, v0, v0, v0 done vm",
            "s_endpgm",
        ]
        spv = translate_lines("ps_dst_exec_halves_w64", lines)
        # Two computed half-writes must emit two bit-extraction shifts (one per half).
        self.assertGreaterEqual(spv.count("OpShiftRightLogical %uint"), 2)

    def test_wave32_s_mov_mask_to_exec_lo_keeps_mask_path(self):
        lines = [
            "v_cmp_lt_f32 s2, v0, v1",
            "s_mov_b32 exec_lo, s2",
            "v_mov_b32 v2, 1",
            "exp mrt0 v2, v2, v2, v2 done vm",
            "s_endpgm",
        ]
        spv = translate_lines("ps_dst_exec_lo_mask_w32", lines)
        self.assertNotIn("OpShiftRightLogical %uint", spv)


if __name__ == "__main__":
    unittest.main()
