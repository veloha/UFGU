#include "render/NFramePresentationState.hpp"

#include <cstdio>

using namespace mfgdlss::render;

namespace
{
int failures = 0;

void check(const bool condition, const char* const what)
{
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

[[nodiscard]] constexpr NFramePresentationState steady_state(
    const std::uint32_t multiplier,
    const std::uint64_t interval_us)
{
    NFramePresentationState state{};
    state.note_multiplier(multiplier);
    state.note_real_frame(1000ULL);
    state.note_real_frame(1000ULL + interval_us);
    return state;
}
}

int main()
{
    {
        NFramePresentationState state{};
        state.note_multiplier(6U);
        state.note_real_frame(1000ULL);
        check(!state.can_generate(),
              "the very first real frame has no predecessor, so nothing can "
              "be interpolated and only the real frame may present");
        check(state.step_count() == 1U,
              "with no predecessor there is exactly one step");
        check(state.step(0).kind == PresentationStepKind::real,
              "that single step is the real frame");
    }

    {
        auto state = steady_state(6U, 16667ULL);
        check(state.can_generate(),
              "two real frames and a supported multiplier is enough to "
              "generate");
        check(state.step_count() == 6U,
              "a 6x multiplier presents six frames per real frame");
        check(state.base_interval_us() == 16667ULL,
              "the base interval is measured between real frames, not "
              "assumed from a configured cap");
        for (std::uint32_t index = 0; index + 1U < state.step_count();
             ++index) {
            check(state.step(index).kind == PresentationStepKind::generated,
                  "every step before the last is generated");
            check(phase_is_interior(state.step(index).phase),
                  "a generated step must sit strictly between the two real "
                  "frames; a phase of exactly 0 or 1 would duplicate one of "
                  "them");
        }
        const auto last = state.step(state.step_count() - 1U);
        check(last.kind == PresentationStepKind::real,
              "the real frame presents LAST, after the frames interpolated "
              "towards it");
        check(last.deadline_us == 1000ULL + 16667ULL,
              "the real frame's deadline is its own render time");
    }

    {
        auto state = steady_state(6U, 16667ULL);
        const auto first = state.step(0).deadline_us;
        const auto second = state.step(1).deadline_us;
        check(second > first,
              "generated deadlines increase, so the pacer never asks for two "
              "frames at the same instant");
        check(state.step(0).deadline_us > 1000ULL,
              "the first generated frame is due after the PREVIOUS real "
              "frame, not before it");
    }

    {
        auto state = steady_state(6U, 16667ULL);
        check(state.can_generate(), "precondition");
        state.note_multiplier(4U);
        check(!state.can_generate(),
              "a multiplier change invalidates the held predecessor, because "
              "the cadence it was captured for no longer applies");
        check(state.last_reset() == PresentationResetCause::multiplier_changed,
              "the reset records WHY it happened so a log can attribute it");
    }

    {
        auto state = steady_state(6U, 16667ULL);
        state.reset(PresentationResetCause::history_reset);
        check(!state.can_generate(),
              "a history reset must stop generation; interpolating across a "
              "camera cut is exactly the artefact this project spent ticks "
              "chasing");
    }

    {
        auto state = steady_state(5U, 16667ULL);
        check(!state.can_generate(),
              "5x is not a supported multiplier and must never generate; the "
              "valid set is 2, 3, 4 and 6");
    }

    {
        NFramePresentationState state{};
        state.note_multiplier(6U);
        state.note_real_frame(1000ULL);
        state.note_real_frame(1000ULL);
        check(!state.can_generate(),
              "two frames with the same timestamp give a zero base interval, "
              "which would divide by zero when spacing deadlines");
    }

    std::printf("=== NFramePresentationStateTest: %d failed ===\n", failures);
    return failures == 0 ? 0 : 1;
}
