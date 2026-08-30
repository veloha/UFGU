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
    kHoleSentinel == 0xFFFFFFFFU,
    "the fill and the reprojection stage must agree on what a hole looks "
    "like, because the reprojection marks them and the fill consumes them");

static_assert(
    compute_fill_iterations(0U) == 0U,
    "a frame with no holes runs no fill passes at all rather than a wasted "
    "dispatch");

static_assert(
    compute_fill_iterations(3U) == 4U,
    "each pass grows the valid region by one texel, and the DEEPEST texel of "
    "a hole of half-width three sits four texels from the nearest valid data, "
    "so it needs FOUR passes. Three was the original guess and the hardware "
    "contract test found the centre texel still empty");

static_assert(
    compute_fill_iterations(9999U) == kFillMaximumIterations,
    "the pass count is capped, because an unbounded loop on a frame budget "
    "is worse than a hole");

static_assert(
    hole_is_fillable(4U, compute_fill_iterations(4U)) &&
        !hole_is_fillable(4U, 4U),
    "a hole inside the budget is fillable with the passes the plan asks for, "
    "and is NOT fillable with one fewer, which is the off-by-one stated as a "
    "contract rather than left to be rediscovered");

static_assert(
    !hole_is_fillable(40U, compute_fill_iterations(40U)),
    "a hole wider than the cap is reported UNFILLABLE rather than quietly "
    "left half filled, so the caller can decide what to do about it");

static_assert(
    hole_radius_from_motion(120.0F, 0.5F) == 60U,
    "a 120 pixel peak at half phase can reveal 60 pixels, which is the "
    "radius the fill has to plan for");

static_assert(
    hole_radius_from_motion(0.4F, 0.5F) == 1U,
    "sub-pixel motion still rounds up to one, because a hole either exists "
    "or it does not and zero passes would leave it");

static_assert(
    hole_radius_from_motion(0.0F, 0.5F) == 0U,
    "a motionless frame reveals nothing and needs no fill");
}

int main()
{
    std::printf("=== Disocclusion fill plan contract ===\n\n");

    check(
        compute_fill_iterations(hole_radius_from_motion(120.0F, 0.5F)) ==
            kFillMaximumIterations,
        "a fast pan asks for more passes than the budget allows, so the plan "
        "clamps and the caller learns the frame will keep some holes");

    check(
        hole_is_fillable(
            hole_radius_from_motion(8.0F, 0.5F),
            compute_fill_iterations(hole_radius_from_motion(8.0F, 0.5F))),
        "ordinary motion of 8 pixels at half phase is comfortably inside the "
        "budget");

    check(
        fill_preserves_valid_texels(),
        "the fill never rewrites a texel that already has a source, which is "
        "what keeps it from softening the parts of the frame that reprojected "
        "correctly");

    std::printf(
        "\n=== DisocclusionFillPlanTest: %d passed, %d failed ===\n",
        g_passed,
        g_failed);
    return g_failed == 0 ? 0 : 1;
}
