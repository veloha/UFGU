#pragma once

#include <cstdint>

namespace mfgdlss::render
{
inline constexpr float kProjectionMinimumBaseFps = 5.0F;
inline constexpr float kThinBaseFps = 45.0F;

struct MultiplierProjection final
{
    float output_fps{};
    float implied_base_fps{};
    bool cap_binds{};
    bool base_is_thin{};
};

[[nodiscard]] constexpr std::uint32_t reflex_limit_preserving_base(
    const std::uint32_t output_frame_limit,
    const std::uint32_t base_frame_limit,
    const std::uint32_t multiplier) noexcept
{
    if (output_frame_limit == 0U) {
        return 0U;
    }
    if (multiplier < 2U || base_frame_limit == 0U) {
        return output_frame_limit;
    }
    return base_frame_limit * multiplier;
}

[[nodiscard]] constexpr MultiplierProjection project_multiplier(
    const float base_fps,
    const std::uint32_t multiplier,
    const std::uint32_t output_cap) noexcept
{
    MultiplierProjection projection{};
    const auto factor =
        static_cast<float>(multiplier == 0U ? 1U : multiplier);
    const auto uncapped = base_fps * factor;
    const auto cap = static_cast<float>(output_cap);

    if (output_cap != 0U && uncapped > cap) {
        projection.cap_binds = true;
        projection.output_fps = cap;
        projection.implied_base_fps = cap / factor;
    } else {
        projection.output_fps = uncapped;
        projection.implied_base_fps = base_fps;
    }

    projection.base_is_thin = multiplier > 1U &&
        projection.implied_base_fps < kThinBaseFps;
    return projection;
}

[[nodiscard]] constexpr float reference_base_fps(
    const float measured_base_fps,
    const std::uint32_t active_multiplier,
    const std::uint32_t output_cap,
    const std::uint32_t base_cap) noexcept
{
    if (base_cap == 0U) {
        return measured_base_fps;
    }
    const auto ceiling = static_cast<float>(base_cap);
    if (measured_base_fps >= ceiling) {
        return measured_base_fps;
    }
    const auto factor =
        static_cast<float>(active_multiplier == 0U ? 1U : active_multiplier);
    if (output_cap != 0U &&
        measured_base_fps * factor >= static_cast<float>(output_cap)) {
        return ceiling;
    }
    return measured_base_fps;
}

[[nodiscard]] constexpr bool projection_is_measurable(
    const float base_fps) noexcept
{
    return base_fps >= kProjectionMinimumBaseFps;
}
}
