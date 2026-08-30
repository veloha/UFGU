#pragma once

#include <cstdint>

namespace mfgdlss::render
{
inline constexpr std::uint32_t kComputeMaxThreadsPerGroup = 1024U;
inline constexpr std::uint32_t kComputeMaxGroupSizeZ = 64U;
inline constexpr std::uint32_t kComputeMaxGroupsPerDimension = 65535U;
inline constexpr std::uint32_t kComputeMaxShaderResources = 16U;
inline constexpr std::uint32_t kComputeMaxUnorderedAccess = 8U;
inline constexpr std::uint32_t kComputeMaxStaticSamplers = 2U;
inline constexpr std::uint32_t kComputeMaxRootSignatureDwords = 64U;
inline constexpr std::uint32_t kComputeInvalidSlot = 0xFFFFFFFFU;

struct ComputeGroupSize final
{
    std::uint32_t x{8U};
    std::uint32_t y{8U};
    std::uint32_t z{1U};
};

struct ComputeDispatchGroups final
{
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t z{};
};

struct ComputeBindingLayout final
{
    std::uint32_t shader_resources{};
    std::uint32_t unordered_access{};
    std::uint32_t root_constant_dwords{};
    std::uint32_t static_samplers{};
};

[[nodiscard]] constexpr bool compute_group_size_is_valid(
    const ComputeGroupSize& size) noexcept
{
    if (size.x == 0U || size.y == 0U || size.z == 0U) {
        return false;
    }
    if (size.z > kComputeMaxGroupSizeZ) {
        return false;
    }
    const auto total = static_cast<std::uint64_t>(size.x) *
        static_cast<std::uint64_t>(size.y) * static_cast<std::uint64_t>(size.z);
    return total <= static_cast<std::uint64_t>(kComputeMaxThreadsPerGroup);
}

[[nodiscard]] constexpr bool compute_dispatch_groups_are_valid(
    const ComputeDispatchGroups& groups) noexcept
{
    if (groups.x == 0U || groups.y == 0U || groups.z == 0U) {
        return false;
    }
    return groups.x <= kComputeMaxGroupsPerDimension &&
        groups.y <= kComputeMaxGroupsPerDimension &&
        groups.z <= kComputeMaxGroupsPerDimension;
}

[[nodiscard]] constexpr ComputeDispatchGroups compute_dispatch_groups(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t depth,
    const ComputeGroupSize& size) noexcept
{
    if (!compute_group_size_is_valid(size)) {
        return {};
    }
    if (width == 0U || height == 0U || depth == 0U) {
        return {};
    }
    const ComputeDispatchGroups groups{
        (width + size.x - 1U) / size.x,
        (height + size.y - 1U) / size.y,
        (depth + size.z - 1U) / size.z};
    if (!compute_dispatch_groups_are_valid(groups)) {
        return {};
    }
    return groups;
}

[[nodiscard]] constexpr std::uint32_t compute_descriptor_count(
    const ComputeBindingLayout& layout) noexcept
{
    return layout.shader_resources + layout.unordered_access;
}

[[nodiscard]] constexpr std::uint32_t compute_root_signature_dwords(
    const ComputeBindingLayout& layout) noexcept
{
    const auto table = compute_descriptor_count(layout) == 0U ? 0U : 1U;
    return table + layout.root_constant_dwords;
}

[[nodiscard]] constexpr bool compute_binding_layout_is_valid(
    const ComputeBindingLayout& layout) noexcept
{
    if (layout.shader_resources > kComputeMaxShaderResources) {
        return false;
    }
    if (layout.unordered_access > kComputeMaxUnorderedAccess) {
        return false;
    }
    if (layout.static_samplers > kComputeMaxStaticSamplers) {
        return false;
    }
    if (compute_descriptor_count(layout) == 0U &&
        layout.root_constant_dwords == 0U) {
        return false;
    }
    return compute_root_signature_dwords(layout) <=
        kComputeMaxRootSignatureDwords;
}

[[nodiscard]] constexpr std::uint32_t compute_shader_resource_slot(
    const ComputeBindingLayout& layout,
    const std::uint32_t index) noexcept
{
    if (index >= layout.shader_resources) {
        return kComputeInvalidSlot;
    }
    return index;
}

[[nodiscard]] constexpr std::uint32_t compute_unordered_access_slot(
    const ComputeBindingLayout& layout,
    const std::uint32_t index) noexcept
{
    if (index >= layout.unordered_access) {
        return kComputeInvalidSlot;
    }
    return layout.shader_resources + index;
}
}
