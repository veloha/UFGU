#pragma once

#include <cstdint>

namespace mfgdlss::render
{
inline constexpr std::uint32_t kMinimumPresentationCap = 20;
inline constexpr std::uint32_t kMaximumPresentationCap = 1000;
inline constexpr std::uint32_t kMultiplierStabilityFrames = 30;

enum class PacingReason : std::uint32_t
{

    uncapped_no_refresh_reported,
    direct_streamline_presentation_limit,
    clamped_below_streamline_minimum,
    clamped_above_streamline_maximum,

    paced_to_display_refresh,
};

[[nodiscard]] constexpr const char* describe(const PacingReason reason) noexcept
{
    switch (reason) {
    case PacingReason::uncapped_no_refresh_reported:
        return "unpaced: Output FPS is Off and no display refresh was reported";
    case PacingReason::direct_streamline_presentation_limit:
        return "applied directly to Streamline's final presentation cadence";
    case PacingReason::clamped_below_streamline_minimum:
        return "clamped up to the lowest supported presentation limit";
    case PacingReason::clamped_above_streamline_maximum:
        return "clamped down to the highest supported presentation limit";
    case PacingReason::paced_to_display_refresh:
        return "Output FPS is Off, so paced to the reported display refresh";
    }
    return "unknown";
}

struct PacingDecision
{
    std::uint32_t presentation_cap_fps{};
    PacingReason reason{PacingReason::uncapped_no_refresh_reported};
    bool target_achievable{true};
};

[[nodiscard]] constexpr PacingDecision decide_pacing(
    const std::uint32_t requested_output_fps,
    const std::uint32_t display_refresh_hz) noexcept
{
    if (requested_output_fps == 0) {
        if (display_refresh_hz == 0) {

            return {0, PacingReason::uncapped_no_refresh_reported, true};
        }
        if (display_refresh_hz < kMinimumPresentationCap) {
            return {
                kMinimumPresentationCap,
                PacingReason::clamped_below_streamline_minimum,
                false};
        }
        if (display_refresh_hz > kMaximumPresentationCap) {
            return {
                kMaximumPresentationCap,
                PacingReason::clamped_above_streamline_maximum,
                false};
        }
        return {
            display_refresh_hz,
            PacingReason::paced_to_display_refresh,
            true};
    }

    if (requested_output_fps < kMinimumPresentationCap) {
        return {
            kMinimumPresentationCap,
            PacingReason::clamped_below_streamline_minimum,
            false};
    }
    if (requested_output_fps > kMaximumPresentationCap) {
        return {
            kMaximumPresentationCap,
            PacingReason::clamped_above_streamline_maximum,
            false};
    }
    return {
        requested_output_fps,
        PacingReason::direct_streamline_presentation_limit,
        true};
}

enum class GenerationCadence : std::uint32_t
{

    fixed_multiplier,

    dynamic_to_display_refresh,

    dynamic_to_output_cap,
};

[[nodiscard]] constexpr const char* describe(
    const GenerationCadence cadence) noexcept
{
    switch (cadence) {
    case GenerationCadence::fixed_multiplier:
        return "fixed multiplier";
    case GenerationCadence::dynamic_to_display_refresh:
        return "dynamic, targeting the detected display refresh";
    case GenerationCadence::dynamic_to_output_cap:
        return "dynamic, targeting the selected Output FPS";
    }
    return "unknown";
}

struct CadenceDecision
{
    GenerationCadence cadence{GenerationCadence::fixed_multiplier};

    float dynamic_target_fps{};

    bool dynamic_unavailable{};
};

[[nodiscard]] constexpr CadenceDecision decide_cadence(
    const bool dynamic_selected,
    const bool dynamic_supported,
    const std::uint32_t multiplier,
    const std::uint32_t presentation_cap_fps) noexcept
{
    if (multiplier < 2) {

        return {};
    }
    if (!dynamic_selected) {
        return {};
    }
    if (!dynamic_supported) {
        return {GenerationCadence::fixed_multiplier, 0.0F, true};
    }
    if (presentation_cap_fps != 0) {
        return {
            GenerationCadence::dynamic_to_output_cap,
            static_cast<float>(presentation_cap_fps),
            false};
    }
    return {GenerationCadence::dynamic_to_display_refresh, 0.0F, false};
}

class MultiplierTracker final
{
public:
    constexpr bool observe(const std::uint32_t presented_this_frame) noexcept
    {
        if (presented_this_frame == 0) {
            return false;
        }
        if (presented_this_frame == candidate_) {
            if (streak_ < kMultiplierStabilityFrames) {
                ++streak_;
            }
        } else {
            candidate_ = presented_this_frame;
            streak_ = 1;
        }
        if (streak_ >= kMultiplierStabilityFrames && stable_ != candidate_) {
            stable_ = candidate_;
            ++transitions_;
            return true;
        }
        return false;
    }

    [[nodiscard]] constexpr std::uint32_t stable() const noexcept
    {
        return stable_;
    }

    [[nodiscard]] constexpr std::uint32_t candidate() const noexcept
    {
        return candidate_;
    }

    [[nodiscard]] constexpr std::uint32_t streak() const noexcept
    {
        return streak_;
    }

    [[nodiscard]] constexpr std::uint32_t transitions() const noexcept
    {
        return transitions_;
    }

    constexpr void reset() noexcept
    {
        candidate_ = 0;
        streak_ = 0;
        stable_ = 0;
        transitions_ = 0;
    }

private:
    std::uint32_t candidate_{};
    std::uint32_t streak_{};
    std::uint32_t stable_{};
    std::uint32_t transitions_{};
};
}
