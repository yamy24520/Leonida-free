"""Summarize an instrumented FRAMEPROOF stream without copying log text. See docs/frame-status.md."""
import argparse
import json
import re
import sys
from contextlib import nullcontext


MAX_BYTES = 4 * 1024 * 1024
MAX_LINE = 4096
MAX_PRESENTERS = 256
MAX_QUEUE_STREAMS = 256
UINT32 = (1 << 32) - 1
UINT64 = (1 << 64) - 1
QUEUE_EVENTS = (b"QUEUE_CALL", b"QUEUE_RETURN")
ACCEPTED_RESULTS = (0, 1000001003)  # VK_SUCCESS / VK_SUBOPTIMAL_KHR
SCHEMAS = {
    b"WINDOW": (b"title", b"visible"),
    b"ACTIVE": (b"id", b"capture"),
    b"DRAW": (b"submitted", b"done"),
    b"REQUEST": (b"id", b"n", b"mode"),
    b"PRESENT": (b"id", b"n", b"image", b"mode", b"draw_target", b"copy_done", b"raw",
                 b"pixels", b"diff", b"postdraw", b"varied"),
    b"QUEUE_CALL": (b"source", b"id", b"n", b"image"),
    b"QUEUE_RETURN": (b"source", b"id", b"n", b"image", b"raw"),
}
FLAGS = {b"visible", b"capture", b"draw_target", b"copy_done", b"submitted", b"done"}
LIMITS = {b"id": (1, UINT32), b"n": (1, UINT32), b"image": (0, UINT32),
          b"raw": (-(1 << 31), (1 << 31) - 1), b"pixels": (0, (1 << 63) - 1),
          b"diff": (-1, (1 << 63) - 1), b"postdraw": (-1, (1 << 63) - 1), b"varied": (-1, 1)}


def number(token, low, high):
    if not token or len(token) > 20 or not re.fullmatch(rb"-?[0-9]+", token):
        raise ValueError
    value = int(token)
    if not low <= value <= high:
        raise ValueError
    return value


def fields(words):
    result = {}
    for word in words:
        key, sep, value = word.partition(b"=")
        if not sep or not key or not value or key in result:
            raise ValueError
        result[key] = value
    return result


def validated(kind, items):
    required = set(SCHEMAS[kind])
    if kind == b"DRAW" and required <= items.keys():
        # Draw producers may add diagnostics; neither retain nor output them.
        items = {k: items[k] for k in required}
    if set(items) != required:
        raise ValueError
    for key, token in items.items():
        if kind in QUEUE_EVENTS and key in (b"id", b"n"):
            items[key] = number(token, 1, UINT64)
        elif key == b"source" and token not in (b"presenter", b"dxgi"):
            raise ValueError
        elif key in FLAGS:
            items[key] = number(token, 0, 1)
        elif key in LIMITS:
            items[key] = number(token, *LIMITS[key])
        elif key == b"mode" and token not in (b"blank", b"planes", b"direct"):
            raise ValueError
    return items


class Observations:
    def __init__(self):
        self.seen = self.window = False
        self.presenters = {}
        # PID has already been checked. Native calls are serialized within a (source, id) pair.
        # Keep its last sequence and pending image (None once retired), not a list of all calls.
        self.queues = {}
        self.counts = dict(requests=0, draw_completed=0, copy_completed=0, present_accepted=0, candidates=0,
                           inconsistent=0, sequence_gaps=0, presenter_overflow=0, queue_calls=0,
                           queue_returns=0, queue_accepted=0, queue_failed=0, queue_unmatched_returns=0,
                           queue_overflow=0)

    def record_queue(self, kind, f):
        key = (f[b"source"], f[b"id"])
        n, image = f[b"n"], f[b"image"]
        last, pending = self.queues.get(key, (0, None))
        if kind == b"QUEUE_CALL":
            if not last and len(self.queues) >= MAX_QUEUE_STREAMS:
                self.counts["queue_overflow"] += 1
                return
            if n <= last:
                self.counts["inconsistent"] += 1
                return
            if n != last + 1:
                self.counts["sequence_gaps"] += 1
            if pending is not None:
                # The intervening return was not observed. A later return must not match that old call.
                self.counts["inconsistent"] += 1
            self.queues[key] = (n, image)
            self.counts["queue_calls"] += 1
            return
        if n != last or image != pending:
            # Includes orphan and duplicate returns. Neither creates a new stream.
            self.counts["queue_unmatched_returns"] += 1
            self.counts["inconsistent"] += 1
            return
        self.queues[key] = (n, None)
        self.counts["queue_returns"] += 1
        self.counts["queue_accepted" if f[b"raw"] in ACCEPTED_RESULTS else "queue_failed"] += 1

    def record(self, kind, f):
        self.seen = True
        if kind in QUEUE_EVENTS:
            self.record_queue(kind, f)
            return
        if kind == b"WINDOW":
            self.window = f[b"title"] == b"sgaWindowClass" and f[b"visible"] == 1
            return
        if kind == b"DRAW":
            if f[b"done"] and not f[b"submitted"]:
                self.counts["inconsistent"] += 1
            elif f[b"done"] and f[b"submitted"]:
                self.counts["draw_completed"] += 1
            return
        ident = f[b"id"]
        if ident not in self.presenters:
            if len(self.presenters) >= MAX_PRESENTERS:
                self.counts["presenter_overflow"] += 1
                return
            self.presenters[ident] = {b"capture": None, b"REQUEST": 0, b"PRESENT": 0}
        state = self.presenters[ident]
        if kind == b"ACTIVE":
            if state[b"capture"] is not None:
                self.counts["inconsistent"] += 1
                # Do not trust an ID that is announced twice with conflicting capture modes.
                if state[b"capture"] != f[b"capture"]:
                    state[b"capture"] = -1
            else:
                state[b"capture"] = f[b"capture"]
            return
        n = f[b"n"]
        if n <= state[kind]:
            self.counts["inconsistent"] += 1
            return
        if n != state[kind] + 1:
            self.counts["sequence_gaps"] += 1
        state[kind] = n
        if kind == b"REQUEST":
            self.counts["requests"] += 1
            return
        done = f[b"copy_done"] == 1
        accepted = f[b"raw"] in ACCEPTED_RESULTS
        self.counts["copy_completed"] += int(done)
        self.counts["present_accepted"] += int(accepted)
        pixels, diff, post, varied = (f[k] for k in (b"pixels", b"diff", b"postdraw", b"varied"))
        if max(diff, post) > pixels or (varied == 1 and pixels < 2) or (
                not done and (diff >= 0 or post >= 0 or varied == 1)):
            self.counts["inconsistent"] += 1
            return
        if (self.window and state[b"capture"] == 1 and f[b"mode"] != b"blank" and done and accepted
                and f[b"draw_target"] == 1 and varied == 1 and diff == 0 and post == 0):
            self.counts["candidates"] += 1

    def status(self):
        for key, label in (("candidates", "candidate_requires_independent_proof"),
                           ("present_accepted", "present_accepted"), ("queue_accepted", "queue_accepted"),
                           ("draw_completed", "gpu_complete"), ("copy_completed", "copy_complete"),
                           ("queue_failed", "queue_failed"), ("queue_calls", "queue_called"),
                           ("requests", "requested")):
            if self.counts[key]:
                return label
        return "instrumented" if self.seen else "no_instrumentation"


def read_records(stream, stats):
    """Never buffer a whole overlong line or parse a partial final record at the byte cap."""
    remaining = MAX_BYTES
    dropping = False
    ended = True
    while remaining:
        chunk = stream.readline(min(MAX_LINE + 2, remaining))
        remaining -= len(chunk)
        if not chunk:
            break
        ended = chunk.endswith(b"\n")
        line = chunk.removesuffix(b"\n").removesuffix(b"\r")
        if dropping:
            dropping = not ended
        elif len(line) > MAX_LINE:
            stats["overlong"] += 1
            dropping = not ended
        elif ended:
            yield line
    # At EOF (including the scan boundary), never promote an uncommitted record.
    stats["unterminated"] = int(not ended)
    # Conservative at exactly the cap; do not read one more byte to probe.
    stats["truncated"] = remaining == 0 or not ended


def summarize(stream, pid, single_process=False):
    explicit = Observations()
    legacy = Observations() if single_process else None
    anchored = foreign = False
    stats = dict(malformed=0, overlong=0, truncated=False)
    for line in read_records(stream, stats):
        if not line.startswith(b"FRAMEPROOF "):
            continue
        words = line.split()
        if len(words) < 2 or words[1] not in SCHEMAS:
            continue
        kind = words[1]
        try:
            items = fields(words[2:])
            tagged = b"pid" in items
            if tagged and number(items.pop(b"pid"), 1, UINT32) != pid:
                foreign = True
                continue
            if kind in QUEUE_EVENTS and not tagged:
                continue  # Both records are explicitly PID-tagged by the native producer.
            if not tagged and not (legacy and anchored and not foreign and kind != b"WINDOW"):
                continue
            f = validated(kind, items)
        except ValueError:
            stats["malformed"] += 1
            continue
        if tagged:
            explicit.record(kind, f)
            if kind == b"WINDOW":
                anchored = explicit.window
        if legacy and not foreign:
            legacy.record(kind, f)
    mixed = legacy is not None and foreign
    obs = legacy if legacy is not None and not mixed else explicit
    if mixed:
        obs.counts["inconsistent"] += 1
    return dict(pid=pid, status=obs.status(), gpu_complete=obs.counts["draw_completed"],
                queue_unreturned=obs.counts["queue_calls"] - obs.counts["queue_returns"],
                **obs.counts, **stats, mixed_pid=bool(mixed))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", nargs="?", default="-", help="binary log file, or - for stdin (default)")
    parser.add_argument("--pid", required=True, help="expected process ID (positive 32-bit integer)")
    parser.add_argument("--single-process", action="store_true",
                        help="input is a dedicated single-process stream; allow untagged records after its WINDOW")
    args = parser.parse_args(argv)
    try:
        pid = number(args.pid.encode("ascii"), 1, UINT32)
    except (ValueError, UnicodeError):
        parser.error("--pid must be a positive 32-bit integer")
    try:
        with (nullcontext(sys.stdin.buffer) if args.path == "-" else open(args.path, "rb")) as stream:
            result = summarize(stream, pid, args.single_process)
    except OSError:
        print("Cannot read input.", file=sys.stderr)
        return 2
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
