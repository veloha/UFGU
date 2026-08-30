#pragma once

#include <cstdint>

namespace mfgdlss::streamline
{
enum class FrameGenerationSuspensionCause : std::uint32_t
{
    none,
    deterministic_gate,
    unhealthy_frame
};

class FrameGenerationRecoveryPolicy final
{
public:
    static constexpr std::uint32_t kGuardedRecoveryFrames{60U};
    static constexpr std::uint32_t kForcedRecoveryFrames{900U};

    void reset() noexcept
    {
        desired_multiplier_ = 1U;
        mark_active();
    }

    void remember_multiplier(const std::uint32_t multiplier) noexcept
    {
        if (multiplier >= 2U) {
            desired_multiplier_ = multiplier;
        }
    }

    [[nodiscard]] bool enter_deterministic_gate() noexcept
    {
        const auto transitioned =
            cause_ == FrameGenerationSuspensionCause::none;
        if (transitioned) {
            cause_ = FrameGenerationSuspensionCause::deterministic_gate;
        }
        recovery_frames_ = 0U;
        recovery_eligible_ = false;
        return transitioned;
    }

    [[nodiscard]] bool enter_unhealthy_frame() noexcept
    {
        const auto transitioned =
            cause_ != FrameGenerationSuspensionCause::unhealthy_frame;
        cause_ = FrameGenerationSuspensionCause::unhealthy_frame;
        recovery_frames_ = 0U;
        recovery_eligible_ = false;
        failed_this_frame_ = true;
        return transitioned;
    }

    [[nodiscard]] bool note_healthy_frame() noexcept
    {
        if (cause_ != FrameGenerationSuspensionCause::unhealthy_frame ||
            recovery_eligible_) {
            return false;
        }
        ++healthy_frames_while_suspended_;
        const auto waited_long_enough =
            healthy_frames_while_suspended_ >= kForcedRecoveryFrames;
        if (failed_this_frame_) {
            failed_this_frame_ = false;
            recovery_frames_ = 0U;
            if (!waited_long_enough) {
                return false;
            }
            recovery_eligible_ = true;
            return true;
        }
        const auto ran_clean = ++recovery_frames_ >= kGuardedRecoveryFrames;
        if (!ran_clean && !waited_long_enough) {
            return false;
        }
        recovery_eligible_ = true;
        return true;
    }

    [[nodiscard]] bool recovery_was_forced() const noexcept
    {
        return recovery_eligible_ &&
            recovery_frames_ < kGuardedRecoveryFrames;
    }

    void mark_active() noexcept
    {
        cause_ = FrameGenerationSuspensionCause::none;
        recovery_frames_ = 0U;
        healthy_frames_while_suspended_ = 0U;
        recovery_eligible_ = false;
        failed_this_frame_ = false;
    }

    [[nodiscard]] bool suspended() const noexcept
    {
        return cause_ != FrameGenerationSuspensionCause::none;
    }

    [[nodiscard]] bool deterministic_gate_active() const noexcept
    {
        return cause_ ==
               FrameGenerationSuspensionCause::deterministic_gate;
    }

    [[nodiscard]] bool guarded_failure_active() const noexcept
    {
        return cause_ == FrameGenerationSuspensionCause::unhealthy_frame;
    }

    [[nodiscard]] bool guarded_recovery_eligible() const noexcept
    {
        return guarded_failure_active() && recovery_eligible_;
    }

    [[nodiscard]] std::uint32_t desired_multiplier() const noexcept
    {
        return desired_multiplier_;
    }

    [[nodiscard]] std::uint32_t recovery_frames() const noexcept
    {
        return recovery_frames_;
    }

    [[nodiscard]] FrameGenerationSuspensionCause cause() const noexcept
    {
        return cause_;
    }

private:
    std::uint32_t desired_multiplier_{1U};
    std::uint32_t recovery_frames_{};
    bool failed_this_frame_{};
    std::uint32_t healthy_frames_while_suspended_{};
    FrameGenerationSuspensionCause cause_{
        FrameGenerationSuspensionCause::none};
    bool recovery_eligible_{};
};
}
