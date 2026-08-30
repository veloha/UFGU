

#include "render/DepthContract.hpp"

#include <cstdio>
#include <limits>
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

[[nodiscard]] bool near(
    const float value, const float expected, const float tolerance)
{
    const auto difference = value - expected;
    return (difference < 0.0F ? -difference : difference) <= tolerance;
}

using namespace mfgdlss::render;

constexpr DepthCopyInputs live_quality_depth()
{
    DepthCopyInputs in{};
    in.source_width = 2560;
    in.source_height = 1440;
    in.destination_width = 2560;
    in.destination_height = 1440;
    in.depth_stencil = true;
    in.sample_count = 1;
    return in;
}

static_assert(
    copy_plan_for(live_quality_depth()) == DepthCopyPlan::whole_subresource,
    "the live 4K Quality depth copy must use the whole-subresource form");

void test_copy_plans()
{
    std::printf("-- which copy form is legal --\n");

    check(copy_plan_for(live_quality_depth()) ==
              DepthCopyPlan::whole_subresource,
          "the live 2560x1440 depth-stencil copy uses the whole subresource: "
          "this is the form that actually delivers the buffer");

    auto motion = live_quality_depth();
    motion.depth_stencil = false;
    check(copy_plan_for(motion) == DepthCopyPlan::whole_subresource,
          "a same-size motion copy also uses the whole subresource, because "
          "the box adds nothing when the extents already match");

    auto partial_motion = live_quality_depth();
    partial_motion.depth_stencil = false;
    partial_motion.source_width = 3840;
    partial_motion.source_height = 2160;
    check(copy_plan_for(partial_motion) == DepthCopyPlan::subregion,
          "a genuine sub-rectangle of a non-depth resource is a legal box copy");

    auto partial_depth = live_quality_depth();
    partial_depth.source_width = 3840;
    partial_depth.source_height = 2160;
    check(copy_plan_for(partial_depth) ==
              DepthCopyPlan::refuse_partial_depth_stencil,
          "a sub-rectangle of a DEPTH-STENCIL resource is refused rather than "
          "issued: D3D11 drops it silently and DLSS would receive zeros");
    check(is_refusal(copy_plan_for(partial_depth)),
          "and that outcome is classified as a refusal, so the caller fails "
          "closed");

    auto multisampled = live_quality_depth();
    multisampled.depth_stencil = false;
    multisampled.source_width = 3840;
    multisampled.source_height = 2160;
    multisampled.sample_count = 4;
    check(copy_plan_for(multisampled) ==
              DepthCopyPlan::refuse_partial_depth_stencil,
          "the same whole-subresource rule applies to multisampled sources");

    auto too_small = live_quality_depth();
    too_small.source_width = 1920;
    check(copy_plan_for(too_small) == DepthCopyPlan::refuse_source_too_small,
          "a source narrower than the render extent is refused");

    auto zero = live_quality_depth();
    zero.source_height = 0;
    check(copy_plan_for(zero) == DepthCopyPlan::refuse_source_too_small,
          "a degenerate extent is refused rather than dispatched");

    std::printf("\n");
}

constexpr DepthProjection standard_finite(
    const float near_plane, const float far_plane)
{
    DepthProjection projection{};
    projection.m33 = far_plane / (far_plane - near_plane);
    projection.m43 = -near_plane * far_plane / (far_plane - near_plane);
    projection.m34 = 1.0F;
    projection.m44 = 0.0F;
    projection.near_plane = near_plane;
    projection.far_plane = far_plane;
    return projection;
}

constexpr DepthProjection reversed_finite(
    const float near_plane, const float far_plane)
{
    DepthProjection projection{};
    projection.m33 = near_plane / (near_plane - far_plane);
    projection.m43 = far_plane * near_plane / (far_plane - near_plane);
    projection.m34 = 1.0F;
    projection.m44 = 0.0F;
    projection.near_plane = near_plane;
    projection.far_plane = far_plane;
    return projection;
}

constexpr DepthProjection reversed_infinite(const float near_plane)
{
    DepthProjection projection{};
    projection.m33 = 0.0F;
    projection.m43 = near_plane;
    projection.m34 = 1.0F;
    projection.m44 = 0.0F;
    projection.near_plane = near_plane;
    projection.far_plane = 0.0F;
    return projection;
}

constexpr DepthProjection standard_infinite(const float near_plane)
{
    DepthProjection projection{};
    projection.m33 = 1.0F;
    projection.m43 = -near_plane;
    projection.m34 = 1.0F;
    projection.m44 = 0.0F;
    projection.near_plane = near_plane;
    projection.far_plane = 0.0F;
    return projection;
}

static_assert(
    classify_depth_orientation(reversed_infinite(15.0F)) ==
        DepthOrientation::reversed,
    "an infinite reversed-Z projection must classify as reversed");
static_assert(
    classify_depth_orientation(standard_finite(15.0F, 250000.0F)) ==
        DepthOrientation::standard,
    "a standard finite projection must classify as standard");
static_assert(
    classify_depth_range(reversed_infinite(15.0F)) == DepthRange::infinite,
    "the provider infinite-depth flag must come from the projection");
static_assert(
    classify_depth_range(standard_finite(15.0F, 250000.0F)) ==
        DepthRange::finite,
    "a finite projection must not receive the provider infinite-depth flag");

void test_depth_orientation()
{
    std::printf("-- which end of the range is near --\n");

    check(classify_depth_orientation(standard_finite(15.0F, 250000.0F)) ==
              DepthOrientation::standard,
          "a standard finite projection is recognised as standard-Z");
    check(classify_depth_orientation(reversed_finite(15.0F, 250000.0F)) ==
              DepthOrientation::reversed,
          "a reversed finite projection is recognised as reversed-Z");
    check(classify_depth_orientation(standard_infinite(15.0F)) ==
              DepthOrientation::standard,
          "a standard INFINITE-far projection is recognised, so Skyrim's "
          "infinite far plane does not defeat the classifier");
    check(classify_depth_orientation(reversed_infinite(15.0F)) ==
              DepthOrientation::reversed,
          "a reversed infinite-far projection is recognised");
    check(classify_depth_range(standard_finite(15.0F, 250000.0F)) ==
              DepthRange::finite,
          "a standard finite projection has a finite far range");
    check(classify_depth_range(reversed_finite(15.0F, 250000.0F)) ==
              DepthRange::finite,
          "a reversed finite projection has a finite far range");
    check(classify_depth_range(standard_infinite(15.0F)) ==
              DepthRange::infinite,
          "a standard infinite projection has an infinite far range");
    check(classify_depth_range(reversed_infinite(15.0F)) ==
              DepthRange::infinite,
          "a reversed infinite projection has an infinite far range");

    float at_near = 0.0F;
    float at_far = 0.0F;
    const auto reversed = reversed_infinite(15.0F);
    const auto near_ok = ndc_depth_at(reversed, 15.0F, at_near);
    const auto far_ok = ndc_depth_at(reversed, 150000.0F, at_far);
    check(near_ok && at_near > 0.999F,
          "reversed-Z puts the near plane at NDC depth 1");
    check(far_ok && at_far < 0.001F,
          "and a distant surface near NDC depth 0, which is the value a "
          "cleared reversed-Z buffer holds - the uniform field the Depth view "
          "showed");

    DepthProjection degenerate{};
    degenerate.near_plane = 15.0F;
    check(classify_depth_orientation(degenerate) ==
              DepthOrientation::indeterminate,
          "an all-zero projection is indeterminate, not silently standard");

    auto no_near = reversed_infinite(15.0F);
    no_near.near_plane = 0.0F;
    check(classify_depth_orientation(no_near) ==
              DepthOrientation::indeterminate,
          "a zero near plane is indeterminate");

    DepthProjection flat{};
    flat.m33 = 0.0F;
    flat.m43 = 0.5F;
    flat.m34 = 0.0F;
    flat.m44 = 1.0F;
    flat.near_plane = 15.0F;
    flat.far_plane = 250000.0F;
    float flat_near = 0.0F;
    float flat_far = 0.0F;
    check(ndc_depth_at(flat, 15.0F, flat_near) &&
              ndc_depth_at(flat, 250000.0F, flat_far) &&
              flat_near == flat_far,
          "the flat projection really does map both ends to the same depth");
    check(classify_depth_orientation(flat) == DepthOrientation::indeterminate,
          "a projection whose two ends are indistinguishable is indeterminate: "
          "the caller keeps its existing behaviour and reports that");
    check(classify_depth_range(flat) == DepthRange::indeterminate,
          "a non-perspective depth row cannot silently enable infinite depth");

    auto small_but_ordered = reversed_infinite(15.0F);
    small_but_ordered.m43 = 0.5F;
    check(classify_depth_orientation(small_but_ordered) ==
              DepthOrientation::reversed,
          "a small but correctly ordered reversed projection is still "
          "reversed");

    auto not_a_number = reversed_infinite(15.0F);
    not_a_number.m33 = std::numeric_limits<float>::quiet_NaN();
    check(classify_depth_orientation(not_a_number) ==
              DepthOrientation::indeterminate,
          "a NaN in the matrix is indeterminate rather than a coin flip");

    std::printf("\n");
}

void test_measured_skyrim_projection()
{
    std::printf("-- the real captured Skyrim block --\n");

    DepthProjection captured{};
    captured.m33 = 0.0F;
    captured.m43 = 1.0F;
    captured.m34 = -0.066664F;
    captured.m44 = 0.066667F;
    captured.near_plane = 15.0F;
    captured.far_plane = 353840.0F;

    float raw_at_near = 0.0F;
    check(
        ndc_depth_at(captured, captured.near_plane, raw_at_near) &&
            raw_at_near < -1.0F,
        "the captured block evaluates to NDC below -1 at the near plane");
    check(
        classify_depth_range(captured) == DepthRange::indeterminate,
        "the captured block is correctly refused as an unusable range");

    DepthProjection used{};
    const auto form = normalize_depth_projection(captured, used);
    check(
        form == ProjectionForm::inverted,
        "the captured block is recognised as an inverse projection");

    check(
        near(used.m33, 1.000045F, 1.0e-4F) && near(used.m34, 1.0F, 1.0e-4F),
        "inversion recovers m33 ~ 1 and m34 = 1");
    check(
        near(used.m43, -15.0006F, 1.0e-2F),
        "inversion recovers m43 = -near");
    check(
        near(used.m44, 0.0F, 1.0e-9F),
        "inversion recovers m44 = 0, which is what the range test requires");

    float at_near = 0.0F;
    float at_far = 0.0F;
    check(
        ndc_depth_at(used, used.near_plane, at_near) && near(at_near, 0.0F, 1.0e-4F),
        "the recovered matrix maps the near plane to NDC 0");
    check(
        ndc_depth_at(used, used.near_plane * 10000.0F, at_far) &&
            near(at_far, 1.0F, 1.0e-3F),
        "the recovered matrix maps far out to NDC 1");

    check(
        classify_depth_orientation(used) == DepthOrientation::standard,
        "Skyrim's depth is standard-Z");

    check(
        classify_depth_range(used) == DepthRange::finite,
        "Skyrim's far plane is finite, not infinite");
    check(
        classify_depth_range(used) != DepthRange::indeterminate,
        "THE FSR REFUSAL IS GONE: the range is determinate");

    std::printf("-- form detection --\n");

    DepthProjection forward{};
    forward.m33 = 1.0F;
    forward.m34 = 1.0F;
    forward.m43 = -15.0F;
    forward.m44 = 0.0F;
    forward.near_plane = 15.0F;
    forward.far_plane = 353840.0F;
    DepthProjection passthrough{};
    check(
        normalize_depth_projection(forward, passthrough) ==
            ProjectionForm::forward,
        "a forward projection is used as-is");
    check(
        passthrough.m33 == forward.m33 && passthrough.m44 == forward.m44,
        "a forward projection is not modified");

    DepthProjection once{};
    DepthProjection twice{};
    check(
        invert_depth_block(captured, once) && invert_depth_block(once, twice) &&
            near(twice.m33, captured.m33, 1.0e-5F) &&
            near(twice.m34, captured.m34, 1.0e-5F) &&
            near(twice.m43, captured.m43, 1.0e-5F) &&
            near(twice.m44, captured.m44, 1.0e-5F),
        "inverting the depth block twice is the identity");

    DepthProjection singular{};
    singular.near_plane = 15.0F;
    DepthProjection unused{};
    check(
        !invert_depth_block(singular, unused),
        "a singular depth block refuses to invert");
    check(
        normalize_depth_projection(singular, unused) ==
            ProjectionForm::indeterminate,
        "a degenerate block is indeterminate rather than guessed");

    auto bad_near = captured;
    bad_near.near_plane = 0.0F;
    check(
        normalize_depth_projection(bad_near, unused) ==
            ProjectionForm::indeterminate,
        "a non-positive near plane is indeterminate");

    std::printf("\n");
}
}

int main()
{
    std::printf("=== The depth contract ===\n\n");
    test_copy_plans();
    test_depth_orientation();
    test_measured_skyrim_projection();
    std::printf(
        "=== DepthContractTest: %d passed, %d failed ===\n",
        g_passed,
        g_failed);
    return g_failed == 0 ? 0 : 1;
}
