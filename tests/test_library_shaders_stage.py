"""make_game.py's pipeline-library step (library_shaders): the compute pipelines of the library in shaders.rpf, which
the executable creates with Xbox device slot 67 (docs/shader_library.md, docs/fspx.md).

    python -I tests/test_library_shaders_stage.py

Synthetic files only, no game data: the tools are mocked the way tests/test_fspx_shaders_stage.py does, and each mock
writes what the real tool writes last.
"""
import json
import os
import sys
import tempfile
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
import make_game  # noqa: E402


class LibraryStage(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.work = os.path.join(self.tmp.name, "work")
        self.game = os.path.join(self.tmp.name, "game")
        os.makedirs(self.game)
        self.archive = os.path.join(self.game, make_game.LIBRARY_ARCHIVE)
        self.write_archive(b"synthetic archive")
        self.unpacked = os.path.join(self.work, "shaders", "library")
        self.spirv = os.path.join(self.work, "shaders", "spirv", "library")
        self.carve = os.path.join(self.work, "shaders", "dxbc")
        self.calls = []
        self.translations = {"lib_cs_00007": ("c" * 64, "s" * 64, b"synthetic module")}

    def write_archive(self, data):
        with open(self.archive, "wb") as f:
            f.write(data)

    def fake_py(self, tool, *args):
        self.calls.append((tool, args))
        if tool == "shader_library.py":
            os.makedirs(os.path.join(self.unpacked, "disasm"), exist_ok=True)
            open(os.path.join(self.unpacked, "disasm", "lib_cs_00007.s"), "w").close()
        elif tool == "rdna2spirv.py":
            os.makedirs(self.spirv, exist_ok=True)
            index = {}
            for name, (code, state, module) in self.translations.items():
                index[name] = {"stage": "cs", "fspx_code_sha256": code, "fspx_state_sha256": state}
                with open(os.path.join(self.spirv, name + ".spv"), "wb") as f:
                    f.write(module)
            with open(os.path.join(self.spirv, "index.json"), "w") as f:
                json.dump(index, f)

    def run_step(self, signature="llvm-mc 23", jobs=4, emulate=False, fallbacks=()):
        self.calls = []
        with mock.patch.object(make_game, "py", self.fake_py), mock.patch.object(make_game, "log", lambda *a: None):
            make_game.library_shaders(self.game, self.work, signature, jobs, emulate=emulate, fallbacks=fallbacks)
        return [tool for tool, _ in self.calls]

    def test_runs_once_then_nothing(self):
        os.makedirs(self.carve)
        self.assertEqual(self.run_step(), ["shader_library.py", "rdna2spirv.py"])
        (_, unpack), (_, translate) = self.calls
        self.assertEqual(unpack, (self.archive, "--out", self.unpacked, "--stages", "cs", "--disasm", "--jobs", "4",
                                  "--carve", self.carve))
        self.assertEqual(translate, (os.path.join(self.unpacked, "disasm"), "--out", self.spirv, "--no-asm"))
        self.assertTrue(os.path.isfile(os.path.join(self.spirv, "library_build.json")))
        self.assertEqual(self.run_step(), [])

    def test_without_a_carve_the_sources_are_not_named(self):
        self.run_step()
        (_, unpack), _ = self.calls
        self.assertNotIn("--carve", unpack)

    def test_no_archive_skips_the_step(self):
        os.unlink(self.archive)
        self.assertEqual(self.run_step(), [])
        self.assertFalse(os.path.exists(self.spirv))

    def test_inputs_that_change_the_output_translate_again(self):
        self.run_step()
        self.assertEqual(self.run_step(), [])
        self.assertEqual(self.run_step(signature="llvm-mc 24"), ["shader_library.py", "rdna2spirv.py"])
        self.assertEqual(self.run_step(signature="llvm-mc 24", emulate=True), ["shader_library.py", "rdna2spirv.py"])
        (_, translate) = self.calls[1]
        self.assertIn("--wave64-emulate", translate)
        self.assertEqual(self.run_step(signature="llvm-mc 24", emulate=True, fallbacks=("--bary-fallback",)),
                         ["shader_library.py", "rdna2spirv.py"])
        self.write_archive(b"another archive")
        self.assertEqual(self.run_step(signature="llvm-mc 24", emulate=True, fallbacks=("--bary-fallback",)),
                         ["shader_library.py", "rdna2spirv.py"])
        with mock.patch.object(make_game, "translation_sources_digest", return_value="another translator"):
            self.assertEqual(self.run_step(signature="llvm-mc 24", emulate=True, fallbacks=("--bary-fallback",)),
                             ["shader_library.py", "rdna2spirv.py"])

    def test_empty_index_is_translated_again_and_drops_stale_modules(self):
        self.run_step()
        stale = os.path.join(self.spirv, "lib_cs_00007.spv")
        self.assertTrue(os.path.isfile(stale))
        with open(os.path.join(self.spirv, "index.json"), "w", encoding="utf-8", newline="\n") as f:
            f.write("{}")
        self.translations = {}
        self.assertEqual(self.run_step(), ["shader_library.py", "rdna2spirv.py"])
        self.assertFalse(os.path.exists(stale))
        self.assertEqual(self.run_step(), ["shader_library.py", "rdna2spirv.py"])

    def test_key_clash_fails_and_leaves_no_index(self):
        self.translations = {"lib_cs_00001": ("c" * 64, "s" * 64, b"module one"),
                             "lib_cs_00002": ("c" * 64, "s" * 64, b"module two")}
        with self.assertRaises(SystemExit), mock.patch("sys.stderr"):
            self.run_step()
        self.assertFalse(os.path.exists(os.path.join(self.spirv, "index.json")))
        self.assertFalse(os.path.exists(os.path.join(self.spirv, "library_build.json")))


if __name__ == "__main__":
    unittest.main()
