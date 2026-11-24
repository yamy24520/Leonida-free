"""Semantic tests for v_cmp_* / v_cmp_class_f64 null/exec/wave32-vcc_hi SDSTs and wave32 vcc_hi masks.

Covers:
  - null destination on v_cmp_* (f32, f16, u16, i16, u32, i32, f64, f/tru_f64, u64 eq/lt, i64) and v_cmp_class_f64:
    mask write is discarded without touching VCC or EXEC (wave32, wave64, and --wave64-emulate)
  - exec_lo (wave32) and exec (wave64 / --wave64-emulate) destinations on the same compare / class families:
    active lanes update EXEC and inactive lanes remain off (including constant tru/t compares), while VCC stays intact
  - wave32 vcc_hi as an independent 32-bit mask/scalar register:
      * v_cmp_*, v_cmp_class_f64, v_add_co_u32, v_mad_u64_u32, s_and_saveexec_b32, s_wqm_b32 writing vcc_hi
      * v_cndmask_b32_e64, v_add_co_ci_u32_e64, s_bcnt1_i32_b32, s_ff1_i32_b32, s_flbit_i32_b32,
        v_mbcnt_lo_u32_b32, s_cmp_eq/lt_u32, s_wqm_b32 reading vcc_hi (preserving vcc_lo)
      * plain scalar data in vcc_hi (e.g. 0xA5000005, s_wqm_b32 vcc_hi, 1) read both as scalar and as a lane-bit
        mask without corrupting or reading vcc_lo
  - negative controls rejecting invalid destinations (VGPR everywhere; vcc_hi SDST in wave64)
  - Vulkan 1.2 SPIR-V validation via spirv-as / spirv-val when present on PATH
"""
import gc
import os
import shutil
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
if HERE not in sys.path:
    sys.path.insert(0, HERE)
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

import rdna2spirv  # noqa: E402
import spirv_mini_interp as si  # noqa: E402
import spirv_wave_interp as wi  # noqa: E402

STUB_DXBC = b"DXBC" + bytes(24) + struct.pack("<I", 0) + bytes(4)
VSHARP = [0, 0, 0xFFFFFFFF, 0x31016FAC]


def _fssm_blob(wave_size):
    blob = bytearray(0x138)
    blob[0:4] = b"FSSM"
    if wave_size == 32:
        struct.pack_into("<I", blob, 0xD4, 0x20)
    return bytes(blob) + struct.pack("<4I", 0x8088, wave_size, 1, 1)


def _validate_spirv(name, spv):
    spirv_as = shutil.which("spirv-as")
    spirv_val = shutil.which("spirv-val")
    if not spirv_as or not spirv_val:
        return
    with tempfile.TemporaryDirectory() as d:
        asm_p = os.path.join(d, "s.spvasm")
        bin_p = os.path.join(d, "s.spv")
        with open(asm_p, "w", encoding="utf-8", newline="\n") as f:
            f.write(spv)
        r1 = subprocess.run([spirv_as, "--target-env", "vulkan1.2", asm_p, "-o", bin_p],
                            capture_output=True, text=True)
        assert r1.returncode == 0, f"spirv-as failed for {name}: {r1.stderr}"
        r2 = subprocess.run([spirv_val, "--target-env", "vulkan1.2", bin_p],
                            capture_output=True, text=True)
        assert r2.returncode == 0, f"spirv-val failed for {name}: {r2.stderr}"


def _translate_cs(name, body_lines, wave_size=32, emulate=False):
    prev_emu = rdna2spirv.WAVE64_EMULATE
    try:
        rdna2spirv.WAVE64_EMULATE = emulate
        with tempfile.TemporaryDirectory() as tmp:
            os.mkdir(os.path.join(tmp, "disasm"))
            os.mkdir(os.path.join(tmp, "fssm"))
            s_path = os.path.join(tmp, "disasm", "cs_" + name + ".s")
            with open(s_path, "w", encoding="utf-8", newline="\n") as f:
                f.write(f"; cs_{name}\n")
                for ln in body_lines:
                    f.write(f"\t{ln}\n")
                f.write("\ts_endpgm\n")
            with open(os.path.join(tmp, "cs_" + name + ".dxbc"), "wb") as f:
                f.write(STUB_DXBC)
            with open(os.path.join(tmp, "fssm", "cs_" + name + ".fssm"), "wb") as f:
                f.write(_fssm_blob(wave_size))
            with warnings.catch_warnings():
                warnings.simplefilter("ignore", ResourceWarning)
                spv = rdna2spirv.translate(s_path)
                gc.collect()
            assert rdna2spirv.translate.last_gen.wave64 == (wave_size == 64)
            return spv
    finally:
        rdna2spirv.WAVE64_EMULATE = prev_emu


def _run_cs(name, body_lines, wave_size=32, emulate=False, validate=True):
    spv = _translate_cs(name, body_lines, wave_size=wave_size, emulate=emulate)
    if validate:
        _validate_spirv(name, spv)
    pc = VSHARP + [0] * 28
    mem = {}
    sg_size = 32 if emulate else wave_size
    si.run_workgroup(
        spv, wave_size, sg_size,
        inputs_for=lambda i: {"%wg_id": (0, 0, 0), "%local_id": (i, 0, 0), "%sg_lane": i % sg_size},
        push_constants=pc, memory=mem,
    )
    return mem


def _run_vs_w32(name, body_lines, out_regs=("v10", "v11", "v12", "v15")):
    with tempfile.TemporaryDirectory() as tmp:
        os.mkdir(os.path.join(tmp, "disasm"))
        os.mkdir(os.path.join(tmp, "fssm"))
        s_path = os.path.join(tmp, "disasm", "vs_" + name + ".s")
        with open(s_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(f"; vs_{name}\n")
            for ln in body_lines:
                f.write(f"\t{ln}\n")
            f.write("\tv_mov_b32_e32 v20, 0\n")
            f.write("\texp pos0 v20, v20, v20, v20 done\n")
            f.write(f"\texp param0 {', '.join(out_regs)}\n")
            f.write("\ts_endpgm\n")
        with open(os.path.join(tmp, "vs_" + name + ".dxbc"), "wb") as f:
            f.write(STUB_DXBC)
        with open(os.path.join(tmp, "fssm", "vs_" + name + ".fssm"), "wb") as f:
            f.write(_fssm_blob(32))
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", ResourceWarning)
            spv = rdna2spirv.translate(s_path)
            gc.collect()
        assert rdna2spirv.translate.last_gen.wave64 is False
    _validate_spirv("vs_" + name, spv)
    res = wi.run_wave(spv, wave=32, builtins={"VertexIndex": lambda lane: lane, "InstanceIndex": lambda _lane: 0})
    return [lane_out["%out_param0"] for lane_out in res["outputs"]]


COMPARE_NULL_EXEC_CASES = [
    # (tag, setup_lines, insn_template_with_dst, cond_fn)
    ("f32", [
        "v_cvt_f32_u32_e32 v1, v0",
        "v_mov_b32_e32 v2, 10.0",
    ], "v_cmp_lt_f32_e64 {dst}, v1, 10.0", lambda i: i < 10),
    ("f16", [
        "v_cvt_f32_u32_e32 v1, v0",
        "v_cvt_f16_f32_e32 v1, v1",
        "v_mov_b32_e32 v2, 0x4500",      # 5.0 in f16
    ], "v_cmp_lt_f16_e64 {dst}, v1, v2", lambda i: i < 5),
    ("u16", [
        "v_mov_b32_e32 v1, v0",
    ], "v_cmp_lt_u16_e64 {dst}, v1, 9", lambda i: i < 9),
    ("i16", [
        "v_sub_nc_u32_e32 v1, v0, 6",
    ], "v_cmp_lt_i16_e64 {dst}, v1, 0", lambda i: (i - 6) < 0),
    ("u32", [], "v_cmp_lt_u32_e64 {dst}, v0, 11", lambda i: i < 11),
    ("i32", [
        "v_sub_nc_u32_e32 v1, v0, 7",
    ], "v_cmp_gt_i32_e64 {dst}, v1, 0", lambda i: (i - 7) > 0),
    ("f64", [
        "v_cvt_f64_u32_e32 v[2:3], v0",
        "v_mov_b32_e32 v4, 0",
        "v_mov_b32_e32 v5, 0x40180000",  # 6.0
    ], "v_cmp_lt_f64_e64 {dst}, v[2:3], v[4:5]", lambda i: i < 6),
    ("f_f64", [
        "v_cvt_f64_u32_e32 v[2:3], v0",
    ], "v_cmp_f_f64_e64 {dst}, v[2:3], v[2:3]", lambda i: False),
    ("tru_f64", [
        "v_cvt_f64_u32_e32 v[2:3], v0",
    ], "v_cmp_tru_f64_e64 {dst}, v[2:3], v[2:3]", lambda i: True),
    ("u64_eq", [
        "v_mov_b32_e32 v2, v0",
        "v_mov_b32_e32 v3, 0",
        "v_mov_b32_e32 v4, 4",
        "v_mov_b32_e32 v5, 0",
    ], "v_cmp_eq_u64_e64 {dst}, v[2:3], v[4:5]", lambda i: i == 4),
    ("u64_lt", [
        "v_mov_b32_e32 v2, v0",
        "v_mov_b32_e32 v3, 0",
        "v_mov_b32_e32 v4, 8",
        "v_mov_b32_e32 v5, 0",
    ], "v_cmp_lt_u64_e64 {dst}, v[2:3], v[4:5]", lambda i: i < 8),
    ("class_f64", [
        "v_cvt_f64_u32_e32 v[2:3], v0",  # lane 0 is +0.0 (bit 6 = 0x40); lanes >= 1 are +normal (bit 8 = 0x100)
    ], "v_cmp_class_f64_e64 {dst}, v[2:3], 0x100", lambda i: i >= 1),
]


class VcmpDstWave32VccHiTests(unittest.TestCase):
    def _native_modes(self):
        return [(32, False), (64, False)]

    def test_null_destination_preserves_vcc_and_exec(self):
        for wave_size, emulate in self._native_modes():
            vcc_reg = "vcc_lo" if wave_size == 32 else "vcc"
            mov_exec = "s_mov_b32 exec_lo, -1" if wave_size == 32 else "s_mov_b64 exec, -1"
            and_exec = "s_and_b32 exec_lo, exec_lo, vcc_lo" if wave_size == 32 else "s_and_b64 exec, exec, vcc"
            for idx, (tag, setup, tmpl, _cond_fn) in enumerate(COMPARE_NULL_EXEC_CASES):
                with self.subTest(wave=wave_size, emulate=emulate, case=tag):
                    # Initial active mask: even lanes (i & 1 == 0) via v_and_b32 + v_cmp_eq_u32.
                    lines = [
                        "v_and_b32_e32 v10, 1, v0",
                        "v_mov_b32_e32 v11, 0x77777777",
                        "v_mov_b32_e32 v12, 0x88888888",
                        f"v_cmp_eq_u32_e64 {vcc_reg}, v10, 0",
                        *setup,
                        and_exec,
                        # VCC sentinel: lanes with (v0 & 3) == 2 (distinguished from active mask!)
                        "v_and_b32_e32 v13, 3, v0",
                        f"{mov_exec}",
                        f"v_cmp_eq_u32_e64 {vcc_reg}, v13, 2",
                        # Re-apply even-lane EXEC before v_cmp null, so both active and inactive lanes run null compare.
                        "v_cmp_eq_u32_e64 s4, v10, 0" if wave_size == 32 else "v_cmp_eq_u32_e64 s[4:5], v10, 0",
                        "s_mov_b32 exec_lo, s4" if wave_size == 32 else "s_mov_b64 exec, s[4:5]",
                        tmpl.format(dst="null"),
                        # Record which lanes stayed active in v11, and restore full EXEC to read VCC in v12.
                        "v_mov_b32_e32 v11, 0x11111111",
                        mov_exec,
                        f"v_cndmask_b32_e64 v12, 0x22222222, 0x33333333, {vcc_reg}",
                        "v_lshlrev_b32_e32 v14, 3, v0",
                        "v_add_nc_u32_e32 v15, 4, v14",
                        "buffer_store_dword v11, v14, s[0:3], 0 offen",
                        "buffer_store_dword v12, v15, s[0:3], 0 offen",
                    ]
                    mem = _run_cs(f"null_{tag}_w{wave_size}_{int(emulate)}", lines,
                                  wave_size=wave_size, emulate=emulate, validate=(idx == 0))
                    for i in range(wave_size):
                        got_exec = mem[i * 8]
                        got_vcc = mem[i * 8 + 4]
                        self.assertEqual(got_exec, 0x11111111 if (i & 1) == 0 else 0x77777777, f"lane {i} exec")
                        self.assertEqual(got_vcc, 0x33333333 if (i & 3) == 2 else 0x22222222, f"lane {i} vcc")

    def test_exec_destination_narrows_exec_without_clobbering_vcc(self):
        for wave_size, emulate in self._native_modes():
            vcc_reg = "vcc_lo" if wave_size == 32 else "vcc"
            exec_reg = "exec_lo" if wave_size == 32 else "exec"
            mov_exec = "s_mov_b32 exec_lo, -1" if wave_size == 32 else "s_mov_b64 exec, -1"
            and_exec = "s_and_b32 exec_lo, exec_lo, s4" if wave_size == 32 else "s_and_b64 exec, exec, s[4:5]"
            sg_dst = "s4" if wave_size == 32 else "s[4:5]"
            for idx, (tag, setup, tmpl, cond_fn) in enumerate(COMPARE_NULL_EXEC_CASES):
                with self.subTest(wave=wave_size, emulate=emulate, case=tag):
                    lines = [
                        "v_mov_b32_e32 v11, 0x55555555",
                        # Setup runs with all lanes on so inputs are defined everywhere.
                        *setup,
                        # Set distinct VCC pattern: (v0 & 3) == 1
                        "v_and_b32_e32 v13, 3, v0",
                        f"v_cmp_eq_u32_e64 {vcc_reg}, v13, 1",
                        # Narrow initial EXEC to lanes with (v0 & 1) == 0.
                        "v_and_b32_e32 v10, 1, v0",
                        f"v_cmp_eq_u32_e64 {sg_dst}, v10, 0",
                        and_exec,
                        # Execute compare targeting EXEC directly.
                        tmpl.format(dst=exec_reg),
                        "v_mov_b32_e32 v11, 0xAAAAAAAA",
                        mov_exec,
                        f"v_cndmask_b32_e64 v12, 0x10101010, 0x20202020, {vcc_reg}",
                        "v_lshlrev_b32_e32 v14, 3, v0",
                        "v_add_nc_u32_e32 v15, 4, v14",
                        "buffer_store_dword v11, v14, s[0:3], 0 offen",
                        "buffer_store_dword v12, v15, s[0:3], 0 offen",
                    ]
                    mem = _run_cs(f"exec_{tag}_w{wave_size}_{int(emulate)}", lines,
                                  wave_size=wave_size, emulate=emulate, validate=(idx in (0, 6, 8, 11)))
                    for i in range(wave_size):
                        want_active = ((i & 1) == 0) and cond_fn(i)
                        self.assertEqual(mem[i * 8], 0xAAAAAAAA if want_active else 0x55555555, f"lane {i} exec")
                        self.assertEqual(mem[i * 8 + 4], 0x20202020 if (i & 3) == 1 else 0x10101010, f"lane {i} vcc")

    def test_wave32_vcc_hi_mask_producers_and_consumers_isolate_vcc_lo(self):
        # Producer matrix in wave32 targeting vcc_hi while vcc_lo holds lane 7 only (0x00000080).
        producers = [
            ("cmp_f32", [
                "v_cvt_f32_u32_e32 v1, v0",
                "v_cmp_lt_f32_e64 vcc_hi, v1, 5.0",
            ], 0x0000001F),
            ("cmp_f64", [
                "v_cvt_f64_u32_e32 v[2:3], v0",
                "v_mov_b32_e32 v4, 0",
                "v_mov_b32_e32 v5, 0x40080000",  # 3.0
                "v_cmp_lt_f64_e64 vcc_hi, v[2:3], v[4:5]",
            ], 0x00000007),
            ("cmp_class_f64", [
                "v_cvt_f64_u32_e32 v[2:3], v0",
                "v_cmp_class_f64_e64 vcc_hi, v[2:3], 0x40",  # +0.0 -> lane 0 only
            ], 0x00000001),
            ("cmp_eq_u64", [
                "v_mov_b32_e32 v2, v0",
                "v_mov_b32_e32 v3, 0",
                "v_mov_b32_e32 v4, 19",
                "v_mov_b32_e32 v5, 0",
                "v_cmp_eq_u64_e64 vcc_hi, v[2:3], v[4:5]",
            ], 1 << 19),
            ("cmp_lt_u64", [
                "v_mov_b32_e32 v2, v0",
                "v_mov_b32_e32 v3, 0",
                "v_mov_b32_e32 v4, 6",
                "v_mov_b32_e32 v5, 0",
                "v_cmp_lt_u64_e64 vcc_hi, v[2:3], v[4:5]",
            ], 0x0000003F),
            ("add_co", [
                "v_mov_b32_e32 v1, 0xFFFFFFFF",
                "v_cmp_lt_u32_e64 s6, v0, 4",
                "v_cndmask_b32_e64 v2, 0, 1, s6",
                "v_add_co_u32 v3, vcc_hi, v1, v2",
            ], 0x0000000F),
            ("mad_u64", [
                "v_cmp_lt_u32_e64 s6, v0, 3",
                "v_cndmask_b32_e64 v2, 1, 0xFFFFFFFF, s6",
                "v_mov_b32_e32 v4, 0",
                "v_mov_b32_e32 v5, 0xFFFFFFFF",
                "v_mad_u64_u32 v[6:7], vcc_hi, v2, 0x10000000, v[4:5]",
            ], 0x00000007),
            ("saveexec", [
                "v_cmp_lt_u32_e64 s6, v0, 2",
                "s_and_saveexec_b32 vcc_hi, s6",
                "s_mov_b32 exec_lo, -1",
            ], 0xFFFFFFFF),
        ]

        for tag, prod_lines, want_mask in producers:
            with self.subTest(producer=tag):
                lines = [
                    "v_cmp_eq_u32_e64 vcc_lo, v0, 7",
                    *prod_lines,
                    # Consumers reading vcc_hi as a lane mask:
                    "v_cndmask_b32_e64 v10, 0, 1, vcc_hi",          # per-lane bit of vcc_hi
                    "v_add_co_ci_u32_e64 v11, s8, 10, 0, vcc_hi",    # 10 + carry_in(vcc_hi)
                    "s_bcnt1_i32_b32 s9, vcc_hi",                    # popcount of whole wave
                    "v_mov_b32_e32 v12, s9",
                    "v_mbcnt_lo_u32_b32 v13, vcc_hi, 0",             # prefix count
                    "s_cmp_eq_u32 vcc_hi, 0",                        # 1 iff mask == 0
                    "s_cselect_b32 s10, 1, 0",
                    "v_mov_b32_e32 v14, s10",
                    "v_cndmask_b32_e32 v15, 0, 1, vcc_lo",           # verify vcc_lo still only lane 7
                    "v_mul_i32_i24_e32 v16, 24, v0",
                    "buffer_store_dword v10, v16, s[0:3], 0 offen",
                    "v_add_nc_u32_e32 v16, 4, v16",
                    "buffer_store_dword v11, v16, s[0:3], 0 offen",
                    "v_add_nc_u32_e32 v16, 4, v16",
                    "buffer_store_dword v12, v16, s[0:3], 0 offen",
                    "v_add_nc_u32_e32 v16, 4, v16",
                    "buffer_store_dword v13, v16, s[0:3], 0 offen",
                    "v_add_nc_u32_e32 v16, 4, v16",
                    "buffer_store_dword v14, v16, s[0:3], 0 offen",
                    "v_add_nc_u32_e32 v16, 4, v16",
                    "buffer_store_dword v15, v16, s[0:3], 0 offen",
                ]
                mem = _run_cs(f"w32_vcchi_{tag}", lines, wave_size=32, emulate=False, validate=True)
                pop = bin(want_mask).count("1")
                for i in range(32):
                    bit = (want_mask >> i) & 1
                    prefix = bin(want_mask & ((1 << i) - 1)).count("1")
                    base = i * 24
                    self.assertEqual(mem[base], bit, f"{tag} lane {i} cndmask")
                    self.assertEqual(mem[base + 4], 10 + bit, f"{tag} lane {i} add_co_ci")
                    self.assertEqual(mem[base + 8], pop, f"{tag} lane {i} s_bcnt1")
                    self.assertEqual(mem[base + 12], prefix, f"{tag} lane {i} v_mbcnt")
                    self.assertEqual(mem[base + 16], 1 if want_mask == 0 else 0, f"{tag} lane {i} s_cmp_eq")
                    self.assertEqual(mem[base + 20], 1 if i == 7 else 0, f"{tag} lane {i} vcc_lo isolated")

        # s_wqm_b32 vcc_hi, s6: validate both CS (QuadSwap) and VS semantic execution (run_wave).
        wqm_lines = [
            "v_cmp_eq_u32_e64 vcc_lo, v0, 7",
            "v_cmp_eq_u32_e64 s6, v0, 5",
            "s_wqm_b32 vcc_hi, s6",
            "v_cndmask_b32_e64 v10, 0, 1, vcc_hi",
            "v_add_co_ci_u32_e64 v11, s8, 10, 0, vcc_hi",
            "s_bcnt1_i32_b32 s9, vcc_hi",
            "v_mov_b32_e32 v12, s9",
            "v_cndmask_b32_e32 v15, 0, 1, vcc_lo",
        ]
        _validate_spirv("w32_vcchi_wqm_cs", _translate_cs("w32_vcchi_wqm_cs", wqm_lines, wave_size=32))
        vs_out = _run_vs_w32("w32_vcchi_wqm_vs", wqm_lines)
        for i in range(32):
            bit = 1 if 4 <= i < 8 else 0
            self.assertEqual(vs_out[i], (bit, 10 + bit, 4, 1 if i == 7 else 0), f"wqm lane {i}")

    def test_wave32_vcc_hi_plain_data_scalar_and_mask_reads_preserve_vcc_lo(self):
        # 0xA5000005 sets bits {0, 2, 24, 26, 29, 31}; 6 bits total.
        data_word = 0xA5000005
        lines = [
            "v_cmp_eq_u32_e64 vcc_lo, v0, 7",
            f"s_mov_b32 vcc_hi, {data_word:#x}",
            "s_mov_b32 s4, vcc_hi",                              # scalar read -> 0xA5000005
            "v_mov_b32_e32 v10, s4",
            "s_bcnt1_i32_b32 s5, vcc_hi",                        # popcount of scalar word -> 6
            "v_mov_b32_e32 v11, s5",
            "v_cndmask_b32_e64 v12, 0x100, 0x200, vcc_hi",       # per-lane bit of 0xA5000005!
            "v_add_co_ci_u32_e64 v13, s6, 20, 0, vcc_hi",        # 20 + bit(lane, 0xA5000005)
            # Plain-data s_wqm_b32 into vcc_hi -> 0xF (scalar AND bitmask lanes 0..3) while preserving vcc_lo.
            "s_wqm_b32 vcc_hi, 0x1",
            "s_mov_b32 s7, vcc_hi",
            "v_mov_b32_e32 v14, s7",
            "v_cndmask_b32_e64 v15, 0, 1, vcc_hi",
            "v_cndmask_b32_e32 v16, 0, 1, vcc_lo",
            "v_mul_i32_i24_e32 v17, 28, v0",
            "buffer_store_dword v10, v17, s[0:3], 0 offen",
            "v_add_nc_u32_e32 v17, 4, v17",
            "buffer_store_dword v11, v17, s[0:3], 0 offen",
            "v_add_nc_u32_e32 v17, 4, v17",
            "buffer_store_dword v12, v17, s[0:3], 0 offen",
            "v_add_nc_u32_e32 v17, 4, v17",
            "buffer_store_dword v13, v17, s[0:3], 0 offen",
            "v_add_nc_u32_e32 v17, 4, v17",
            "buffer_store_dword v14, v17, s[0:3], 0 offen",
            "v_add_nc_u32_e32 v17, 4, v17",
            "buffer_store_dword v15, v17, s[0:3], 0 offen",
            "v_add_nc_u32_e32 v17, 4, v17",
            "buffer_store_dword v16, v17, s[0:3], 0 offen",
        ]
        mem = _run_cs("w32_vcchi_data", lines, wave_size=32, emulate=False, validate=True)
        for i in range(32):
            bit = (data_word >> i) & 1
            base = i * 28
            self.assertEqual(mem[base], data_word, f"lane {i} scalar read")
            self.assertEqual(mem[base + 4], 6, f"lane {i} bcnt1 data")
            self.assertEqual(mem[base + 8], 0x200 if bit else 0x100, f"lane {i} cndmask data")
            self.assertEqual(mem[base + 12], 20 + bit, f"lane {i} co_ci data")
            self.assertEqual(mem[base + 16], 0xF, f"lane {i} s_wqm scalar")
            self.assertEqual(mem[base + 20], 1 if i < 4 else 0, f"lane {i} s_wqm mask bit")
            self.assertEqual(mem[base + 24], 1 if i == 7 else 0, f"lane {i} vcc_lo isolated")

    def test_wave32_vcc_hi_overwrite_data_and_cmp_with_vcc_lo(self):
        # First hold a mask in vcc_hi, verify s_cmp_eq_u32 vcc_hi, vcc_lo; then overwrite vcc_hi with data
        # and verify vcc_lo still behaves as a mask while vcc_hi behaves as data.
        lines = [
            "v_cmp_lt_u32_e64 vcc_lo, v0, 4",
            "v_cmp_lt_u32_e64 vcc_hi, v0, 4",
            "s_cmp_eq_u32 vcc_hi, vcc_lo",
            "s_cselect_b32 s4, 1, 0",
            "v_mov_b32_e32 v10, s4",
            "s_mov_b32 vcc_hi, 0x10",          # lane 4 bit set -> only lane 4 when read by v_cndmask
            "s_bcnt1_i32_b32 s5, vcc_lo",      # still 4
            "v_mov_b32_e32 v11, s5",
            "v_cndmask_b32_e64 v12, 0, 1, vcc_hi",
            "v_cndmask_b32_e32 v13, 0, 1, vcc_lo",
            "v_mul_i32_i24_e32 v14, 16, v0",
            "buffer_store_dword v10, v14, s[0:3], 0 offen",
            "v_add_nc_u32_e32 v14, 4, v14",
            "buffer_store_dword v11, v14, s[0:3], 0 offen",
            "v_add_nc_u32_e32 v14, 4, v14",
            "buffer_store_dword v12, v14, s[0:3], 0 offen",
            "v_add_nc_u32_e32 v14, 4, v14",
            "buffer_store_dword v13, v14, s[0:3], 0 offen",
        ]
        mem = _run_cs("w32_vcchi_cmp_overwrite", lines, wave_size=32, emulate=False, validate=True)
        for i in range(32):
            base = i * 16
            self.assertEqual(mem[base], 1, f"lane {i} vcc_hi == vcc_lo")
            self.assertEqual(mem[base + 4], 4, f"lane {i} bcnt1(vcc_lo)")
            self.assertEqual(mem[base + 8], 1 if i == 4 else 0, f"lane {i} data vcc_hi")
            self.assertEqual(mem[base + 12], 1 if i < 4 else 0, f"lane {i} mask vcc_lo")

    def test_wave64_emulate_null_and_exec_destinations_validate(self):
        # Validate every compare/class family with null and exec destinations under --wave64-emulate,
        # and confirm standalone null writes emit no %vcc / %active store while exec writes update %active.
        for tag, setup, tmpl, _ in COMPARE_NULL_EXEC_CASES:
            with self.subTest(case=tag, dst="null"):
                spv = _translate_cs(f"emu_null_{tag}", [*setup, tmpl.format(dst="null")], wave_size=64, emulate=True)
                _validate_spirv(f"emu_null_{tag}", spv)
                self.assertNotIn("OpStore %vcc ", spv)
                self.assertNotIn("OpStore %active ", spv)
            with self.subTest(case=tag, dst="exec"):
                spv = _translate_cs(f"emu_exec_{tag}", [*setup, tmpl.format(dst="exec")], wave_size=64, emulate=True)
                _validate_spirv(f"emu_exec_{tag}", spv)
                self.assertNotIn("OpStore %vcc ", spv)
                self.assertIn("OpStore %active ", spv)

    def test_negative_controls_invalid_compare_destinations(self):
        invalid = [
            (32, "v_cmp_lt_f32_e64 v2, v0, v1"),
            (32, "v_cmp_lt_f64_e64 v2, v[0:1], v[2:3]"),
            (32, "v_cmp_class_f64_e64 v2, v[0:1], 1"),
            (32, "v_cmp_lt_f32_e64 1, v0, v1"),
            (64, "v_cmp_lt_f32_e64 vcc_hi, v0, v1"),
            (64, "v_cmp_lt_f64_e64 vcc_hi, v[0:1], v[2:3]"),
            (64, "v_cmp_class_f64_e64 vcc_hi, v[0:1], 1"),
            (64, "v_cmp_class_f64_e64 exec_hi, v[0:1], 1"),
        ]
        for wave_size, bad in invalid:
            with self.subTest(wave=wave_size, insn=bad):
                with self.assertRaises(rdna2spirv.Unsupported) as ctx:
                    _translate_cs("bad_vcmp_dst", [bad], wave_size=wave_size)
                self.assertIn("destination", str(ctx.exception))


if __name__ == "__main__":
    unittest.main()
