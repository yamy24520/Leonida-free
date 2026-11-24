#pragma once

// COM wrappers for IDXGIFactory7, IDXGIAdapter3, IDXGIOutput6, and IDXGISwapChain3 backed by VulkanSwapchain (dxgi_swapchain.h).
// Keeps the host dxgi.dll out of the Vulkan presentation path (docs/d3d12_vulkan.md section 3.1 & 4).

#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>

#include "dxgi_swapchain.h"

namespace rt::dxgi {

// Creates an IDXGIFactory4 COM object whose CreateSwapChainForHwnd creates a VulkanSwapchain
// and registers its back-buffer images with `context` (keyed by the returned ID3D12Resource* pointer).
HRESULT create_factory(leonida::VulkanContext& context, PFN_vkGetInstanceProcAddr gipa, VkInstance instance,
                       VkPhysicalDevice physical, PFN_vkGetDeviceProcAddr gdpa, VkQueue queue,
                       std::uint32_t queue_family, REFIID riid, void** out);

// Makes `adapter`, when it is this layer's, hand out its outputs in the console's IDXGIOutput slot order (the one a GDKX
// title calls: slot 21 is GetDisplayModeList1). For the adapter IDXGIDevice1::GetAdapter gives the title. False, and
// nothing changes, for any other adapter.
bool hand_out_console_outputs(IDXGIAdapter* adapter);

}  // namespace rt::dxgi
