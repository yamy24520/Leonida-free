"""Synthetic native queue markers only; no Vulkan driver, renderer, or game is involved."""
import importlib.util
import io
from pathlib import Path
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location(
    "frame_status", Path(__file__).resolve().parents[1] / "tools" / "frame_status.py")
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)


def marker(kind="CALL", *, pid=73, source="presenter", id=5, n=1, image=0, raw=0):
    prefix = f"FRAMEPROOF QUEUE_{kind} source={source} id={id} n={n} image={image}"
    return prefix + (f" raw={raw}" if kind == "RETURN" else "") + (f" pid={pid}" if pid is not None else "")


def scan(*rows, single=False):
    return tool.summarize(io.BytesIO(("\n".join(rows) + "\n").encode()), 73, single_process=single)


class NativeQueueObservations(unittest.TestCase):
    def assert_no_image_evidence(self, out):
        for counter in ("requests", "gpu_complete", "draw_completed", "copy_completed", "present_accepted", "candidates"):
            self.assertEqual(out[counter], 0, counter)

    def test_call_without_return_means_only_the_call_started(self):
        out = scan(marker())
        self.assertEqual(out["status"], "queue_called")
        self.assertEqual((out["queue_calls"], out["queue_returns"], out["queue_unreturned"]), (1, 0, 1))
        self.assert_no_image_evidence(out)

    def test_vk_result_is_accepted_only_for_success_or_suboptimal(self):
        for raw, ok in ((0, True), (1000001003, True), (-1000001004, False), (-4, False), (1, False)):
            with self.subTest(raw=raw):
                out = scan(marker(), marker("RETURN", raw=raw))
                self.assertEqual(out["status"], "queue_accepted" if ok else "queue_failed")
                self.assertEqual((out["queue_returns"], out["queue_accepted"], out["queue_failed"]), (1, int(ok), int(not ok)))
                self.assertEqual((out["queue_success"], out["queue_suboptimal"]), (int(raw == 0), int(raw == 1000001003)))
                self.assert_no_image_evidence(out)

    def test_mismatch_each_key_dimension_then_valid_pair(self):
        for changed in (dict(pid=74), dict(source="dxgi"), dict(id=6), dict(n=2), dict(image=1)):
            with self.subTest(changed=changed):
                wrong = marker("RETURN", **changed)
                out = scan(marker(), wrong)
                self.assertEqual((out["queue_accepted"], out["queue_unreturned"]), (0, 1))
                self.assertEqual(out["queue_unmatched_returns"], 0 if "pid" in changed else 1)
                self.assertEqual(scan(marker(), wrong, marker("RETURN"))["queue_accepted"], 1)

    def test_interleaved_sources_and_objects_with_same_sequence_are_independent(self):
        a = dict(source="presenter", id=1, image=0)
        b = dict(source="dxgi", id=1, image=0)
        c = dict(source="dxgi", id=2, image=3)
        out = scan(marker(**a), marker(**b), marker(**c),
                   marker("RETURN", **c), marker("RETURN", **b, raw=-4), marker("RETURN", **a))
        self.assertEqual((out["queue_calls"], out["queue_accepted"], out["queue_failed"], out["inconsistent"]),
                         (3, 2, 1, 0))

    def test_orphan_duplicates_and_old_sequence_cannot_validate_a_second_return(self):
        out = scan(marker("RETURN"), marker(), marker(), marker("RETURN"), marker("RETURN"),
                   marker(n=3, image=1), marker(n=2), marker("RETURN", n=2))
        self.assertEqual((out["queue_calls"], out["queue_accepted"], out["queue_unreturned"]), (2, 1, 1))
        self.assertEqual((out["queue_unmatched_returns"], out["sequence_gaps"]), (3, 1))

    def test_superseded_call_cannot_be_paired_with_late_return(self):
        out = scan(marker(), marker(n=2), marker("RETURN"), marker("RETURN", n=2))
        self.assertEqual((out["queue_calls"], out["queue_returns"], out["queue_unreturned"]), (2, 1, 1))
        self.assertEqual(out["inconsistent"], 2)

    def test_legacy_mode_does_not_infer_missing_pid_for_native_records(self):
        w = "FRAMEPROOF WINDOW title=sgaWindowClass visible=1 pid=73"
        for pid in (None, 74):
            self.assertEqual(scan(w, marker(pid=pid), marker("RETURN", pid=pid), single=True)["queue_accepted"], 0)
        self.assertEqual(scan(w, marker(), marker("RETURN", pid=None), single=True)["queue_unreturned"], 1)

    def test_limits_unsigned_sequence_signed_result_and_stream_cap(self):
        big = dict(id=(1 << 64) - 1, n=(1 << 64) - 1, image=(1 << 32) - 1)
        self.assertEqual(scan(marker(**big), marker("RETURN", **big, raw=-(1 << 31)))["queue_failed"], 1)
        for wrong in (dict(id=1 << 64), dict(id=0), dict(n=1 << 64), dict(n=0), dict(image=1 << 32),
                      dict(source="invalid"), dict(raw=1 << 31)):
            self.assertEqual(scan(marker("RETURN", **wrong))["malformed"], 1)
        with patch.object(tool, "MAX_QUEUE_STREAMS", 1):
            out = scan(marker("RETURN", id=7), marker(), marker(id=2), marker("RETURN"))
        self.assertEqual((out["queue_overflow"], out["queue_accepted"]), (1, 1))

    def test_native_and_high_level_results_are_not_added_or_matched_to_each_other(self):
        high = ("FRAMEPROOF PRESENT pid=73 id=5 n=1 image=0 mode=blank draw_target=0 copy_done=0 "
                "raw=0 pixels=16 diff=-1 postdraw=-1 varied=-1")
        out = scan(marker(), high, marker("RETURN"))
        self.assertEqual((out["queue_accepted"], out["present_accepted"], out["candidates"]), (1, 1, 0))
        self.assertEqual(out["status"], "present_accepted")
        self.assertEqual(scan(high, marker("RETURN"))["queue_accepted"], 0)


if __name__ == "__main__":
    unittest.main()
