#include "render/CameraData.hpp"

#include "config/Settings.hpp"
#include "render/CameraReprojection.hpp"
#include "render/DepthContract.hpp"
#include "render/DynamicResolution.hpp"
#include "render/JitterContract.hpp"
#include "render/JitterInjection.hpp"
#include "render/JitterOwnership.hpp"
#include "streamline/SuperResolution.hpp"

#include <Windows.h>
#include <d3d11.h>

#include <DirectXMath.h>
#include <REL/Relocation.h>
#include <SKSE/SKSE.h>

#include <sl_consts.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>

namespace mfgdlss::render
{
namespace
{
namespace logger = SKSE::log;
using Matrix = DirectX::XMFLOAT4X4;
using Vector4 = DirectX::XMFLOAT4;

struct FrameBuffer
{
    Matrix camera_view;
    Matrix camera_projection;
    Matrix camera_view_projection;
    Matrix camera_view_projection_unjittered;
    Matrix camera_previous_view_projection_unjittered;
    Matrix camera_projection_unjittered;
    Matrix camera_projection_unjittered_inverse;
    Matrix camera_view_inverse;
    Matrix camera_view_projection_inverse;
    Matrix camera_projection_inverse;
    Vector4 camera_position_adjust;
    Vector4 camera_previous_position_adjust;
    Vector4 frame_parameters;
    Vector4 dynamic_resolution_parameters_1;
    Vector4 dynamic_resolution_parameters_2;
};

static_assert(sizeof(FrameBuffer) == 720);

using MapFunction = HRESULT(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*,
    ID3D11Resource*,
    UINT,
    D3D11_MAP,
    UINT,
    D3D11_MAPPED_SUBRESOURCE*);
using UnmapFunction = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*,
    ID3D11Resource*,
    UINT);

MapFunction original_map{};
UnmapFunction original_unmap{};
REL::Relocation<ID3D11Buffer**> per_frame_buffer;
float* camera_near{};
float* camera_far{};
ID3D11Resource* mapped_resource{};
void* mapped_bytes{};
bool mapped_writable{};
FrameBuffer cached_frame{};
std::atomic_uint64_t non_world_camera_writes{};
std::atomic_bool capture_frozen{};
std::atomic_uint64_t frozen_writes{};
std::atomic_uint64_t world_camera_writes{};

[[nodiscard]] bool looks_like_world_camera(
    const FrameBuffer& candidate) noexcept
{
    const auto w = candidate.camera_projection.m[3][3];
    return w > -0.5F && w < 0.5F;
}
std::atomic_bool cached_frame_valid{};
std::atomic_uint64_t cached_frame_sequence{};
std::atomic_bool capture_enabled{};

std::uint64_t frame_jitter_sequence{(std::numeric_limits<std::uint64_t>::max)()};
config::JitterSource frame_jitter_source{config::JitterSource::requested};
ReportedJitter frame_jitter_snapshot{};

std::atomic<float> injected_ndc_x{};
std::atomic<float> injected_ndc_y{};
std::atomic_bool injection_active{};
std::atomic<std::uint32_t> injection_verdict{
    static_cast<std::uint32_t>(InjectionVerdict::refused_buffer_unavailable)};
std::atomic<std::uint32_t> injection_layout{
    static_cast<std::uint32_t>(ProjectionLayout::indeterminate)};

BoundedLogBudget injection_log_budget{6U};
BoundedLogBudget jitter_log_budget{8U};
std::atomic_bool engine_applies_jitter{};
std::atomic<float> relocated_ndc_x{};
std::atomic<float> relocated_ndc_y{};
std::atomic_bool relocated_found{};

[[nodiscard]] constexpr std::uint32_t encode_depth_contract(
    const DepthOrientation orientation, const DepthRange range) noexcept
{
    return (static_cast<std::uint32_t>(orientation) << 16U) |
           (static_cast<std::uint32_t>(range) & 0xFFFFU);
}

std::atomic<std::uint32_t> measured_depth_contract{
    encode_depth_contract(
        DepthOrientation::indeterminate, DepthRange::indeterminate)};
std::atomic_bool depth_orientation_logged{};

void note_depth_orientation(
    const DepthOrientation orientation,
    const DepthRange range,
    const DepthProjection& projection) noexcept
{
    const auto encoded = encode_depth_contract(orientation, range);
    const auto previous =
        measured_depth_contract.exchange(encoded, std::memory_order_relaxed);
    if (previous == encoded &&
        depth_orientation_logged.load(std::memory_order_relaxed)) {
        return;
    }
    depth_orientation_logged.store(true, std::memory_order_relaxed);
    float at_near = 0.0F;
    float at_far = 0.0F;
    static_cast<void>(
        ndc_depth_at(projection, projection.near_plane, at_near));
    static_cast<void>(
        ndc_depth_at(projection, projection.near_plane * 10000.0F, at_far));
    logger::info(
        "Depth convention measured from Skyrim's own projection: {}, {}. "
        "near={:.3f} -> NDC {:.6f}; 10000x near -> NDC {:.6f}; "
        "m33={:.6f} m43={:.6f} m34={:.6f} m44={:.6f}; far-plane value={:.1f}. "
        "Streamline depthInverted is set from this, not from a constant.",
        describe(orientation),
        describe(range),
        projection.near_plane,
        at_near,
        at_far,
        projection.m33,
        projection.m43,
        projection.m34,
        projection.m44,
        projection.far_plane);
}

std::atomic<std::uint32_t> measured_projection_form{
    static_cast<std::uint32_t>(ProjectionForm::indeterminate)};
std::atomic_bool projection_form_logged{};

void note_projection_form(
    const ProjectionForm form,
    const DepthProjection& captured,
    const DepthProjection& normalized) noexcept
{
    const auto encoded = static_cast<std::uint32_t>(form);
    const auto previous =
        measured_projection_form.exchange(encoded, std::memory_order_relaxed);
    if (previous == encoded &&
        projection_form_logged.load(std::memory_order_relaxed)) {
        return;
    }
    projection_form_logged.store(true, std::memory_order_relaxed);
    logger::info(
        "Projection form measured: {}. captured m33={:.6f} m43={:.6f} "
        "m34={:.6f} m44={:.6f}; used m33={:.6f} m43={:.6f} m34={:.6f} "
        "m44={:.9f}; near={:.3f} far={:.1f}.",
        describe(form),
        captured.m33,
        captured.m43,
        captured.m34,
        captured.m44,
        normalized.m33,
        normalized.m43,
        normalized.m34,
        normalized.m44,
        normalized.near_plane,
        normalized.far_plane);
}

std::atomic<std::uint32_t> measured_jitter_verdict{
    static_cast<std::uint32_t>(JitterVerdict::indeterminate)};
std::atomic_bool jitter_contract_logged{};
std::atomic_bool forward_projection_logged{};
std::atomic_bool forward_projection_failure_logged{};
std::atomic_bool camera_reprojection_logged{};
std::atomic_bool camera_reprojection_failure_logged{};
std::atomic_bool previous_camera_unavailable_logged{};

std::atomic_uint32_t camera_reprojection_successes{};
std::atomic_uint32_t camera_reprojection_failures{};
std::atomic_uint32_t camera_history_substitutions{};

std::atomic<float> camera_identity_minimum{
    (std::numeric_limits<float>::max)()};

[[nodiscard]] ReportedJitter measure_and_report_jitter() noexcept
{
    const auto& dynamic_resolution = DynamicResolution::instance();
    const auto& super_resolution = streamline::SuperResolution::instance();

    JitterContract contract;
    contract.requested_pixels_x = dynamic_resolution.jitter_x();
    contract.requested_pixels_y = dynamic_resolution.jitter_y();
    contract.render_width = super_resolution.render_width();
    contract.render_height = super_resolution.render_height();

    contract.written_ndc_x = -pixels_to_ndc_offset(
        contract.requested_pixels_x, contract.render_width);
    contract.written_ndc_y = pixels_to_ndc_offset(
        contract.requested_pixels_y, contract.render_height);

    const auto engine_applied = locate_applied_jitter(
        &cached_frame.camera_projection.m[0][0],
        &cached_frame.camera_projection_unjittered.m[0][0]);

    const auto engine_owns =
        engine_applied.located &&
        (engine_applied.ndc_x != 0.0F || engine_applied.ndc_y != 0.0F);
    engine_applies_jitter.store(engine_owns, std::memory_order_relaxed);

    const auto injecting =
        !engine_owns && injection_active.load(std::memory_order_relaxed);
    const AppliedJitter applied = engine_applied;
    contract.applied_ndc_x = applied.ndc_x;
    contract.applied_ndc_y = applied.ndc_y;
    contract.applied_located = applied.located;

    const auto reported = reported_jitter_for(
        applied, contract.render_width, contract.render_height);
    contract.reported_pixels_x = reported.pixels_x;
    contract.reported_pixels_y = reported.pixels_y;

    contract.applied_is_comparable =
        !DynamicResolution::instance().jitter_fold_patched();
    const auto verdict = classify_jitter(contract);
    const auto encoded = static_cast<std::uint32_t>(verdict);
    measured_jitter_verdict.store(encoded, std::memory_order_relaxed);

    if (!jitter_log_budget.admit(
            encoded | (engine_owns ? 0x100U : 0U) |
            (injecting ? 0x200U : 0U))) {
        return reported;
    }
    jitter_contract_logged.store(true, std::memory_order_relaxed);

    const auto gate = DynamicResolution::instance().observe_fold_gate();

    float max_delta = 0.0F;
    std::size_t max_delta_index = 0U;
    {
        const auto* const jittered = &cached_frame.camera_projection.m[0][0];
        const auto* const clean =
            &cached_frame.camera_projection_unjittered.m[0][0];
        for (std::size_t index = 0; index < 16U; ++index) {
            const auto delta = std::fabs(jittered[index] - clean[index]);
            if (delta > max_delta) {
                max_delta = delta;
                max_delta_index = index;
            }
        }
    }
    logger::info(
        "Jitter contract: {}. source={}; engine-applied={:.6f},{:.6f} NDC; "
        "requested={:.4f},{:.4f} px; written={:.6f},{:.6f} NDC; "
        "raster={:.6f},{:.6f} NDC; "
        "reported to every provider={:.4f},{:.4f} px; render={}x{}; "
        "located={}; coherent={}. The reported value is derived from the "
        "raster this frame was drawn with, not from the request, so it cannot "
        "disagree with the frame. "
        "FOLD GATE at camera-publish: projectionPosScale={:.8f},{:.8f} "
        "(state-available={}), live-TAA-state={} (available={}), "
        "branches-patched={}, "
        "CameraProj-vs-Unjittered max-delta={:.9f} at element {} of 16. "
        "A max-delta of exactly 0 means the two matrices are bit-identical "
        "and the fold did not run at all; a non-zero max-delta at an element "
        "other than 2/6/8/9 means the engine folded something the jitter "
        "locator does not inspect. "
        "Non-zero projectionPosScale with engine-applied 0,0 means the engine "
        "had the offset and did not fold it, so the gate rejected. Zero "
        "projectionPosScale means it was cleared before the camera build. An "
        "unavailable TAA state means this plugin's writes to it did nothing.",
        describe(verdict),
        describe(select_jitter_owner(
            JitterOwnershipState{
                true,
                contract.requested_pixels_x != 0.0F ||
                    contract.requested_pixels_y != 0.0F,
                engine_owns,
                0U,
                false},
            injecting)),
        engine_applied.ndc_x,
        engine_applied.ndc_y,
        contract.requested_pixels_x,
        contract.requested_pixels_y,
        contract.written_ndc_x,
        contract.written_ndc_y,
        contract.applied_ndc_x,
        contract.applied_ndc_y,
        contract.reported_pixels_x,
        contract.reported_pixels_y,
        contract.render_width,
        contract.render_height,
        contract.applied_located,
        reported.coherent,
        gate.projection_scale_x,
        gate.projection_scale_y,
        gate.state_available,
        gate.taa_flag_enabled,
        gate.taa_flag_available,
        DynamicResolution::instance().jitter_fold_patched(),
        max_delta,
        max_delta_index);
    return reported;
}

HRESULT STDMETHODCALLTYPE map_hook(
    ID3D11DeviceContext* context,
    ID3D11Resource* resource,
    const UINT subresource,
    const D3D11_MAP map_type,
    const UINT map_flags,
    D3D11_MAPPED_SUBRESOURCE* mapped)
{
    const auto result = original_map(
        context,
        resource,
        subresource,
        map_type,
        map_flags,
        mapped);
    if (capture_enabled.load(std::memory_order_relaxed) &&
        SUCCEEDED(result) &&
        mapped != nullptr &&
        per_frame_buffer.get() != nullptr &&
        *per_frame_buffer.get() == resource) {
        mapped_resource = resource;

        mapped_bytes = mapped->pData;

        mapped_writable =
            map_type == D3D11_MAP_WRITE ||
            map_type == D3D11_MAP_WRITE_DISCARD ||
            map_type == D3D11_MAP_WRITE_NO_OVERWRITE ||
            map_type == D3D11_MAP_READ_WRITE;
    }
    return result;
}

void inject_frame_jitter(void* const bytes) noexcept
{
    if (bytes == nullptr || camera_near == nullptr || camera_far == nullptr) {
        injection_active.store(false, std::memory_order_relaxed);
        return;
    }

    const auto& dynamic_resolution = DynamicResolution::instance();
    const auto& super_resolution = streamline::SuperResolution::instance();
    const auto render_width = super_resolution.render_width();
    const auto render_height = super_resolution.render_height();

    const auto ndc_x = -pixels_to_ndc_offset(
        dynamic_resolution.jitter_x(), render_width);
    const auto ndc_y = pixels_to_ndc_offset(
        dynamic_resolution.jitter_y(), render_height);

    JitterOwnershipState ownership{};
    ownership.upscaler_active = true;
    ownership.offset_requested = ndc_x != 0.0F || ndc_y != 0.0F;
    ownership.engine_applied_jitter =
        engine_applies_jitter.load(std::memory_order_relaxed);
    if (!injection_permitted(ownership)) {
        injection_active.store(false, std::memory_order_relaxed);
        injected_ndc_x.store(0.0F, std::memory_order_relaxed);
        injected_ndc_y.store(0.0F, std::memory_order_relaxed);
        return;
    }

    auto* const frame = static_cast<FrameBuffer*>(bytes);
    const auto result = inject_raster_jitter(
        &frame->camera_projection.m[0][0],
        &frame->camera_view_projection.m[0][0],
        &frame->camera_view.m[0][0],
        *camera_near,
        *camera_far,
        ndc_x,
        ndc_y);

    const auto active = injected(result.verdict);
    injection_active.store(active, std::memory_order_relaxed);
    injected_ndc_x.store(
        active ? result.ndc_x : 0.0F, std::memory_order_relaxed);
    injected_ndc_y.store(
        active ? result.ndc_y : 0.0F, std::memory_order_relaxed);
    injection_layout.store(
        static_cast<std::uint32_t>(result.layout), std::memory_order_relaxed);

    AppliedJitter relocated{};
    if (active) {
        relocated = locate_applied_jitter(
            &frame->camera_projection.m[0][0],
            &cached_frame.camera_projection.m[0][0]);
    }
    relocated_found.store(relocated.located, std::memory_order_relaxed);
    relocated_ndc_x.store(relocated.ndc_x, std::memory_order_relaxed);
    relocated_ndc_y.store(relocated.ndc_y, std::memory_order_relaxed);

    const auto encoded = static_cast<std::uint32_t>(result.verdict);
    injection_verdict.store(encoded, std::memory_order_relaxed);
    if (!injection_log_budget.admit(encoded)) {
        return;
    }
    logger::info(
        "Raster jitter injection: {}. layout={}; offset={:.6f},{:.6f} NDC "
        "({:.4f},{:.4f} px at {}x{}); independently re-located in the patched "
        "matrix at {:.6f},{:.6f} NDC (found={}). Skyrim folds its own jitter "
        "in only along the vanilla TAA path, which this plugin disables, so "
        "the offset is written into the mapped camera matrices instead.",
        describe(result.verdict),
        describe(result.layout),
        result.ndc_x,
        result.ndc_y,
        dynamic_resolution.jitter_x(),
        dynamic_resolution.jitter_y(),
        render_width,
        render_height,
        relocated.ndc_x,
        relocated.ndc_y,
        relocated.located);
    if (injection_log_budget.exhausted()) {
        logger::info(
            "Raster jitter injection: further reports suppressed. The verdict "
            "alternates within a frame because this buffer is mapped more than "
            "once with different contents, and an unbounded report is a "
            "per-frame disk write.");
    }
}

void STDMETHODCALLTYPE unmap_hook(
    ID3D11DeviceContext* context,
    ID3D11Resource* resource,
    const UINT subresource)
{
    if (capture_enabled.load(std::memory_order_relaxed) &&
        resource == mapped_resource &&
        mapped_bytes != nullptr) {

        FrameBuffer candidate{};
        std::memcpy(&candidate, mapped_bytes, sizeof(candidate));
        const auto frozen = capture_frozen.load(std::memory_order_relaxed);
        if (frozen) {
            const auto count =
                frozen_writes.fetch_add(1, std::memory_order_relaxed) + 1ULL;
            if (count == 1ULL || count == 1000ULL ||
                count % 100000ULL == 0ULL) {
                logger::warn(
                    "Ignored a camera write that arrived AFTER scene "
                    "finalisation, occurrence {}. Skyrim's screen blood pass "
                    "calls SetCameraData(ScreenSplatter camera) as the last "
                    "draw of the world render, which is after this point, and "
                    "the frame constant buffer is last-write-wins. Accepting "
                    "it replaced the world camera with a screen-space one, "
                    "leaving no usable previous view-projection and forcing an "
                    "identity reprojection with Streamline's reset flag every "
                    "frame. That stops DLSS-G interpolating, so it presents "
                    "duplicates that look like plain base FPS while the output "
                    "rate still reads 6x",
                    count);
            }
        }
        const auto world_camera = !frozen && looks_like_world_camera(candidate);
        if (world_camera) {
            world_camera_writes.fetch_add(1, std::memory_order_relaxed);
            std::memcpy(
                &cached_frame,
                mapped_bytes,
                sizeof(cached_frame));
        } else {
            const auto count =
                non_world_camera_writes.fetch_add(
                    1, std::memory_order_relaxed) + 1ULL;
            if (count == 1ULL || count == 100ULL || count == 10000ULL ||
                count % 100000ULL == 0ULL) {
                logger::warn(
                    "Rejected a NON-WORLD camera write to Skyrim's frame "
                    "constant buffer, occurrence {}. Its projection has "
                    "m[3][3]={:.4f}, which is orthographic rather than "
                    "perspective, so it is not the world camera. Skyrim's "
                    "screen blood pass calls SetCameraData(ScreenSplatter "
                    "camera) as the LAST draw of the world render, and this "
                    "buffer is mapped several times per frame with different "
                    "contents, so before 2026-08-29 the last write won and "
                    "the splatter camera became the cached world camera. That "
                    "left no usable previous view-projection, forcing an "
                    "identity reprojection with Streamline's reset flag on "
                    "every frame, which stops DLSS-G interpolating and "
                    "presents duplicates that look like plain base FPS while "
                    "the output rate still reads 6x",
                    count,
                    candidate.camera_projection.m[3][3]);
            }
        }
        if (mapped_writable) {
            inject_frame_jitter(mapped_bytes);
        } else {
            injection_active.store(false, std::memory_order_relaxed);
        }
        cached_frame_sequence.fetch_add(1, std::memory_order_release);
        cached_frame_valid.store(true, std::memory_order_release);
        mapped_resource = nullptr;
        mapped_bytes = nullptr;
        mapped_writable = false;
    }
    original_unmap(context, resource, subresource);
}

[[nodiscard]] Matrix transpose(const Matrix& matrix) noexcept
{
    Matrix result{};
    DirectX::XMStoreFloat4x4(
        &result,
        DirectX::XMMatrixTranspose(
            DirectX::XMLoadFloat4x4(&matrix)));
    return result;
}

[[nodiscard]] sl::float4x4 to_streamline_matrix(
    const Matrix& matrix) noexcept
{
    sl::float4x4 result{};
    static_assert(sizeof(result) == sizeof(matrix));
    std::memcpy(&result, &matrix, sizeof(result));
    return result;
}
}

CameraData& CameraData::instance() noexcept
{
    static CameraData data;
    return data;
}

bool CameraData::install(ID3D11DeviceContext* context)
{
    if (installed_) {
        capture_enabled.store(true, std::memory_order_relaxed);
        return true;
    }
    if (context == nullptr) {
        return false;
    }

    per_frame_buffer = REL::Relocation<ID3D11Buffer**>{
        REL::RelocationID(524768, 411384)};
    const auto camera_parameters =
        REL::RelocationID(517032, 403540).address();
    camera_near = reinterpret_cast<float*>(camera_parameters + 0x40);
    camera_far = reinterpret_cast<float*>(camera_parameters + 0x44);
    if (per_frame_buffer.address() == 0 ||
        camera_near == nullptr ||
        camera_far == nullptr) {
        logger::error("Unable to locate Skyrim camera-frame data");
        return false;
    }

    const auto vtable_address =
        *reinterpret_cast<std::uintptr_t*>(context);
    REL::Relocation<std::uintptr_t> vtable{vtable_address};
    original_map = reinterpret_cast<MapFunction>(
        vtable.write_vfunc(14, map_hook));
    original_unmap = reinterpret_cast<UnmapFunction>(
        vtable.write_vfunc(15, unmap_hook));
    if (original_map == nullptr || original_unmap == nullptr) {
        logger::error("Unable to install D3D11 camera-buffer hooks");
        return false;
    }

    installed_ = true;
    capture_enabled.store(true, std::memory_order_relaxed);
    logger::info("Native camera constant capture installed");
    return true;
}

bool CameraData::build_constants(
    sl::Constants& constants,
    const bool reset,
    const bool motion_vectors_dilated) const noexcept
{
    if (!temporal_inputs_valid()) {
        return false;
    }

    const auto view_inverse = transpose(cached_frame.camera_view_inverse);

    const auto captured_inverse_projection =
        transpose(cached_frame.camera_projection_unjittered);
    auto projection = captured_inverse_projection;
    if (recover_forward_projection(
            captured_inverse_projection,
            projection)) {
        if (!forward_projection_logged.exchange(
                true,
                std::memory_order_relaxed)) {
            logger::info(
                "Streamline cameraViewToClip now uses Skyrim's recovered "
                "forward unjittered projection");
        }
    } else if (!forward_projection_failure_logged.exchange(
                   true,
                   std::memory_order_relaxed)) {
        logger::warn(
            "Unable to recover Skyrim's forward projection; preserving the "
            "BFEB camera matrix for this frame without suspending generation");
    }

    constants.cameraAspectRatio =
        std::abs(projection._22 / projection._11);
    constants.cameraFOV =
        2.0F * std::atan(1.0F / std::abs(projection._22));
    constants.cameraNear = *camera_near;
    constants.cameraFar = *camera_far;
    constants.cameraMotionIncluded = sl::Boolean::eTrue;
    constants.cameraPinholeOffset = {0.0F, 0.0F};
    constants.cameraRight = {
        view_inverse._11,
        view_inverse._12,
        view_inverse._13};
    constants.cameraUp = {
        view_inverse._21,
        view_inverse._22,
        view_inverse._23};
    constants.cameraFwd = {
        view_inverse._31,
        view_inverse._32,
        view_inverse._33};
    constants.cameraPos = {
        cached_frame.camera_position_adjust.x,
        cached_frame.camera_position_adjust.y,
        cached_frame.camera_position_adjust.z};
    constants.cameraViewToClip =
        to_streamline_matrix(projection);

    const auto previous_view_projection =
        transpose(cached_frame.camera_previous_view_projection_unjittered);
    const DirectX::XMFLOAT3 current_position_adjust{
        cached_frame.camera_position_adjust.x,
        cached_frame.camera_position_adjust.y,
        cached_frame.camera_position_adjust.z};
    const DirectX::XMFLOAT3 previous_position_adjust{
        cached_frame.camera_previous_position_adjust.x,
        cached_frame.camera_previous_position_adjust.y,
        cached_frame.camera_previous_position_adjust.z};
    CameraReprojectionMatrices reprojection{};
    CameraReprojectionFailure reprojection_failure{};
    auto history_reset = false;
    if (!build_camera_reprojection(
            captured_inverse_projection,
            view_inverse,
            previous_view_projection,
            current_position_adjust,
            previous_position_adjust,
            reprojection,
            &reprojection_failure)) {
        const auto degenerate_history = reprojection_failure ==
            CameraReprojectionFailure::noninvertible_clip_to_previous_clip;
        if (!camera_reprojection_failure_logged.exchange(
                true,
                std::memory_order_relaxed)) {
            if (degenerate_history) {
                logger::warn(
                    "Streamline camera history rejected: {}. Skyrim's previous "
                    "view-projection is finite but degenerate, so this frame "
                    "is submitted with an identity reprojection and marked as "
                    "a history reset rather than having its whole camera "
                    "contract refused, which used to cost the frame its depth "
                    "and motion tags and drop generation to 1x",
                    describe(reprojection_failure));
            } else {
                logger::warn(
                    "Streamline camera history rejected: {}. This frame will "
                    "not reuse a sample-only static previous-camera state.",
                    describe(reprojection_failure));
            }
        }

        camera_reprojection_failures.fetch_add(1, std::memory_order_relaxed);
        if (!degenerate_history) {
            return false;
        }
        reprojection =
            camera_reprojection_without_history(captured_inverse_projection);
    } else {
        camera_reprojection_failure_logged.store(
            false, std::memory_order_relaxed);
    }
    if (reprojection.previous_camera_unavailable != 0U) {
        history_reset = true;
        camera_history_substitutions.fetch_add(1, std::memory_order_relaxed);
        if (!previous_camera_unavailable_logged.exchange(
                true,
                std::memory_order_relaxed)) {
            logger::info(
                "Skyrim had no usable previous view-projection for this frame, "
                "which is what every history reset leaves behind, so the frame "
                "is submitted with an identity reprojection and Streamline's "
                "reset flag set. The reset flag is the field NVIDIA documents "
                "for a frame with no connection to the one before it; "
                "cameraMotionIncluded describes Skyrim's motion vector buffer, "
                "which carries camera motion on every frame including this one");
        }
    } else {
        previous_camera_unavailable_logged.store(
            false, std::memory_order_relaxed);
    }

    if (reprojection.previous_camera_unavailable == 0U) {
        const auto successes =
            camera_reprojection_successes.fetch_add(
                1, std::memory_order_relaxed) + 1U;
        const auto& m = reprojection.clip_to_previous_clip;
        auto deviation = 0.0F;
        for (auto row = 0; row < 4; ++row) {
            for (auto column = 0; column < 4; ++column) {
                const auto expected = row == column ? 1.0F : 0.0F;
                deviation = (std::max)(
                    deviation,
                    std::abs(m.m[row][column] - expected));
            }
        }
        auto observed_minimum =
            camera_identity_minimum.load(std::memory_order_relaxed);
        while (deviation < observed_minimum &&
               !camera_identity_minimum.compare_exchange_weak(
                   observed_minimum,
                   deviation,
                   std::memory_order_relaxed)) {
        }
        if (successes % 3000U == 0U) {
            const auto failures =
                camera_reprojection_failures.load(std::memory_order_relaxed);
            const auto substitutions =
                camera_history_substitutions.load(std::memory_order_relaxed);
            const auto minimum =
                camera_identity_minimum.exchange(
                    (std::numeric_limits<float>::max)(),
                    std::memory_order_relaxed);
            logger::info(
                "Camera history census: {} accepted, {} rejected ({:.2f}% of "
                "attempts), {} submitted with an identity reprojection and the "
                "reset flag. clipToPrevClip deviation from identity, measured "
                "only on frames with real history: current {:.6f}, minimum "
                "this window {:.6f}. Standing still must drive the minimum to "
                "~0; anything else means the previous-camera matrices are "
                "wrong rather than merely invertible.",
                successes,
                failures,
                100.0 * static_cast<double>(failures) /
                    static_cast<double>(successes + failures),
                substitutions,
                deviation,
                minimum);
        }
    }
    constants.clipToCameraView =
        to_streamline_matrix(reprojection.clip_to_camera_view);
    constants.clipToPrevClip =
        to_streamline_matrix(reprojection.clip_to_previous_clip);
    constants.prevClipToClip =
        to_streamline_matrix(reprojection.previous_clip_to_clip);
    if (!camera_reprojection_logged.exchange(
            true,
            std::memory_order_relaxed)) {
        logger::info(
            "Streamline camera history now follows Skyrim's captured current "
            "and previous frame matrices");
    }

    const auto orientation = measure_depth_contract();
    const auto measured_inverted = orientation == DepthOrientation::reversed;

    const auto depth_override =
        config::Settings::instance().depth_inverted_override();
    const auto effective_inverted =
        depth_override == config::DepthInvertedOverride::force_inverted ? true :
        depth_override == config::DepthInvertedOverride::force_standard ? false :
        measured_inverted;
    constants.depthInverted =
        effective_inverted ? sl::Boolean::eTrue : sl::Boolean::eFalse;

    const auto jitter = frame_jitter();
    constants.jitterOffset = {jitter.pixels_x, jitter.pixels_y};
    constants.reset =
        (reset || history_reset) ? sl::Boolean::eTrue : sl::Boolean::eFalse;

    const auto& motion_settings = config::Settings::instance();
    constants.mvecScale = {
        motion_settings.motion_scale_x(),
        motion_settings.motion_scale_y()};

    {
        static auto logged = false;
        static auto last_scale_x = 0.0F;
        static auto last_scale_y = 0.0F;
        static auto last_inverted = false;
        const auto scale_x = constants.mvecScale.x;
        const auto scale_y = constants.mvecScale.y;
        if (!logged || last_scale_x != scale_x || last_scale_y != scale_y ||
            last_inverted != effective_inverted) {
            logged = true;
            last_scale_x = scale_x;
            last_scale_y = scale_y;
            last_inverted = effective_inverted;
            logger::info(
                "Streamline camera constants: mvecScale=({:.4f}, {:.4f}) from "
                "[Upscaling] MotionScaleX/Y; depthInverted={} (projection "
                "measured {}, [Upscaling] DepthInverted={})",
                scale_x,
                scale_y,
                effective_inverted ? "true" : "false",
                measured_inverted ? "reversed-Z" : "standard-Z",
                depth_override == config::DepthInvertedOverride::force_inverted ?
                    "Inverted" :
                depth_override == config::DepthInvertedOverride::force_standard ?
                    "Standard" :
                    "Measured");
        }
    }
    constants.motionVectors3D = sl::Boolean::eFalse;
    constants.motionVectorsInvalidValue =
        (std::numeric_limits<float>::min)();
    constants.orthographicProjection = sl::Boolean::eFalse;
    const auto dilation_active =
        config::Settings::instance().motion_dilation() !=
        config::MotionDilation::off;
    constants.motionVectorsDilated =
        motion_vectors_dilated && dilation_active ?
            sl::Boolean::eTrue :
            sl::Boolean::eFalse;
    constants.minRelativeLinearDepthObjectSeparation =
        config::Settings::instance().depth_object_separation();

    {
        static auto separation_logged = false;
        static float last_separation = -1.0F;
        const auto separation =
            constants.minRelativeLinearDepthObjectSeparation;
        if (!separation_logged || last_separation != separation) {
            separation_logged = true;
            last_separation = separation;
            logger::info(
                "DLSS-G depth heuristics: "
                "minRelativeLinearDepthObjectSeparation={:.2f} (Streamline "
                "default is 40.0), motionVectorsDilated={}. NVIDIA documents "
                "that a SMALLER separation is needed when the depth range is "
                "compressed close to 1.0. Skyrim measures near=15 far=353840, "
                "so at the 40.0 default two pixels within roughly 600 game "
                "units are treated as ONE contiguous surface, which covers "
                "every leaf card against its own branch and every grass blade "
                "against the ground. Solid rock, water and sand are genuinely "
                "contiguous at that distance and are unaffected, which matches "
                "the measured selectivity of the artefact exactly",
                separation,
                constants.motionVectorsDilated == sl::Boolean::eTrue);
        }
    }
    constants.motionVectorsJittered = sl::Boolean::eFalse;
    return true;
}

DepthOrientation CameraData::measure_depth_contract() const noexcept
{
    if (!temporal_inputs_valid()) {
        return DepthOrientation::indeterminate;
    }

    const auto projection =
        transpose(cached_frame.camera_projection_unjittered);

    DepthProjection captured{};
    captured.m33 = projection._33;
    captured.m43 = projection._43;
    captured.m34 = projection._34;
    captured.m44 = projection._44;
    captured.near_plane = *camera_near;
    captured.far_plane = *camera_far;

    DepthProjection depth_projection{};
    const auto form = normalize_depth_projection(captured, depth_projection);
    const auto orientation = classify_depth_orientation(depth_projection);
    const auto range = classify_depth_range(depth_projection);
    note_projection_form(form, captured, depth_projection);
    note_depth_orientation(orientation, range, depth_projection);
    return orientation;
}

DepthContractSnapshot CameraData::depth_contract() const noexcept
{
    const auto encoded =
        measured_depth_contract.load(std::memory_order_relaxed);
    DepthContractSnapshot snapshot{};
    snapshot.orientation =
        static_cast<DepthOrientation>((encoded >> 16U) & 0xFFFFU);
    snapshot.range = static_cast<DepthRange>(encoded & 0xFFFFU);
    return snapshot;
}

bool CameraData::camera_basis(
    float (&position)[3],
    float (&up)[3],
    float (&right)[3],
    float (&forward)[3]) const noexcept
{
    if (!temporal_inputs_valid()) {
        return false;
    }

    const auto view_inverse = transpose(cached_frame.camera_view_inverse);
    right[0] = view_inverse._11;
    right[1] = view_inverse._12;
    right[2] = view_inverse._13;
    up[0] = view_inverse._21;
    up[1] = view_inverse._22;
    up[2] = view_inverse._23;
    forward[0] = view_inverse._31;
    forward[1] = view_inverse._32;
    forward[2] = view_inverse._33;
    position[0] = cached_frame.camera_position_adjust.x;
    position[1] = cached_frame.camera_position_adjust.y;
    position[2] = cached_frame.camera_position_adjust.z;
    return true;
}

bool CameraData::frame_matrices(
    float (&view)[16],
    float (&projection)[16]) const noexcept
{
    if (!temporal_inputs_valid()) {
        return false;
    }

    const auto view_matrix = transpose(cached_frame.camera_view);
    const auto captured_inverse_projection =
        transpose(cached_frame.camera_projection_unjittered);
    Matrix projection_matrix{};
    if (!recover_forward_projection(
            captured_inverse_projection,
            projection_matrix)) {
        return false;
    }
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            const auto index = static_cast<std::size_t>(row * 4 + column);
            view[index] = view_matrix.m[row][column];
            projection[index] = projection_matrix.m[row][column];
        }
    }
    return true;
}

DepthOrientation CameraData::depth_orientation() const noexcept
{
    return depth_contract().orientation;
}

DepthRange CameraData::depth_range() const noexcept
{
    return depth_contract().range;
}

bool CameraData::engine_applied_jitter() const noexcept
{
    return engine_applies_jitter.load(std::memory_order_relaxed);
}

ReportedJitter CameraData::reported_jitter() const noexcept
{

    if (!temporal_inputs_valid()) {
        return ReportedJitter{};
    }
    return measure_and_report_jitter();
}

ReportedJitter CameraData::frame_jitter() const noexcept
{
    const auto sequence = capture_sequence();
    const auto source = config::Settings::instance().jitter_source();
    if (frame_jitter_sequence != sequence || frame_jitter_source != source) {
        const auto measured = measure_and_report_jitter();
        const auto& dynamic_resolution = DynamicResolution::instance();
        frame_jitter_snapshot = select_reported_jitter(
            source == config::JitterSource::requested,
            dynamic_resolution.jitter_x(),
            dynamic_resolution.jitter_y(),
            measured);
        frame_jitter_sequence = sequence;
        frame_jitter_source = source;
    }
    return frame_jitter_snapshot;
}

bool CameraData::depth_reversed_for_display() const noexcept
{
    return depth_orientation() != DepthOrientation::standard;
}

bool CameraData::depth_infinite_for_display() const noexcept
{
    return depth_range() == DepthRange::infinite;
}

void CameraData::shutdown() noexcept
{
    capture_enabled.store(false, std::memory_order_relaxed);
    measured_depth_contract.store(
        encode_depth_contract(
            DepthOrientation::indeterminate, DepthRange::indeterminate),
        std::memory_order_relaxed);
    depth_orientation_logged.store(false, std::memory_order_relaxed);
    cached_frame_valid.store(false, std::memory_order_relaxed);
    cached_frame_sequence.store(0, std::memory_order_relaxed);
    frame_jitter_sequence = (std::numeric_limits<std::uint64_t>::max)();
    frame_jitter_source = config::JitterSource::requested;
    frame_jitter_snapshot = {};
    injection_active.store(false, std::memory_order_relaxed);
    injected_ndc_x.store(0.0F, std::memory_order_relaxed);
    injected_ndc_y.store(0.0F, std::memory_order_relaxed);
    jitter_contract_logged.store(false, std::memory_order_relaxed);
    forward_projection_logged.store(false, std::memory_order_relaxed);
    forward_projection_failure_logged.store(false, std::memory_order_relaxed);
    camera_reprojection_logged.store(false, std::memory_order_relaxed);
    camera_reprojection_failure_logged.store(false, std::memory_order_relaxed);
    previous_camera_unavailable_logged.store(false, std::memory_order_relaxed);
    engine_applies_jitter.store(false, std::memory_order_relaxed);
    injection_log_budget.reset();
    jitter_log_budget.reset();
    mapped_resource = nullptr;
    mapped_bytes = nullptr;
    mapped_writable = false;
}

bool CameraData::ready() const noexcept
{
    return installed_ &&
           cached_frame_valid.load(std::memory_order_acquire);
}

bool CameraData::camera_planes(
    float& near_plane,
    float& far_plane) const noexcept
{
    if (!temporal_inputs_valid()) {
        return false;
    }
    near_plane = *camera_near;
    far_plane = *camera_far;
    return true;
}

bool CameraData::camera_fov_vertical(float& radians) const noexcept
{
    if (!temporal_inputs_valid()) {
        return false;
    }
    const auto captured_inverse_projection =
        transpose(cached_frame.camera_projection_unjittered);
    Matrix projection{};
    if (!recover_forward_projection(
            captured_inverse_projection,
            projection)) {
        return false;
    }
    const auto focal = std::abs(projection._22);
    if (!std::isfinite(focal) || focal <= 0.0F) {
        return false;
    }
    const auto vertical = 2.0F * std::atan(1.0F / focal);
    if (!std::isfinite(vertical) || vertical <= 0.0F) {
        return false;
    }
    radians = vertical;
    return true;
}

bool CameraData::temporal_inputs_valid() const noexcept
{
    if (!ready() || camera_near == nullptr || camera_far == nullptr) {
        return false;
    }
    const auto near_plane = *camera_near;
    const auto far_plane = *camera_far;
    return std::isfinite(near_plane) &&
           std::isfinite(far_plane) &&
           near_plane > 0.0F &&
           far_plane > near_plane;
}

void CameraData::set_capture_frozen(const bool frozen) noexcept
{
    capture_frozen.store(frozen, std::memory_order_relaxed);
}

std::uint64_t CameraData::frozen_write_count() noexcept
{
    return frozen_writes.load(std::memory_order_relaxed);
}

std::uint64_t CameraData::capture_sequence() const noexcept
{
    return cached_frame_sequence.load(std::memory_order_acquire);
}
}
