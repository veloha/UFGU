#pragma once

#include "config/Settings.hpp"

#include <cstdint>

namespace mfgdlss::render
{
[[nodiscard]] constexpr bool uses_complete_frame_presentation(
    const config::UpscalingMode mode) noexcept
{
    return mode != config::UpscalingMode::off;
}

[[nodiscard]] constexpr bool requires_reduced_render_extent(
    const config::UpscalingMode mode) noexcept
{
    return mode != config::UpscalingMode::off &&
           mode != config::UpscalingMode::dlaa;
}

[[nodiscard]] constexpr bool valid_complete_frame_extent(
    const config::UpscalingMode mode,
    const std::uint32_t render_width,
    const std::uint32_t render_height,
    const std::uint32_t output_width,
    const std::uint32_t output_height) noexcept
{
    if (!uses_complete_frame_presentation(mode) ||
        render_width == 0 || render_height == 0 ||
        output_width == 0 || output_height == 0 ||
        render_width > output_width || render_height > output_height) {
        return false;
    }

    const auto reduced =
        render_width < output_width || render_height < output_height;
    return requires_reduced_render_extent(mode) ? reduced : !reduced;
}
}
