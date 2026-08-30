#pragma once

namespace mfgdlss::render
{

struct NativeUiPolicy
{
    bool install_scaleform_boundary{};
    bool capture_at_scaleform_boundary{};
    bool service_at_present{};
};

[[nodiscard]] constexpr NativeUiPolicy make_native_ui_policy(
    const bool frame_generation_enabled,
    const bool virtual_render_surface,
    const bool direct_capture_active) noexcept
{
    return {
        frame_generation_enabled,
        frame_generation_enabled && !virtual_render_surface,
        !virtual_render_surface &&
            (frame_generation_enabled || direct_capture_active)};
}
}
