#include "render/ComputeDispatchPlan.hpp"

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
    compute_group_size_is_valid(ComputeGroupSize{8U, 8U, 1U}),
    "the default 8x8 tile is a legal thread group");
static_assert(
    !compute_group_size_is_valid(ComputeGroupSize{32U, 32U, 2U}),
    "2048 threads exceeds the 1024 per group the hardware allows");
static_assert(
    !compute_group_size_is_valid(ComputeGroupSize{1U, 1U, 128U}),
    "the Z dimension of a thread group is capped at 64");
static_assert(
    !compute_group_size_is_valid(ComputeGroupSize{0U, 8U, 1U}),
    "a zero dimension would dispatch nothing");

static_assert(
    compute_dispatch_groups(1920U, 1080U, 1U, ComputeGroupSize{8U, 8U, 1U}).x ==
        240U,
    "1920 pixels across 8 wide tiles is exactly 240 groups");
static_assert(
    compute_dispatch_groups(1920U, 1080U, 1U, ComputeGroupSize{8U, 8U, 1U}).y ==
        135U,
    "1080 pixels across 8 tall tiles is exactly 135 groups");
static_assert(
    compute_dispatch_groups(1921U, 1080U, 1U, ComputeGroupSize{8U, 8U, 1U}).x ==
        241U,
    "one pixel past a tile boundary needs a whole extra group");
static_assert(
    compute_root_signature_dwords(ComputeBindingLayout{4U, 1U, 16U, 1U}) == 17U,
    "one descriptor table costs a single DWORD however many descriptors it "
    "holds");

void test_dispatch_covers_every_pixel()
{
    std::printf("Every pixel is covered, and no group is wasted\n");

    const ComputeGroupSize tile{8U, 8U, 1U};
    const std::uint32_t widths[] = {1U, 7U, 8U, 9U, 1280U, 1920U, 2559U, 3840U};
    auto covered = true;
    auto minimal = true;
    for (const auto width : widths) {
        const auto groups = compute_dispatch_groups(width, 8U, 1U, tile);
        if (groups.x * tile.x < width) {
            covered = false;
        }
        if (groups.x > 1U && (groups.x - 1U) * tile.x >= width) {
            minimal = false;
        }
    }
    check(covered, "every dispatch reaches at least the last pixel");
    check(minimal, "no dispatch launches a group with nothing to do");
    std::printf("\n");
}

void test_a_zero_extent_never_dispatches()
{
    std::printf("A zero extent produces no dispatch rather than a bad one\n");

    const ComputeGroupSize tile{8U, 8U, 1U};
    check(
        !compute_dispatch_groups_are_valid(
            compute_dispatch_groups(0U, 1080U, 1U, tile)),
        "zero width is rejected");
    check(
        !compute_dispatch_groups_are_valid(
            compute_dispatch_groups(1920U, 0U, 1U, tile)),
        "zero height is rejected");
    check(
        !compute_dispatch_groups_are_valid(
            compute_dispatch_groups(1920U, 1080U, 0U, tile)),
        "zero depth is rejected");
    check(
        !compute_dispatch_groups_are_valid(compute_dispatch_groups(
            1920U, 1080U, 1U, ComputeGroupSize{0U, 8U, 1U})),
        "an illegal tile is rejected before it reaches the driver");
    std::printf("\n");
}

void test_the_group_count_ceiling_is_honoured()
{
    std::printf("An extent too large to dispatch is refused, not truncated\n");

    const ComputeGroupSize single{1U, 1U, 1U};
    const auto at_ceiling =
        compute_dispatch_groups(kComputeMaxGroupsPerDimension, 1U, 1U, single);
    check(
        compute_dispatch_groups_are_valid(at_ceiling),
        "65535 groups in a dimension is still legal");
    const auto past_ceiling = compute_dispatch_groups(
        kComputeMaxGroupsPerDimension + 1U, 1U, 1U, single);
    check(
        !compute_dispatch_groups_are_valid(past_ceiling),
        "one group past the ceiling is refused rather than silently wrapped");
    std::printf("\n");
}

void test_descriptor_slots_do_not_collide()
{
    std::printf("Shader resources and unordered access never share a slot\n");

    const ComputeBindingLayout layout{5U, 3U, 8U, 1U};
    check(
        compute_descriptor_count(layout) == 8U,
        "the heap holds every shader resource and unordered access view");

    auto collided = false;
    for (std::uint32_t srv = 0U; srv < layout.shader_resources; ++srv) {
        for (std::uint32_t uav = 0U; uav < layout.unordered_access; ++uav) {
            if (compute_shader_resource_slot(layout, srv) ==
                compute_unordered_access_slot(layout, uav)) {
                collided = true;
            }
        }
    }
    check(!collided, "no shader resource slot aliases an output slot");

    check(
        compute_unordered_access_slot(layout, 0U) == layout.shader_resources,
        "the unordered access range begins where the resource range ends");
    check(
        compute_shader_resource_slot(layout, layout.shader_resources) ==
            kComputeInvalidSlot,
        "an out of range shader resource is reported, not wrapped to zero");
    check(
        compute_unordered_access_slot(layout, layout.unordered_access) ==
            kComputeInvalidSlot,
        "an out of range unordered access view is reported, not wrapped");
    std::printf("\n");
}

void test_a_root_signature_that_cannot_be_created_is_rejected_first()
{
    std::printf("Layouts the runtime would refuse are refused here\n");

    check(
        compute_binding_layout_is_valid(ComputeBindingLayout{4U, 1U, 16U, 1U}),
        "a realistic interpolation layout is accepted");
    check(
        !compute_binding_layout_is_valid(ComputeBindingLayout{0U, 0U, 0U, 0U}),
        "a layout that binds nothing at all is refused");
    check(
        !compute_binding_layout_is_valid(
            ComputeBindingLayout{kComputeMaxShaderResources + 1U, 1U, 4U, 0U}),
        "more shader resources than the harness supports is refused");
    check(
        !compute_binding_layout_is_valid(
            ComputeBindingLayout{1U, kComputeMaxUnorderedAccess + 1U, 4U, 0U}),
        "more outputs than the harness supports is refused");
    check(
        !compute_binding_layout_is_valid(
            ComputeBindingLayout{1U, 1U, kComputeMaxRootSignatureDwords, 0U}),
        "root constants that overflow the 64 DWORD budget are refused");
    check(
        compute_binding_layout_is_valid(ComputeBindingLayout{
            1U, 1U, kComputeMaxRootSignatureDwords - 1U, 0U}),
        "root constants that exactly fill the budget alongside one table are "
        "accepted");
    check(
        !compute_binding_layout_is_valid(
            ComputeBindingLayout{1U, 1U, 4U, kComputeMaxStaticSamplers + 1U}),
        "more static samplers than the harness declares is refused");
    std::printf("\n");
}

void test_root_constants_alone_are_a_legal_pass()
{
    std::printf("A pass with constants but no table costs no table DWORD\n");

    const ComputeBindingLayout constants_only{0U, 0U, 4U, 0U};
    check(
        compute_root_signature_dwords(constants_only) == 4U,
        "no descriptor table means no DWORD spent on one");
    check(
        compute_binding_layout_is_valid(constants_only),
        "constants alone are a legal layout");
    std::printf("\n");
}
}

int main()
{
    std::printf("=== Compute dispatch plan ===\n\n");
    test_dispatch_covers_every_pixel();
    test_a_zero_extent_never_dispatches();
    test_the_group_count_ceiling_is_honoured();
    test_descriptor_slots_do_not_collide();
    test_a_root_signature_that_cannot_be_created_is_rejected_first();
    test_root_constants_alone_are_a_legal_pass();
    std::printf(
        "=== ComputeDispatchPlanTest: %d passed, %d failed ===\n",
        g_passed,
        g_failed);
    return g_failed == 0 ? 0 : 1;
}
