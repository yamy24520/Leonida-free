# Native Vulkan queue-present trace

Set **LEONIDA_PRESENT_QUEUE_TRACE=1** before starting the process to enable two stderr records per native queue-present attempt. Other values leave tracing off. Each presenter samples the setting when constructed. Both writes and both flushes happen on the CPU under the existing queue lock. If stderr is not drained, either write or flush can block the calling thread and delay other work waiting on the lock. Drain stderr throughout the capture; trace timings are not a benchmark.

Both the runtime VulkanPresenter (source=presenter) and the DXGI shim's VulkanSwapchain (source=dxgi) bracket their actual vkQueuePresentKHR dispatch. Early exits that never reach this API produce **no** queue record. For example, a failed upstream vkQueueSubmit in VulkanPresenter produces neither record. The probe requests no additional GPU synchronization, readback, or Vulkan operation.

Example (synthetic; decimal fields):

    FRAMEPROOF QUEUE_CALL pid=42 source=presenter id=3 n=1 image=0
    FRAMEPROOF QUEUE_RETURN pid=42 source=presenter id=3 n=1 image=0 raw=-1000001004

CALL means the host is about to enter the dispatch. RETURN records the original signed VkResult before the runtime translates it, including errors. Only raw=0 (VK_SUCCESS) or raw=1000001003 (VK_SUBOPTIMAL_KHR) indicates an accepted queue presentation. Neither proves GPU completion, pixel variation, scan-out or a rendered game. A missing return is unknown, not a success or a diagnosis of a hang.

Match records from one run by PID, source, presenter ID, sequence and native swapchain image index. IDs are opaque nonzero 64-bit serials assigned per loaded producer module; n starts at 1 per presenter and advances for each dispatch, not each success. Different presenters may interleave. Split traces at process restart or producer unload/reload. Do not match a return with an absent call, or double-count these records alongside another presentation observer. No window, image data, pointers or file names are logged.

The [frame status CLI](../docs/frame-status.md) recognizes QUEUE_CALL/QUEUE_RETURN; these records are not aliases for legacy PRESENT or DRAW. The optional CTests `present_queue_trace` and `present_queue_reader_contract` share a small C++ fixture. The latter pipes the actual header's output to the Python CLI with a PID obtained from the operating system, not from the trace. Neither test needs Vulkan, a window, or a game.

To run the integration check without building the runtime, use Python 3 and a C++17 compiler. For example, from the repository root on a POSIX system:

```sh
fixture_dir=$(mktemp -d)
c++ -std=c++17 runtime/test/present_queue_trace_fixture.cpp -o "$fixture_dir/queue-fixture"
python3 -I runtime/test/check_present_queue_reader.py "$fixture_dir/queue-fixture"
```

On Windows, in a Visual Studio Developer Command Prompt:

```bat
cl /nologo /EHsc /std:c++17 runtime\test\present_queue_trace_fixture.cpp /Fo"%TEMP%\leonida-present-queue-fixture.obj" /Fe"%TEMP%\leonida-present-queue-fixture.exe"
python -I runtime\test\check_present_queue_reader.py "%TEMP%\leonida-present-queue-fixture.exe"
```

The dispatch is a synthetic callback returning the same signed values the API would use. Passing demonstrates producer/reader compatibility, not an actual driver call or an image.
