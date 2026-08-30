#pragma once

#include "render/ComputeDispatchPlan.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace mfgdlss::render
{
class ComputePass final
{
public:
    ComputePass();
    ~ComputePass();

    ComputePass(const ComputePass&) = delete;
    ComputePass& operator=(const ComputePass&) = delete;
    ComputePass(ComputePass&&) = delete;
    ComputePass& operator=(ComputePass&&) = delete;

    [[nodiscard]] bool create(
        std::string_view name,
        std::string_view hlsl,
        std::string_view entry_point,
        const ComputeBindingLayout& layout,
        void* d3d12_device = nullptr);

    void destroy() noexcept;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] const std::string& detail() const noexcept;
    [[nodiscard]] const ComputeBindingLayout& layout() const noexcept;

    [[nodiscard]] bool bind_shader_resource(
        std::uint32_t index,
        void* d3d12_resource,
        std::uint32_t format_override = 0U);

    [[nodiscard]] bool bind_unordered_access(
        std::uint32_t index,
        void* d3d12_resource,
        std::uint32_t format_override = 0U);

    [[nodiscard]] bool bind_unordered_access_buffer(
        std::uint32_t index,
        void* d3d12_resource,
        std::uint32_t byte_size);

    [[nodiscard]] bool dispatch(
        void* d3d12_graphics_command_list,
        const ComputeDispatchGroups& groups,
        const void* root_constants,
        std::uint32_t root_constant_dwords);

private:
    struct State;
    std::unique_ptr<State> state_;
};
}
