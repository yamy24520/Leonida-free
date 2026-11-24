"""Boundary and attribution tests for the aggregate reader. All records are synthetic."""
import importlib.util
import io
from pathlib import Path
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location(
    "frame_status", Path(__file__).resolve().parents[1] / "tools" / "frame_status.py")
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)
WINDOW = b"FRAMEPROOF WINDOW title=sgaWindowClass visible=1 pid=42\n"
ACTIVE = b"FRAMEPROOF ACTIVE id=1 capture=1\n"
REQUEST = b"FRAMEPROOF REQUEST id=1 n=1 mode=direct\n"
PRESENT = (b"FRAMEPROOF PRESENT id=1 n=1 mode=direct image=0 copy_done=1 raw=0 pixels=4 "
           b"draw_target=1 diff=0 postdraw=0 varied=1\n")


def tagged(line):
    return line.rstrip(b"\n") + b" pid=42\n"


def summary(data, single=False):
    return tool.summarize(io.BytesIO(data), 42, single_process=single)


class BoundsAndAttribution(unittest.TestCase):
    def test_draw_accepts_extra_diagnostic_fields_and_rejects_impossible_completion(self):
        row = b"FRAMEPROOF DRAW extra=private submitted=1 done=1 pid=42\n"
        result = summary(row + row.replace(b"submitted=1", b"submitted=0"))
        self.assertEqual((result["draw_completed"], result["copy_completed"], result["inconsistent"]), (1, 0, 1))
        self.assertEqual(result["status"], "gpu_complete")
        self.assertNotIn("private", repr(result))

    def test_anonymous_draw_before_window_is_not_backfilled(self):
        row = b"FRAMEPROOF DRAW submitted=1 done=1\n"
        self.assertEqual(summary(row + WINDOW, True)["gpu_complete"], 0)
        self.assertEqual(summary(row + WINDOW + row, True)["gpu_complete"], 1)
        self.assertEqual(summary(WINDOW + row + WINDOW.replace(b"42", b"12"), True)["gpu_complete"], 0)

    def test_unterminated_draw_at_eof_is_ignored_regular_crlf_is_accepted(self):
        row = b"FRAMEPROOF DRAW submitted=1 done=1 pid=42"
        for ending in (b"", b"\r"):
            result = summary(row + ending)
            self.assertEqual((result["draw_completed"], result["unterminated"], result["truncated"]), (0, 1, True))
        self.assertEqual(summary(row + b"\r\n")["gpu_complete"], 1)

    def test_single_process_requires_window_then_discards_unattributed_on_foreign_pid(self):
        self.assertEqual(summary(ACTIVE + REQUEST + PRESENT, True)["requests"], 0)
        trace = WINDOW + ACTIVE + REQUEST + PRESENT
        self.assertEqual(summary(trace, True)["candidates"], 1)
        mixed = summary(trace + WINDOW.replace(b"pid=42", b"pid=43"), True)
        self.assertTrue(mixed["mixed_pid"])
        self.assertEqual((mixed["requests"], mixed["candidates"]), (0, 0))
        self.assertEqual(mixed["status"], "instrumented")

    def test_input_budget_ignores_partial_record_and_does_not_peek_past_it(self):
        row = tagged(REQUEST)
        buf = io.BytesIO(WINDOW + row + row)
        cap = len(WINDOW) + len(row) - 1
        with patch.object(tool, "MAX_BYTES", cap):
            out = tool.summarize(buf, 42)
        self.assertEqual(buf.tell(), cap)
        self.assertTrue(out["truncated"])
        self.assertEqual(out["requests"], 0)

    def test_overlong_record_skipped_in_full_including_fake_marker_at_chunk_boundary(self):
        data = b"x" * (tool.MAX_LINE + 2) + tagged(PRESENT) + tagged(REQUEST)
        out = summary(data)
        self.assertEqual((out["requests"], out["present_accepted"], out["overlong"]), (1, 0, 1))

    def test_sequence_counters_independent_gaps_reported_duplicates_not_counted(self):
        req = tagged(REQUEST.replace(b"n=1", b"n=3"))
        result = summary(WINDOW + tagged(ACTIVE) + req + req + tagged(PRESENT))
        self.assertEqual((result["requests"], result["candidates"]), (1, 1))
        self.assertEqual((result["inconsistent"], result["sequence_gaps"]), (1, 1))

    def test_impossible_pixels_or_duplicate_fields_cannot_make_candidate(self):
        trace = WINDOW + tagged(ACTIVE) + tagged(REQUEST)
        bad = summary(trace + tagged(PRESENT.replace(b"diff=0", b"diff=5")))
        self.assertEqual((bad["inconsistent"], bad["present_accepted"], bad["candidates"]), (1, 1, 0))
        dup = summary(trace + tagged(PRESENT.replace(b"varied=1", b"varied=1 varied=0")))
        self.assertEqual((dup["malformed"], dup["present_accepted"]), (1, 0))

    def test_presenter_limit_does_not_evict_and_reuse_sequence_ids(self):
        with patch.object(tool, "MAX_PRESENTERS", 1):
            out = summary(tagged(ACTIVE) + tagged(REQUEST.replace(b"id=1", b"id=2")) + tagged(REQUEST))
        self.assertEqual((out["requests"], out["presenter_overflow"]), (1, 1))


if __name__ == "__main__":
    unittest.main()
