#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace mfgdlss::providers
{

enum class Vendor : std::uint32_t
{
    automatic,
    nvidia,
    amd,
    intel,
    none
};

enum class Role : std::uint32_t
{
    upscaling,
    frame_generation,
    latency
};

enum class Availability : std::uint32_t
{
    unknown,
    available,
    disabled_by_configuration,
    runtime_missing,
    runtime_version_mismatch,
    runtime_rejected_signature,
    adapter_unsupported,
    shader_model_unsupported,
    operating_system_unsupported,
    driver_too_old,
    d3d12_unavailable,
    context_creation_failed,
    required_input_unavailable,
    hudless_boundary_unavailable,
    swapchain_ownership_conflict,
    latency_provider_unavailable,
    unsupported_generated_frame_count,
    headers_absent_at_build_time
};

enum class QualityMode : std::uint32_t
{
    off,
    native_antialiasing,
    ultra_quality_plus,
    ultra_quality,
    quality,
    balanced,
    performance,
    ultra_performance
};

inline constexpr std::uint32_t kQualityModeCount = 8U;

[[nodiscard]] constexpr std::uint32_t quality_bit(const QualityMode mode) noexcept
{
    return 1U << static_cast<std::uint32_t>(mode);
}

[[nodiscard]] std::string_view vendor_key(Vendor vendor) noexcept;
[[nodiscard]] std::string_view vendor_display_name(Vendor vendor) noexcept;
[[nodiscard]] bool parse_vendor(std::string_view text, Vendor& out) noexcept;

[[nodiscard]] std::string_view role_name(Role role) noexcept;
[[nodiscard]] std::string_view availability_name(Availability availability) noexcept;

[[nodiscard]] std::string_view availability_explanation(
    Availability availability) noexcept;

[[nodiscard]] std::string_view quality_mode_key(QualityMode mode) noexcept;
[[nodiscard]] std::string_view quality_mode_display_name(QualityMode mode) noexcept;

struct UpscalingCapability
{
    Availability availability{Availability::unknown};
    std::string version;
    std::string detail;
    std::uint32_t quality_mask{};
    bool supports_native_antialiasing{};
    bool requires_d3d12{};
};

struct FrameGenerationCapability
{
    Availability availability{Availability::unknown};
    std::string version;
    std::string detail;

    std::uint32_t maximum_generated_frames{};
    bool requires_d3d12{};
    bool owns_presentation{};
    bool requires_latency_provider{};
    bool requires_swapchain_recreation{};
};

struct LatencyCapability
{
    Availability availability{Availability::unknown};
    std::string version;
    std::string detail;
    bool supports_d3d11{};
    bool supports_d3d12{};
};

struct RoleStatus
{
    Vendor requested{Vendor::automatic};
    Vendor active{Vendor::none};
    Availability availability{Availability::unknown};
    std::string version;
    std::string detail;
    bool restart_required{};
};
}
