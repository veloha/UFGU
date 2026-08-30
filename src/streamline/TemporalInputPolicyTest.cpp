#include "streamline/TemporalInputPolicy.hpp"

#include <cstdio>

namespace
{
int passed{};
int failed{};

void expect(const bool condition, const char* const name)
{
    if (condition) {
        ++passed;
        std::printf("PASS  %s\n", name);
    } else {
        ++failed;
        std::printf("FAIL  %s\n", name);
    }
}
}

int main()
{
    using mfgdlss::config::DlssTemporalInputs;
    using mfgdlss::streamline::select_temporal_inputs;

    for (const auto mode : {
             DlssTemporalInputs::standard,
             DlssTemporalInputs::no_hints,
             DlssTemporalInputs::raw_motion,
             DlssTemporalInputs::raw_motion_no_hints}) {
        const auto unavailable = select_temporal_inputs(mode, false, true);
        expect(
            !unavailable.use_dilated_motion && !unavailable.use_hints,
            "unavailable prepared inputs always fail back to raw/no-hints");
    }

    const auto standard =
        select_temporal_inputs(DlssTemporalInputs::standard, true, true);
    expect(
        standard.use_dilated_motion && standard.use_hints,
        "Standard selects dilated motion and both hints");

    const auto no_hints =
        select_temporal_inputs(DlssTemporalInputs::no_hints, true, true);
    expect(
        no_hints.use_dilated_motion && !no_hints.use_hints,
        "NoHints selects dilated motion without hints");

    const auto raw =
        select_temporal_inputs(DlssTemporalInputs::raw_motion, true, true);
    expect(
        !raw.use_dilated_motion && raw.use_hints,
        "RawMotion selects raw motion with hints");

    const auto raw_no_hints =
        select_temporal_inputs(
            DlssTemporalInputs::raw_motion_no_hints,
            true,
            true);
    expect(
        !raw_no_hints.use_dilated_motion && !raw_no_hints.use_hints,
        "RawMotionNoHints selects raw motion without hints");

    const auto configured_off =
        select_temporal_inputs(DlssTemporalInputs::standard, true, false);
    expect(
        !configured_off.use_dilated_motion && configured_off.use_hints,
        "Standard with dilation disabled declares and tags raw motion");

    std::printf(
        "=== TemporalInputPolicyTest: %d passed, %d failed ===\n",
        passed,
        failed);
    return failed == 0 ? 0 : 1;
}
