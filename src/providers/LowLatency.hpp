#pragma once

#include <cstdint>

namespace mfgdlss::providers
{

enum class LatencyBackend : std::uint32_t
{
    none,
    nvidia_reflex,
    amd_antilag2,
    intel_xell,
};

enum class LatencyMode : std::uint32_t
{
    off,
    on,
    boost,
};

enum class LatencyUnavailableReason : std::uint32_t
{
    available,
    no_supported_gpu,
    runtime_absent,
    incompatible_presentation_api,
    presentation_owned_by_frame_generator,
    conflicting_latency_backend,
    restart_required,
};

[[nodiscard]] constexpr const char* backend_display_name(
    const LatencyBackend backend) noexcept
{
    switch (backend) {
    case LatencyBackend::none:
        return "None";
    case LatencyBackend::nvidia_reflex:
        return "NVIDIA Reflex";
    case LatencyBackend::amd_antilag2:
        return "AMD Radeon Anti-Lag 2";
    case LatencyBackend::intel_xell:
        return "Intel XeLL";
    }
    return "Unknown";
}

[[nodiscard]] constexpr const char* unavailable_reason_text(
    const LatencyUnavailableReason reason) noexcept
{
    switch (reason) {
    case LatencyUnavailableReason::available:
        return "";
    case LatencyUnavailableReason::no_supported_gpu:
        return "no supported GPU for this backend";
    case LatencyUnavailableReason::runtime_absent:
        return "the vendor runtime is not present";
    case LatencyUnavailableReason::incompatible_presentation_api:
        return "the vendor runtime requires a D3D12 presentation path; this "
               "game presents through D3D11";
    case LatencyUnavailableReason::presentation_owned_by_frame_generator:
        return "the selected frame generator owns presentation, and this "
               "latency runtime cannot be inserted into it";
    case LatencyUnavailableReason::conflicting_latency_backend:
        return "another latency backend is active and the vendor marks "
               "concurrent use unsupported";
    case LatencyUnavailableReason::restart_required:
        return "a Skyrim restart is required to change the latency backend";
    }
    return "unknown";
}

[[nodiscard]] constexpr bool supports_boost(
    const LatencyBackend backend) noexcept
{
    return backend == LatencyBackend::nvidia_reflex;
}

[[nodiscard]] constexpr bool mode_supported(
    const LatencyBackend backend, const LatencyMode mode) noexcept
{
    if (backend == LatencyBackend::none) {
        return mode == LatencyMode::off;
    }
    if (mode == LatencyMode::boost) {
        return supports_boost(backend);
    }
    return true;
}

[[nodiscard]] constexpr LatencyMode clamp_mode(
    const LatencyBackend backend, const LatencyMode mode) noexcept
{
    if (backend == LatencyBackend::none) {
        return LatencyMode::off;
    }
    if (mode == LatencyMode::boost && !supports_boost(backend)) {
        return LatencyMode::on;
    }
    return mode;
}

[[nodiscard]] constexpr const char* mode_display_name(
    const LatencyBackend backend, const LatencyMode mode) noexcept
{
    switch (clamp_mode(backend, mode)) {
    case LatencyMode::off:
        return "Off";
    case LatencyMode::on:
        return "On";
    case LatencyMode::boost:
        return "Boost";
    }
    return "Off";
}

struct LatencyEnvironment final
{
    bool nvidia_gpu{};
    bool amd_gpu{};
    bool intel_gpu{};

    bool reflex_runtime{};
    bool antilag2_runtime{};
    bool xell_runtime{};

    bool d3d12_presentation{};

    bool xess_frame_generation_active{};
    bool fsr_frame_generation_active{};
};

struct LatencySelection final
{
    LatencyBackend backend{LatencyBackend::none};
    LatencyUnavailableReason reason{
        LatencyUnavailableReason::no_supported_gpu};
};

[[nodiscard]] constexpr LatencySelection select_latency_backend(
    const LatencyEnvironment& environment) noexcept
{

    if (environment.xess_frame_generation_active) {
        if (!environment.intel_gpu) {
            return {LatencyBackend::none,
                    LatencyUnavailableReason::no_supported_gpu};
        }
        if (!environment.xell_runtime) {
            return {LatencyBackend::none,
                    LatencyUnavailableReason::runtime_absent};
        }
        if (!environment.d3d12_presentation) {
            return {LatencyBackend::none,
                    LatencyUnavailableReason::incompatible_presentation_api};
        }
        return {LatencyBackend::intel_xell,
                LatencyUnavailableReason::available};
    }

    if (environment.nvidia_gpu && environment.reflex_runtime) {
        if (environment.fsr_frame_generation_active) {
            return {
                LatencyBackend::none,
                LatencyUnavailableReason::
                    presentation_owned_by_frame_generator};
        }
        return {LatencyBackend::nvidia_reflex,
                LatencyUnavailableReason::available};
    }
    if (environment.amd_gpu && environment.antilag2_runtime) {
        return {LatencyBackend::amd_antilag2,
                LatencyUnavailableReason::available};
    }
    if (environment.intel_gpu) {
        if (!environment.xell_runtime) {
            return {LatencyBackend::none,
                    LatencyUnavailableReason::runtime_absent};
        }

        return {LatencyBackend::none,
                LatencyUnavailableReason::incompatible_presentation_api};
    }
    if (environment.nvidia_gpu || environment.amd_gpu) {
        return {LatencyBackend::none,
                LatencyUnavailableReason::runtime_absent};
    }
    return {LatencyBackend::none, LatencyUnavailableReason::no_supported_gpu};
}
}
