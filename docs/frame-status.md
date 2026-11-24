# Reading FRAMEPROOF counts

`tools/frame_status.py` reads a local file or stdin and writes one JSON object. It does not open a window, collect a trace, or verify what appeared on screen. **The public runtime at `3a3d9348` does not emit these markers.** Use an instrumented build or collector that implements the profile below. An ordinary runtime log returning `no_instrumentation` says nothing about whether a frame was drawn.

```sh
python -I tools/frame_status.py --pid 42 capture.log
python -I tools/frame_status.py --pid 42 - < capture.log
```

Use the PID from the process being observed. By default, **every record needs its own PID**, supplied by the producer or a collector that actually knows the source. Do not stamp a PID onto a merged log. For a dedicated file known to contain only one process, `--single-process` also accepts untagged records after that process's tagged, visible title-window marker. A recognized record tagged with another PID anywhere in the scanned prefix invalidates this assumption: only explicitly attributed records count, and `mixed_pid` is true. Unattributed window records never anchor a stream.

## Minimal profile

These are invented examples for PID 42. Each record starts at column one; fields can appear in any order. Unknown marker names, free text and self-reported evidence messages cannot promote a result. A recognized record with malformed, missing, extra or duplicate fields is not used.

```text
FRAMEPROOF WINDOW pid=42 title=sgaWindowClass visible=1
FRAMEPROOF ACTIVE pid=42 id=7 capture=1
FRAMEPROOF REQUEST pid=42 id=7 n=1 mode=planes
FRAMEPROOF PRESENT pid=42 id=7 n=1 image=0 mode=planes draw_target=1 copy_done=1 raw=0 pixels=16 diff=0 postdraw=0 varied=1
```

- `WINDOW` records a window observation, not a frame. The title-window anchor for this profile is the visible class shown above. `ACTIVE` gives a presenter ID and whether readback capture is enabled.
- `REQUEST` counts requests. `mode` can be `blank`, `direct`, or `planes`. Each presenter's request and present sequence numbers are **independent** positive counters. Duplicates and reversals do not count; forward jumps are noted. The tool does not match a request to a present by sequence number.
- `PRESENT` contains the producer's result (`raw`), swapchain image index, completed readback flag (`copy_done`), draw-target flag and pixel count. `diff` is the source vs. presented-buffer mismatch count; `postdraw` compares the draw snapshot with the source. Either is −1 if not measured. `varied` is −1 (unknown), 0 (uniform) or 1 (varying RGB). Use ASCII minus signs in records.

| Highest observation | Interpretation |
|---|---|
| `no_instrumentation` | No valid marker attributable to the target PID was observed in the scanned prefix. |
| `instrumented` | A marker was observed, but there is no request, completed readback or accepted present to report. |
| `requested` | A presentation was requested; it may have failed or never completed. |
| `gpu_complete` | A readback completed; that fence alone does not establish presentation. |
| `present_accepted` | Vulkan reported success (0) or suboptimal success (1000001003). Neither proves visible pixels. |
| `candidate_requires_independent_proof` | After a target window and capture-enabled presenter were observed, a nonblank present reported completion, acceptance, a draw target, varying pixels and zero mismatches in both comparisons. |

`gpu_complete` and `present_accepted` are separate counts: either can be nonzero without the other. Impossible pixel counts are flagged and cannot produce a candidate. Neither a candidate nor the strongest status identifies the source of the shaders, authenticates the window-to-presenter mapping, or establishes that the compositor displayed the expected image. Confirm provenance and inspect the actual output independently.

**Limits and privacy.** At most 4 MiB, 4,096 bytes per record (excluding its line ending), and 256 presenter IDs per attribution mode. Oversize records are skipped in bounded chunks; at the byte cap, even an exactly full input is marked `truncated` and no partial last record is interpreted. Check `truncated`, `overlong`, `presenter_overflow`, `sequence_gaps`, `malformed` and `inconsistent` before interpreting totals. Only counts, flags, a status, and the caller-supplied PID are output. No log text, paths, handles, or image data are emitted. The tool never uploads a log.
