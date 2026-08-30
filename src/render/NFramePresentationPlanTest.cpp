#include "render/NFramePresentationPlan.hpp"

#include "render/DisocclusionFillPlan.hpp"

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
    multiplier_is_supported(2U) && multiplier_is_supported(3U) &&
        multiplier_is_supported(4U) && multiplier_is_supported(6U) &&
        !multiplier_is_supported(5U),
    "the presenter accepts exactly the set the rest of the plugin does. 5x "
    "does not exist in any pipeline here and must not appear in a new one");

static_assert(
    generated_frames_for(6U) == 5U && generated_frames_for(1U) == 0U,
    "6x means five generated frames, which is what Streamline's "
    "numFramesToGenerateMax reports for a 6x capable device");

static_assert(
    compute_phase_for_frame(0U, 6U) == 0.0F,
    "frame zero of a group IS the rendered frame and carries no phase, so it "
    "is presented rather than interpolated");

static_assert(
    compute_phase_for_frame(3U, 6U) == 0.5F,
    "the middle frame of a 6x group sits at exactly one half, which is the "
    "single phase FidelityFX hardcodes. Our step 5 reprojection proved every "
    "other phase, and this is where they get used");

static_assert(
    compute_phase_for_frame(1U, 2U) == 0.5F,
    "at 2x the one generated frame is the half-phase frame, so 2x here is "
    "exactly what a conventional interpolator does");

static_assert(
    phase_is_interior(compute_phase_for_frame(1U, 6U)) &&
        phase_is_interior(compute_phase_for_frame(5U, 6U)),
    "every generated frame lands strictly between the two rendered frames. A "
    "phase of 0 or 1 would be a duplicate of a real frame rather than a new "
    "one");

[[nodiscard]] constexpr bool phases_increase(
    const std::uint32_t multiplier) noexcept
{
    for (std::uint32_t index = 1U; index + 1U < multiplier; ++index) {
        if (!(compute_phase_for_frame(index, multiplier) <
              compute_phase_for_frame(index + 1U, multiplier))) {
            return false;
        }
    }
    return true;
}

static_assert(
    phases_increase(2U) && phases_increase(3U) && phases_increase(4U) &&
        phases_increase(6U),
    "phases advance strictly, so the generated frames of a group are ordered "
    "in time and cannot present out of sequence");

static_assert(
    compute_back_buffer_count(1U) == kPresentationMinimumBackBuffers,
    "with generation off the presenter still wants the ordinary three, "
    "because dropping below that would change the working path");

static_assert(
    compute_back_buffer_count(6U) == 7U,
    "6x needs six frames queued plus the one being written. The current swap "
    "chain creates max(requested, 3), which is why PresentationBridge cannot "
    "present six frames without this change");

static_assert(
    compute_back_buffer_count(4U) == 5U && compute_back_buffer_count(2U) == 3U,
    "the count scales with the multiplier rather than doubling blindly. "
    "Streamline's cloneFakeBuffers doubles OUR count and made 6 from 3, which "
    "is a different policy and not the requirement being derived here");

static_assert(
    compute_back_buffer_count(64U) <= kPresentationMaximumBackBuffers,
    "the count is capped, because back buffers at 4K are the largest "
    "allocation this plugin makes");

static_assert(
    compute_frame_interval_us(16667ULL, 1U) == 16667ULL,
    "with generation off the cadence is the base cadence untouched");

static_assert(
    compute_frame_interval_us(16800ULL, 6U) == 2800ULL,
    "a 60 FPS base at 6x spaces frames 2.8ms apart");

static_assert(
    compute_present_deadline_us(1000ULL, 0U, 16800ULL, 6U) == 1000ULL,
    "the rendered frame presents immediately rather than waiting for a slot "
    "it already owns");

static_assert(
    compute_present_deadline_us(1000ULL, 5U, 16800ULL, 6U) == 15000ULL,
    "the last generated frame of a 6x group presents before the next rendered "
    "frame is due, which is what stops a group overrunning into the next");

static_assert(
    !pacing_is_even(16667ULL, 6U),
    "a 60 FPS base of 16667us does not divide by six, so a presenter that "
    "computed a step and multiplied it would lose 5us per group, 300us per "
    "second, 18ms over a minute, which is a whole frame at 60");

static_assert(
    group_tiles_exactly(16667ULL, 2U) && group_tiles_exactly(16667ULL, 3U) &&
        group_tiles_exactly(16667ULL, 4U) && group_tiles_exactly(16667ULL, 6U),
    "and yet every group lands EXACTLY on the next base frame at that same "
    "awkward interval, because the deadline is the product divided by the "
    "multiplier rather than a truncated step multiplied up. Integer division "
    "distributes the residual by itself, so the drift is eliminated rather "
    "than carried");

static_assert(
    group_tiles_exactly(16667ULL, 6U) && group_tiles_exactly(8333ULL, 6U) &&
        group_tiles_exactly(11111ULL, 3U),
    "exact tiling holds for awkward base intervals generally, not just the "
    "one that was measured");

[[nodiscard]] constexpr bool gaps_are_within_one_microsecond(
    const std::uint64_t base_interval_us,
    const std::uint32_t multiplier) noexcept
{
    std::uint64_t smallest = base_interval_us;
    std::uint64_t largest = 0ULL;
    for (std::uint32_t index = 1U; index <= multiplier; ++index) {
        const auto gap =
            compute_gap_us(index, base_interval_us, multiplier);
        smallest = gap < smallest ? gap : smallest;
        largest = gap > largest ? gap : largest;
    }
    return largest - smallest <= 1ULL;
}

static_assert(
    gaps_are_within_one_microsecond(16667ULL, 6U) &&
        gaps_are_within_one_microsecond(16667ULL, 4U),
    "the residual is spread one microsecond at a time rather than dumped on "
    "one frame, so no single gap in a group is visibly longer than its "
    "neighbours");

static_assert(
    kHoleSentinel == 0xFFFFFFFFU,
    "the presenter shares the reprojection and fill sentinel, so a frame that "
    "still carries holes is identifiable at presentation time");
}

int main()
{
    std::printf("=== N frame presentation plan contract ===\n\n");

    check(
        compute_back_buffer_count(6U) > kPresentationMinimumBackBuffers,
        "6x genuinely needs more back buffers than the swap chain currently "
        "creates, which is the measured blocker recorded from the 13:36 log");

    auto ordered = true;
    std::uint64_t previous = 0ULL;
    for (std::uint32_t index = 0U; index < 6U; ++index) {
        const auto deadline =
            compute_present_deadline_us(0ULL, index, 16800ULL, 6U);
        if (index != 0U && deadline <= previous) {
            ordered = false;
        }
        previous = deadline;
    }
    check(
        ordered,
        "the six deadlines of a 6x group are strictly increasing, so the "
        "presenter never schedules two frames for the same instant");

    check(
        compute_present_deadline_us(0ULL, 6U, 16800ULL, 6U) == 16800ULL,
        "the frame after a full group lands exactly on the next base frame, "
        "so groups tile the timeline without gap or overlap");

    check(
        !multiplier_is_supported(5U) && !multiplier_is_supported(0U),
        "unsupported multipliers are refused by the plan rather than "
        "producing a group nobody can present");

    std::printf(
        "\n=== NFramePresentationPlanTest: %d passed, %d failed ===\n",
        g_passed,
        g_failed);
    return g_failed == 0 ? 0 : 1;
}
