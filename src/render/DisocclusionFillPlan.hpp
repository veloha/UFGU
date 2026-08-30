#pragma once

#include <cstdint>

namespace mfgdlss::render
{
inline constexpr std::uint32_t kHoleSentinel = 0xFFFFFFFFU;
inline constexpr std::uint32_t kFillMaximumIterations = 16U;

[[nodiscard]] constexpr std::uint32_t fill_radius_covered(
    const std::uint32_t iterations) noexcept
{
    return iterations;
}

[[nodiscard]] constexpr std::uint32_t compute_fill_iterations(
    const std::uint32_t widest_hole_radius) noexcept
{
    if (widest_hole_radius == 0U) {
        return 0U;
    }
    const auto needed = widest_hole_radius + 1U;
    return needed > kFillMaximumIterations ? kFillMaximumIterations : needed;
}

[[nodiscard]] constexpr bool hole_is_fillable(
    const std::uint32_t radius,
    const std::uint32_t iterations) noexcept
{
    return radius + 1U <= fill_radius_covered(iterations);
}

[[nodiscard]] constexpr std::uint32_t hole_radius_from_motion(
    const float peak_motion_pixels,
    const float phase) noexcept
{
    const auto displaced = peak_motion_pixels * phase;
    if (displaced <= 0.0F) {
        return 0U;
    }
    const auto rounded = static_cast<std::uint32_t>(displaced + 0.5F);
    return rounded == 0U ? 1U : rounded;
}

[[nodiscard]] constexpr bool fill_preserves_valid_texels() noexcept
{
    return true;
}
}
