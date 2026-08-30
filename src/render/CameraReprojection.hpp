#pragma once

#include <DirectXMath.h>

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <limits>

namespace mfgdlss::render
{
struct CameraReprojectionMatrices
{
    DirectX::XMFLOAT4X4 clip_to_camera_view{};
    DirectX::XMFLOAT4X4 clip_to_previous_clip{};
    DirectX::XMFLOAT4X4 previous_clip_to_clip{};
    std::uint32_t previous_camera_unavailable{};
};

enum class CameraReprojectionFailure
{
    none,
    invalid_clip_to_camera_view,
    invalid_camera_view_to_world,
    invalid_previous_view_projection,
    invalid_clip_to_current_world,
    invalid_clip_to_previous_clip,
    noninvertible_clip_to_previous_clip
};

[[nodiscard]] inline const char* describe(
    const CameraReprojectionFailure failure) noexcept
{
    switch (failure) {
    case CameraReprojectionFailure::none:
        return "none";
    case CameraReprojectionFailure::invalid_clip_to_camera_view:
        return "Skyrim inverse projection is non-finite";
    case CameraReprojectionFailure::invalid_camera_view_to_world:
        return "Skyrim inverse view is non-finite";
    case CameraReprojectionFailure::invalid_previous_view_projection:
        return "Skyrim previous view-projection is non-finite";
    case CameraReprojectionFailure::invalid_clip_to_current_world:
        return "composed current clip-to-world is non-finite";
    case CameraReprojectionFailure::invalid_clip_to_previous_clip:
        return "current-to-previous clip transform is non-finite";
    case CameraReprojectionFailure::noninvertible_clip_to_previous_clip:
        return "current-to-previous clip transform is non-invertible";
    }
    return "unknown camera reprojection failure";
}

[[nodiscard]] inline bool camera_matrix_is_zero(
    const DirectX::XMFLOAT4X4& matrix) noexcept
{
    const auto* const values = &matrix.m[0][0];
    for (std::size_t index = 0; index < 16U; ++index) {
        if (values[index] != 0.0F) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool camera_matrix_is_finite(
    const DirectX::XMFLOAT4X4& matrix) noexcept
{
    const auto* const values = &matrix.m[0][0];
    for (std::size_t index = 0; index < 16U; ++index) {
        if (!std::isfinite(values[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool invert_camera_matrix(
    const DirectX::XMFLOAT4X4& matrix,
    DirectX::XMFLOAT4X4& inverse) noexcept
{
    DirectX::XMVECTOR determinant{};
    const auto inverted = DirectX::XMMatrixInverse(
        &determinant,
        DirectX::XMLoadFloat4x4(&matrix));
    const auto determinant_value = DirectX::XMVectorGetX(determinant);
    if (!std::isfinite(determinant_value) ||
        std::abs(determinant_value) <=
            (std::numeric_limits<float>::min)()) {
        return false;
    }
    DirectX::XMStoreFloat4x4(&inverse, inverted);
    return camera_matrix_is_finite(inverse);
}

[[nodiscard]] inline bool recover_forward_projection(
    const DirectX::XMFLOAT4X4& captured_inverse_projection,
    DirectX::XMFLOAT4X4& forward_projection) noexcept
{
    return invert_camera_matrix(
        captured_inverse_projection,
        forward_projection);
}

[[nodiscard]] inline CameraReprojectionMatrices
camera_reprojection_without_history(
    const DirectX::XMFLOAT4X4& clip_to_camera_view) noexcept
{
    CameraReprojectionMatrices output{};
    output.clip_to_camera_view = clip_to_camera_view;
    DirectX::XMStoreFloat4x4(
        &output.clip_to_previous_clip,
        DirectX::XMMatrixIdentity());
    DirectX::XMStoreFloat4x4(
        &output.previous_clip_to_clip,
        DirectX::XMMatrixIdentity());
    output.previous_camera_unavailable = 1U;
    return output;
}

[[nodiscard]] inline bool build_camera_reprojection(
    const DirectX::XMFLOAT4X4& clip_to_camera_view,
    const DirectX::XMFLOAT4X4& camera_view_to_world,
    const DirectX::XMFLOAT4X4& previous_view_projection,
    const DirectX::XMFLOAT3& current_position_adjust,
    const DirectX::XMFLOAT3& previous_position_adjust,
    CameraReprojectionMatrices& output,
    CameraReprojectionFailure* const failure = nullptr) noexcept
{
    const auto fail = [failure](const CameraReprojectionFailure reason) {
        if (failure != nullptr) {
            *failure = reason;
        }
        return false;
    };
    if (failure != nullptr) {
        *failure = CameraReprojectionFailure::none;
    }
    if (!camera_matrix_is_finite(clip_to_camera_view)) {
        return fail(CameraReprojectionFailure::invalid_clip_to_camera_view);
    }
    if (!camera_matrix_is_finite(camera_view_to_world)) {
        return fail(CameraReprojectionFailure::invalid_camera_view_to_world);
    }
    if (!camera_matrix_is_finite(previous_view_projection)) {
        return fail(
            CameraReprojectionFailure::invalid_previous_view_projection);
    }

    output.clip_to_camera_view = clip_to_camera_view;
    if (camera_matrix_is_zero(previous_view_projection)) {
        output = camera_reprojection_without_history(clip_to_camera_view);
        return true;
    }
    DirectX::XMFLOAT4X4 clip_to_current_world{};
    DirectX::XMStoreFloat4x4(
        &clip_to_current_world,
        DirectX::XMMatrixMultiply(
            DirectX::XMLoadFloat4x4(&clip_to_camera_view),
            DirectX::XMLoadFloat4x4(&camera_view_to_world)));
    if (!camera_matrix_is_finite(clip_to_current_world)) {
        return fail(
            CameraReprojectionFailure::invalid_clip_to_current_world);
    }

    const auto current_to_previous_origin = DirectX::XMMatrixTranslation(
        current_position_adjust.x - previous_position_adjust.x,
        current_position_adjust.y - previous_position_adjust.y,
        current_position_adjust.z - previous_position_adjust.z);
    const auto clip_to_previous = DirectX::XMMatrixMultiply(
        DirectX::XMMatrixMultiply(
            DirectX::XMLoadFloat4x4(&clip_to_current_world),
            current_to_previous_origin),
        DirectX::XMLoadFloat4x4(&previous_view_projection));
    DirectX::XMStoreFloat4x4(
        &output.clip_to_previous_clip,
        clip_to_previous);
    if (!camera_matrix_is_finite(output.clip_to_previous_clip)) {
        return fail(
            CameraReprojectionFailure::invalid_clip_to_previous_clip);
    }
    if (!invert_camera_matrix(
            output.clip_to_previous_clip,
            output.previous_clip_to_clip)) {
        return fail(
            CameraReprojectionFailure::noninvertible_clip_to_previous_clip);
    }
    return true;
}
}
