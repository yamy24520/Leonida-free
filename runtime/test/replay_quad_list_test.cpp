// Quad lists without an index buffer are one triangle-list draw. A switch back to Vulkan triangle fans
// changes the provoking vertex for flat attributes and the order of overlapping blended instances.
#include "../src/vulkan_replay.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {
using namespace leonida;

struct DrawCall {
    VkPrimitiveTopology topology;
    VkBuffer buffer;
    VkDeviceSize offset;
    VkIndexType index_type;
    std::uint32_t count, instances, first_index;
    std::int32_t base_vertex;
    std::uint32_t first_instance;
    bool operator==(const DrawCall&) const = default;
};

DrawCall recorded{};
std::vector<DrawCall> calls;

bool check(bool condition, const char* description) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", description);
    return condition;
}
}

int main() {
    using namespace leonida;
    ReplayFunctions vk{};
    vk.begin_rendering = +[](VkCommandBuffer, const VkRenderingInfo*) {};
    vk.end_rendering = +[](VkCommandBuffer) {};
    vk.bind_pipeline = +[](VkCommandBuffer, VkPipelineBindPoint, VkPipeline) {};
    vk.push_constants = +[](VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, std::uint32_t,
                            std::uint32_t, const void*) {};
    vk.set_viewport = +[](VkCommandBuffer, std::uint32_t, std::uint32_t, const VkViewport*) {};
    vk.set_scissor = +[](VkCommandBuffer, std::uint32_t, std::uint32_t, const VkRect2D*) {};
    vk.set_primitive_topology = +[](VkCommandBuffer, VkPrimitiveTopology topology) { recorded.topology = topology; };
    vk.set_primitive_restart_enable = +[](VkCommandBuffer, VkBool32) {};
    vk.set_stencil_reference = +[](VkCommandBuffer, VkStencilFaceFlags, std::uint32_t) {};
    vk.set_blend_constants = +[](VkCommandBuffer, const float*) {};
    vk.bind_index_buffer = +[](VkCommandBuffer, VkBuffer buffer, VkDeviceSize offset, VkIndexType type) {
        recorded.buffer = buffer;
        recorded.offset = offset;
        recorded.index_type = type;
    };
    vk.draw_indexed = +[](VkCommandBuffer, std::uint32_t count, std::uint32_t instances, std::uint32_t first,
                         std::int32_t base, std::uint32_t first_instance) {
        recorded.count = count;
        recorded.instances = instances;
        recorded.first_index = first;
        recorded.base_vertex = base;
        recorded.first_instance = first_instance;
        calls.push_back(recorded);
    };

    vk.quad_indices = reinterpret_cast<VkBuffer>(std::uintptr_t{1});
    vk.quad_indices_offset = 64;
    vk.quad_index_count = 12;
    const auto app_buffer = reinterpret_cast<VkBuffer>(std::uintptr_t{2});

    GraphicsPipeline graphics{};
    const auto collect = [&](const Draw& draw) {
        calls.clear();
        CommandList list;
        if (!list.draw(draw) || !list.close()) return false;
        PreparedList prepared;
        prepared.list = &list;
        prepared.commands.resize(1);
        prepared.commands[0].draw = 0;
        auto& entry = prepared.draws.emplace_back();
        entry.graphics = &graphics;
        entry.index_buffer = app_buffer;
        entry.index_offset = 32;
        emit(prepared, VK_NULL_HANDLE, vk);
        return true;
    };

    Draw draw{};
    draw.pipeline = 1;
    draw.topology = topology_quad_list;
    draw.count = 8;
    draw.instances = 2;
    draw.first = 13;
    draw.first_instance = 7;

    const DrawCall triangles{VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, vk.quad_indices, 64, VK_INDEX_TYPE_UINT32,
                             12, 2, 0, 13, 7};
    bool ok = check(collect(draw) && calls == std::vector{triangles},
                    "non-indexed quads use one triangle-list draw and preserve draw bases");
    draw.count = 13;
    ok &= check(collect(draw) && calls == std::vector{triangles},
                "a draw is limited to whole quads in the generated index buffer");
    draw.count = 3;
    ok &= check(collect(draw) && calls.empty(), "an incomplete quad emits no draw");

    draw.indexed = true;
    draw.count = 8;
    draw.first = 5;
    draw.base_vertex = -2;
    const DrawCall first_fan{VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN, app_buffer, 32, VK_INDEX_TYPE_UINT16, 4, 2, 5, -2, 7};
    const DrawCall second_fan{VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN, app_buffer, 32, VK_INDEX_TYPE_UINT16, 4, 2, 9, -2, 7};
    ok &= check(collect(draw) && calls == std::vector{first_fan, second_fan},
                "indexed quads use the application's index buffer and base vertex");
    return ok ? 0 : 1;
}
