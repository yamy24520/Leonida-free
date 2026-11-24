"""v_dot2_f32_f16 `clamp` is a no-op on RDNA2: the translator gives the words measured on the hardware.

Measured on an AMD Radeon RX 6700 XT (gfx1031, RADV / amdgpu DRM, `tools/hw_oracle`) in wave32 and wave64, which
agree word for word. Over 2,560 random and special inputs per wave size the words with and without `clamp` were
identical, while `v_fma_f32 ... clamp` in the same kernels clamped. The console's gfx1013 has no `v_dot2_f32_f16`
(docs/design.md, GFX10.1 / GFX10.3 opcode differences), so desktop gfx103x is the only hardware for it.
"""

import math
from pathlib import Path
import struct
import unittest

from support import HERE, mrt0_words, translate_listing, translate_lines

# (S0, S1, S2, extra modifiers, measured v_dot2_f32_f16 ... clamp word)
HW_WORDS = [
    (0x42004000, 0x38004400, 0x3f800000, "", 0x41280000),
    (0x42004000, 0x38004400, 0x3f800000, "neg_lo:[1,0,0] neg_hi:[0,1,0]", 0xc1080000),
    (0x38003400, 0x3c003c00, 0x3f000000, "", 0x3fa00000),
    (0x38003400, 0x3c003c00, 0x3e800000, "", 0x3f800000),
    (0x38003400, 0x3c003c00, 0xbe800000, "", 0x3f000000),
    (0x3c00bc00, 0x3c003c00, 0x00000000, "", 0x00000000),
    (0xbc00bc00, 0x3c003c00, 0x00000000, "", 0xc0000000),
    (0x7bff7bff, 0x7bff7bff, 0x00000000, "", 0x4fffc004),
    (0x42004000, 0x38004400, 0x7fc00000, "", 0x7fc00000),
    (0x42004000, 0x38004400, 0x7f800000, "", 0x7f800000),
    (0x42004000, 0x38004400, 0xff800000, "", 0xff800000),
    (0x7c003c00, 0x00003c00, 0x00000000, "", 0xffc00000),
    (0x40003c00, 0x44004200, 0x3f800000, "op_sel:[1,0,0] op_sel_hi:[0,1,1]", 0x41300000),
    (0x40003c00, 0x44004200, 0xc1a00000, "neg_lo:[0,0,1]", 0x41f80000),
]


def is_nan(word):
    return math.isnan(struct.unpack("<f", struct.pack("<I", word))[0])


class Dot2F32F16ClampHw(unittest.TestCase):
    def test_fixture(self):
        fixture = Path(HERE) / "fixtures" / "dot2_f32_f16_clamp.asm"
        text = translate_listing("ps_dot2_f32_f16_clamp", fixture.read_bytes())
        self.assertEqual(mrt0_words(text), [0x41280000, 0xC1080000, 0x3FA00000, 0x4FFFC004])
        self.assertNotIn("FClamp", text)

    def test_measured_words(self):
        for s0, s1, s2, mods, want in HW_WORDS:
            with self.subTest(s0=hex(s0), s1=hex(s1), s2=hex(s2), mods=mods):
                text = translate_lines("ps_dot2_f32_f16_clamp_hw", [
                    f"v_mov_b32 v1, {s0:#x}",
                    f"v_mov_b32 v2, {s1:#x}",
                    f"v_mov_b32 v3, {s2:#x}",
                    f"v_dot2_f32_f16 v0, v1, v2, v3 {mods} clamp".replace("  ", " "),
                    "exp mrt0 v0, v0, v0, v0 done vm",
                    "s_endpgm",
                ])
                got = mrt0_words(text)[0]
                if is_nan(want):
                    # SPIR-V keeps neither the payload nor the sign of a NaN.
                    self.assertTrue(is_nan(got), hex(got))
                else:
                    self.assertEqual(got, want)


if __name__ == "__main__":
    unittest.main()
