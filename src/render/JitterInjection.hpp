#pragma once

#include "render/DepthContract.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace mfgdlss::render
{

[[nodiscard]] constexpr std::size_t mat_index(
    const std::size_t row, const std::size_t column) noexcept
{
    return row * 4U + column;
}

enum class ProjectionLayout : std::uint32_t
{

    row_vector,

    column_vector,

    indeterminate,
};

[[nodiscard]] constexpr const char* describe(
    const ProjectionLayout layout) noexcept
{
    switch (layout) {
    case ProjectionLayout::row_vector:
        return "row-vector (clip = v * P)";
    case ProjectionLayout::column_vector:
        return "column-vector (clip = P * v)";
    case ProjectionLayout::indeterminate:
        return "indeterminate";
    }
    return "unknown";
}

namespace detail
{
[[nodiscard]] constexpr bool near_zero(
    const float value, const float epsilon = 1.0e-5F) noexcept
{
    return (value < 0.0F ? -value : value) <= epsilon;
}

[[nodiscard]] constexpr bool near_unit(
    const float value, const float epsilon = 1.0e-3F) noexcept
{
    const auto magnitude = value < 0.0F ? -value : value;
    const auto difference = magnitude - 1.0F;
    return (difference < 0.0F ? -difference : difference) <= epsilon;
}
}

[[nodiscard]] constexpr ProjectionLayout classify_projection_layout(
    const float* const matrix) noexcept
{
    if (matrix == nullptr) {
        return ProjectionLayout::indeterminate;
    }
    if (!detail::near_zero(matrix[mat_index(3U, 3U)])) {
        return ProjectionLayout::indeterminate;
    }

    const auto row_vector =
        detail::near_unit(matrix[mat_index(2U, 3U)]) &&
        detail::near_zero(matrix[mat_index(0U, 3U)]) &&
        detail::near_zero(matrix[mat_index(1U, 3U)]) &&
        detail::near_zero(matrix[mat_index(3U, 0U)]) &&
        detail::near_zero(matrix[mat_index(3U, 1U)]);
    const auto column_vector =
        detail::near_unit(matrix[mat_index(3U, 2U)]) &&
        detail::near_zero(matrix[mat_index(3U, 0U)]) &&
        detail::near_zero(matrix[mat_index(3U, 1U)]) &&
        detail::near_zero(matrix[mat_index(0U, 3U)]) &&
        detail::near_zero(matrix[mat_index(1U, 3U)]);

    if (row_vector == column_vector) {

        return ProjectionLayout::indeterminate;
    }
    return row_vector ?
        ProjectionLayout::row_vector :
        ProjectionLayout::column_vector;
}

[[nodiscard]] constexpr bool extract_depth_block(
    const float* const matrix,
    const ProjectionLayout layout,
    const float near_plane,
    const float far_plane,
    DepthProjection& out) noexcept
{
    if (matrix == nullptr || layout == ProjectionLayout::indeterminate) {
        return false;
    }
    out.m33 = matrix[mat_index(2U, 2U)];
    out.m44 = matrix[mat_index(3U, 3U)];
    if (layout == ProjectionLayout::row_vector) {
        out.m34 = matrix[mat_index(2U, 3U)];
        out.m43 = matrix[mat_index(3U, 2U)];
    } else {
        out.m34 = matrix[mat_index(3U, 2U)];
        out.m43 = matrix[mat_index(2U, 3U)];
    }
    out.near_plane = near_plane;
    out.far_plane = far_plane;
    return true;
}

[[nodiscard]] constexpr bool projection_matches_camera(
    const float* const matrix,
    const ProjectionLayout layout,
    const float near_plane,
    const float far_plane,
    const float tolerance = 2.0e-3F) noexcept
{
    DepthProjection block{};
    if (!extract_depth_block(matrix, layout, near_plane, far_plane, block)) {
        return false;
    }
    if (!(near_plane > 0.0F) || !(far_plane > near_plane)) {
        return false;
    }

    float at_near = 0.0F;
    if (!ndc_depth_at(block, near_plane, at_near)) {
        return false;
    }
    const auto near_is_zero =
        (at_near < 0.0F ? -at_near : at_near) <= tolerance;
    const auto difference = at_near - 1.0F;
    const auto near_is_one =
        (difference < 0.0F ? -difference : difference) <= tolerance;
    return near_is_zero || near_is_one;
}

constexpr void apply_clip_offset(
    float* const matrix,
    const ProjectionLayout layout,
    const float ndc_x,
    const float ndc_y) noexcept
{
    if (matrix == nullptr || layout == ProjectionLayout::indeterminate) {
        return;
    }
    for (std::size_t i = 0U; i < 4U; ++i) {
        if (layout == ProjectionLayout::row_vector) {
            const auto w = matrix[mat_index(i, 3U)];
            matrix[mat_index(i, 0U)] += w * ndc_x;
            matrix[mat_index(i, 1U)] += w * ndc_y;
        } else {
            const auto w = matrix[mat_index(3U, i)];
            matrix[mat_index(0U, i)] += w * ndc_x;
            matrix[mat_index(1U, i)] += w * ndc_y;
        }
    }
}

[[nodiscard]] inline bool verify_matrix_product(
    const float* const product,
    const float* const left,
    const float* const right,
    const ProjectionLayout layout,
    const float relative_tolerance = 5.0e-3F) noexcept
{
    if (product == nullptr || left == nullptr || right == nullptr ||
        layout == ProjectionLayout::indeterminate) {
        return false;
    }

    const auto* const a =
        layout == ProjectionLayout::row_vector ? left : right;
    const auto* const b =
        layout == ProjectionLayout::row_vector ? right : left;

    float magnitude = 0.0F;
    for (std::size_t i = 0U; i < 16U; ++i) {
        const auto element = std::fabs(product[i]);
        magnitude = element > magnitude ? element : magnitude;
    }
    if (!(magnitude > 0.0F) || !std::isfinite(magnitude)) {
        return false;
    }
    const auto tolerance = magnitude * relative_tolerance;

    for (std::size_t row = 0U; row < 4U; ++row) {
        for (std::size_t column = 0U; column < 4U; ++column) {
            float sum = 0.0F;
            for (std::size_t k = 0U; k < 4U; ++k) {
                sum += a[mat_index(row, k)] * b[mat_index(k, column)];
            }
            const auto delta = sum - product[mat_index(row, column)];
            if (!std::isfinite(delta) || std::fabs(delta) > tolerance) {
                return false;
            }
        }
    }
    return true;
}

enum class InjectionVerdict : std::uint32_t
{
    injected_projection_and_view_projection,
    injected_projection_only,
    skipped_zero_offset,
    refused_layout_indeterminate,
    refused_not_the_camera_projection,
    refused_buffer_unavailable,
};

[[nodiscard]] constexpr const char* describe(
    const InjectionVerdict verdict) noexcept
{
    switch (verdict) {
    case InjectionVerdict::injected_projection_and_view_projection:
        return "the camera projection and view-projection were jittered";
    case InjectionVerdict::injected_projection_only:
        return "the camera projection was jittered; the view-projection did "
               "not verify as its product and was left untouched";
    case InjectionVerdict::skipped_zero_offset:
        return "no offset was requested for this frame";
    case InjectionVerdict::refused_layout_indeterminate:
        return "REFUSED: the projection is not a perspective matrix in either "
               "vector convention";
    case InjectionVerdict::refused_not_the_camera_projection:
        return "REFUSED: the projection does not place the live camera's near "
               "plane at an end of the depth interval";
    case InjectionVerdict::refused_buffer_unavailable:
        return "REFUSED: the mapped constant buffer was unavailable";
    }
    return "unknown";
}

[[nodiscard]] constexpr bool injected(const InjectionVerdict verdict) noexcept
{
    return verdict ==
               InjectionVerdict::injected_projection_and_view_projection ||
           verdict == InjectionVerdict::injected_projection_only;
}

struct InjectionResult final
{
    InjectionVerdict verdict{InjectionVerdict::refused_buffer_unavailable};
    ProjectionLayout layout{ProjectionLayout::indeterminate};
    float ndc_x{};
    float ndc_y{};
};

[[nodiscard]] inline InjectionResult inject_raster_jitter(
    float* const projection,
    float* const view_projection,
    const float* const view,
    const float near_plane,
    const float far_plane,
    const float ndc_x,
    const float ndc_y) noexcept
{
    InjectionResult result{};
    result.ndc_x = ndc_x;
    result.ndc_y = ndc_y;
    if (projection == nullptr) {
        result.verdict = InjectionVerdict::refused_buffer_unavailable;
        return result;
    }
    if (!std::isfinite(ndc_x) || !std::isfinite(ndc_y)) {
        result.verdict = InjectionVerdict::refused_buffer_unavailable;
        return result;
    }
    if (detail::near_zero(ndc_x, 1.0e-9F) &&
        detail::near_zero(ndc_y, 1.0e-9F)) {
        result.verdict = InjectionVerdict::skipped_zero_offset;
        return result;
    }

    const auto layout = classify_projection_layout(projection);
    result.layout = layout;
    if (layout == ProjectionLayout::indeterminate) {
        result.verdict = InjectionVerdict::refused_layout_indeterminate;
        return result;
    }
    if (!projection_matches_camera(projection, layout, near_plane, far_plane)) {
        result.verdict = InjectionVerdict::refused_not_the_camera_projection;
        return result;
    }

    const auto composes =
        view_projection != nullptr && view != nullptr &&
        verify_matrix_product(view_projection, view, projection, layout);

    apply_clip_offset(projection, layout, ndc_x, ndc_y);
    if (composes) {
        apply_clip_offset(view_projection, layout, ndc_x, ndc_y);
        result.verdict =
            InjectionVerdict::injected_projection_and_view_projection;
    } else {
        result.verdict = InjectionVerdict::injected_projection_only;
    }
    return result;
}
}
