"""Synthetic unit tests for tools/timecycle_pairs.py (no game files)."""
from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import timecycle_pairs as tp  # noqa: E402

SYNTH_KF = b"""<?xml version="1.0" encoding="UTF-8"?>
<timecycle_keyframe_data version="1.000000">
  <cycle name="SYNTH_WEATHER" regions="2">
    <region name="REG_A">
      <far_clip>500 600 700</far_clip>
      <light_dir_col_r>0.1 0.5 1.0</light_dir_col_r>
      <light_dir_col_g>0.1 0.5 1.0</light_dir_col_g>
      <light_dir_col_b>0.1 0.5 1.0</light_dir_col_b>
      <postfx_exposure>-1.0 0.0 2.0</postfx_exposure>
    </region>
  </cycle>
</timecycle_keyframe_data>
"""

SYNTH_MOD = b"""<?xml version="1.0" encoding="UTF-8"?>
<timecycle_modifier_data version="1.000000">
  <modifier name="SYNTH_PLAIN_1" userFlags="0">
    <postfx_exposure>0.5 0.0</postfx_exposure>
    <light_dir_col_r>0.8 0.25</light_dir_col_r>
    <light_dir_col_g>0.4 0.25</light_dir_col_g>
    <light_dir_col_b>0.2 0.25</light_dir_col_b>
    <far_clip>100.0 500.0</far_clip>
    <empty_var></empty_var>
  </modifier>
  <modifier name="SYNTH_PLAIN_2" userFlags="0">
    <postfx_exposure>1.2 1.0</postfx_exposure>
    <far_clip>250.0 250.0</far_clip>
    <submodifiers><item>SYNTH_PLAIN_1</item></submodifiers>
  </modifier>
  <modifier name="SYNTH_AXIS" numAxes="1" numKeyframes="2">
    <axis controlValue="SYNTH_CTRL">
      <keyframe_t_value>0.0 1.0</keyframe_t_value>
      <postfx_exposure>0.0 1.0</postfx_exposure>
    </axis>
  </modifier>
</timecycle_modifier_data>
"""


class TimecyclePairsTests(unittest.TestCase):
    def test_summarize_synthetic_documents(self):
        summary = tp.summarize_documents([
            ("timecycle_keyframe_data", SYNTH_KF),
            ("timecycle_modifier_data", SYNTH_MOD),
        ])
        self.assertEqual(summary["keyframe_docs"], 1)
        self.assertEqual(summary["modifier_docs"], 1)
        self.assertEqual(summary["modifiers"], {"total": 3, "plain": 2, "axis": 1})
        self.assertEqual(summary["plain_token_counts"], {"0": 1, "2": 7})
        self.assertEqual(summary["second_number"]["eq0"], 1)
        self.assertEqual(summary["second_number"]["between_0_1"], 3)
        self.assertEqual(summary["second_number"]["eq1"], 1)
        self.assertEqual(summary["second_number"]["gt1"], 2)
        self.assertEqual(summary["rgb_triplet_b_equality"], {"frac_all_equal": 1})
        self.assertEqual(summary["per_variable_b_constancy"]["both_b0_and_b1"], 1)
        self.assertEqual(summary["per_variable_b_constancy"]["mixed_within_single_doc"], 1)
        fc = summary["vars_with_b_outside_0_1"]["far_clip"]
        self.assertEqual(fc["count"], 2)
        self.assertEqual(fc["a_eq_b"], 1)
        self.assertEqual(fc["a_lt_b"], 1)
        self.assertEqual(fc["keyframe_range"], [500.0, 700.0])

    def test_modifier_patterns_do_not_round_values_onto_endpoints(self):
        values = ("-0.0000001", "0.0000001", "0.9999999", "1.0000001", "0", "1")
        modifiers = "".join(f"<modifier><v>2 {b}</v></modifier>" for b in values)
        blob = f"<timecycle_modifier_data>{modifiers}</timecycle_modifier_data>".encode()
        summary = tp.summarize_documents([("timecycle_modifier_data", blob)])
        self.assertEqual(summary["per_modifier_b_pattern"], {
            "outside_0_1": 2, "in_0_1_with_frac": 2, "all_b_eq_0": 1, "all_b_eq_1": 1,
        })
        self.assertEqual(summary["second_number"]["distinct_round6"], 2)

    def test_cli_xml_directory_and_out(self):
        with tempfile.TemporaryDirectory() as tmp:
            with open(os.path.join(tmp, "kf.xml"), "wb") as f:
                f.write(SYNTH_KF)
            with open(os.path.join(tmp, "mod.xml"), "wb") as f:
                f.write(SYNTH_MOD)
            out_path = os.path.join(tmp, "report.json")
            rc = tp.main([tmp, "--out", out_path])
            self.assertEqual(rc, 0)
            with open(out_path, encoding="utf-8") as f:
                data = json.load(f)
            self.assertEqual(data["modifiers"]["plain"], 2)
            self.assertEqual(data["second_number"]["count"], 7)

    def test_cli_validation_guards(self):
        with tempfile.TemporaryDirectory() as tmp:
            in_xml = os.path.join(tmp, "mod.xml")
            with open(in_xml, "wb") as f:
                f.write(SYNTH_MOD)
            with self.assertRaises(SystemExit) as cm:
                tp.main([in_xml, "--out", in_xml])
            self.assertEqual(cm.exception.code, 2)
            with self.assertRaises(SystemExit) as cm:
                tp.main([in_xml, "--out", tmp])
            self.assertEqual(cm.exception.code, 2)
            with self.assertRaises(SystemExit) as cm:
                tp.main([os.path.join(tmp, "missing.rpf")])
            self.assertEqual(cm.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
