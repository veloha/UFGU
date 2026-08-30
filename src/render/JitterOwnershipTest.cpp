

#include "render/JitterOwnership.hpp"

#include <cstdio>
#include <string_view>

namespace
{
using namespace mfgdlss::render;

int failures = 0;
int checks = 0;

void check(const bool condition, const std::string_view what)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL  %.*s\n",
            static_cast<int>(what.size()), what.data());
        return;
    }
    std::printf("PASS  %.*s\n",
        static_cast<int>(what.size()), what.data());
}

void test_engine_wins_whenever_it_applies_an_offset()
{
    JitterOwnershipState engine{};
    engine.upscaler_active = true;
    engine.offset_requested = true;
    engine.engine_applied_jitter = true;

    check(
        select_jitter_owner(engine, false) == JitterOwner::engine,
        "the engine owns the jitter once it measurably applies one");
    check(
        select_jitter_owner(engine, true) == JitterOwner::engine,
        "and still owns it even when the injector also succeeded");
    check(
        !injection_permitted(engine),
        "THE BUFFER PATCH STANDS DOWN once the engine owns the jitter");
    check(
        select_temporal_aa_policy(engine) ==
            TemporalAaPolicy::engine_jitter_without_resolve,
        "and the jitter path is kept enabled with the resolve off");
}

void test_the_buffer_patch_is_retired_in_every_state()
{

    JitterOwnershipState no_engine{};
    no_engine.upscaler_active = true;
    no_engine.offset_requested = true;
    no_engine.engine_applied_jitter = false;
    check(
        !injection_permitted(no_engine),
        "THE BUFFER PATCH IS RETIRED even when the engine applies nothing "
        "and an offset was requested -- the state it used to run in");

    JitterOwnershipState engine{};
    engine.upscaler_active = true;
    engine.offset_requested = true;
    engine.engine_applied_jitter = true;
    check(
        !injection_permitted(engine),
        "and it is still refused once the engine owns the jitter");

    JitterOwnershipState abandoned{};
    abandoned.upscaler_active = true;
    abandoned.offset_requested = true;
    abandoned.split_abandoned = true;
    abandoned.frames_since_split_enabled = kSplitProbeFrames + 1U;
    check(
        !injection_permitted(abandoned),
        "and after the TAA split is abandoned, which is when the old rule "
        "would have permitted it indefinitely");

    check(
        select_jitter_owner(no_engine, false) == JitterOwner::none,
        "a refused patch reports NO jitter rather than pretending");

    const std::string_view described =
        describe(JitterOwner::plugin_injection);
    check(
        described.find("UNPROVEN") != std::string_view::npos,
        "the plugin patch remains UNPROVEN until world-raster evidence confirms it");
}

void test_nothing_is_claimed_when_nothing_is_asked_for()
{
    JitterOwnershipState idle{};
    idle.upscaler_active = true;
    idle.offset_requested = false;
    check(
        select_jitter_owner(idle, true) == JitterOwner::none,
        "a frame with no requested offset reports no jitter");
    check(
        !injection_permitted(idle),
        "and nothing is written into the buffer");

    JitterOwnershipState off{};
    check(
        select_temporal_aa_policy(off) ==
            TemporalAaPolicy::restore_user_setting,
        "with no upscaler the user's own TAA setting is restored");
    check(
        !injection_permitted(off),
        "and the patch is not permitted");
}

void test_the_split_is_abandoned_if_it_does_not_work()
{
    JitterOwnershipState probing{};
    probing.upscaler_active = true;
    probing.offset_requested = true;
    probing.engine_applied_jitter = false;
    probing.frames_since_split_enabled = 1U;

    check(
        select_temporal_aa_policy(probing) ==
            TemporalAaPolicy::engine_jitter_without_resolve,
        "the split is attempted while the probe is still running");
    check(
        !split_probe_exhausted(probing),
        "and one frame does not condemn it");

    probing.frames_since_split_enabled = kSplitProbeFrames;
    check(
        split_probe_exhausted(probing),
        "the probe expires after a bounded number of frames");
    check(
        select_temporal_aa_policy(probing) ==
            TemporalAaPolicy::fully_disabled,
        "AND THE GAME IS PUT BACK to the previously shipped TAA state "
        "rather than left in an unproven one");

    probing.split_abandoned = true;
    probing.engine_applied_jitter = true;
    check(
        select_temporal_aa_policy(probing) ==
            TemporalAaPolicy::fully_disabled,
        "an abandoned split stays abandoned for the session");
}

void test_log_budget_has_a_ceiling_not_just_a_state_check()
{

    BoundedLogBudget budget{4U};
    int admitted = 0;
    for (int frame = 0; frame < 1000; ++frame) {
        if (budget.admit(frame % 2 == 0 ? 1U : 2U)) {
            ++admitted;
        }
    }
    check(
        admitted == 4,
        "AN ALTERNATING VERDICT CANNOT LOG FOREVER: 1000 frames of the "
        "alternating pattern admit exactly the budget");
    check(budget.exhausted(), "and the budget reports itself exhausted");

    BoundedLogBudget quiet{4U};
    int repeats = 0;
    for (int frame = 0; frame < 100; ++frame) {
        if (quiet.admit(7U)) {
            ++repeats;
        }
    }
    check(
        repeats == 1,
        "an unchanging state still logs exactly once");

    BoundedLogBudget reset_case{2U};
    static_cast<void>(reset_case.admit(1U));
    static_cast<void>(reset_case.admit(2U));
    check(reset_case.exhausted(), "the budget is spent");
    reset_case.reset();
    check(
        !reset_case.exhausted() && reset_case.spent() == 0U,
        "and a shutdown resets it for the next session");
    check(
        reset_case.admit(1U),
        "which admits the first line again after a restart");
}
}

int main()
{
    test_engine_wins_whenever_it_applies_an_offset();
    test_the_buffer_patch_is_retired_in_every_state();
    test_nothing_is_claimed_when_nothing_is_asked_for();
    test_the_split_is_abandoned_if_it_does_not_work();
    test_log_budget_has_a_ceiling_not_just_a_state_check();

    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
