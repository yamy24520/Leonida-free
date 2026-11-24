"""Pipe the compiled C++ probe fixture's real stderr into the public Python CLI; no game or GPU."""
import json
import os
from pathlib import Path
import subprocess
import sys


CLI = Path(__file__).resolve().parents[2] / "tools" / "frame_status.py"


def observe(exe, *, enabled, wrong_pid=False):
    env = os.environ.copy()
    env["LEONIDA_PRESENT_QUEUE_TRACE"] = "1" if enabled else "0"
    with subprocess.Popen([exe], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=env) as child:
        try:
            # Use the OS child identity, not the PID reported by a record.
            pid = child.pid
            target = (1 if pid != 1 else 2) if wrong_pid else pid
            parsed = subprocess.run([sys.executable, "-I", str(CLI), "--pid", str(target), "-"],
                                    stdin=child.stderr, capture_output=True, timeout=15, check=True)
            if child.wait(timeout=5) != 0:
                raise AssertionError("the native fixture failed")
        finally:
            if child.poll() is None:
                child.kill()
                child.wait()
    report = json.loads(parsed.stdout)
    if parsed.stderr or report["pid"] != target:
        raise AssertionError("unexpected CLI output or PID")
    return report


def expect(report, **values):
    for field, value in values.items():
        if report[field] != value:
            raise AssertionError(f"{field}: expected {value!r}, got {report[field]!r}")


def main(exe):
    enabled = observe(exe, enabled=True)
    expect(enabled, status="queue_accepted", queue_calls=4, queue_returns=4,
           queue_success=1, queue_suboptimal=1, queue_failed=2, queue_accepted=2,
           queue_unreturned=0, queue_unmatched_returns=0, sequence_gaps=0, inconsistent=0,
           malformed=0, truncated=False)
    for quiet in (observe(exe, enabled=False), observe(exe, enabled=True, wrong_pid=True)):
        expect(quiet, status="no_instrumentation", queue_calls=0, queue_returns=0,
               queue_success=0, queue_suboptimal=0, queue_failed=0, queue_accepted=0)
    expect(enabled, requests=0, gpu_complete=0, draw_completed=0, copy_completed=0,
           present_accepted=0, candidates=0)
    print("Native header -> CLI: 1 success, 1 suboptimal, 2 errors; "
          "disabled and foreign PID ignored; no draw, copy or image evidence.")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: check_present_queue_reader.py PATH_TO_FIXTURE_EXECUTABLE")
    main(str(Path(sys.argv[1]).resolve(strict=True)))
