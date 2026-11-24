"""A runtime configure without Vulkan headers must say so loudly, or fail when asked to.

Everything under ``if(TARGET Vulkan::Headers)`` in runtime/CMakeLists.txt (the D3D12 COM objects, the Vulkan D3D12
device in gta6rt and most of ctest) is left out without Vulkan headers. It used to be one STATUS line, so a build
whose VULKAN_SDK never reached CMake ran 45 of 125 tests and reported "100% tests passed".

Runs runtime/cmake/vulkan_headers_missing.cmake with ``cmake -P`` (no compiler needed); skipped without cmake.
"""
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MODULE = ROOT / "runtime" / "cmake" / "vulkan_headers_missing.cmake"
CMAKE_LISTS = ROOT / "runtime" / "CMakeLists.txt"
CMAKE = shutil.which("cmake")


def run_module(require):
    with tempfile.TemporaryDirectory() as tmp:
        script = Path(tmp) / "driver.cmake"
        script.write_text(f'include("{MODULE.as_posix()}")\nleonida_vulkan_headers_missing()\n', encoding="utf-8")
        args = [CMAKE]
        if require is not None:
            args.append(f"-DLEONIDA_REQUIRE_VULKAN={require}")
        args += ["-P", str(script)]
        return subprocess.run(args, capture_output=True, text=True, timeout=60)


@unittest.skipUnless(CMAKE, "cmake not on PATH")
class VulkanMissingMessageTest(unittest.TestCase):
    def test_default_warns_and_names_what_is_left_out(self):
        r = run_module(None)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("CMake Warning", r.stderr)
        text = " ".join(r.stderr.split())
        for part in ("Vulkan D3D12 device in gta6rt", "LEONIDA_D3D12=vulkan", "D3D12 layer and GPU tests",
                     "CMakeCache.txt", "LEONIDA_VULKAN_HEADERS_FETCH=ON", "LEONIDA_REQUIRE_VULKAN=ON"):
            self.assertIn(part, text)

    def test_off_still_warns(self):
        r = run_module("OFF")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("CMake Warning", r.stderr)

    def test_require_fails_the_configure(self):
        r = run_module("ON")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("CMake Error", r.stderr)
        self.assertIn("Vulkan headers not found", r.stderr)


class CMakeListsUsesTheModuleTest(unittest.TestCase):
    def test_no_headers_branch_calls_the_module(self):
        text = CMAKE_LISTS.read_text(encoding="utf-8")
        self.assertIn('option(LEONIDA_REQUIRE_VULKAN ', text)
        # The else() of if(TARGET Vulkan::Headers) is the last top-level else before its endif.
        m = re.search(r"^if\(TARGET Vulkan::Headers\)\n.*?^else\(\)\n(.*?)^endif\(\)", text, re.S | re.M)
        self.assertIsNotNone(m, "if(TARGET Vulkan::Headers) ... else() ... endif() not found at top level")
        self.assertIn("leonida_vulkan_headers_missing()", m.group(1))
        self.assertNotIn("message(STATUS", m.group(1))

    def test_ci_requires_vulkan(self):
        ci = (ROOT / ".github" / "workflows" / "runtime.yml").read_text(encoding="utf-8")
        configures = re.findall(r"cmake -S runtime -B runtime/build.*?(?=\n\s*- name:)", ci, re.S)
        self.assertGreaterEqual(len(configures), 2)
        for c in configures:
            self.assertIn("-DLEONIDA_REQUIRE_VULKAN=ON", c)


TOOLCHAIN = ROOT / "runtime" / "cmake" / "clang-cl-xwin.cmake"


@unittest.skipUnless(CMAKE, "cmake not on PATH")
@unittest.skipIf(os.name == "nt", "clang-cl-xwin.cmake symlinks host Vulkan headers on POSIX")
class ClangClXwinVulkanTest(unittest.TestCase):
    def run_toolchain(self, tmp, *, vulkan_sdk="", prelude=""):
        xwin = tmp / "xwin"
        (xwin / "VC" / "Tools" / "MSVC").mkdir(parents=True, exist_ok=True)
        (xwin / "Windows Kits" / "10" / "Lib" / "10.0.26100" / "um" / "x64").mkdir(parents=True, exist_ok=True)
        build = tmp / "build"
        build.mkdir(exist_ok=True)
        script = tmp / "driver.cmake"
        script.write_text(
            f'set(CMAKE_BINARY_DIR "{build.as_posix()}")\n'
            f'{prelude}\n'
            f'include("{TOOLCHAIN.as_posix()}")\n'
            'message(STATUS "RESULT_VK=${Vulkan_INCLUDE_DIR}")\n',
            encoding="utf-8",
        )
        env = {k: v for k, v in os.environ.items() if k not in ("VULKAN_SDK", "XWIN_DIR")}
        env["XWIN_DIR"] = str(xwin)
        if vulkan_sdk:
            env["VULKAN_SDK"] = str(vulkan_sdk)
        r = subprocess.run([CMAKE, "-P", str(script)], capture_output=True, text=True, env=env, timeout=60)
        self.assertEqual(r.returncode, 0, r.stderr)
        m = re.search(r"RESULT_VK=(.*)", r.stdout)
        self.assertIsNotNone(m, r.stdout)
        return build, m.group(1).strip()

    def test_vulkan_sdk_is_linked_without_exposing_sibling_headers(self):
        with tempfile.TemporaryDirectory() as d:
            tmp = Path(d)
            sdk = tmp / "vksdk"
            (sdk / "include" / "vulkan").mkdir(parents=True)
            (sdk / "include" / "vk_video").mkdir(parents=True)
            (sdk / "include" / "vulkan" / "vulkan.h").write_text("// synth\n", encoding="utf-8")
            (sdk / "include" / "vk_video" / "video.h").write_text("// synth\n", encoding="utf-8")
            (sdk / "include" / "stdio.h").write_text("// host libc\n", encoding="utf-8")
            build, result = self.run_toolchain(tmp, vulkan_sdk=sdk)
            expected = build / "xwin-vulkan-include"
            self.assertEqual(result, expected.as_posix())
            self.assertEqual(os.readlink(expected / "vulkan"), (sdk / "include" / "vulkan").as_posix())
            self.assertEqual(os.readlink(expected / "vk_video"), (sdk / "include" / "vk_video").as_posix())
            self.assertTrue((expected / "vulkan" / "vulkan.h").is_file())
            self.assertFalse((expected / "stdio.h").exists())

    def test_replaces_stale_notfound_cache_entry(self):
        with tempfile.TemporaryDirectory() as d:
            tmp = Path(d)
            sdk = tmp / "vksdk"
            (sdk / "Include" / "vulkan").mkdir(parents=True)
            (sdk / "Include" / "vulkan" / "vulkan.h").write_text("// synth\n", encoding="utf-8")
            build, result = self.run_toolchain(
                tmp,
                vulkan_sdk=sdk,
                prelude='set(Vulkan_INCLUDE_DIR "Vulkan_INCLUDE_DIR-NOTFOUND" CACHE PATH "")',
            )
            self.assertEqual(result, (build / "xwin-vulkan-include").as_posix())

    def test_preserves_explicit_vulkan_include_dir(self):
        with tempfile.TemporaryDirectory() as d:
            tmp = Path(d)
            sdk = tmp / "vksdk"
            (sdk / "include" / "vulkan").mkdir(parents=True)
            (sdk / "include" / "vulkan" / "vulkan.h").write_text("// synth\n", encoding="utf-8")
            custom = tmp / "custom-headers"
            (custom / "vulkan").mkdir(parents=True)
            (custom / "vulkan" / "vulkan.h").write_text("// synth\n", encoding="utf-8")
            _, result = self.run_toolchain(
                tmp,
                vulkan_sdk=sdk,
                prelude=f'set(Vulkan_INCLUDE_DIR "{custom.as_posix()}" CACHE PATH "")',
            )
            self.assertEqual(result, custom.as_posix())

    def test_replaces_stale_custom_vulkan_include_dir(self):
        with tempfile.TemporaryDirectory() as d:
            tmp = Path(d)
            sdk = tmp / "vksdk"
            (sdk / "include" / "vulkan").mkdir(parents=True)
            (sdk / "include" / "vulkan" / "vulkan.h").write_text("// synth\n", encoding="utf-8")
            stale = (tmp / "moved-headers").as_posix()
            build, result = self.run_toolchain(
                tmp,
                vulkan_sdk=sdk,
                prelude=f'set(Vulkan_INCLUDE_DIR "{stale}" CACHE PATH "")',
            )
            self.assertEqual(result, (build / "xwin-vulkan-include").as_posix())


if __name__ == "__main__":
    unittest.main()
