// A depth texture in the legacy state DEPTH_READ | (NON_)PIXEL_SHADER_RESOURCE is in the layout its shader-resource
// views were written with. D3D12 lets that state combine the read-only depth test with shader reads of the same
// subresource ("It can be combined with other read states (for example, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
// such that the resource can be used for the depth or stencil test, and accessed by a shader within the same draw
// call", D3D12_RESOURCE_STATES). With the bindless set 0 every sampled view is written in GENERAL
// (ResourceBinding::sampled_in_general), and Vulkan wants the image in the descriptor's layout when a shader reads it
// (VUID-vkCmdDraw-None-09600, VUID-vkCmdDispatch-None-09600). DEPTH_STENCIL_READ_ONLY_OPTIMAL, the layout the
// state takes without set 0, is not GENERAL. DEPTH_READ alone (no shader reads) keeps the read-only depth layout.
// vkCmdPipelineBarrier is a function that records its barriers. No Vulkan loader, GPU or game data.
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include "../src/vulkan_replay.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using namespace leonida;

std::vector<VkImageMemoryBarrier> g_barriers;

void VKAPI_CALL record_barrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags,
                               std::uint32_t, const VkMemoryBarrier*, std::uint32_t, const VkBufferMemoryBarrier*,
                               std::uint32_t count, const VkImageMemoryBarrier* barriers) {
    for (std::uint32_t i = 0; i < count; ++i) g_barriers.push_back(barriers[i]);
}

int failures = 0;
void expect(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

const char* name(VkImageLayout layout) {
    switch (layout) {
    case VK_IMAGE_LAYOUT_GENERAL: return "GENERAL";
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return "DEPTH_STENCIL_ATTACHMENT_OPTIMAL";
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL: return "DEPTH_STENCIL_READ_ONLY_OPTIMAL";
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return "SHADER_READ_ONLY_OPTIMAL";
    default: return "other";
    }
}

}  // namespace

int main() {
    namespace s = d3d12_state;
    constexpr ResourceId id = 1;
    constexpr VkImageLayout write = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    struct Case {
        const char* what;
        VkFormat format;
        VkImageAspectFlags aspects;
        bool sampled_in_general;  // set 0 names the texture's views in GENERAL
        std::uint32_t after;
        VkImageLayout expected;
    };
    constexpr VkImageAspectFlags d = VK_IMAGE_ASPECT_DEPTH_BIT;
    constexpr VkImageAspectFlags ds = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    const Case cases[] = {
        {"D32 to DEPTH_READ | PIXEL_SHADER_RESOURCE with set 0", VK_FORMAT_D32_SFLOAT, d, true,
         s::depth_read | s::pixel_shader_resource, VK_IMAGE_LAYOUT_GENERAL},
        {"D32 to DEPTH_READ | NON_PIXEL_SHADER_RESOURCE with set 0", VK_FORMAT_D32_SFLOAT, d, true,
         s::depth_read | s::non_pixel_shader_resource, VK_IMAGE_LAYOUT_GENERAL},
        {"D24S8 to DEPTH_READ | ALL_SHADER_RESOURCE with set 0", VK_FORMAT_D24_UNORM_S8_UINT, ds, true,
         s::depth_read | s::pixel_shader_resource | s::non_pixel_shader_resource, VK_IMAGE_LAYOUT_GENERAL},
        // Controls: shader reads alone already go to GENERAL with set 0, and DEPTH_READ alone or without set 0
        // keeps the read-only depth layout.
        {"D32 to PIXEL_SHADER_RESOURCE with set 0", VK_FORMAT_D32_SFLOAT, d, true, s::pixel_shader_resource,
         VK_IMAGE_LAYOUT_GENERAL},
        {"D32 to DEPTH_READ with set 0", VK_FORMAT_D32_SFLOAT, d, true, s::depth_read,
         VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
        {"D32 to DEPTH_READ | PIXEL_SHADER_RESOURCE without set 0", VK_FORMAT_D32_SFLOAT, d, false,
         s::depth_read | s::pixel_shader_resource, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
    };

    const VkCommandBuffer cb = reinterpret_cast<VkCommandBuffer>(std::uintptr_t(0x10));
    ReplayFunctions vk{};
    vk.pipeline_barrier = record_barrier;
    for (const Case& c : cases) {
        ResourceBinding image{};
        image.image = reinterpret_cast<VkImage>(std::uintptr_t(0x1000));
        image.format = c.format;
        image.aspects = c.aspects;
        image.width = image.height = 16;
        image.sampled_in_general = c.sampled_in_general;
        const auto resolve = [&](ResourceId r) { return r == id ? &image : nullptr; };
        LayoutTracker tracker;
        tracker.track(id, image, write);
        g_barriers.clear();

        CommandList list;
        expect(list.transition({id, all_subresources, s::depth_write, c.after}, 0) && list.close(), c.what);
        PreparedList prepared;
        expect(prepare(list, resolve, vk, prepared) == ReplayResult::success, c.what);
        emit(prepared, cb, vk, &tracker);
        expect(!g_barriers.empty(), c.what);
        for (const VkImageMemoryBarrier& b : g_barriers) {
            if (b.newLayout != c.expected)
                std::fprintf(stderr, "%s: barrier to %s, expected %s\n", c.what, name(b.newLayout), name(c.expected));
            expect(b.oldLayout == write && b.newLayout == c.expected, c.what);
        }
        if (tracker.layout(id, 0) != c.expected)
            std::fprintf(stderr, "%s: tracked as %s, expected %s\n", c.what, name(tracker.layout(id, 0)),
                         name(c.expected));
        expect(tracker.layout(id, 0) == c.expected, c.what);
    }

    if (failures) return 1;
    std::printf("replay_depth_read_layout_test: OK\n");
    return 0;
}
