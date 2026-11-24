"""s_bitreplicate_b64_b32 into EXEC, s_flbit_i32_b64 of EXEC, and a wave32 64-bit VCC data write.

RDNA2 ISA SOP1: s_bitreplicate_b64_b32 D[2i+1] = D[2i] = S0[i], i in 0..31, SCC not written; with D = EXEC lane L is
active when S0[L // 2] is set (wave32: exec_hi is not a lane). s_flbit_i32_b64 counts the zeros above the highest set
bit of the 64-bit source, -1 for 0; with S0 = EXEC the source is the whole wave's lanes, lane L at bit L, and in a
wave32 shader exec_hi reads 0. In a wave32 shader the 64-bit VCC is vcc_lo and vcc_hi, separate registers: a plain
write to the pair ends vcc_hi's life as a lane mask, so an s_load that takes the pair as its base reads data.

Hand-written listings translated with tools/rdna2spirv.py and run on the CPU in spirv_mini_interp.py.
Standard library only; no GPU, no game files.
"""
import gc
import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
import warnings

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TOOLS_DIR = os.environ.get("LEONIDA_TOOLS_DIR") or os.path.join(ROOT, "tools")

sys.dont_write_bytecode = True
for _p in (HERE, TOOLS_DIR):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import rdna2spirv  # noqa: E402
import spirv_mini_interp as si  # noqa: E402

M32 = 0xFFFFFFFF
STUB_DXBC = b"DXBC" + bytes(24) + struct.pack("<I", 0) + bytes(4)
VSHARP = [0, 0, 0xFFFFFFFF, 0x31016FAC]
WORDS = (0, 1, 0x80000001, 0x0F0F0F0F, 0xFF00FF00, 0x00030003, 0x55555555, M32)


def bitreplicate(x):
    return sum(3 << (2 * i) for i in range(32) if (x >> i) & 1)


def flbit64(m):
    return (64 - m.bit_length()) if m else M32


def flbit_i64(m):
    return flbit64(m ^ ((1 << 64) - 1) if m >> 63 else m)


def translate_cs(name, lines, threads, wave32=False):
    """Translate `lines` as an FSPX compute program (disasm/, fspx/<name>.json and .code, no fssm/): its dispatch
    state gives the wave size, which the DLL's wave32 programs need since they move VCC as an ordinary SGPR pair."""
    name = f"cs_{name}"
    with tempfile.TemporaryDirectory() as tmp:
        for d in ("disasm", "fspx"):
            os.mkdir(os.path.join(tmp, d))
        with open(os.path.join(tmp, name + ".dxbc"), "wb") as f:
            f.write(STUB_DXBC)
        with open(os.path.join(tmp, "fspx", name + ".json"), "w", encoding="utf-8") as f:
            json.dump({"rsrc2": 0x8090, "threads": [threads, 1, 1], "wave32": wave32}, f)
        with open(os.path.join(tmp, "fspx", name + ".code"), "wb") as f:
            f.write(b"\0" * 64)
        src = os.path.join(tmp, "disasm", name + ".s")
        with open(src, "w", encoding="utf-8", newline="\n") as f:
            f.write(f"; {name}\n" + "".join(f"\t{ln}\n" for ln in lines))
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", ResourceWarning)
            spv = rdna2spirv.translate(src)
            gc.collect()
        assert rdna2spirv.translate.last_gen.wave64 == (not wave32)
        return spv


def run_cs(spv, threads, s4=0, s5=0):
    pc = VSHARP + [s4 & M32, s5 & M32] + [0] * 26
    return si.run_workgroup(
        spv, threads, threads,
        inputs_for=lambda i: {"%wg_id": (0, 0, 0), "%local_id": (i, 0, 0), "%sg_lane": i},
        push_constants=pc, memory={},
    )


STORE = [
    "v_lshlrev_b32 v6, 2, v0",
    "buffer_store_dword v4, v6, s[0:3], 0 offen",
    "buffer_store_dword v5, v6, s[0:3], 0 offen offset:256",
    "s_endpgm",
]


class BitreplicateIntoExec(unittest.TestCase):
    def check(self, name, src, wave64, lines_before=()):
        asm = list(lines_before) + [
            "v_mov_b32 v4, 0",
            "s_cmp_eq_u32 0, 0",
            f"s_bitreplicate_b64_b32 exec, {src}",
            "v_mov_b32 v4, 1",
            "s_cselect_b32 s5, 1, 0",
            "s_mov_b64 exec, -1" if wave64 else "s_mov_b32 exec_lo, -1",
            "v_mov_b32 v5, s5",
        ] + STORE
        threads = 64 if wave64 else 32
        return translate_cs(name, asm, threads, wave32=not wave64), threads

    def test_wave64_sgpr_source_keeps_scc(self):
        spv, n = self.check("br_exec64", "s4", True)
        for w in WORDS:
            with self.subTest(word=hex(w)):
                mem = run_cs(spv, n, s4=w)
                self.assertEqual([mem[4 * i] for i in range(n)], [w >> (i // 2) & 1 for i in range(n)])
                self.assertEqual([mem[256 + 4 * i] for i in range(n)], [1] * n)

    def test_wave64_literal_source(self):
        for w in (0xFF00FF00, 0x03030303, 0x01010101, 0x000F000F, 0x0F0F0F0F):
            with self.subTest(word=hex(w)):
                spv, n = self.check("br_exec64_lit", hex(w), True)
                mem = run_cs(spv, n)
                self.assertEqual([mem[4 * i] for i in range(n)], [w >> (i // 2) & 1 for i in range(n)])

    def test_wave32_literal_source_is_lanes_0_to_31(self):
        w = 0xFF00FF00
        asm = ["v_mov_b32 v4, 0", "s_mov_b32 exec_lo, -1", f"s_bitreplicate_b64_b32 exec, {hex(w)}",
               "v_mov_b32 v4, 1", "s_mov_b32 exec_lo, -1", "v_mov_b32 v5, 0"] + STORE
        spv = translate_cs("br_exec32", asm, 32, wave32=True)
        mem = run_cs(spv, 32)
        self.assertEqual([mem[4 * i] for i in range(32)], [w >> (i // 2) & 1 for i in range(32)])


class FlbitOfExec(unittest.TestCase):
    def run_words(self, name, wave64):
        n = 64 if wave64 else 32
        restore = "s_mov_b64 exec, -1" if wave64 else "s_mov_b32 exec_lo, -1"
        asm = [
            "v_mov_b32 v4, 0",
            f"s_bitreplicate_b64_b32 exec, s4",
            "s_flbit_i32_b64 s10, exec",
            "s_flbit_i32_i64 s11, exec",
            restore,
            "v_mov_b32 v4, s10",
            "v_mov_b32 v5, s11",
        ] + STORE
        return translate_cs(name, asm, n, wave32=not wave64), n

    def test_wave64(self):
        spv, n = self.run_words("flbit_exec64", True)
        for w in WORDS:
            with self.subTest(word=hex(w)):
                mem = run_cs(spv, n, s4=w)
                want = flbit64(bitreplicate(w))
                self.assertEqual({mem[4 * i] for i in range(n)}, {want})
                self.assertEqual({mem[256 + 4 * i] for i in range(n)}, {flbit_i64(bitreplicate(w))})

    def test_wave32_exec_hi_reads_zero(self):
        spv, n = self.run_words("flbit_exec32", False)
        for w in WORDS:
            with self.subTest(word=hex(w)):
                mem = run_cs(spv, n, s4=w)
                lanes = bitreplicate(w) & M32
                self.assertEqual({mem[4 * i] for i in range(n)}, {flbit64(lanes)})


class Wave32VccPairIsData(unittest.TestCase):
    def test_vcc_hi_after_64bit_write_reads_the_scalar(self):
        asm = [
            "v_cmp_lt_u32_e64 vcc_hi, v0, 5",          # vcc_hi as a lane mask first
            "s_mov_b64 vcc, s[4:5]",                   # then plain data in both halves
            "v_mov_b32 v4, vcc_hi",
            "v_mov_b32 v5, vcc_lo",
        ] + STORE
        spv = translate_cs("vcc_pair_data", asm, 32, wave32=True)
        mem = run_cs(spv, 32, s4=0x11111111, s5=0x22222222)
        self.assertEqual({mem[4 * i] for i in range(32)}, {0x22222222})
        self.assertEqual({mem[256 + 4 * i] for i in range(32)}, {0x11111111})

    def test_s_load_with_vcc_base_after_a_vcc_hi_mask(self):
        asm = [
            "v_cmp_lt_u32_e64 vcc_hi, v0, 5",
            "s_mov_b64 vcc, s[4:5]",
            "s_load_dwordx4 s[16:19], vcc, 0x460",
            "s_load_dwordx8 s[24:31], vcc, s6",
            "s_waitcnt lgkmcnt(0)",
            "s_endpgm",
        ]
        spv = translate_cs("vcc_pair_sload", asm, 32, wave32=True)
        self.assertIn("OpFunction", spv)


if __name__ == "__main__":
    unittest.main()
