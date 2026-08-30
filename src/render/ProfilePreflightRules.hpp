#pragma once

#include <cstdint>

namespace mfgdlss::render
{
struct ProfilePreflightInputs
{

    bool bridge_ready{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};

    bool renderer_available{};

    bool extent_resolved{};
    std::uint32_t requested_width{};
    std::uint32_t requested_height{};

    std::uint32_t current_width{};
    std::uint32_t current_height{};

    bool stable_proxy_identity_published{};

    bool live_extent_reconfiguration_supported{};
    bool current_complete_frame_route{};
    bool requested_complete_frame_route{};
};

enum class ProfilePreflightDecision : std::uint32_t
{
    accepted,
    rejected_no_output_extent,
    rejected_no_renderer,
    rejected_extent_unresolved,
    rejected_extent_unusable,
    rejected_active_extent_unavailable,
    rejected_proxy_already_published,
    rejected_live_extent_reconfiguration_unavailable,
    rejected_presentation_route_change_requires_restart,
};

[[nodiscard]] constexpr bool is_rejection(
    const ProfilePreflightDecision decision) noexcept
{
    return decision != ProfilePreflightDecision::accepted;
}

[[nodiscard]] constexpr bool is_user_facing(
    const ProfilePreflightDecision decision) noexcept
{
    return decision == ProfilePreflightDecision::rejected_extent_unresolved ||
           decision == ProfilePreflightDecision::rejected_extent_unusable ||
           decision ==
               ProfilePreflightDecision::rejected_proxy_already_published ||
           decision == ProfilePreflightDecision::
               rejected_live_extent_reconfiguration_unavailable ||
           decision == ProfilePreflightDecision::
               rejected_presentation_route_change_requires_restart;
}

[[nodiscard]] constexpr ProfilePreflightDecision evaluate_profile_preflight(
    const ProfilePreflightInputs& in) noexcept
{
    if (!in.bridge_ready || in.output_width == 0 || in.output_height == 0) {
        return ProfilePreflightDecision::rejected_no_output_extent;
    }
    if (!in.renderer_available) {
        return ProfilePreflightDecision::rejected_no_renderer;
    }
    if (!in.extent_resolved) {
        return ProfilePreflightDecision::rejected_extent_unresolved;
    }
    if (in.requested_width == 0 || in.requested_height == 0 ||
        in.requested_width > in.output_width ||
        in.requested_height > in.output_height) {
        return ProfilePreflightDecision::rejected_extent_unusable;
    }
    if (in.current_width == 0 || in.current_height == 0) {
        return ProfilePreflightDecision::rejected_active_extent_unavailable;
    }
    if (in.current_complete_frame_route !=
        in.requested_complete_frame_route) {
        return ProfilePreflightDecision::
            rejected_presentation_route_change_requires_restart;
    }
    const auto extent_changes =
        in.requested_width != in.current_width ||
        in.requested_height != in.current_height;
    if (extent_changes && in.stable_proxy_identity_published) {
        return ProfilePreflightDecision::rejected_proxy_already_published;
    }
    if (extent_changes && !in.live_extent_reconfiguration_supported) {
        return ProfilePreflightDecision::
            rejected_live_extent_reconfiguration_unavailable;
    }
    return ProfilePreflightDecision::accepted;
}

[[nodiscard]] constexpr const char* describe(
    const ProfilePreflightDecision decision) noexcept
{
    switch (decision) {
    case ProfilePreflightDecision::accepted:
        return "accepted";
    case ProfilePreflightDecision::rejected_no_output_extent:
        return "the presentation bridge has no usable output extent";
    case ProfilePreflightDecision::rejected_no_renderer:
        return "Skyrim's renderer singleton is unavailable";
    case ProfilePreflightDecision::rejected_extent_unresolved:
        return "the requested logical render extent could not be resolved";
    case ProfilePreflightDecision::rejected_extent_unusable:
        return "the requested extent is not usable against the output";
    case ProfilePreflightDecision::rejected_active_extent_unavailable:
        return "the active logical render extent is unavailable";
    case ProfilePreflightDecision::rejected_proxy_already_published:
        return "Restart required: the render surface has already been "
               "acquired; an extent-changing profile needs a restart.";
    case ProfilePreflightDecision::
            rejected_live_extent_reconfiguration_unavailable:
        return "Restart required: this renderer cannot safely change the "
               "logical render extent during a running session.";
    case ProfilePreflightDecision::
            rejected_presentation_route_change_requires_restart:
        return "Restart required: enabling or disabling complete-frame "
               "presentation requires a new render surface.";
    }
    return "unknown";
}
}
