"""The MIMG unorm bit on image_sample* and image_gather4_lz(_o): texel-unit coordinates.

Standard library only (spirv-val is used when it is on PATH); no GPU, no LLVM, no game files.

GPU test: ps_sample_unorm (check.py). Here the shape of the SPIR-V: each spatial coordinate the sample or gather reads
is the instruction's VGPR divided by the image's level-0 size on that axis; an array layer, an explicit LOD, a bias and
a texel offset are left as they are, and _d gradients are divided like the coordinates. Without unorm nothing is
divided, and a cube with unorm is refused. The explicit-LOD gathers (image_gather4_l and the GATHER4_LOD_OPS) do
their own texel arithmetic and are not changed here.
"""
import os
import re
import shutil
import struct
import subprocess
import tempfile
import unittest

import support

DESC = "s[8:15], s[16:19]"
PRO = ["v_mov_b32 v4, 1.0", "v_mov_b32 v5, 0.5", "v_mov_b32 v6, 0.25", "v_mov_b32 v7, 0.75", "v_mov_b32 v8, 0.125",
       "v_mov_b32 v9, 0", "v_mov_b32 v10, 2.0", "v_mov_b32 v11, 3.0", "v_mov_b32 v12, 4.0"]
CS_FSSM = bytearray(0x148)
struct.pack_into("<4I", CS_FSSM, 0x138, 0x8088, 32, 1, 1)
CS_FSSM = bytes(CS_FSSM)


def translate(name, line):
    body = PRO + [line] + (["exp mrt0 v9, v9, v9, v9 done vm"] if name.startswith("ps_") else []) + ["s_endpgm"]
    return support.translate_lines(name, body, fssm=CS_FSSM if name.startswith("cs_") else None)


def defs(spv):
    """id -> (opcode, operand list) for every instruction with a result."""
    out = {}
    for m in re.finditer(r"^\s*(%\w+) = (Op\w+)(.*)$", spv, re.M):
        out[m.group(1)] = (m.group(2), m.group(3).split())
    return out


def through_casts(d, v):
    while v in d and d[v][0] == "OpBitcast":
        v = d[v][1][1]
    return v


def vgpr_of(spv, d, v):
    """The VGPR index the id `v` was loaded from (through bitcasts), or None."""
    consts = {k: int(c) for k, c in re.findall(r"^\s*(%\w+) = OpConstant %uint (\d+)", spv, re.M)}
    v = through_casts(d, v)
    if v not in d or d[v][0] != "OpLoad":
        return None
    ptr = d[v][1][1]
    if ptr not in d or d[ptr][0] != "OpAccessChain" or d[ptr][1][1] != "%vgpr":
        return None
    return consts.get(d[ptr][1][2])


def is_zero(spv, d, v):
    v = through_casts(d, v)
    return bool(re.search(rf"^\s*{re.escape(v)} = OpConstant %u?int 0$", spv, re.M))


def size_axis(spv, d, v):
    """For an OpConvertSToF of a level-0 OpImageQuerySizeLod component, that component's axis (None otherwise)."""
    if v not in d or d[v][0] != "OpConvertSToF":
        return None
    c = d[v][1][1]
    axis = 0
    if d.get(c, ("",))[0] == "OpCompositeExtract":
        c, axis = d[c][1][1], int(d[c][1][2])
    if d.get(c, ("",))[0] != "OpImageQuerySizeLod" or not is_zero(spv, d, d[c][1][2]):
        return None
    return axis


def component(spv, d, v):
    """('div', vgpr, axis) for vgpr / size[axis], ('raw', vgpr) for a plain VGPR, else None."""
    if d.get(v, ("",))[0] == "OpFDiv":
        _, num, den = d[v][1]
        return ("div", vgpr_of(spv, d, num), size_axis(spv, d, den))
    r = vgpr_of(spv, d, v)
    return ("raw", r) if r is not None else None


def coord_components(spv, d, coord):
    if d.get(coord, ("",))[0] == "OpCompositeConstruct":
        return [component(spv, d, c) for c in d[coord][1][1:]]
    return [component(spv, d, coord)]


def the_one(spv, opcode):
    """The coordinate id and the operands after it. A typed read emits one access per numeric class (float, uint,
    sint), all with the same coordinate and operands."""
    lines = [ln.strip() for ln in spv.splitlines() if re.search(rf"= {opcode} ", ln)]
    found = {re.match(r"%\w+ = \w+ \S+ \S+ (\S+)(.*)", ln).groups() for ln in lines}
    if len(found) != 1:
        raise AssertionError(f"expected one {opcode} form, got {lines}")
    coord, rest = found.pop()
    return coord, rest.split()


class SampleUnorm(unittest.TestCase):
    def check(self, name, line, opcode, want, extra=None):
        spv = translate(name, line)
        d = defs(spv)
        coord, rest = the_one(spv, opcode)
        self.assertEqual(coord_components(spv, d, coord), want, line)
        if extra:
            extra(spv, d, rest)
        return spv

    def test_explicit_lod_2d_divides_u_and_v_and_keeps_the_lod(self):
        def lod_kept(spv, d, rest):
            self.assertEqual(rest[0], "Lod")
            self.assertEqual(vgpr_of(spv, d, rest[1]), 6)
        # Typed explicit-LOD path (2D image_sample_l) and the plain one (1D, 3D, 2D array).
        self.check("ps_l", f"image_sample_l v[0:3], v[4:6], {DESC} dmask:0xf dim:SQ_RSRC_IMG_2D unorm",
                   "OpImageSampleExplicitLod", [("div", 4, 0), ("div", 5, 1)], lod_kept)

    def test_explicit_lod_in_compute(self):
        self.check("cs_lz", f"image_sample_lz v[0:3], v[4:5], {DESC} dmask:0xf dim:SQ_RSRC_IMG_2D unorm",
                   "OpImageSampleExplicitLod", [("div", 4, 0), ("div", 5, 1)])

    def test_array_layer_is_not_divided(self):
        self.check("ps_arr", f"image_sample_lz v[0:3], v[4:6], {DESC} dmask:0xf dim:SQ_RSRC_IMG_2D_ARRAY unorm",
                   "OpImageSampleExplicitLod", [("div", 4, 0), ("div", 5, 1), ("raw", 6)])

    def test_3d_divides_three_axes(self):
        self.check("ps_3d", f"image_sample_lz v[0:3], v[4:6], {DESC} dmask:0xf dim:SQ_RSRC_IMG_3D unorm",
                   "OpImageSampleExplicitLod", [("div", 4, 0), ("div", 5, 1), ("div", 6, 2)])

    def test_1d(self):
        self.check("ps_1d", f"image_sample_lz v[0:3], v4, {DESC} dmask:0xf dim:SQ_RSRC_IMG_1D unorm",
                   "OpImageSampleExplicitLod", [("div", 4, 0)])

    def test_implicit_lod_samples_the_divided_coordinates(self):
        # The derivatives are taken of the normalized coordinates, so the LOD is the one of the texel-unit ones.
        self.check("ps_s", f"image_sample v[0:3], v[4:5], {DESC} dmask:0xf dim:SQ_RSRC_IMG_2D unorm",
                   "OpImageSampleExplicitLod", [("div", 4, 0), ("div", 5, 1)])
        self.check("ps_s3", f"image_sample v[0:3], v[4:6], {DESC} dmask:0xf dim:SQ_RSRC_IMG_3D unorm",
                   "OpImageSampleImplicitLod", [("div", 4, 0), ("div", 5, 1), ("div", 6, 2)])

    def test_bias_and_offset_are_not_divided(self):
        def bias_kept(spv, d, rest):
            self.assertEqual(rest[0], "Bias|Offset")
            self.assertEqual(vgpr_of(spv, d, rest[1]), 5)
            offset = rest[2]
            self.assertEqual(d[offset][0], "OpCompositeConstruct")      # built from the offset dword, not divided
            self.assertNotIn("OpFDiv", [d[c][0] for c in d[offset][1][1:]])
        self.check("ps_b_o", f"image_sample_b_o v[0:3], v[4:8], {DESC} dmask:0xf dim:SQ_RSRC_IMG_2D_ARRAY unorm",
                   "OpImageSampleImplicitLod", [("div", 6, 0), ("div", 7, 1), ("raw", 8)], bias_kept)

    def test_gradients_are_divided_like_the_coordinates(self):
        def grads(spv, d, rest):
            self.assertEqual(rest[0], "Grad")
            for g in rest[1:3]:
                self.assertEqual(coord_components(spv, d, g)[0][2], 0)
                self.assertEqual(coord_components(spv, d, g)[1][2], 1)
            self.assertEqual([c[1] for c in coord_components(spv, d, rest[1])], [4, 5])
            self.assertEqual([c[1] for c in coord_components(spv, d, rest[2])], [6, 7])
        self.check("ps_d", f"image_sample_d v[0:3], v[4:9], {DESC} dmask:0xf dim:SQ_RSRC_IMG_2D unorm",
                   "OpImageSampleExplicitLod", [("div", 8, 0), ("div", 9, 1)], grads)

    def test_depth_compare_keeps_the_reference(self):
        def ref_kept(spv, d, rest):
            self.assertEqual(vgpr_of(spv, d, rest[0]), 4)
        self.check("ps_c_lz", f"image_sample_c_lz v0, v[4:6], {DESC} dmask:0x1 dim:SQ_RSRC_IMG_2D unorm",
                   "OpImageSampleDrefExplicitLod", [("div", 5, 0), ("div", 6, 1)], ref_kept)

    def test_gather4_lz_and_its_offset_form(self):
        self.check("ps_g", f"image_gather4_lz v[0:3], v[4:5], {DESC} dmask:0x1 dim:SQ_RSRC_IMG_2D unorm",
                   "OpImageGather", [("div", 4, 0), ("div", 5, 1)])
        self.check("ps_go", f"image_gather4_lz_o v[0:3], v[4:7], {DESC} dmask:0x1 dim:SQ_RSRC_IMG_2D_ARRAY unorm",
                   "OpImageGather", [("div", 5, 0), ("div", 6, 1), ("raw", 7)])

    def test_a16_divides_the_unpacked_halves(self):
        spv = translate("ps_a16", f"image_sample_lz v[0:3], v4, {DESC} dmask:0xf dim:SQ_RSRC_IMG_2D unorm a16")
        d = defs(spv)
        coord, _ = the_one(spv, "OpImageSampleExplicitLod")
        comps = d[coord][1][1:]
        self.assertEqual([d[c][0] for c in comps], ["OpFDiv", "OpFDiv"])
        self.assertEqual([size_axis(spv, d, d[c][1][2]) for c in comps], [0, 1])

    def test_without_unorm_nothing_is_divided(self):
        for line, opcode in ((f"image_sample_l v[0:3], v[4:6], {DESC} dmask:0xf dim:SQ_RSRC_IMG_2D",
                              "OpImageSampleExplicitLod"),
                             (f"image_gather4_lz v[0:3], v[4:5], {DESC} dmask:0x1 dim:SQ_RSRC_IMG_2D", "OpImageGather")):
            with self.subTest(line=line):
                spv = self.check("ps_n", line, opcode, [("raw", 4), ("raw", 5)])
                self.assertNotIn("OpImageQuerySizeLod", spv)

    def test_cube_with_unorm_is_refused(self):
        for op, addr in (("image_sample_lz", "v[4:6]"), ("image_gather4_lz", "v[4:6]")):
            with self.subTest(op=op), self.assertRaises(support.rdna2spirv.Unsupported):
                translate("ps_cube", f"{op} v[0:3], {addr}, {DESC} dmask:0x1 dim:SQ_RSRC_IMG_CUBE unorm")

    def test_sample_c_l_still_refuses_unorm(self):
        # image_sample_c_l takes only dim, dmask, a16 and d16; unorm stays refused there rather than ignored.
        with self.assertRaises(support.rdna2spirv.Unsupported):
            translate("ps_c_l", f"image_sample_c_l v0, v[4:7], {DESC} dmask:0x1 dim:SQ_RSRC_IMG_2D unorm")

    @unittest.skipUnless(shutil.which("spirv-as") and shutil.which("spirv-val"), "needs spirv-as and spirv-val on PATH")
    def test_modules_validate(self):
        lines = [("ps_v1", f"image_sample_l v[0:3], v[4:6], {DESC} dmask:0xf dim:SQ_RSRC_IMG_2D unorm"),
                 ("ps_v2", f"image_sample v[0:3], v[4:6], {DESC} dmask:0xf dim:SQ_RSRC_IMG_3D unorm"),
                 ("ps_v3", f"image_sample_d v[0:3], v[4:9], {DESC} dmask:0xf dim:SQ_RSRC_IMG_2D unorm"),
                 ("ps_v4", f"image_gather4_lz_o v[0:3], v[4:7], {DESC} dmask:0x1 dim:SQ_RSRC_IMG_2D_ARRAY unorm"),
                 ("cs_v5", f"image_sample_lz v[0:3], v4, {DESC} dmask:0xf dim:SQ_RSRC_IMG_1D unorm")]
        with tempfile.TemporaryDirectory() as tmp:
            for name, line in lines:
                with self.subTest(line=line):
                    a, b = os.path.join(tmp, name + ".spvasm"), os.path.join(tmp, name + ".spv")
                    with open(a, "w", encoding="utf-8", newline="\n") as f:
                        f.write(translate(name, line))
                    subprocess.run(["spirv-as", "--target-env", "vulkan1.2", a, "-o", b], check=True)
                    subprocess.run(["spirv-val", "--target-env", "vulkan1.2", "--allow-offset-texture-operand", b],
                                   check=True)


if __name__ == "__main__":
    unittest.main()
