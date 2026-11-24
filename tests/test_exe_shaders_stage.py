"""make_game.py's executable shader step (exe_shaders) and the index key the runtime finds its programs by (fssm_sha256).

    python -I tests/test_exe_shaders_stage.py

Zero game data: the "executable" is synthetic bytes around hand-built DXBC containers whose PSV0 chunk names a
graphics stage (VS/PS) and whose XBCH record is a zlib stream of a made-up FSSM blob; llvm-mc and the translator are
stood in for the way tests/test_bvh_shaders_stage.py does. The extracting, unpacking and disassembling tools are the
repository's own, run as make_game.py runs them.
"""
import hashlib
import io
import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tests"))
import make_game  # noqa: E402
import rdna2spirv  # noqa: E402
import shader_meta  # noqa: E402
from test_pe_shader_containers import container, xdat  # noqa: E402

TOOLS = ["pe_shader_containers.py", "fssm_unpack.py", "shader_disasm.py", "rdna2spirv.py"]
ENDPGM = b"\x00\x00\x81\xbf"
PSV0_STAGE_CODE = {"ps": 0, "vs": 1}


def fssm_blob(seed: int) -> bytes:
    """A made-up FSSM blob shader_disasm.py accepts: the code section offset at 0xC0, code 0x28 bytes into it, a
    few filler words that differ per seed and the closing s_endpgm."""
    head = bytearray(b"FSSM" + b"\x01\x01" + bytes(0xF0 - 6))
    struct.pack_into("<I", head, 0xC0, 0xC8)
    return bytes(head) + b"".join(struct.pack("<I", 0x7E000280 + seed + k) for k in range(3)) + ENDPGM


def psv0(stage: str) -> bytes:
    """A minimal PSV0 chunk whose ShaderStage byte at payload offset 4 + 24 names `stage`."""
    payload = bytearray(struct.pack("<I", 28) + bytes(28))
    payload[4 + 24] = PSV0_STAGE_CODE[stage]
    return bytes(payload)


def exe_container(stage: str, blob: bytes) -> bytes:
    """DXBC { SFI0, PSV0 = stage, DXIL = program header + XDAT { 0x12, 0x0D = XBCH + zlib(blob), 0x07 = bitcode } }:
    the parts of game_scarlett_master.exe's embedded containers the tools read (no ILDN chunk; docs/embedded_shaders.md
    section 2)."""
    packed = zlib.compress(blob)
    xbch = b"XBCH" + struct.pack("<IIIII", 1, len(packed), 0, len(blob), 0) + packed
    prefix = b"e\x00\x05\x00" + struct.pack("<I", 24) + b"DXIL" + b"\x05\x01\x00\x00" + struct.pack("<II", 0x70, 0x668)
    dxil = prefix + xdat([(0x12, bytes(16)), (0x0D, xbch), (0x07, b"BC\xc0\xde" + bytes(8))], len(prefix))
    return container([(b"SFI0", bytes(8)), (b"PSV0", psv0(stage)), (b"DXIL", dxil)])


def exe_like(*containers: bytes) -> bytes:
    """Containers between unrelated bytes, as the executable holds them."""
    return b"MZ" + b"\0" * 126 + b"".join(c + b"\xcc" * 12 for c in containers)


def tool(name: str, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", name), *args],
                          capture_output=True, text=True)


class IndexKey(unittest.TestCase):
    """Extracted PSV0 containers without ILDN unpack to FSSM and give rdna2spirv the stage (vs/ps) and fssm_sha256."""

    def test_the_extracted_layout_gives_the_translator_the_key_and_graphics_stages(self):
        programs = [("container_00", "vs", fssm_blob(1)), ("container_01", "ps", fssm_blob(2))]
        with tempfile.TemporaryDirectory() as t:
            exe = os.path.join(t, make_game.EXE_FILE)
            with open(exe, "wb") as f:
                f.write(exe_like(*(exe_container(stage, blob) for _, stage, blob in programs)))
            out = os.path.join(t, "exe")
            r = tool("pe_shader_containers.py", exe, "--extract", out)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = tool("fssm_unpack.py", out, "--carve-index", os.path.join(out, "index.json"))
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            os.makedirs(os.path.join(out, "disasm"))
            rdna2spirv._STAGES = None
            self.addCleanup(setattr, rdna2spirv, "_STAGES", None)
            for name, stage, blob in programs:
                with open(os.path.join(out, "fssm", name + ".fssm"), "rb") as f:
                    self.assertEqual(f.read(), blob)
                listing = os.path.join(out, "disasm", name + ".s")
                meta = shader_meta.collect(listing, name, [[]], [[]])
                self.assertEqual(meta.get("fssm_sha256"), hashlib.sha256(blob).hexdigest())
                self.assertNotIn("fspx_code_sha256", meta)
                self.assertEqual(rdna2spirv.stage_of(name, listing), stage)


class ExeStage(unittest.TestCase):
    """Which tools exe_shaders() runs and what it reports. Each stand-in writes what the real tool writes last."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.work = os.path.join(self.tmp.name, "work")
        self.game = os.path.join(self.tmp.name, "game")
        os.makedirs(self.game)
        self.exe = os.path.join(self.game, make_game.EXE_FILE)
        self.programs = {
            "container_00": ("vs", fssm_blob(1)),
            "container_01": ("ps", fssm_blob(2)),
            "container_02": ("ps", fssm_blob(3)),
        }
        self.untranslated = set()
        self.write_exe()
        self.unpacked = os.path.join(self.work, "shaders", "exe")
        self.spirv = os.path.join(self.work, "shaders", "spirv", "exe")
        self.calls, self.logs = [], []

    def write_exe(self):
        with open(self.exe, "wb") as f:
            f.write(exe_like(*(exe_container(stage, blob) for stage, blob in self.programs.values())))

    def fake_translate(self, *args):
        out = args[args.index("--out") + 1]
        os.makedirs(out, exist_ok=True)
        index = {}
        for listing in sorted(os.listdir(args[0])):
            name = listing[:-2]
            if not listing.endswith(".s") or name in self.untranslated:
                continue
            with open(os.path.join(os.path.dirname(args[0]), "fssm", name + ".fssm"), "rb") as f:
                stage = self.programs[name][0]
                index[name] = {"stage": stage, "fssm_sha256": hashlib.sha256(f.read()).hexdigest()}
            with open(os.path.join(out, name + ".spv"), "wb") as f:
                f.write(b"module of " + name.encode())
        with open(os.path.join(out, "index.json"), "w", encoding="utf-8", newline="\n") as f:
            json.dump(index, f)

    def fake_py(self, tool_name, *args):
        self.calls.append((tool_name, args))
        if tool_name == "pe_shader_containers.py":
            os.makedirs(self.unpacked, exist_ok=True)
            for name in self.programs:
                open(os.path.join(self.unpacked, name + ".dxbc"), "wb").close()
            with open(os.path.join(self.unpacked, "index.json"), "w", encoding="utf-8", newline="\n") as f:
                json.dump([{"name": n, "file": n + ".dxbc", "stage": st} for n, (st, _) in self.programs.items()], f)
        elif tool_name == "fssm_unpack.py":
            os.makedirs(os.path.join(self.unpacked, "fssm"), exist_ok=True)
            for name, (_, blob) in self.programs.items():
                with open(os.path.join(self.unpacked, "fssm", name + ".fssm"), "wb") as f:
                    f.write(blob)
            with open(os.path.join(self.unpacked, "fssm_index.json"), "w", encoding="utf-8", newline="\n") as f:
                json.dump([{"name": n} for n in self.programs], f)
        elif tool_name == "shader_disasm.py":
            os.makedirs(os.path.join(self.unpacked, "disasm"), exist_ok=True)
            for name in self.programs:
                with open(os.path.join(self.unpacked, "disasm", name + ".s"), "w", encoding="utf-8",
                          newline="\n") as f:
                    f.write("s_endpgm\n")
        elif tool_name == "rdna2spirv.py":
            self.fake_translate(*args)

    def run_step(self, signature="llvm-mc 23", emulate=False, fallbacks=(), py=None):
        self.calls, self.logs = [], []
        with mock.patch.object(make_game, "py", py or self.fake_py), \
                mock.patch.object(make_game, "log", self.logs.append):
            make_game.exe_shaders(self.game, self.work, signature, 3, emulate=emulate, fallbacks=fallbacks)
        return [name for name, _ in self.calls]

    def test_runs_once_then_only_reports(self):
        self.assertEqual(self.run_step(), TOOLS)
        (_, extract), (_, unpack), (_, disasm), (_, translate) = self.calls
        self.assertEqual(extract, (self.exe, "--extract", self.unpacked))
        self.assertEqual(unpack, (self.unpacked, "--carve-index", os.path.join(self.unpacked, "index.json")))
        self.assertEqual(disasm, (os.path.join(self.unpacked, "fssm"), "--unpack-index",
                                  os.path.join(self.unpacked, "fssm_index.json"), "--out",
                                  os.path.join(self.unpacked, "disasm"), "--mcpu", "gfx1013", "--jobs", 3))
        # Graphics shaders: no --max-shared-memory compute LDS cap.
        self.assertEqual(translate, (os.path.join(self.unpacked, "disasm"), "--out", self.spirv, "--no-asm"))
        self.assertTrue(os.path.isfile(os.path.join(self.spirv, "exe_build.json")))
        self.assertIn(f"shaders: 3 of 3 {make_game.EXE_FILE} programs translated", self.logs)
        self.assertEqual(self.run_step(), [])
        self.assertEqual(self.logs, [f"shaders: 3 of 3 {make_game.EXE_FILE} programs translated"])

    def test_the_real_tools_fill_the_folder_and_a_second_run_leaves_no_duplicates(self):
        mc = os.path.join(self.tmp.name, "fake_mc.py")
        with open(mc, "w", encoding="utf-8", newline="\n") as f:
            f.write("print('\\ts_endpgm')\n")

        def run_tool(tool_name, *args):
            self.calls.append((tool_name, args))
            if tool_name == "rdna2spirv.py":
                return self.fake_translate(*args)
            make_game.run([sys.executable, "-I", os.path.join(make_game.TOOLS, tool_name), *args],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        with mock.patch.dict(os.environ, {"LLVM_MC": f'"{sys.executable}" "{mc}"'}):
            for signature in ("llvm-mc 23", "llvm-mc 24"):
                self.assertEqual(self.run_step(signature=signature, py=run_tool), TOOLS)
                names = sorted(self.programs)
                self.assertEqual(sorted(f for f in os.listdir(self.unpacked) if f.endswith(".dxbc")),
                                 [n + ".dxbc" for n in names])
                self.assertEqual(sorted(os.listdir(os.path.join(self.unpacked, "fssm"))),
                                 [n + ".fssm" for n in names])
                self.assertEqual(sorted(os.listdir(os.path.join(self.unpacked, "disasm"))),
                                 [n + ".s" for n in names])
                with open(os.path.join(self.spirv, "index.json"), encoding="utf-8") as f:
                    index = json.load(f)
                self.assertEqual({n: (index[n]["stage"], index[n]["fssm_sha256"]) for n in index},
                                 {n: (st, hashlib.sha256(blob).hexdigest()) for n, (st, blob) in self.programs.items()})
                self.assertIn(f"shaders: 3 of 3 {make_game.EXE_FILE} programs translated", self.logs)
            self.assertEqual(self.run_step(signature="llvm-mc 24", py=run_tool), [])

    def test_no_exe_no_step(self):
        os.remove(self.exe)
        self.assertEqual(self.run_step(), [])
        self.assertFalse(os.path.exists(self.spirv))
        self.assertFalse(os.path.exists(self.unpacked))

    def test_an_exe_that_is_gone_takes_the_old_index_with_it(self):
        self.assertEqual(self.run_step(), TOOLS)
        index, settings = os.path.join(self.spirv, "index.json"), os.path.join(self.spirv, "exe_build.json")
        self.assertTrue(os.path.isfile(index) and os.path.isfile(settings))
        os.remove(self.exe)
        self.assertEqual(self.run_step(), [])
        self.assertFalse(os.path.exists(index))
        self.assertFalse(os.path.exists(settings))
        self.assertEqual(self.logs, [f"shaders: no {make_game.EXE_FILE} in the game folder, so no executable "
                                     "shader programs to translate"])
        self.assertEqual(self.run_step(), [])
        self.write_exe()
        self.assertEqual(self.run_step(), TOOLS)
        self.assertTrue(os.path.isfile(index) and os.path.isfile(settings))

    def test_each_input_change_runs_it_again(self):
        self.run_step()
        self.assertEqual(self.run_step(signature="llvm-mc 24"), TOOLS)
        self.assertEqual(self.run_step(signature="llvm-mc 24", emulate=True), TOOLS)
        self.assertIn("--wave64-emulate", self.calls[3][1])
        flags = ("--no-float-controls",)
        self.assertEqual(self.run_step(signature="llvm-mc 24", emulate=True, fallbacks=flags), TOOLS)
        self.assertIn("--no-float-controls", self.calls[3][1])
        self.programs["container_02"] = ("ps", fssm_blob(4))
        self.write_exe()
        self.assertEqual(self.run_step(signature="llvm-mc 24", emulate=True, fallbacks=flags), TOOLS)
        self.assertEqual(self.run_step(signature="llvm-mc 24", emulate=True, fallbacks=flags), [])
        with mock.patch.object(make_game, "translation_sources_digest", return_value="another translator"):
            self.assertEqual(self.run_step(signature="llvm-mc 24", emulate=True, fallbacks=flags), TOOLS)

    def test_an_untranslated_program_is_named_on_every_run_and_the_step_still_finishes(self):
        self.untranslated = {"container_01"}
        for expected in (TOOLS, [], []):
            self.assertEqual(self.run_step(), expected)
            self.assertIn(f"shaders: 2 of 3 {make_game.EXE_FILE} programs translated", self.logs)
            named = [line for line in self.logs if "NOT translated: container_01." in line]
            self.assertEqual(len(named), 1, self.logs)
            self.assertNotIn("container_00", named[0])
        with open(os.path.join(self.spirv, "index.json"), encoding="utf-8") as f:
            self.assertEqual(sorted(json.load(f)), ["container_00", "container_02"])
        self.assertFalse(os.path.exists(os.path.join(self.spirv, "container_01.spv")))

    def test_nothing_translated_fails_and_keeps_no_index(self):
        self.untranslated = set(self.programs)
        with self.assertRaises(SystemExit), mock.patch("sys.stderr", new_callable=io.StringIO) as err:
            self.run_step()
        self.assertIn(f"none of the {make_game.EXE_FILE} programs translated", err.getvalue())
        self.assertFalse(os.path.exists(os.path.join(self.spirv, "index.json")))
        self.assertFalse(os.path.exists(os.path.join(self.spirv, "exe_build.json")))
        self.untranslated = set()
        self.assertEqual(self.run_step(), TOOLS)

    def test_missing_cached_module_or_index_is_rebuilt(self):
        self.assertEqual(self.run_step(), TOOLS)
        module = os.path.join(self.spirv, "container_00.spv")
        os.remove(module)
        self.assertEqual(self.run_step(), TOOLS)
        with open(module, "rb") as f:
            self.assertEqual(f.read(), b"module of container_00")
        self.assertEqual(self.run_step(), [])
        os.remove(os.path.join(self.unpacked, "index.json"))
        self.assertEqual(self.run_step(), TOOLS)
        self.assertEqual(self.run_step(), [])

    def test_empty_cached_index_is_rebuilt(self):
        """A cached index.json with no usable entries ({} or all error entries) must not be reused as finished."""
        self.assertEqual(self.run_step(), TOOLS)
        for bad_index in ("{}", '{"container_00": {"error": "ValueError: synthetic"}}'):
            with open(os.path.join(self.spirv, "index.json"), "w", encoding="utf-8", newline="\n") as f:
                f.write(bad_index)
            self.assertEqual(self.run_step(), TOOLS)
            self.assertIn(f"shaders: 3 of 3 {make_game.EXE_FILE} programs translated", self.logs)
            self.assertEqual(self.run_step(), [])
            self.assertEqual(self.logs, [f"shaders: 3 of 3 {make_game.EXE_FILE} programs translated"])


class ShadersStageWithTheExe(unittest.TestCase):
    """The whole shaders() stage with both DLLs and the executable: exe_shaders() runs last and runs alone when only
    the executable changes."""

    def test_exe_runs_after_bvh_and_alone_when_only_the_exe_changes(self):
        from test_bvh_shaders_stage import builder_container, dll_like as bvh_dll
        from test_fspx_shaders_stage import container as fspx_container, dll_like as fspx_dll, program
        with tempfile.TemporaryDirectory() as t:
            work, game = os.path.join(t, "work"), os.path.join(t, "game")
            os.makedirs(game)
            with open(os.path.join(game, make_game.FSPX_DLL), "wb") as f:
                f.write(fspx_dll(fspx_container(program())))
            with open(os.path.join(game, make_game.BVH_DLL), "wb") as f:
                f.write(bvh_dll(builder_container("SortKeys", fssm_blob(1))))
            exe = os.path.join(game, make_game.EXE_FILE)
            with open(exe, "wb") as f:
                f.write(exe_like(exe_container("vs", fssm_blob(2))))
            ran = []
            dxbc = os.path.join(work, "shaders", "dxbc")
            bvh = os.path.join(work, "shaders", "bvh")
            exe_dir = os.path.join(work, "shaders", "exe")

            def write(text, *parts):
                os.makedirs(os.path.dirname(os.path.join(*parts)), exist_ok=True)
                with open(os.path.join(*parts), "w", encoding="utf-8", newline="\n") as f:
                    f.write(text)

            def fake_py(tool_name, *args):
                ran.append(tool_name)
                if tool_name == "shader_carve.py":
                    write('[{"file": "shader.dxbc"}]', dxbc, "index.json")
                elif tool_name == "pe_shader_containers.py":
                    if args[2] == exe_dir:
                        write('[{"name": "container_00", "file": "container_00.dxbc", "stage": "vs"}]',
                              exe_dir, "index.json")
                    else:
                        write('[{"name": "SortKeys", "file": "SortKeys.dxbc", "stage": "cs"}]', bvh, "index.json")
                elif tool_name == "fssm_unpack.py":
                    if args[0] == exe_dir:
                        write('[{"name": "container_00"}]', exe_dir, "fssm_index.json")
                    elif args[0] == bvh:
                        write('[{"name": "SortKeys"}]', bvh, "fssm_index.json")
                    else:
                        write('[{"name": "shader"}]', dxbc, "fssm_index.json")
                elif tool_name == "shader_disasm.py":
                    if args[0].startswith(exe_dir):
                        write("s_endpgm\n", exe_dir, "disasm", "container_00.s")
                    elif args[0].startswith(bvh):
                        write("s_endpgm\n", bvh, "disasm", "SortKeys.s")
                    else:
                        write("s_endpgm\n", dxbc, "disasm", "shader.s")
                elif tool_name == "rdna2spirv.py":
                    out = args[args.index("--out") + 1]
                    if args[0].startswith(exe_dir):
                        write('{"container_00": {"stage": "vs", "fssm_sha256": "%s"}}' % ("b" * 64), out, "index.json")
                        write("", out, "container_00.spv")
                    elif args[0].startswith(bvh):
                        write('{"SortKeys": {"stage": "cs", "fssm_sha256": "%s"}}' % ("a" * 64), out, "index.json")
                        write("", out, "SortKeys.spv")
                    else:
                        write('{"Upscale": {"stage": "cs"}}', out, "index.json")
                        write("", out, "Upscale.spv")

            def fake_translate(disasm, spirv, jobs, emulate=False, fallbacks=()):
                ran.append("translate")
                write("", spirv, "index.json")

            def stage():
                ran.clear()
                with mock.patch.object(make_game, "py", fake_py), \
                        mock.patch.object(make_game, "need", lambda *a: None), \
                        mock.patch.object(make_game, "translate", fake_translate), \
                        mock.patch.object(make_game, "llvm_mc_signature", return_value="llvm-mc 23"), \
                        mock.patch.object(make_game, "log", lambda *a: None):
                    make_game.shaders(game, work, 2)
                return list(ran)

            self.assertEqual(stage(), ["shader_carve.py", "fssm_unpack.py", "shader_disasm.py", "translate",
                                       "fspx_unpack.py", "rdna2spirv.py", *TOOLS, *TOOLS])
            self.assertEqual(stage(), [])
            with open(exe, "wb") as f:
                f.write(exe_like(exe_container("vs", fssm_blob(3))))
            self.assertEqual(stage(), TOOLS)
            for sub in ((), ("fspx",), ("bvh",), ("exe",)):
                self.assertTrue(os.path.isfile(os.path.join(work, "shaders", "spirv", *sub, "index.json")))


if __name__ == "__main__":
    unittest.main()
