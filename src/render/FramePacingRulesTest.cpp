

#include "render/FramePacingRules.hpp"

#include <cstdio>
#include <string>

namespace
{
int g_passed = 0;
int g_failed = 0;

void check(const bool condition, const std::string& what)
{
    if (condition) {
        ++g_passed;
        std::printf("  PASS  %s\n", what.c_str());
    } else {
        ++g_failed;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}

using namespace mfgdlss::render;

static_assert(
    decide_pacing(240, 240).presentation_cap_fps == 240,
    "a 240 FPS output target must be handed directly to Streamline");
static_assert(
    decide_pacing(0, 240).presentation_cap_fps == 240,
    "Output FPS Off must pace to the display refresh, not disable pacing");
static_assert(
    decide_pacing(0, 480).presentation_cap_fps == 480,
    "the paced rate follows the display, so a 480 Hz panel gets 480");

void test_direct_output_semantics()
{
    std::printf("-- direct final-presentation limits --\n");

    const auto unknown_display = decide_pacing(0, 0);
    check(unknown_display.presentation_cap_fps == 0,
          "a display that reports no refresh leaves presentation unpaced");
    check(unknown_display.reason ==
              PacingReason::uncapped_no_refresh_reported,
          "the unpaced state names the missing refresh as its cause");

    for (const auto requested : {30U, 40U, 45U, 60U, 72U, 80U, 90U,
                                 120U, 144U, 165U, 240U, 360U, 480U}) {
        const auto decision = decide_pacing(requested, 240);
        check(decision.presentation_cap_fps == requested,
              std::to_string(requested) +
                  " is passed through without multiplier division");
        check(decision.reason ==
                  PacingReason::direct_streamline_presentation_limit,
              std::to_string(requested) +
                  " is identified as a final presentation limit");
        check(decision.target_achievable,
              std::to_string(requested) + " needs no clamp");
    }

    std::printf("\n");
}

void test_the_observed_regression_cannot_return()
{
    std::printf("-- fixed-multiplier pacing regression --\n");

    const auto selected_240 = decide_pacing(240, 240);
    check(selected_240.presentation_cap_fps != 40,
          "240 can never become 40 when MFG is 6x");
    check(selected_240.presentation_cap_fps == 240,
          "the failed 7-render/40-output state is structurally excluded");

    for (const auto multiplier : {1U, 2U, 3U, 4U, 6U}) {
        static_cast<void>(multiplier);
        check(decide_pacing(240, 240).presentation_cap_fps == 240,
              "240 remains 240 across every MFG multiplier");
    }

    std::printf("\n");
}

void test_output_fps_off_is_paced_not_unpaced()
{
    std::printf("-- Output FPS Off paces to the display --\n");

    for (const auto refresh : {60U, 75U, 120U, 144U, 165U, 240U, 360U, 480U}) {
        const auto decision = decide_pacing(0, refresh);
        check(decision.presentation_cap_fps == refresh,
              "Output FPS Off on a " + std::to_string(refresh) +
                  " Hz display paces to " + std::to_string(refresh));
        check(decision.reason == PacingReason::paced_to_display_refresh,
              std::to_string(refresh) +
                  " Hz pacing is labelled as following the display");
        check(decision.presentation_cap_fps != 0,
              "Off never means unpaced once a refresh is known");
        check(decision.target_achievable,
              "pacing to the panel's own rate is always achievable");
    }

    check(decide_pacing(0, 480).presentation_cap_fps !=
              decide_pacing(0, 240).presentation_cap_fps,
          "two different displays must not receive the same paced rate");
    check(decide_pacing(0, 60).presentation_cap_fps == 60,
          "a 60 Hz panel is paced to 60, not to anything higher");

    check(decide_pacing(480, 240).presentation_cap_fps == 480,
          "a typed 480 survives on a 240 Hz display");
    check(decide_pacing(30, 240).presentation_cap_fps == 30,
          "a typed value below the refresh is obeyed exactly");
    check(decide_pacing(120, 0).presentation_cap_fps == 120,
          "a typed value works even when no refresh was reported");

    std::printf("\n");
}

void test_bounds()
{
    std::printf("-- bounded Streamline input --\n");

    const auto low = decide_pacing(1, 240);
    check(low.presentation_cap_fps == kMinimumPresentationCap,
          "a nonzero value below the supported floor clamps to 20");
    check(low.reason == PacingReason::clamped_below_streamline_minimum &&
              !low.target_achievable,
          "the low clamp is reported honestly");

    const auto high = decide_pacing(6000, 240);
    check(high.presentation_cap_fps == kMaximumPresentationCap,
          "a value above the supported ceiling clamps to 1000");
    check(high.reason == PacingReason::clamped_above_streamline_maximum &&
              !high.target_achievable,
          "the high clamp is reported honestly");

    check(decide_pacing(0, 6000).presentation_cap_fps ==
              kMaximumPresentationCap,
          "an implausible reported refresh clamps rather than passing through");
    check(decide_pacing(0, 5).presentation_cap_fps ==
              kMinimumPresentationCap,
          "an implausibly low reported refresh clamps up to the floor");

    std::printf("\n");
}

void test_multiplier_is_telemetry_only()
{
    std::printf("-- measured multiplier telemetry --\n");

    MultiplierTracker tracker;
    check(tracker.stable() == 0,
          "no multiplier is invented before measurement");
    for (std::uint32_t i = 0; i < kMultiplierStabilityFrames - 1; ++i) {
        static_cast<void>(tracker.observe(6));
    }
    check(tracker.stable() == 0,
          "29 samples do not establish a stable multiplier");
    check(tracker.observe(6) && tracker.stable() == 6,
          "the 30th stable sample records 6x telemetry");

    for (std::uint32_t i = 0; i < 10; ++i) {
        static_cast<void>(tracker.observe(1));
    }
    check(tracker.stable() == 6,
          "a short suspension does not falsify the stable measurement");
    check(decide_pacing(240, 240).presentation_cap_fps == 240,
          "telemetry transitions cannot alter the presentation cap");

    for (std::uint32_t i = 0; i < kMultiplierStabilityFrames; ++i) {
        static_cast<void>(tracker.observe(2));
    }
    check(tracker.stable() == 2 && tracker.transitions() == 2,
          "a sustained 2x transition remains available to telemetry");
    check(decide_pacing(240, 240).presentation_cap_fps == 240,
          "even a sustained multiplier change leaves the cap untouched");

    static_cast<void>(tracker.observe(0));
    check(tracker.stable() == 2,
          "a failed presentation query does not fabricate a multiplier");
    tracker.reset();
    check(tracker.stable() == 0 && tracker.transitions() == 0,
          "reset clears telemetry state");

    std::printf("\n");
}

void test_dynamic_cadence_selection()
{
    std::printf("-- dynamic multi frame generation cadence --\n");

    const auto unsupported = decide_cadence(true, false, 6, 0);
    check(unsupported.cadence == GenerationCadence::fixed_multiplier,
          "dynamic is never chosen when the runtime says it is unsupported");
    check(unsupported.dynamic_unavailable,
          "an unavailable dynamic request is reported, not swallowed");
    check(unsupported.dynamic_target_fps == 0.0F,
          "no target rate is invented for a cadence that will not be used");

    const auto uncapped = decide_cadence(true, true, 6, 0);
    check(uncapped.cadence == GenerationCadence::dynamic_to_display_refresh,
          "an uncapped selection targets the display refresh");
    check(uncapped.dynamic_target_fps == 0.0F,
          "0.0f is passed so DLSS-G auto-detects the refresh itself");
    check(!uncapped.dynamic_unavailable,
          "a satisfied dynamic request reports nothing unavailable");

    for (const auto refresh : {60U, 120U, 144U, 240U, 360U, 480U}) {
        const auto decision = decide_cadence(true, true, 6, refresh);
        check(decision.cadence == GenerationCadence::dynamic_to_output_cap,
              std::to_string(refresh) +
                  " FPS selected becomes an explicit dynamic target");
        check(decision.dynamic_target_fps ==
                  static_cast<float>(refresh),
              std::to_string(refresh) +
                  " reaches dynamicTargetFrameRate unmodified");
    }

    const auto fixed = decide_cadence(false, true, 6, 0);
    check(fixed.cadence == GenerationCadence::fixed_multiplier,
          "selecting Fixed keeps fixed-multiplier cadence available");
    check(!fixed.dynamic_unavailable,
          "choosing Fixed is not reported as dynamic being unavailable");

    for (const auto off : {0U, 1U}) {
        const auto decision = decide_cadence(true, false, off, 0);
        check(decision.cadence == GenerationCadence::fixed_multiplier,
              "no cadence is chosen while generation is off");
        check(!decision.dynamic_unavailable,
              "generation being off never reports dynamic as unavailable");
    }

    std::printf("\n");
}

void test_cadence_does_not_disturb_the_output_cap()
{
    std::printf("-- cadence and cap remain independent --\n");

    for (const auto cap : {0U, 240U, 360U, 480U}) {
        const auto dynamic_chosen = decide_cadence(true, true, 6, cap);
        const auto fixed_chosen = decide_cadence(false, true, 6, cap);
        static_cast<void>(dynamic_chosen);
        static_cast<void>(fixed_chosen);
        const auto paced = decide_pacing(cap, 240);
        check(paced.presentation_cap_fps ==
                  (cap == 0U ? 240U : cap),
              "pacing for a selected cap of " + std::to_string(cap) +
                  " is unaffected by the cadence chosen");
        check(paced.presentation_cap_fps != 0U,
              "no cadence choice can leave presentation unpaced at cap " +
                  std::to_string(cap));
    }

    std::printf("\n");
}
}

int main()
{
    std::printf("=== Frame pacing rules ===\n\n");
    test_direct_output_semantics();
    test_the_observed_regression_cannot_return();
    test_output_fps_off_is_paced_not_unpaced();
    test_bounds();
    test_multiplier_is_telemetry_only();
    test_dynamic_cadence_selection();
    test_cadence_does_not_disturb_the_output_cap();
    std::printf(
        "=== FramePacingRulesTest: %d passed, %d failed ===\n",
        g_passed,
        g_failed);
    return g_failed == 0 ? 0 : 1;
}
