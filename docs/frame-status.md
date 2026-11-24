# Reading FRAMEPROOF counts

`tools/frame_status.py` reads a local file or stdin and writes one JSON object. It does not open a window, collect a trace, or verify what appeared on screen. **The public runtime at `3a3d9348` does not emit these markers.** Use an instrumented build or collector that implements the profile below. An ordinary runtime log returning `no_instrumentation` says nothing about whether a frame was drawn.

```sh
python -I tools/frame_status.py --pid 42 capture.log
python -I tools/frame_status.py --pid 42 - < capture.log
```

Use the PID from the process being observed. By default, **every record needs its own PID**, supplied by the producer or a collector that actually knows the source. Do not stamp a PID onto a merged log. For a dedicated file known to contain only one process, `--single-process` also accepts untagged records after that process's tagged, visible title-window marker. A recognized record tagged with another PID anywhere in the scanned prefix invalidates this assumption: only explicitly attributed records count, and `mixed_pid` is true. Unattributed window records never anchor a stream. An untagged DRAW before the first matching WINDOW cannot be attributed, even in single-process mode; it is not counted retroactively.

## Minimal profile

These are invented examples for PID 42. Each record starts at column one and must end with a newline; fields can appear in any order. Unknown marker names, free text and self-reported evidence messages cannot promote a result. A recognized record with malformed, missing or duplicate fields is not used. Only DRAW accepts additional diagnostic key/value fields, which are discarded.

```text
FRAMEPROOF WINDOW pid=42 title=sgaWindowClass visible=1
FRAMEPROOF ACTIVE pid=42 id=7 capture=1
FRAMEPROOF DRAW pid=42 submitted=1 done=1
FRAMEPROOF REQUEST pid=42 id=7 n=1 mode=planes
FRAMEPROOF PRESENT pid=42 id=7 n=1 image=0 mode=planes draw_target=1 copy_done=1 raw=0 pixels=16 diff=0 postdraw=0 varied=1
```

- `WINDOW` records a window observation, not a frame. The title-window anchor for this profile is the visible class shown above. `ACTIVE` gives a presenter ID and whether readback capture is enabled.
- `DRAW` reports whether the draw was submitted and whether the producer observed its completion. A completed draw counts even when there is **no REQUEST and no PRESENT**. A draw reported done but not submitted is inconsistent. These are counts of records, not unique draw IDs.
- `REQUEST` counts requests. `mode` can be `blank`, `direct`, or `planes`. Each presenter's request and present sequence numbers are **independent** positive counters. Duplicates and reversals do not count; forward jumps are noted. The tool does not match a request to a present by sequence number.
- `PRESENT` contains the producer's result (`raw`), swapchain image index, completed readback flag (`copy_done`), draw-target flag and pixel count. `diff` is the source vs. presented-buffer mismatch count; `postdraw` compares the draw snapshot with the source. Either is −1 if not measured. `varied` is −1 (unknown), 0 (uniform) or 1 (varying RGB). Use ASCII minus signs in records.

| Highest observation | Interpretation |
|---|---|
| `no_instrumentation` | No valid marker attributable to the target PID was observed in the scanned prefix. |
| `instrumented` | A marker was observed, but no request, completed draw/copy, or accepted present was recorded. |
| `requested` | A presentation was requested; it may have failed or never completed. |
| `queue_called` | An instrumented producer reached a native queue presentation call. No matching return is needed to count a call. |
| `queue_failed` | A matched native return had a result other than success or suboptimal success. |
| `copy_complete` | The producer observed a completed image readback in a PRESENT record. This is not evidence that a draw completed. |
| `gpu_complete` | The producer observed a submitted DRAW complete on the GPU. A window, readback, or successful present alone cannot establish this. |
| `queue_accepted` | A native queue call and its matching return were observed, and Vulkan accepted the presentation request. This is not an observation of pixels on screen. |
| `present_accepted` | Vulkan reported success (0) or suboptimal success (1000001003). Neither proves visible pixels. |
| `candidate_requires_independent_proof` | After a target window and capture-enabled presenter were observed, a nonblank present reported completion, acceptance, a draw target, varying pixels and zero mismatches in both comparisons. |

`draw_completed` (also returned as `gpu_complete`), `copy_completed`, and `present_accepted` count different observations. None implies either of the others. A candidate from PRESENT does not invent a missing DRAW event. Impossible pixel counts are flagged and cannot produce a candidate. Neither a candidate nor the strongest status identifies the source of the shaders, authenticates the window-to-presenter mapping, or establishes that the compositor displayed the expected image. Confirm provenance and inspect the actual output independently.

### Native queue profile (opt-in)

This separate producer profile can diagnose a call that did not return. It is not present in the uninstrumented base, and does not require WINDOW, ACTIVE, DRAW, or PRESENT records. Example (invented):

```text
FRAMEPROOF QUEUE_CALL pid=42 source=dxgi id=7 n=1 image=2
FRAMEPROOF QUEUE_RETURN pid=42 source=dxgi id=7 n=1 image=2 raw=0
```

Use a single process lifetime per input, not concatenated sessions that might reuse a PID. **Both records must explicitly name the expected PID, even with `--single-process`.** A return matches only an open call with the same source (`presenter` or `dxgi`), object `id`, sequence number `n`, and swapchain image index. Different objects or sources can interleave. Within one object calls are serialized and sequence numbers increase. IDs and sequence numbers are positive 64-bit unsigned integers; image indexes are unsigned 32-bit integers, and Vulkan results are signed 32-bit integers.

The reader keeps one pending call per object and source. Duplicates, old sequences, mismatched images and orphan returns never create a successful pair. A newer call supersedes any pending call for that object. A mismatched return does not consume a matching call still in flight. At most 256 distinct queue objects are tracked separately from the presenter-ID budget. Check `inconsistent`, `sequence_gaps`, `queue_unmatched_returns` and `queue_overflow`.

- `queue_calls` counts observed calls; `queue_returns` counts matched returns. `queue_unreturned` is their difference, not proof of a hang: input may be incomplete.
- `queue_accepted` counts only matched results 0 and 1000001003 (`VK_SUCCESS` and `VK_SUBOPTIMAL_KHR`); other matched results increment `queue_failed`.
- Do **not** add `queue_accepted` and `present_accepted`: the native and higher-level probes can observe the same presentation. Neither native marker counts as a draw, copy or image candidate. The top-level status reports the strongest observation anywhere in the scanned input, not the outcome of the last call.

**Limits and privacy.** At most 4 MiB, 4,096 bytes per record (excluding its line ending), and 256 presenter IDs per attribution mode. Oversize records are skipped in bounded chunks. If the scanned prefix ends mid-line, including at ordinary EOF, the line is never interpreted; `unterminated` is 1 and `truncated` is true. At the byte cap even an exactly full input is marked `truncated`. Check those flags plus `overlong`, `presenter_overflow`, `sequence_gaps`, `malformed` and `inconsistent` before interpreting totals. Only counts, flags, a status, and the caller-supplied PID are output. No log text, paths, handles, or image data are emitted. The tool never uploads a log.
