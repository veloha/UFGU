#include "render/CameraReprojection.hpp"

#include <DirectXMath.h>

#include <cstddef>
#include <cmath>
#include <cstdio>
#include <cstring>

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

[[nodiscard]] DirectX::XMFLOAT4X4 stored(
    const DirectX::XMMATRIX& matrix) noexcept
{
    DirectX::XMFLOAT4X4 result{};
    DirectX::XMStoreFloat4x4(&result, matrix);
    return result;
}

[[nodiscard]] bool near(
    const DirectX::XMFLOAT4X4& left,
    const DirectX::XMFLOAT4X4& right,
    const float epsilon = 1.0e-4F) noexcept
{
    const auto* const a = &left.m[0][0];
    const auto* const b = &right.m[0][0];
    for (std::size_t index = 0; index < 16U; ++index) {
        if (std::abs(a[index] - b[index]) > epsilon) {
            return false;
        }
    }
    return true;
}
}

int main()
{
    using namespace DirectX;
    using namespace mfgdlss::render;

    const auto identity = stored(XMMatrixIdentity());
    const auto projection = stored(
        XMMatrixPerspectiveFovLH(XM_PIDIV4, 16.0F / 9.0F, 15.0F, 353840.0F));
    XMFLOAT4X4 captured_inverse{};
    expect(
        invert_camera_matrix(projection, captured_inverse),
        "perspective projection is invertible");
    XMFLOAT4X4 recovered{};
    expect(
        recover_forward_projection(captured_inverse, recovered) &&
            near(recovered, projection),
        "captured inverse projection is recovered in full");

    CameraReprojectionMatrices same{};
    expect(
        build_camera_reprojection(
            captured_inverse,
            identity,
            projection,
            {},
            {},
            same) &&
            near(same.clip_to_camera_view, captured_inverse) &&
            near(same.clip_to_previous_clip, identity) &&
            near(same.previous_clip_to_clip, identity),
        "stationary camera maps current clip to identical previous clip");

    CameraReprojectionMatrices shifted{};
    expect(
        build_camera_reprojection(
            identity,
            identity,
            identity,
            {10.0F, 20.0F, 30.0F},
            {1.0F, 2.0F, 3.0F},
            shifted),
        "floating-origin shift builds a valid reprojection");
    const auto expected_shift = stored(XMMatrixTranslation(9.0F, 18.0F, 27.0F));
    expect(
        near(shifted.clip_to_previous_clip, expected_shift),
        "floating-origin delta follows current-adjust minus previous-adjust");

    const auto current_view = XMMatrixLookAtLH(
        XMVectorSet(3.0F, 2.0F, -5.0F, 1.0F),
        XMVectorSet(0.0F, 0.0F, 0.0F, 1.0F),
        XMVectorSet(0.0F, 1.0F, 0.0F, 0.0F));
    const auto previous_view = XMMatrixLookAtLH(
        XMVectorSet(2.5F, 2.0F, -5.0F, 1.0F),
        XMVectorSet(0.0F, 0.0F, 0.0F, 1.0F),
        XMVectorSet(0.0F, 1.0F, 0.0F, 0.0F));
    const auto current_view_projection = stored(
        XMMatrixMultiply(current_view, XMLoadFloat4x4(&projection)));
    const auto previous_view_projection = stored(
        XMMatrixMultiply(previous_view, XMLoadFloat4x4(&projection)));
    XMFLOAT4X4 current_view_inverse{};
    expect(
        invert_camera_matrix(stored(current_view), current_view_inverse),
        "moving current view has a supplied inverse");
    CameraReprojectionMatrices moving{};
    expect(
        build_camera_reprojection(
            captured_inverse,
            current_view_inverse,
            previous_view_projection,
            {},
            {},
            moving),
        "moving camera builds a valid reprojection");
    XMFLOAT4X4 inverse_current{};
    static_cast<void>(
        invert_camera_matrix(current_view_projection, inverse_current));
    const auto expected_moving = stored(XMMatrixMultiply(
        XMLoadFloat4x4(&inverse_current),
        XMLoadFloat4x4(&previous_view_projection)));
    expect(
        near(moving.clip_to_previous_clip, expected_moving),
        "camera reprojection equals inverse(current VP) times previous VP");
    const auto round_trip = stored(XMMatrixMultiply(
        XMLoadFloat4x4(&moving.clip_to_previous_clip),
        XMLoadFloat4x4(&moving.previous_clip_to_clip)));
    expect(
        near(round_trip, identity),
        "forward and inverse clip transforms round-trip");

    CameraReprojectionMatrices duplicate{};
    expect(
        build_camera_reprojection(
            captured_inverse,
            current_view_inverse,
            previous_view_projection,
            {},
            {},
            duplicate) &&
            std::memcmp(&duplicate, &moving, sizeof(moving)) == 0,
        "a second provider call in one frame returns bit-identical matrices");

    XMFLOAT4X4 singular{};
    CameraReprojectionMatrices rejected{};
    CameraReprojectionFailure failure{};
    expect(
        !build_camera_reprojection(
            singular,
            identity,
            identity,
            {},
            {},
            rejected,
            &failure) &&
            failure ==
                CameraReprojectionFailure::
                    noninvertible_clip_to_previous_clip,
        "singular camera matrix fails closed");

    const auto steep_view = XMMatrixLookToLH(
        XMVectorSet(125000.0F, -83000.0F, 47000.0F, 1.0F),
        XMVector3Normalize(
            XMVectorSet(0.0007F, -0.999999F, 0.0011F, 0.0F)),
        XMVectorSet(0.0F, 0.0F, 1.0F, 0.0F));
    const auto steep_view_projection = stored(
        XMMatrixMultiply(steep_view, XMLoadFloat4x4(&projection)));
    XMFLOAT4X4 steep_view_inverse{};
    expect(
        invert_camera_matrix(stored(steep_view), steep_view_inverse),
        "steep translated camera exposes its inverse view");
    CameraReprojectionMatrices steep{};
    expect(
        build_camera_reprojection(
            captured_inverse,
            steep_view_inverse,
            steep_view_projection,
            {},
            {},
            steep) &&
            near(steep.clip_to_previous_clip, identity, 5.0e-2F) &&
            near(steep.previous_clip_to_clip, identity, 5.0e-2F),
        "steep translated camera composes stable same-frame reprojection");

    {
        using mfgdlss::render::camera_matrix_is_finite;
        const auto identity_matrix = stored(DirectX::XMMatrixIdentity());
        expect(
            camera_matrix_is_finite(identity_matrix),
            "identity matrix is finite");

        const auto quiet_nan = std::numeric_limits<float>::quiet_NaN();
        const auto infinity = std::numeric_limits<float>::infinity();
        for (std::size_t index = 0; index < 16U; ++index) {
            auto poisoned = identity_matrix;
            (&poisoned.m[0][0])[index] = quiet_nan;
            expect(
                !camera_matrix_is_finite(poisoned),
                "a NaN anywhere in the matrix is rejected");
        }

        auto positive_infinity = identity_matrix;
        positive_infinity.m[3][3] = infinity;
        expect(
            !camera_matrix_is_finite(positive_infinity),
            "positive infinity is rejected");

        auto negative_infinity = identity_matrix;
        negative_infinity.m[0][0] = -infinity;
        expect(
            !camera_matrix_is_finite(negative_infinity),
            "negative infinity is rejected");
    }

    {
        using mfgdlss::render::build_camera_reprojection;
        using mfgdlss::render::CameraReprojectionFailure;
        using mfgdlss::render::CameraReprojectionMatrices;

        const auto identity_matrix = stored(DirectX::XMMatrixIdentity());
        const auto quiet_nan = std::numeric_limits<float>::quiet_NaN();
        CameraReprojectionMatrices output{};
        auto reason = CameraReprojectionFailure::none;

        auto poisoned = identity_matrix;
        poisoned.m[2][2] = quiet_nan;

        expect(
            !build_camera_reprojection(
                poisoned, identity_matrix, identity_matrix, {}, {}, output,
                &reason) &&
                reason ==
                    CameraReprojectionFailure::invalid_clip_to_camera_view,
            "a non-finite inverse projection is named as such");

        expect(
            !build_camera_reprojection(
                identity_matrix, poisoned, identity_matrix, {}, {}, output,
                &reason) &&
                reason ==
                    CameraReprojectionFailure::invalid_camera_view_to_world,
            "a non-finite inverse view is named as such");

        expect(
            !build_camera_reprojection(
                identity_matrix, identity_matrix, poisoned, {}, {}, output,
                &reason) &&
                reason ==
                    CameraReprojectionFailure::
                        invalid_previous_view_projection,
            "a non-finite previous view-projection is named as such");

        expect(
            !build_camera_reprojection(
                poisoned, poisoned, poisoned, {}, {}, output, &reason) &&
                reason ==
                    CameraReprojectionFailure::invalid_clip_to_camera_view,
            "the earliest non-finite input is the one reported");

        reason = CameraReprojectionFailure::invalid_clip_to_previous_clip;
        expect(
            build_camera_reprojection(
                identity_matrix, identity_matrix, identity_matrix, {}, {},
                output, &reason) &&
                reason == CameraReprojectionFailure::none,
            "success clears the failure reason");

        expect(
            !build_camera_reprojection(
                poisoned, identity_matrix, identity_matrix, {}, {}, output,
                nullptr),
            "a null failure pointer is safe on the failing path");
        expect(
            build_camera_reprojection(
                identity_matrix, identity_matrix, identity_matrix, {}, {},
                output, nullptr),
            "a null failure pointer is safe on the succeeding path");

        auto enormous = identity_matrix;
        for (std::size_t index = 0; index < 16U; ++index) {
            (&enormous.m[0][0])[index] = 1.0e38F;
        }
        expect(
            !build_camera_reprojection(
                enormous, enormous, identity_matrix, {}, {}, output,
                &reason) &&
                reason ==
                    CameraReprojectionFailure::invalid_clip_to_current_world,
            "finite inputs that overflow when composed are named as such");
    }

    {
        using mfgdlss::render::CameraReprojectionFailure;
        using mfgdlss::render::describe;
        constexpr CameraReprojectionFailure all[]{
            CameraReprojectionFailure::none,
            CameraReprojectionFailure::invalid_clip_to_camera_view,
            CameraReprojectionFailure::invalid_camera_view_to_world,
            CameraReprojectionFailure::invalid_previous_view_projection,
            CameraReprojectionFailure::invalid_clip_to_current_world,
            CameraReprojectionFailure::invalid_clip_to_previous_clip,
            CameraReprojectionFailure::noninvertible_clip_to_previous_clip};
        bool every_reason_described = true;
        for (const auto failure_reason : all) {
            const auto* const text = describe(failure_reason);
            every_reason_described = every_reason_described &&
                text != nullptr && text[0] != '\0' &&
                std::strcmp(text, "unknown camera reprojection failure") != 0;
        }
        expect(
            every_reason_described,
            "every camera reprojection failure has its own description");
    }

    {
        using mfgdlss::render::build_camera_reprojection;
        using mfgdlss::render::camera_reprojection_without_history;
        using mfgdlss::render::CameraReprojectionMatrices;

        DirectX::XMFLOAT4X4 clip_to_camera_view{};
        DirectX::XMStoreFloat4x4(
            &clip_to_camera_view,
            DirectX::XMMatrixScaling(2.0F, 3.0F, 4.0F));

        const auto substituted =
            camera_reprojection_without_history(clip_to_camera_view);
        expect(
            substituted.previous_camera_unavailable != 0U,
            "a substituted reprojection announces that it has no history");

        auto identity_both_ways = true;
        for (auto row = 0; row < 4; ++row) {
            for (auto column = 0; column < 4; ++column) {
                const auto expected = row == column ? 1.0F : 0.0F;
                identity_both_ways = identity_both_ways &&
                    substituted.clip_to_previous_clip.m[row][column] ==
                        expected &&
                    substituted.previous_clip_to_clip.m[row][column] ==
                        expected;
            }
        }
        expect(
            identity_both_ways,
            "both reprojection directions are exactly identity, so nothing is "
            "displaced on a frame with no history");
        expect(
            substituted.clip_to_camera_view.m[0][0] ==
                clip_to_camera_view.m[0][0],
            "the projection Skyrim actually supplied is preserved");

        const DirectX::XMFLOAT4X4 zero_previous{};
        DirectX::XMFLOAT4X4 camera_view_to_world{};
        DirectX::XMStoreFloat4x4(
            &camera_view_to_world, DirectX::XMMatrixIdentity());
        CameraReprojectionMatrices from_zero{};
        const auto accepted = build_camera_reprojection(
            clip_to_camera_view,
            camera_view_to_world,
            zero_previous,
            DirectX::XMFLOAT3{0.0F, 0.0F, 0.0F},
            DirectX::XMFLOAT3{0.0F, 0.0F, 0.0F},
            from_zero);
        expect(
            accepted,
            "an all-zero previous view-projection is accepted rather than "
            "refusing the whole camera contract");
        expect(
            std::memcmp(&from_zero, &substituted, sizeof(from_zero)) == 0,
            "the zero-history path and the substitution helper produce an "
            "identical state, so there is only one no-history contract");
    }

    std::printf(
        "=== CameraReprojectionTest: %d passed, %d failed ===\n",
        passed,
        failed);
    return failed == 0 ? 0 : 1;
}
