"""End-to-end stderr contract; the executable dispatches only to a synthetic callback."""
import os
import re
import subprocess
import sys


def run(exe, setting):
    env = os.environ.copy()
    env.pop("LEONIDA_PRESENT_QUEUE_TRACE", None)
    if setting is not None:
        env["LEONIDA_PRESENT_QUEUE_TRACE"] = setting
    with subprocess.Popen([exe], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True, env=env) as proc:
        try:
            out, err = proc.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.communicate()
            raise
        assert proc.returncode == 0 and not out
        return proc.pid, err.splitlines()


def main(exe):
    cases = [("presenter", 1, 2, 0), ("presenter", 1, 1, -1000001004),
             ("presenter", 2, 0, 1000001003), ("dxgi", 1, 3, -4)]
    dispatch = [f"dispatch {raw}" for _, _, _, raw in cases]
    for disabled in (None, "0", "true", "11"):
        _, rows = run(exe, disabled)
        assert rows == dispatch, rows
    pid, rows = run(exe, "1")
    assert len(rows) == 3 * len(cases), rows
    ids = []
    for i, (source, n, image, raw) in enumerate(cases):
        before, call, after = rows[3 * i:3 * i + 3]
        fields = rf"pid={pid} source={source} id=([1-9][0-9]*) n={n} image={image}"
        match = re.fullmatch("FRAMEPROOF QUEUE_CALL " + fields, before)
        assert match and call == dispatch[i], (before, call)
        ids.append(match[1])
        assert after == before.replace("QUEUE_CALL", "QUEUE_RETURN") + f" raw={raw}", after
    assert ids[0] == ids[2] and ids[0] != ids[1]
    print("opt-in, actual PID, presenter IDs, sequence, pre-call and raw return: OK")


if __name__ == "__main__":
    main(sys.argv[1])
