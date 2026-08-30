#pragma once

#include "render/DepthContract.hpp"
#include "render/JitterContract.hpp"

#include <cstdint>

struct ID3D11DeviceContext;

namespace sl
{
struct Constants;
}

namespace mfgdlss::render
{
class CameraData final
{
public:
    [[nodiscard]] static CameraData& instance() noexcept;

    [[nodiscard]] bool install(ID3D11DeviceContext* context);
    [[nodiscard]] bool build_constants(
        sl::Constants& constants,
        bool reset = false,
        bool motion_vectors_dilated = false) const noexcept;
    void shutdown() noexcept;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] bool temporal_inputs_valid() const noexcept;

    [[nodiscard]] bool camera_planes(
        float& near_plane,
        float& far_plane) const noexcept;
    [[nodiscard]] bool camera_fov_vertical(
        float& radians) const noexcept;
    [[nodiscard]] std::uint64_t capture_sequence() const noexcept;

    static void set_capture_frozen(bool frozen) noexcept;
    [[nodiscard]] static std::uint64_t frozen_write_count() noexcept;

    [[nodiscard]] DepthOrientation depth_orientation() const noexcept;
    [[nodiscard]] DepthRange depth_range() const noexcept;
    [[nodiscard]] bool depth_reversed_for_display() const noexcept;
    [[nodiscard]] bool depth_infinite_for_display() const noexcept;

    [[nodiscard]] DepthContractSnapshot depth_contract() const noexcept;

    [[nodiscard]] bool frame_matrices(
        float (&view)[16],
        float (&projection)[16]) const noexcept;

    [[nodiscard]] bool camera_basis(
        float (&position)[3],
        float (&up)[3],
        float (&right)[3],
        float (&forward)[3]) const noexcept;

    DepthOrientation measure_depth_contract() const noexcept;

    [[nodiscard]] ReportedJitter reported_jitter() const noexcept;

    [[nodiscard]] ReportedJitter frame_jitter() const noexcept;

    [[nodiscard]] bool engine_applied_jitter() const noexcept;

private:
    bool installed_{};
};
}
