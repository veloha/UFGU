#include "providers/LowLatency.hpp"

#include <cstdio>
#include <cstring>

namespace
{
int failures{};

void check(const bool condition, const char* const description)
{
    std::printf("  %s  %s\n", condition ? "PASS" : "FAIL", description);
    failures += condition ? 0 : 1;
}

using mfgdlss::providers::LatencyBackend;
using mfgdlss::providers::LatencyEnvironment;
using mfgdlss::providers::LatencyMode;
using mfgdlss::providers::LatencyUnavailableReason;

[[nodiscard]] LatencyEnvironment nvidia_machine()
{
    LatencyEnvironment e;
    e.nvidia_gpu = true;
    e.reflex_runtime = true;
    return e;
}

[[nodiscard]] LatencyEnvironment amd_machine()
{
    LatencyEnvironment e;
    e.amd_gpu = true;
    e.antilag2_runtime = true;
    return e;
}

[[nodiscard]] LatencyEnvironment intel_machine()
{
    LatencyEnvironment e;
    e.intel_gpu = true;
    e.xell_runtime = true;
    return e;
}
}

int main()
{
    using mfgdlss::providers::backend_display_name;
    using mfgdlss::providers::clamp_mode;
    using mfgdlss::providers::mode_display_name;
    using mfgdlss::providers::mode_supported;
    using mfgdlss::providers::select_latency_backend;
    using mfgdlss::providers::supports_boost;
    using mfgdlss::providers::unavailable_reason_text;

    std::printf("backend selection follows the GPU, not the upscaler\n");
    {
        const auto nvidia = select_latency_backend(nvidia_machine());
        check(
            nvidia.backend == LatencyBackend::nvidia_reflex &&
                nvidia.reason == LatencyUnavailableReason::available,
            "an NVIDIA card with the Reflex runtime selects Reflex");

        auto nvidia_running_fsr = nvidia_machine();
        nvidia_running_fsr.amd_gpu = false;
        check(
            select_latency_backend(nvidia_running_fsr).backend ==
                LatencyBackend::nvidia_reflex,
            "Reflex is preserved on NVIDIA regardless of the upscaler");

        check(
            select_latency_backend(amd_machine()).backend ==
                LatencyBackend::amd_antilag2,
            "a Radeon card with the Anti-Lag 2 runtime selects Anti-Lag 2");
    }

    std::printf("honest unavailability\n");
    {
        auto nvidia_no_runtime = nvidia_machine();
        nvidia_no_runtime.reflex_runtime = false;
        const auto result = select_latency_backend(nvidia_no_runtime);
        check(
            result.backend == LatencyBackend::none &&
                result.reason == LatencyUnavailableReason::runtime_absent,
            "a missing runtime is reported as a missing runtime");

        auto amd_no_runtime = amd_machine();
        amd_no_runtime.antilag2_runtime = false;
        check(
            select_latency_backend(amd_no_runtime).reason ==
                LatencyUnavailableReason::runtime_absent,
            "a Radeon card without the driver extension says so");

        const auto intel = select_latency_backend(intel_machine());
        check(
            intel.backend == LatencyBackend::none &&
                intel.reason ==
                    LatencyUnavailableReason::incompatible_presentation_api,
            "Intel XeLL reports the D3D11 presentation blocker specifically");
        check(
            std::strstr(
                unavailable_reason_text(
                    LatencyUnavailableReason::incompatible_presentation_api),
                "D3D12") != nullptr,
            "the blocker text names the API requirement");

        LatencyEnvironment nothing;
        check(
            select_latency_backend(nothing).reason ==
                LatencyUnavailableReason::no_supported_gpu,
            "an unrecognised adapter reports no supported GPU");
    }

    std::printf("XeSS frame generation forbids a second latency backend\n");
    {
        auto intel_fg = intel_machine();
        intel_fg.xess_frame_generation_active = true;
        intel_fg.d3d12_presentation = true;
        check(
            select_latency_backend(intel_fg).backend ==
                LatencyBackend::intel_xell,
            "XeSS FG on a D3D12 presentation path selects XeLL");

        auto intel_fg_d3d11 = intel_machine();
        intel_fg_d3d11.xess_frame_generation_active = true;
        check(
            select_latency_backend(intel_fg_d3d11).reason ==
                LatencyUnavailableReason::incompatible_presentation_api,
            "XeSS FG under D3D11 presentation fails closed with the reason");

        auto nvidia_with_xess_fg = nvidia_machine();
        nvidia_with_xess_fg.xess_frame_generation_active = true;
        check(
            select_latency_backend(nvidia_with_xess_fg).backend ==
                LatencyBackend::none,
            "Reflex is not substituted when XeSS FG demands XeLL");
    }

    std::printf("no backend is given semantics it does not have\n");
    {
        check(
            supports_boost(LatencyBackend::nvidia_reflex),
            "Reflex has Boost");
        check(
            !supports_boost(LatencyBackend::amd_antilag2) &&
                !supports_boost(LatencyBackend::intel_xell),
            "Anti-Lag 2 and XeLL do not have Boost");
        check(
            !mode_supported(LatencyBackend::amd_antilag2, LatencyMode::boost),
            "Boost is not offered for Anti-Lag 2");
        check(
            mode_supported(LatencyBackend::amd_antilag2, LatencyMode::on) &&
                mode_supported(LatencyBackend::amd_antilag2, LatencyMode::off),
            "Anti-Lag 2 offers exactly Off and On");
        check(
            clamp_mode(LatencyBackend::amd_antilag2, LatencyMode::boost) ==
                LatencyMode::on,
            "a Boost request on Anti-Lag 2 degrades to On");
        check(
            std::strcmp(
                mode_display_name(
                    LatencyBackend::amd_antilag2, LatencyMode::boost),
                "Boost") != 0,
            "Anti-Lag 2 never displays the word Boost");
        check(
            std::strcmp(
                mode_display_name(
                    LatencyBackend::nvidia_reflex, LatencyMode::boost),
                "Boost") == 0,
            "Reflex does display Boost");
        check(
            clamp_mode(LatencyBackend::none, LatencyMode::boost) ==
                    LatencyMode::off &&
                !mode_supported(LatencyBackend::none, LatencyMode::on),
            "with no backend the only supported mode is Off");
    }

    std::printf("display names\n");
    check(
        std::strcmp(
            backend_display_name(LatencyBackend::amd_antilag2),
            "AMD Radeon Anti-Lag 2") == 0 &&
            std::strcmp(
                backend_display_name(LatencyBackend::nvidia_reflex),
                "NVIDIA Reflex") == 0 &&
            std::strcmp(
                backend_display_name(LatencyBackend::intel_xell),
                "Intel XeLL") == 0,
        "each backend reports its real product name");
    check(
        unavailable_reason_text(LatencyUnavailableReason::available)[0] == '\0',
        "an available backend has no reason text");

    {
        LatencyEnvironment nvidia_with_fsr{};
        nvidia_with_fsr.nvidia_gpu = true;
        nvidia_with_fsr.reflex_runtime = true;
        nvidia_with_fsr.d3d12_presentation = true;
        nvidia_with_fsr.fsr_frame_generation_active = true;
        const auto selected = select_latency_backend(nvidia_with_fsr);
        check(
            selected.backend == LatencyBackend::none,
            "AMD frame generation on an NVIDIA card rules Reflex out, because "
            "the vendor generator owns the swap chain and Streamline is not "
            "proxying presentation");
        check(
            selected.reason ==
                LatencyUnavailableReason::
                    presentation_owned_by_frame_generator,
            "and the reason names the frame generator owning presentation "
            "rather than the GPU");
        check(
            std::strstr(
                unavailable_reason_text(
                    LatencyUnavailableReason::
                        presentation_owned_by_frame_generator),
                "D3D11") == nullptr &&
                std::strstr(
                    unavailable_reason_text(
                        LatencyUnavailableReason::
                            presentation_owned_by_frame_generator),
                    "D3D12") == nullptr,
            "and it does not blame the presentation API, because this machine "
            "presents through D3D12 and Reflex has no D3D12 requirement");

        auto nvidia_without_fsr = nvidia_with_fsr;
        nvidia_without_fsr.fsr_frame_generation_active = false;
        check(
            select_latency_backend(nvidia_without_fsr).backend ==
                LatencyBackend::nvidia_reflex,
            "the same card keeps Reflex when no vendor generator is active");
    }

    {
        LatencyEnvironment amd_with_fsr{};
        amd_with_fsr.amd_gpu = true;
        amd_with_fsr.antilag2_runtime = true;
        amd_with_fsr.d3d12_presentation = true;
        amd_with_fsr.fsr_frame_generation_active = true;
        check(
            select_latency_backend(amd_with_fsr).backend ==
                LatencyBackend::amd_antilag2,
            "AMD frame generation does not disturb Anti-Lag 2 on an AMD card");
    }

    {
        LatencyEnvironment nvidia_with_xess{};
        nvidia_with_xess.nvidia_gpu = true;
        nvidia_with_xess.reflex_runtime = true;
        nvidia_with_xess.d3d12_presentation = true;
        nvidia_with_xess.xess_frame_generation_active = true;
        check(
            select_latency_backend(nvidia_with_xess).backend ==
                LatencyBackend::none,
            "Intel frame generation on an NVIDIA card was already ruled out");
    }

    std::printf("LowLatencyTest: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
