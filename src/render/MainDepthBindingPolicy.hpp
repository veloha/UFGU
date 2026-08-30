#pragma once

#include <cstdint>

namespace mfgdlss::render
{
struct MainDepthCandidate final
{
    std::uint32_t color_width{};
    std::uint32_t color_height{};
    std::uint32_t depth_width{};
    std::uint32_t depth_height{};
    std::uint32_t depth_array_size{};
    std::uint32_t depth_samples{};
    bool depth_stencil_bind{};
    bool shader_resource_bind{};
};

[[nodiscard]] constexpr bool valid_main_depth_candidate(
    const MainDepthCandidate& candidate) noexcept
{
    return candidate.color_width != 0U &&
           candidate.color_height != 0U &&
           candidate.depth_width == candidate.color_width &&
           candidate.depth_height == candidate.color_height &&
           candidate.depth_array_size == 1U &&
           candidate.depth_samples == 1U &&
           candidate.depth_stencil_bind &&
           candidate.shader_resource_bind;
}
}
