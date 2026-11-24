# Native Vulkan queue-present trace

Set **LEONIDA_PRESENT_QUEUE_TRACE=1** before starting the process to enable two stderr records per native queue-present attempt. Other values leave tracing off. Each presenter samples the setting when constructed. This is a diagnostic mode: stderr is flushed under the existing queue lock, so drain it and do not use trace timings as performance measurements.

Both the runtime VulkanPresenter (source=presenter) and the DXGI shim's VulkanSwapchain (source=dxgi) bracket their actual vkQueuePresentKHR dispatch. Early exits that never reach this API produce **no** queue record. For example, a failed upstream vkQueueSubmit in VulkanPresenter produces neither record. No new synchronization, readback or Vulkan operation is requested.

Example (synthetic; decimal fields):

    FRAMEPROOF QUEUE_CALL pid=42 source=presenter id=3 n=1 image=0
    FRAMEPROOF QUEUE_RETURN pid=42 source=presenter id=3 n=1 image=0 raw=-1000001004

CALL means the host is about to enter the dispatch. RETURN records the original signed VkResult before the runtime translates it, including errors. Only raw=0 (VK_SUCCESS) or raw=1000001003 (VK_SUBOPTIMAL_KHR) indicates an accepted queue presentation. Neither proves GPU completion, pixel variation, scan-out or a rendered game. A missing return is unknown, not a success or a diagnosis of a hang.

Match records from one run by PID, source, presenter ID, sequence and native swapchain image index. IDs are opaque nonzero 64-bit serials assigned per loaded producer module; n starts at 1 per presenter and advances for each dispatch, not each success. Different presenters may interleave. Split traces at process restart or producer unload/reload. Do not match a return with an absent call, or double-count these records alongside another presentation observer. No window, image data, pointers or file names are logged.

Readers must explicitly support QUEUE_CALL/QUEUE_RETURN; these records are not aliases for legacy PRESENT or DRAW. The optional CTest present_queue_trace uses one small native executable and a Python subprocess to cover both sides of the call with no Vulkan device, window or game.
