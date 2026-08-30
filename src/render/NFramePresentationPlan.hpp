#pragma once

#include <cstdint>

namespace mfgdlss::render
{
inline constexpr std::uint32_t kPresentationMinimumBackBuffers = 3U;
inline constexpr std::uint32_t kPresentationMaximumBackBuffers = 16U;
inline constexpr std::uint32_t kPresentationMaximumMultiplier = 6U;

[[nodiscard]] constexpr bool multiplier_is_supported(
    const std::uint32_t multiplier) noexcept
{
    return multiplier == 1U || multiplier == 2U || multiplier == 3U ||
        multiplier == 4U || multiplier == 6U;
}

[[nodiscard]] constexpr std::uint32_t generated_frames_for(
    const std::uint32_t multiplier) noexcept
{
    return multiplier <= 1U ? 0U : multiplier - 1U;
}

[[nodiscard]] constexpr std::uint32_t compute_back_buffer_count(
    const std::uint32_t multiplier) noexcept
{
    const auto queued = multiplier <= 1U ? 1U : multiplier;
    const auto needed = queued + 1U;
    if (needed < kPresentationMinimumBackBuffers) {
        return kPresentationMinimumBackBuffers;
    }
    return needed > kPresentationMaximumBackBuffers ?
        kPresentationMaximumBackBuffers :
        needed;
}

[[nodiscard]] constexpr float compute_phase_for_frame(
    const std::uint32_t frame_index,
    const std::uint32_t multiplier) noexcept
{
    if (multiplier <= 1U || frame_index == 0U) {
        return 0.0F;
    }
    if (frame_index >= multiplier) {
        return 0.0F;
    }
    return static_cast<float>(frame_index) / static_cast<float>(multiplier);
}

[[nodiscard]] constexpr bool phase_is_interior(const float phase) noexcept
{
    return phase > 0.0F && phase < 1.0F;
}

[[nodiscard]] constexpr std::uint64_t compute_frame_interval_us(
    const std::uint64_t base_interval_us,
    const std::uint32_t multiplier) noexcept
{
    if (multiplier <= 1U || base_interval_us == 0ULL) {
        return base_interval_us;
    }
    return base_interval_us / multiplier;
}

[[nodiscard]] constexpr std::uint64_t compute_present_deadline_us(
    const std::uint64_t rendered_at_us,
    const std::uint32_t frame_index,
    const std::uint64_t base_interval_us,
    const std::uint32_t multiplier) noexcept
{
    if (multiplier <= 1U) {
        return rendered_at_us +
            base_interval_us * static_cast<std::uint64_t>(frame_index);
    }
    return rendered_at_us +
        (base_interval_us * static_cast<std::uint64_t>(frame_index)) /
        static_cast<std::uint64_t>(multiplier);
}

[[nodiscard]] constexpr std::uint64_t compute_gap_us(
    const std::uint32_t frame_index,
    const std::uint64_t base_interval_us,
    const std::uint32_t multiplier) noexcept
{
    if (frame_index == 0U) {
        return 0ULL;
    }
    return compute_present_deadline_us(
               0ULL, frame_index, base_interval_us, multiplier) -
        compute_present_deadline_us(
               0ULL, frame_index - 1U, base_interval_us, multiplier);
}

[[nodiscard]] constexpr bool group_tiles_exactly(
    const std::uint64_t base_interval_us,
    const std::uint32_t multiplier) noexcept
{
    return compute_present_deadline_us(
               0ULL, multiplier, base_interval_us, multiplier) ==
        base_interval_us;
}

[[nodiscard]] constexpr bool pacing_is_even(
    const std::uint64_t base_interval_us,
    const std::uint32_t multiplier) noexcept
{
    if (multiplier <= 1U) {
        return true;
    }
    const auto step = compute_frame_interval_us(base_interval_us, multiplier);
    return step * multiplier == base_interval_us;
}
}
