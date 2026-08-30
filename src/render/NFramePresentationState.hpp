#pragma once

#include "render/NFramePresentationPlan.hpp"

#include <cstdint>

namespace mfgdlss::render
{

enum class PresentationStepKind : std::uint32_t
{
    generated,
    real
};

struct PresentationStep final
{
    PresentationStepKind kind{PresentationStepKind::real};
    std::uint32_t frame_index{};
    float phase{};
    std::uint64_t deadline_us{};
};

enum class PresentationResetCause : std::uint32_t
{
    none,
    first_frame,
    multiplier_changed,
    history_reset,
    device_lost
};

class NFramePresentationState final
{
public:
    constexpr void reset(const PresentationResetCause cause) noexcept
    {
        have_previous_ = false;
        previous_us_ = 0ULL;
        current_us_ = 0ULL;
        last_reset_ = cause;
    }

    constexpr void note_multiplier(const std::uint32_t multiplier) noexcept
    {
        if (multiplier == multiplier_) {
            return;
        }
        multiplier_ = multiplier;
        reset(PresentationResetCause::multiplier_changed);
    }

    constexpr void note_real_frame(const std::uint64_t timestamp_us) noexcept
    {
        if (current_us_ != 0ULL && timestamp_us > current_us_) {
            previous_us_ = current_us_;
            have_previous_ = true;
        } else if (current_us_ == 0ULL) {
            last_reset_ = PresentationResetCause::first_frame;
        }
        current_us_ = timestamp_us;
    }

    [[nodiscard]] constexpr bool can_generate() const noexcept
    {
        return have_previous_ && multiplier_is_supported(multiplier_) &&
            multiplier_ > 1U && base_interval_us() != 0ULL;
    }

    [[nodiscard]] constexpr std::uint64_t base_interval_us() const noexcept
    {
        return have_previous_ && current_us_ > previous_us_ ?
            current_us_ - previous_us_ : 0ULL;
    }

    [[nodiscard]] constexpr std::uint32_t step_count() const noexcept
    {
        return can_generate() ? multiplier_ : 1U;
    }

    [[nodiscard]] constexpr PresentationStep step(
        const std::uint32_t index) const noexcept
    {
        PresentationStep out{};
        if (index >= step_count()) {
            return out;
        }
        if (!can_generate() || index + 1U == step_count()) {
            out.kind = PresentationStepKind::real;
            out.frame_index = index;
            out.phase = 1.0F;
            out.deadline_us = current_us_;
            return out;
        }
        out.kind = PresentationStepKind::generated;
        out.frame_index = index + 1U;
        out.phase = compute_phase_for_frame(index + 1U, multiplier_);
        out.deadline_us = compute_present_deadline_us(
            previous_us_, index + 1U, base_interval_us(), multiplier_);
        return out;
    }

    [[nodiscard]] constexpr std::uint32_t multiplier() const noexcept
    {
        return multiplier_;
    }

    [[nodiscard]] constexpr PresentationResetCause last_reset()
        const noexcept
    {
        return last_reset_;
    }

private:
    std::uint32_t multiplier_{1U};
    bool have_previous_{};
    std::uint64_t previous_us_{};
    std::uint64_t current_us_{};
    PresentationResetCause last_reset_{PresentationResetCause::first_frame};
};
}
