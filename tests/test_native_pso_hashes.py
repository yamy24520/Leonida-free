"""tools/native_pso_hashes.py on hand-built DXBC containers and indexes: no game data."""
import json
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import native_pso_hashes as tool  # noqa: E402


def container(digest: bytes, other=b"") -> bytes:
    """A DXBC container with a HASH part (flags 0, then the digest) and an optional second part."""
    parts = [b"HASH" + struct.pack("<I", 20) + struct.pack("<I", 0) + digest]
    if other:
        parts.insert(0, b"PRIV" + struct.pack("<I", len(other)) + other)
    head = 32 + 4 * len(parts)
    offsets, at = [], head
    for p in parts:
        offsets.append(at)
        at += len(p)
    body = b"DXBC" + bytes(16) + struct.pack("<II", 1, at) + struct.pack("<I", len(parts))
    return body + b"".join(struct.pack("<I", o) for o in offsets) + b"".join(parts)


class NativePsoHashesTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dxbc = os.path.join(self.tmp.name, "shaders", "dxbc")
        self.spirv = os.path.join(self.tmp.name, "shaders", "spirv")
        os.makedirs(self.dxbc)
        os.makedirs(self.spirv)

    def tearDown(self):
        self.tmp.cleanup()

    def put(self, name, data):
        with open(os.path.join(self.dxbc, name + ".dxbc"), "wb") as f:
            f.write(data)

    def index(self, entries):
        with open(os.path.join(self.spirv, "index.json"), "w", encoding="utf-8", newline="\n") as f:
            json.dump(entries, f)

    def read(self):
        with open(os.path.join(self.spirv, tool.OUT_NAME), encoding="utf-8") as f:
            return f.read().splitlines()

    def test_digest_of_a_container(self):
        d = bytes(range(16))
        self.assertEqual(tool.hash_digest(container(d)), d)
        self.assertEqual(tool.hash_digest(container(d, b"x" * 9)), d)
        self.assertIsNone(tool.hash_digest(b"DXBC" + bytes(28)))
        self.assertIsNone(tool.hash_digest(container(d)[:-1]))
        self.assertIsNone(tool.hash_digest(b"FSSM" + bytes(60)))

    def test_map_lines(self):
        a, b, c = bytes([1] * 16), bytes([2] * 16), bytes([3] * 16)
        self.put("vs one", container(a))
        self.put(".ms_dot", container(b))
        self.put("same_a", container(a, b"other"))   # another container with digest a and the same program
        self.put("untranslated", container(c))
        self.put("not_dxbc", b"nothing")
        self.index({"vs one": {"stage": "vs", "fssm_sha256": "f1"}, ".ms_dot": {"stage": "ms", "fssm_sha256": "f2"},
                    "same_a": {"stage": "vs", "fssm_sha256": "f1"}, "not_dxbc": {"stage": "ps", "fssm_sha256": "f3"}})
        self.assertEqual(tool.main(["x", self.dxbc, self.spirv]), 0)
        lines = self.read()
        self.assertEqual(lines[0], tool.HEADER)
        self.assertEqual(lines[1:], [f"{a.hex()}\tvs\t../dxbc/same_a.dxbc", f"{b.hex()}\tms\t../dxbc/.ms_dot.dxbc"])
        with open(os.path.join(self.spirv, tool.OUT_NAME), "rb") as f:
            self.assertNotIn(b"\r", f.read())

    def test_a_digest_with_two_programs_fails_and_leaves_no_map(self):
        a = bytes([7] * 16)
        self.put("one", container(a))
        self.put("two", container(a, b"x"))
        self.index({"one": {"stage": "ps", "fssm_sha256": "f1"}, "two": {"stage": "ps", "fssm_sha256": "f2"}})
        with open(os.path.join(self.spirv, tool.OUT_NAME), "w", encoding="utf-8", newline="\n") as f:
            f.write("stale\n")
        with self.assertRaises(tool.DigestClash):
            tool.build(self.dxbc, self.spirv)
        self.assertEqual(tool.main(["x", self.dxbc, self.spirv]), 1)
        self.assertFalse(os.path.exists(os.path.join(self.spirv, tool.OUT_NAME)))

    def test_no_index(self):
        self.assertEqual(tool.main(["x", self.dxbc, self.spirv]), 1)
        self.assertEqual(tool.main(["x"]), 2)


if __name__ == "__main__":
    unittest.main()
