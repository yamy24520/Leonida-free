"""TG_SIZE_EN: the compute thread-group SGPR (COMPUTE_PGM_RSRC2 bit 10).

Standard library only; no GPU, no LLVM, no game files.

With TG_SIZE_EN set, the SGPR after the user data and the enabled workgroup ids holds the number of waves in the
workgroup in bits [5:0] and the wave's index in it in bits [11:6] (Mesa, src/amd/common/nir/
ac_nir_lower_intrinsics_to_args.c, GFX6-10: load_num_subgroups = tg_size[5:0], load_subgroup_id = tg_size[11:6];
a wave64 compute shader's local invocation index is mbcnt(~0, tg_size & 0xfc0)). The translator writes that value
in the prologue: thread i is in wave i / W (W = 32 or 64), the rule s_get_waveid_in_workgroup uses. It also has
the wave index in [24:20] and bit 31 set in the first wave, as measured on gfx1036 (#6446, tools/hw_oracle);
not measured on gfx1013. 91 compute programs of the pipeline library set the bit (docs/shader_library.md);
the reads of it all start with `s_and_b32 sN, sTG, 0xfc0`, and most feed that to v_mbcnt_lo_u32_b32, as below.

Each case translates a compute listing (4 user SGPRs, workgroup id x in s4, so the thread-group SGPR is s5), runs
every invocation with run_workgroup on the CPU and has each one store a value at buffer word LocalInvocationIndex.
"""
import os
import struct
import sys
import tempfile
import unittest
import warnings

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TOOLS_DIR = os.environ.get("LEONIDA_TOOLS_DIR") or os.path.join(ROOT, "tools")

sys.dont_write_bytecode = True
sys.path.insert(0, TOOLS_DIR)
sys.path.insert(0, HERE)

import rdna2spirv  # noqa: E402
import spirv_mini_interp as si  # noqa: E402

STUB_DXBC = b"DXBC" + bytes(24) + struct.pack("<I", 0) + bytes(4)
VSHARP = [0, 0, 0x4000, 0x31016FAC]
TG_SIZE_EN = 1 << 10
RSRC2 = 0x88 | 2 << 11          # 4 user SGPRs, workgroup id x in s4, thread ids x, y, z in v0, v1, v2


def translate(lines, size, rsrc2, name="cs_tg_size"):
    with tempfile.TemporaryDirectory() as tmp:
        for d in ("disasm", "fssm"):
            os.mkdir(os.path.join(tmp, d))
        with open(os.path.join(tmp, "disasm", name + ".s"), "w", encoding="utf-8") as f:
            f.write("; " + name + "\n" + "".join(f"\t{ln}\n" for ln in lines) + "\ts_endpgm\n")
        with open(os.path.join(tmp, name + ".dxbc"), "wb") as f:
            f.write(STUB_DXBC)
        blob = bytearray(0x148)
        struct.pack_into("<4I", blob, 0x138, rsrc2, *size)
        with open(os.path.join(tmp, "fssm", name + ".fssm"), "wb") as f:
            f.write(bytes(blob))
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", ResourceWarning)
            return rdna2spirv.translate(os.path.join(tmp, "disasm", name + ".s"))


def run(body, wave64, size, rsrc2=RSRC2 | TG_SIZE_EN):
    """Translate `body` (each thread then stores v5 at buffer word = its flat index) and run every invocation."""
    sx, sy, sz = size
    lines = (["s_mov_b64 exec, -1"] if wave64 else []) + list(body) + [
        f"v_mad_u32_u24 v3, v1, {sx}, v0", f"v_mad_u32_u24 v3, v2, {sx * sy}, v3", "v_lshlrev_b32 v3, 2, v3",
        "buffer_store_dword v5, v3, s[0:3], 0 offen"]
    spv = translate(lines, size, rsrc2)
    n = sx * sy * sz
    w = 64 if wave64 else 32

    def inputs(i):
        return {"%wg_id": (0, 0, 0), "%local_id": (i % sx, (i // sx) % sy, i // (sx * sy)), "%sg_lane": i % w}
    mem = si.run_workgroup(spv, n, w, inputs_for=inputs, push_constants=VSHARP + [0] * 28, memory={})
    return [mem.get(4 * i) for i in range(n)]


LOCAL_INDEX_W64 = ["s_and_b32 s6, s5, 0xfc0", "v_mbcnt_lo_u32_b32 v5, -1, s6", "v_mbcnt_hi_u32_b32 v5, -1, v5"]
LOCAL_INDEX_W32 = ["s_and_b32 s6, s5, 0xfc0", "s_lshr_b32 s6, s6, 1", "v_mbcnt_lo_u32_b32 v5, -1, s6"]


class TgSizeTests(unittest.TestCase):
    def test_local_index_wave64(self):
        # Mesa's wave64 local invocation index: mbcnt(~0, tg_size & 0xfc0).
        for size in ((256, 1, 1), (16, 16, 1), (8, 8, 4)):
            with self.subTest(size=size):
                n = size[0] * size[1] * size[2]
                self.assertEqual(run(LOCAL_INDEX_W64, True, size), list(range(n)))

    def test_local_index_wave32(self):
        # Wave index * 32 from bits [11:6]: (tg_size & 0xfc0) >> 1.
        for size in ((128, 1, 1), (8, 8, 1), (4, 4, 4)):
            with self.subTest(size=size):
                n = size[0] * size[1] * size[2]
                self.assertEqual(run(LOCAL_INDEX_W32, False, size), list(range(n)))

    def test_wave_count(self):
        # Bits [5:0]: waves in the group, a partial last wave included.
        for wave64, size, waves in ((True, (256, 1, 1), 4), (False, (256, 1, 1), 8), (True, (100, 1, 1), 2),
                                    (False, (6, 6, 1), 2), (True, (1, 1, 1), 1), (False, (32, 32, 1), 32)):
            with self.subTest(wave64=wave64, size=size):
                got = run(["s_and_b32 s6, s5, 0x3f", "v_mov_b32 v5, s6"], wave64, size)
                self.assertEqual(set(got), {waves})

    def test_whole_value(self):
        got = run(["v_mov_b32 v5, s5"], True, (192, 1, 1))
        self.assertEqual(got, [(i // 64) << 20 | (i // 64) << 6 | 3 | (0x80000000 if i < 64 else 0)
                               for i in range(192)])

    def test_matches_the_gfx1036_measurement(self):
        # tools/hw_oracle on gfx1036 (#6446), wave32 x 128 threads: one word per wave.
        got = run(["v_mov_b32 v5, s5"], False, (128, 1, 1))
        self.assertEqual([got[32 * k] for k in range(4)], [0x80000004, 0x00100044, 0x00200084, 0x003000C4])
        self.assertEqual(got, [got[32 * (i // 32)] for i in range(128)])

    def test_follows_the_workgroup_ids(self):
        # TGID_X/Y/Z all enabled: the ids are s4, s5, s6, the thread-group SGPR is s7.
        rsrc2 = 0x388 | 2 << 11 | TG_SIZE_EN
        got = run(["s_and_b32 s8, s7, 0xfc0", "v_mbcnt_lo_u32_b32 v5, -1, s8", "v_mbcnt_hi_u32_b32 v5, -1, v5"],
                  True, (128, 1, 1), rsrc2)
        self.assertEqual(got, list(range(128)))

    def test_not_written_without_the_bit(self):
        spv = translate(["v_mov_b32 v5, s5", "buffer_store_dword v5, v0, s[0:3], 0 offen"], (64, 1, 1), RSRC2)
        self.assertNotIn("%tgv", spv)


if __name__ == "__main__":
    unittest.main()
