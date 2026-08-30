#pragma once

#include <cstdint>

namespace mfgdlss::streamline
{

struct FrameTagPolicy
{
    bool submission_allowed{};
    bool use_hudless_ui{};
    bool clear_optional_color_tags{};
    bool tag_backbuffer_extent{};
    std::uint32_t tag_count{};
};

[[nodiscard]] constexpr FrameTagPolicy make_frame_tag_policy(
    const bool hudless_ui_available,
    const bool reduced_resolution) noexcept
{

    constexpr std::uint32_t kTagCount = 5;
    static_cast<void>(reduced_resolution);
    return {
        true,
        hudless_ui_available,
        !hudless_ui_available,
        true,
        kTagCount};
}
}
