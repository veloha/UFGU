#include "streamline/FrameGenerationRecoveryPolicy.hpp"

static_assert(
    mfgdlss::streamline::FrameGenerationRecoveryPolicy::
            kForcedRecoveryFrames >
        mfgdlss::streamline::FrameGenerationRecoveryPolicy::
            kGuardedRecoveryFrames,
    "the watchdog is a fallback, not a shortcut. A clean 60 frame run must "
    "still be the normal way back");

int main()
{
    using mfgdlss::streamline::FrameGenerationRecoveryPolicy;
    using mfgdlss::streamline::FrameGenerationSuspensionCause;

    int failures = 0;
    const auto check = [&failures](const bool condition) {
        failures += condition ? 0 : 1;
    };

    FrameGenerationRecoveryPolicy policy;
    check(!policy.suspended());
    check(policy.desired_multiplier() == 1U);

    policy.remember_multiplier(4U);
    check(policy.enter_deterministic_gate());
    check(policy.suspended());
    check(policy.deterministic_gate_active());
    check(!policy.guarded_failure_active());
    check(!policy.note_healthy_frame());

    policy.remember_multiplier(6U);
    check(!policy.enter_deterministic_gate());
    check(policy.desired_multiplier() == 6U);
    policy.mark_active();
    check(!policy.suspended());
    check(policy.desired_multiplier() == 6U);

    check(policy.enter_unhealthy_frame());
    static_cast<void>(policy.note_healthy_frame());
    for (std::uint32_t frame = 1U;
         frame < FrameGenerationRecoveryPolicy::kGuardedRecoveryFrames;
         ++frame) {
        check(!policy.note_healthy_frame());
    }
    check(policy.note_healthy_frame());
    check(policy.guarded_recovery_eligible());

    static_cast<void>(policy.enter_unhealthy_frame());
    static_cast<void>(policy.note_healthy_frame());
    for (std::uint32_t frame = 0U; frame < 15U; ++frame) {
        static_cast<void>(policy.note_healthy_frame());
    }
    check(policy.recovery_frames() == 15U);
    check(!policy.enter_deterministic_gate());
    check(policy.cause() ==
          FrameGenerationSuspensionCause::unhealthy_frame);
    check(policy.recovery_frames() == 0U);
    check(!policy.guarded_recovery_eligible());

    policy.reset();
    check(!policy.suspended());
    check(policy.desired_multiplier() == 1U);

    {
        FrameGenerationRecoveryPolicy never_enabled;
        check(never_enabled.desired_multiplier() == 1U);
        never_enabled.remember_multiplier(1U);
        check(never_enabled.desired_multiplier() == 1U);
        never_enabled.remember_multiplier(0U);
        check(never_enabled.desired_multiplier() == 1U);

        static_cast<void>(never_enabled.enter_unhealthy_frame());
        static_cast<void>(never_enabled.note_healthy_frame());
        for (std::uint32_t frame = 0U;
             frame < FrameGenerationRecoveryPolicy::kGuardedRecoveryFrames;
             ++frame) {
            static_cast<void>(never_enabled.note_healthy_frame());
        }
        check(never_enabled.guarded_recovery_eligible());

        never_enabled.mark_active();
        check(!never_enabled.guarded_failure_active());
        check(!never_enabled.guarded_recovery_eligible());
        for (std::uint32_t frame = 0U;
             frame < FrameGenerationRecoveryPolicy::kGuardedRecoveryFrames * 4U;
             ++frame) {
            check(!never_enabled.note_healthy_frame());
        }
        check(!never_enabled.guarded_recovery_eligible());
    }

    {
        FrameGenerationRecoveryPolicy combat;
        static_cast<void>(combat.enter_unhealthy_frame());
        auto recovered = false;
        const auto ceiling =
            FrameGenerationRecoveryPolicy::kForcedRecoveryFrames * 4U;
        for (std::uint32_t frame = 0U; frame < ceiling; ++frame) {
            if (combat.note_healthy_frame()) {
                recovered = true;
                break;
            }
            static_cast<void>(combat.enter_unhealthy_frame());
        }
        check(recovered);
        check(combat.recovery_was_forced());
    }

    {
        FrameGenerationRecoveryPolicy cleared;
        static_cast<void>(cleared.enter_unhealthy_frame());
        static_cast<void>(cleared.note_healthy_frame());
        auto recovered = false;
        std::uint32_t clean_frames = 0U;
        while (clean_frames <
               FrameGenerationRecoveryPolicy::kForcedRecoveryFrames - 1U) {
            ++clean_frames;
            if (cleared.note_healthy_frame()) {
                recovered = true;
                break;
            }
        }
        check(recovered);
        check(!cleared.recovery_was_forced());
        check(clean_frames ==
              FrameGenerationRecoveryPolicy::kGuardedRecoveryFrames);
    }

    {
        FrameGenerationRecoveryPolicy oscillating;
        auto guarded_fired = false;
        for (std::uint32_t frame = 0U;
             frame < FrameGenerationRecoveryPolicy::kForcedRecoveryFrames - 1U;
             ++frame) {
            static_cast<void>(oscillating.enter_unhealthy_frame());
            if (oscillating.note_healthy_frame()) {
                guarded_fired = true;
                break;
            }
        }
        check(!guarded_fired);
        check(oscillating.recovery_frames() == 0U);
    }

    return failures;
}
