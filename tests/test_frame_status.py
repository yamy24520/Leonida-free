"""Exercise the public CLI with generated records; no game, GPU or external packages."""
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


CLI = Path(__file__).resolve().parents[1] / "tools" / "frame_status.py"
PID = 42
CANARY = "SYNTHETIC_PRIVATE_4291_must_not_leak"


def marker(kind, fields, pid=PID):
    suffix = "" if pid is None else f" pid={pid}"
    return f"FRAMEPROOF {kind} {fields}{suffix}"


def window(pid=PID):
    return marker("WINDOW", "title=sgaWindowClass visible=1", pid)


def active(pid=PID):
    return marker("ACTIVE", "id=1 capture=1", pid)


def request(pid=PID):
    return marker("REQUEST", "id=1 n=1 mode=planes", pid)


def draw(pid=PID, *, submitted=1, done=1):
    return marker("DRAW", f"submitted={submitted} done={done}", pid)


def present(pid=PID, **override):
    fields = dict(id=1, n=1, image=0, mode="planes", draw_target=1, copy_done=1,
                  raw=0, pixels=16, diff=0, postdraw=0, varied=1)
    fields.update(override)
    return marker("PRESENT", " ".join(f"{k}={v}" for k, v in fields.items()), pid)


class FrameStatusCLI(unittest.TestCase):
    def classify(self, rows, *, single_process=False, newline=True):
        # Run from an unrelated directory and check both output channels for leaks.
        with tempfile.TemporaryDirectory() as td:
            sample = Path(td) / (CANARY + ".log")
            sample.write_bytes(("\n".join(rows) + ("\n" if newline else "")).encode("utf-8"))
            proc = subprocess.run(
                [sys.executable, "-I", str(CLI), str(sample), "--pid", str(PID),
                 *(["--single-process"] if single_process else [])],
                cwd=td, capture_output=True, encoding="utf-8", timeout=10,
            )
        self.assertNotIn(CANARY, proc.stdout)
        self.assertNotIn(CANARY, proc.stderr)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        result = json.loads(proc.stdout)
        self.assertIsInstance(result, dict)
        self.assertEqual(result["pid"], PID)
        self.assertIs(result.get("game_rendered", False), False)
        self.assertIs(result.get("playable", False), False)
        return result

    def expect(self, rows, status, *, gpu=0, copied=0, accepted=0, candidates=0, **options):
        result = self.classify(rows, **options)
        self.assertEqual(result["status"], status)
        for key, expected in (("gpu_complete", gpu), ("draw_completed", gpu), ("copy_completed", copied),
                              ("present_accepted", accepted),
                              ("candidates", candidates)):
            self.assertIs(type(result[key]), int, key)
            self.assertEqual(result[key], expected, key)
        return result

    def test_visible_window_alone_is_not_frame_evidence(self):
        self.expect([window()], "instrumented")

    def test_missing_sensor_is_not_evidence_of_no_draw(self):
        self.expect([f"unstructured synthetic message: {CANARY}"], "no_instrumentation")

    def test_other_or_missing_pid_cannot_supply_evidence(self):
        for foreign in (75, None):
            with self.subTest(pid=foreign):
                rows = [window(foreign), active(foreign), request(foreign), present(foreign)]
                self.expect(rows, "no_instrumentation")
                self.expect([window(), active(), *rows], "instrumented")

    def test_request_is_not_completion(self):
        result = self.expect([window(), active(), request()], "requested")
        self.assertEqual(result["requests"], 1)

    def test_single_process_draw_completed_without_any_present(self):
        self.expect([window(), draw(None)], "gpu_complete", gpu=1, single_process=True)

    def test_submitted_draw_without_completion_does_not_count(self):
        self.expect([window(), draw(None, done=0)], "instrumented", single_process=True)
        self.expect([draw(None), window()], "instrumented", single_process=True)

    def test_foreign_or_unattributed_draw_has_no_strict_completion(self):
        for pid in (75, None):
            with self.subTest(pid=pid):
                self.expect([window(), draw(pid)], "instrumented")
        self.expect([window(), draw()], "gpu_complete", gpu=1)

    def test_copy_completion_does_not_prove_a_completed_draw(self):
        self.expect([window(), active(), request(), present(raw=-1)], "copy_complete", copied=1)

    def test_unterminated_positive_record_cannot_promote_status(self):
        for tail in (draw(), present()):
            with self.subTest(tail=tail.split()[1]):
                result = self.expect([window(), active(), request(), tail], "requested", newline=False)
                self.assertEqual(result["unterminated"], 1)
                self.assertIs(result["truncated"], True)

    def test_present_accepted_without_pixel_evidence(self):
        for raw in (0, 1000001003):
            with self.subTest(raw=raw):
                self.expect([window(), active(), request(),
                             present(raw=raw, copy_done=0, pixels=0, varied=0,
                                     diff=-1, postdraw=-1)],
                            "present_accepted", accepted=1)

    def test_pixels_without_variation_do_not_make_a_candidate(self):
        self.expect([window(), active(), request(), present(varied=0)],
                    "present_accepted", copied=1, accepted=1)

    def test_nonuniform_candidate_still_needs_independent_proof(self):
        self.expect([window(), active(), request(), present()],
                    "candidate_requires_independent_proof", copied=1, accepted=1, candidates=1)

    def test_private_text_and_self_reported_claims_cannot_be_echoed_as_proof(self):
        self.expect([window(), active(), f"FRAMEPROOF EVIDENCE game_rendered=1 "
                     f"detail={CANARY} pid={PID}", f"private annotation {CANARY}"], "instrumented")


if __name__ == "__main__":
    unittest.main()
