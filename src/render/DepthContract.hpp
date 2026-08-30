#pragma once

#include <cstdint>

namespace mfgdlss::render
{

struct DepthCopyInputs
{
    std::uint32_t source_width{};
    std::uint32_t source_height{};
    std::uint32_t destination_width{};
    std::uint32_t destination_height{};

    bool depth_stencil{};

    std::uint32_t sample_count{1};
};

enum class DepthCopyPlan : std::uint32_t
{

    whole_subresource,

    subregion,

    refuse_source_too_small,
    refuse_partial_depth_stencil,
};

[[nodiscard]] constexpr bool is_refusal(const DepthCopyPlan plan) noexcept
{
    return plan == DepthCopyPlan::refuse_source_too_small ||
           plan == DepthCopyPlan::refuse_partial_depth_stencil;
}

[[nodiscard]] constexpr DepthCopyPlan copy_plan_for(
    const DepthCopyInputs& in) noexcept
{
    if (in.source_width == 0 || in.source_height == 0 ||
        in.destination_width == 0 || in.destination_height == 0 ||
        in.source_width < in.destination_width ||
        in.source_height < in.destination_height) {
        return DepthCopyPlan::refuse_source_too_small;
    }

    if (in.source_width == in.destination_width &&
        in.source_height == in.destination_height) {
        return DepthCopyPlan::whole_subresource;
    }

    if (in.depth_stencil || in.sample_count != 1) {
        return DepthCopyPlan::refuse_partial_depth_stencil;
    }
    return DepthCopyPlan::subregion;
}

[[nodiscard]] constexpr const char* describe(const DepthCopyPlan plan) noexcept
{
    switch (plan) {
    case DepthCopyPlan::whole_subresource:
        return "whole-subresource copy";
    case DepthCopyPlan::subregion:
        return "sub-rectangle copy";
    case DepthCopyPlan::refuse_source_too_small:
        return "the source is smaller than the required render extent";
    case DepthCopyPlan::refuse_partial_depth_stencil:
        return "a sub-rectangle of a depth-stencil or multisampled resource "
               "cannot be copied by D3D11";
    }
    return "unknown";
}

struct DepthProjection
{
    float m33{};
    float m43{};
    float m34{};
    float m44{};
    float near_plane{};
    float far_plane{};
};

enum class DepthOrientation : std::uint32_t
{

    reversed,

    standard,

    indeterminate,
};

enum class DepthRange : std::uint32_t
{

    infinite,

    finite,

    indeterminate,
};

struct DepthContractSnapshot final
{
    DepthOrientation orientation{DepthOrientation::indeterminate};
    DepthRange range{DepthRange::indeterminate};

    [[nodiscard]] constexpr bool determinate() const noexcept
    {
        return orientation != DepthOrientation::indeterminate &&
               range != DepthRange::indeterminate;
    }

    [[nodiscard]] constexpr bool reversed() const noexcept
    {
        return orientation == DepthOrientation::reversed;
    }

    [[nodiscard]] constexpr bool infinite() const noexcept
    {
        return range == DepthRange::infinite;
    }
};

[[nodiscard]] constexpr const char* describe(
    const DepthOrientation orientation) noexcept
{
    switch (orientation) {
    case DepthOrientation::reversed:
        return "reversed-Z (near=1, far=0)";
    case DepthOrientation::standard:
        return "standard-Z (near=0, far=1)";
    case DepthOrientation::indeterminate:
        return "indeterminate";
    }
    return "unknown";
}

[[nodiscard]] constexpr const char* describe(
    const DepthRange range) noexcept
{
    switch (range) {
    case DepthRange::infinite:
        return "infinite far plane";
    case DepthRange::finite:
        return "finite far plane";
    case DepthRange::indeterminate:
        return "indeterminate far plane";
    }
    return "unknown";
}

namespace detail
{

[[nodiscard]] constexpr float absolute(const float value) noexcept
{
    return value < 0.0F ? -value : value;
}

[[nodiscard]] constexpr bool finite(const float value) noexcept
{

    return value == value && absolute(value) < 3.0e38F;
}
}

[[nodiscard]] constexpr bool ndc_depth_at(
    const DepthProjection& projection,
    const float view_z,
    float& out_depth) noexcept
{
    const auto w = view_z * projection.m34 + projection.m44;
    if (detail::absolute(w) < 1.0e-9F || !detail::finite(w)) {
        return false;
    }
    const auto z = (view_z * projection.m33 + projection.m43) / w;
    if (!detail::finite(z)) {
        return false;
    }
    out_depth = z;
    return true;
}

[[nodiscard]] constexpr DepthOrientation classify_depth_orientation(
    const DepthProjection& projection) noexcept
{
    const auto near_plane = projection.near_plane;
    if (!detail::finite(near_plane) || near_plane <= 0.0F ||
        !detail::finite(projection.m33) || !detail::finite(projection.m43) ||
        !detail::finite(projection.m34) || !detail::finite(projection.m44)) {
        return DepthOrientation::indeterminate;
    }

    auto far_probe = near_plane * 10000.0F;
    if (detail::finite(projection.far_plane) &&
        projection.far_plane > near_plane * 2.0F) {
        far_probe = projection.far_plane;
    }

    float at_near = 0.0F;
    float at_far = 0.0F;
    if (!ndc_depth_at(projection, near_plane, at_near) ||
        !ndc_depth_at(projection, far_probe, at_far)) {
        return DepthOrientation::indeterminate;
    }

    constexpr float kMinimumSeparation = 1.0e-4F;
    const auto separation = at_near - at_far;
    if (detail::absolute(separation) < kMinimumSeparation) {
        return DepthOrientation::indeterminate;
    }
    return separation > 0.0F ?
        DepthOrientation::reversed :
        DepthOrientation::standard;
}

enum class ProjectionForm : std::uint32_t
{

    forward,

    inverted,

    indeterminate,
};

[[nodiscard]] constexpr const char* describe(const ProjectionForm form) noexcept
{
    switch (form) {
    case ProjectionForm::forward:
        return "forward projection";
    case ProjectionForm::inverted:
        return "inverse projection, inverted back";
    case ProjectionForm::indeterminate:
        return "unrecognised projection";
    }
    return "unknown";
}

[[nodiscard]] constexpr bool invert_depth_block(
    const DepthProjection& in, DepthProjection& out) noexcept
{
    const auto determinant = in.m33 * in.m44 - in.m34 * in.m43;
    if (!detail::finite(determinant) ||
        detail::absolute(determinant) < 1.0e-12F) {
        return false;
    }
    out = in;
    out.m33 = in.m44 / determinant;
    out.m34 = -in.m34 / determinant;
    out.m43 = -in.m43 / determinant;
    out.m44 = in.m33 / determinant;
    return detail::finite(out.m33) && detail::finite(out.m34) &&
           detail::finite(out.m43) && detail::finite(out.m44);
}

namespace detail
{

[[nodiscard]] constexpr bool maps_into_depth_interval(
    const DepthProjection& projection, const float far_probe) noexcept
{
    constexpr float kIntervalTolerance = 1.0e-3F;
    constexpr float kMinimumSeparation = 1.0e-4F;
    float at_near = 0.0F;
    float at_far = 0.0F;
    if (!ndc_depth_at(projection, projection.near_plane, at_near) ||
        !ndc_depth_at(projection, far_probe, at_far)) {
        return false;
    }
    const auto inside = [](const float value) {
        return value >= -kIntervalTolerance && value <= 1.0F + kIntervalTolerance;
    };
    return inside(at_near) && inside(at_far) &&
           absolute(at_near - at_far) >= kMinimumSeparation;
}

[[nodiscard]] constexpr float far_probe_for(
    const DepthProjection& projection) noexcept
{
    auto probe = projection.near_plane * 10000.0F;
    if (finite(projection.far_plane) &&
        projection.far_plane > projection.near_plane * 2.0F) {
        probe = projection.far_plane;
    }
    return probe;
}
}

[[nodiscard]] constexpr ProjectionForm normalize_depth_projection(
    const DepthProjection& captured, DepthProjection& out) noexcept
{
    if (!detail::finite(captured.near_plane) || captured.near_plane <= 0.0F) {
        out = captured;
        return ProjectionForm::indeterminate;
    }
    const auto probe = detail::far_probe_for(captured);

    if (detail::maps_into_depth_interval(captured, probe)) {
        out = captured;
        return ProjectionForm::forward;
    }
    DepthProjection inverted{};
    if (invert_depth_block(captured, inverted) &&
        detail::maps_into_depth_interval(inverted, probe)) {
        out = inverted;
        return ProjectionForm::inverted;
    }
    out = captured;
    return ProjectionForm::indeterminate;
}

[[nodiscard]] constexpr DepthRange classify_depth_range(
    const DepthProjection& projection) noexcept
{
    const auto orientation = classify_depth_orientation(projection);
    if (orientation == DepthOrientation::indeterminate ||
        !detail::finite(projection.m34) ||
        !detail::finite(projection.m44) ||
        detail::absolute(projection.m34) < 1.0e-9F ||
        detail::absolute(projection.m44) > 1.0e-6F) {
        return DepthRange::indeterminate;
    }

    const auto far_limit = projection.m33 / projection.m34;
    if (!detail::finite(far_limit)) {
        return DepthRange::indeterminate;
    }
    const auto far_endpoint =
        orientation == DepthOrientation::reversed ? 0.0F : 1.0F;

    constexpr float kInfiniteEndpointTolerance = 5.0e-7F;
    return detail::absolute(far_limit - far_endpoint) <=
                   kInfiniteEndpointTolerance ?
        DepthRange::infinite :
        DepthRange::finite;
}
}
