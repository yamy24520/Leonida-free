"""FSPX translator rules (docs/design.md, contract table): the 24-byte VOP3 0x180 prologue, s_sethalt, and VOPC DPP
compares that llvm-mc rejects. Listings are hand-written, as llvm-mc prints them with the rejected words missing.

Standard library only; no GPU, no LLVM, no game files.
"""
from __future__ import annotations

import json
import os
import struct
import sys
import tempfile
import unittest
import warnings

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.dont_write_bytecode = True
sys.path.insert(0, os.environ.get("LEONIDA_TOOLS_DIR") or os.path.join(ROOT, "tools"))
sys.path.insert(0, HERE)

import rdna2spirv  # noqa: E402

STUB_DXBC = b"DXBC" + bytes(24) + struct.pack("<I", 0) + bytes(4)
MOV = (b"\x80\x02\x02\x7e", "v_mov_b32_e32 v1, 0")
SEL = (b"\x02\x07\x02\x00", "v_cndmask_b32_e32 v1, v2, v3, vcc_lo")
END = (b"\x00\x00\x81\xbf", "s_endpgm")
HALT = (b"\x01\x00\x8d\xbf", "s_sethalt 1")
JUNK = (b"\x96\xf9\xff\x03", "v_cndmask_b32_e32 v255, v150, v252, vcc_lo")


def translate(parts, invalid=0):
    """parts: bytes (a rejected or data run, no listing line) or (bytes, text) tuples listed as llvm-mc prints them."""
    blob = b"".join(p if isinstance(p, bytes) else p[0] for p in parts)
    with tempfile.TemporaryDirectory() as tmp:
        for d in ("disasm", "fspx"):
            os.mkdir(os.path.join(tmp, d))
        with open(os.path.join(tmp, "cs_t.dxbc"), "wb") as f:
            f.write(STUB_DXBC)
        with open(os.path.join(tmp, "fspx", "cs_t.code"), "wb") as f:
            f.write(blob)
        with open(os.path.join(tmp, "fspx", "cs_t.json"), "w", encoding="utf-8", newline="\n") as f:
            json.dump({"rsrc2": 0x88, "threads": [32, 1, 1], "wave32": True}, f)
        lines, pc = [f"; cs_t  code 0x0..0x{len(blob):x} (fspx), gfx1013, {invalid} invalid words"], 0
        for p in parts:
            if not isinstance(p, bytes):
                lines.append("\t%-40s; encoding: [%s]" % (p[1], ",".join("0x%02x" % b for b in p[0])))
        lines.append("")
        lines += ["; --- llvm-mc messages ---"] + ["; <stdin>:1:1: warning: invalid instruction encoding"] * invalid
        src = os.path.join(tmp, "disasm", "cs_t.s")
        with open(src, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines) + "\n")
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", ResourceWarning)
            return rdna2spirv.translate(src)


class FspxPrologueTest(unittest.TestCase):
    def test_two_group_prologue_is_skipped(self):
        # llvm-mc rejects the two 0xD580 words and decodes the other words of the 24-byte prologue as instructions
        a, d = struct.pack("<I", 0xD5800001), struct.pack("<I", 0xD5800002)
        parts = [a, JUNK, (b"\xdb\x21\x5f\x72", "v_max_f16_e32 v47, v219, v144"), d, JUNK, JUNK, MOV, END]
        self.assertTrue(translate(parts, 2))

    def test_prologue_swallowing_the_next_word_is_skipped(self):
        # the last literal and the s_inst_prefetch after it decode as one 8-byte instruction
        a, d = struct.pack("<I", 0xD5800001), struct.pack("<I", 0xD5800002)
        parts = [a, JUNK, (b"\xdb\x21\x5f\x72", "v_max_f16_e32 v47, v219, v144"), d, JUNK,
                 (b"\xe2\x8f\xfa\xf8\x03\x00\xa0\xbf", "exp param30, off, v3, off, off done compr"), MOV, END]
        self.assertTrue(translate(parts, 2))


class SethaltTest(unittest.TestCase):
    def test_sethalt_ends_the_wave(self):
        self.assertTrue(translate([MOV, HALT, END]))


class VopcDppTest(unittest.TestCase):
    def test_bound_ctrl_compare_translates(self):
        w = struct.pack("<2I", 0x7C000000 | 4 << 17 | 20 << 9 | 0xFA, 13 | 0xB1 << 8 | 1 << 19 | 0xFF << 24)
        self.assertTrue(translate([MOV, w, SEL, END], 1))

    def test_dpp8_compare_translates(self):
        w0 = 0x7C000000 | 4 << 17 | 20 << 9 | 0xE9
        w = struct.pack("<2I", w0, 13 | 0x688FAC << 8)
        self.assertTrue(translate([MOV, w, SEL, END], 1))

    def test_compare_without_bound_ctrl_is_refused(self):
        w = struct.pack("<2I", 0x7C000000 | 4 << 17 | 20 << 9 | 0xFA, 13 | 0xFF << 24 | 0xB1 << 8)
        with self.assertRaises(rdna2spirv.Unsupported):
            translate([MOV, w, SEL, END], 1)


if __name__ == "__main__":
    unittest.main()
