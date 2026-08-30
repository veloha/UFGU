#pragma once

namespace mfgdlss::streamline
{
enum class IncompleteFrameAction
{
    none,
    publish_inactive_tags_keep_mode,
    suspend_mode
};

[[nodiscard]] constexpr IncompleteFrameAction incomplete_frame_action(
    const bool copied,
    const bool submitted,
    const bool configured) noexcept
{
    if (configured) {
        return IncompleteFrameAction::none;
    }
    if (!copied || !submitted) {
        return IncompleteFrameAction::publish_inactive_tags_keep_mode;
    }
    return IncompleteFrameAction::suspend_mode;
}
}
