"""ds_append / ds_consume (runtime/test/shaders/cs_w32_ds_append.asm, cs_w64_ds_append.asm; docs/design.md).

One LDS add / subtract of the wave's active-lane count at the offset address, done by the first active lane; every
active lane gets the old value, inactive lanes keep vD. tests/semantic/spirv_mini_interp.py shares the workgroup's LDS, so these tests check the count, the new LDS word
(read back by every lane), the returned value in every active lane, and the inactive lanes. The GPU fixtures check it
on both GPUs (wave64 native and emulated). Before, both
raised Unsupported.
"""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import support  # noqa: E402
import spirv_mini_interp as si  # noqa: E402
import rdna2spirv  # noqa: E402

SHADERS = os.path.join(support.ROOT, "runtime", "test", "shaders")
VSHARP = [0, 0, 0xFFFFFFFF, 0x31016FAC]


def header(name):
    sys.path.insert(0, SHADERS)
    from compute_fixture import compute_header
    return bytes(compute_header(os.path.join(SHADERS, name + ".asm")))


def run(wave, exec_words, op="ds_append", off=0):
    """LDS word [off] = 100 in every invocation's copy; EXEC = exec_words; `op` v8; every lane stores
    (v8, its LDS word) at 8 * lane. Returns [(v8, lds) per lane]."""
    lines = ["v_mov_b32 v8, 0xdead", "v_mov_b32 v1, 0", "v_mov_b32 v2, 100", f"ds_write_b32 v1, v2 offset:{off}"]
    lines += [f"s_mov_b32 exec_lo, {exec_words[0]:#x}"] + ([f"s_mov_b32 exec_hi, {exec_words[1]:#x}"] if wave == 64 else [])
    lines += [f"{op} v8 offset:{off}" if off else f"{op} v8",
              "s_mov_b64 exec, -1" if wave == 64 else "s_mov_b32 exec_lo, -1",
              f"ds_read_b32 v9, v1 offset:{off}", "v_lshlrev_b32 v21, 3, v0",
              "buffer_store_dwordx2 v[8:9], v21, s[0:3], 0 offen", "s_endpgm"]
    hdr = bytearray(header("cs_w64_ds_append" if wave == 64 else "cs_w32_ds_append"))
    spv = support.translate_lines(f"cs_dsapp{wave}", lines, fssm=bytes(hdr))
    memory = {}
    si.run_workgroup(spv, wave, wave, inputs_for=lambda i: {"%wg_id": (0, 0, 0), "%local_id": (i, 0, 0), "%sg_lane": i},
                     push_constants=VSHARP + [0] * 28, memory=memory)
    return [(memory[8 * l], memory[8 * l + 4]) for l in range(wave)]


def expect(wave, exec_bits, op):
    """Plain-Python reference (shared LDS): the first active lane's LDS changes by the
    active count, every active lane returns the old value, inactive lanes keep 0xdead."""
    act = [l for l in range(wave) if exec_bits >> l & 1]
    cnt = len(act)
    out = []
    for l in range(wave):
        ret = 100 if l in act else 0xDEAD
        lds = (100 + cnt if op == "ds_append" else 100 - cnt) & 0xFFFFFFFF if act else 100
        out.append((ret, lds))
    return out


class DsAppend(unittest.TestCase):
    def test_wave32(self):
        for bits, op in ((0xF0F0, "ds_append"), (0x7, "ds_consume"), (0xFFFFFFFF, "ds_append"), (0x80000000, "ds_consume")):
            with self.subTest(exec=hex(bits), op=op):
                self.assertEqual(run(32, (bits,), op), expect(32, bits, op))

    def test_wave64_both_halves(self):
        for bits, op in (((1 << 63) | (1 << 32) | 1, "ds_append"), (1 << 33, "ds_consume")):
            with self.subTest(exec=hex(bits), op=op):
                self.assertEqual(run(64, (bits & 0xFFFFFFFF, bits >> 32), op), expect(64, bits, op))

    def test_offset_is_the_address(self):
        self.assertEqual(run(32, (0x3,), "ds_append", off=8), expect(32, 0x3, "ds_append"))

    def test_fixtures_translate_and_gds_is_refused(self):
        for name in ("cs_w32_ds_append", "cs_w64_ds_append"):
            with self.subTest(fixture=name):
                self.assertIn("OpAtomicIAdd", support.translate_fixture(name, fssm=header(name)))
        with self.assertRaisesRegex(rdna2spirv.Unsupported, "ds_append gds"):
            support.translate_lines("cs_dsapp_gds", ["ds_append v8 gds", "s_endpgm"], fssm=header("cs_w32_ds_append"))


if __name__ == "__main__":
    unittest.main()
