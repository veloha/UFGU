#include "providers/ProviderTypes.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace mfgdlss::providers
{
namespace
{
[[nodiscard]] bool equals_ignoring_case(
    const std::string_view left,
    const std::string_view right) noexcept
{
    if (left.size() != right.size()) {
        return false;
    }
    return std::equal(
        left.begin(),
        left.end(),
        right.begin(),
        [](const char a, const char b) noexcept {
            const auto lower = [](const char c) noexcept {
                return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            };
            return lower(a) == lower(b);
        });
}
}

std::string_view vendor_key(const Vendor vendor) noexcept
{
    switch (vendor) {
    case Vendor::automatic:
        return "Auto";
    case Vendor::nvidia:
        return "NVIDIA";
    case Vendor::amd:
        return "AMD";
    case Vendor::intel:
        return "Intel";
    case Vendor::none:
        return "Off";
    }
    return "Off";
}

std::string_view vendor_display_name(const Vendor vendor) noexcept
{
    switch (vendor) {
    case Vendor::automatic:
        return "Auto";
    case Vendor::nvidia:
        return "NVIDIA";
    case Vendor::amd:
        return "AMD";
    case Vendor::intel:
        return "Intel";
    case Vendor::none:
        return "Off";
    }
    return "Off";
}

bool parse_vendor(const std::string_view text, Vendor& out) noexcept
{

    struct Entry
    {
        std::string_view text;
        Vendor vendor;
    };
    constexpr std::array<Entry, 12> entries{{
        {"Auto", Vendor::automatic},
        {"Automatic", Vendor::automatic},
        {"Default", Vendor::automatic},
        {"NVIDIA", Vendor::nvidia},
        {"Nvidia", Vendor::nvidia},
        {"DLSS", Vendor::nvidia},
        {"AMD", Vendor::amd},
        {"FSR", Vendor::amd},
        {"Intel", Vendor::intel},
        {"XeSS", Vendor::intel},
        {"Off", Vendor::none},
        {"None", Vendor::none},
    }};
    for (const auto& entry : entries) {
        if (equals_ignoring_case(text, entry.text)) {
            out = entry.vendor;
            return true;
        }
    }
    return false;
}

std::string_view role_name(const Role role) noexcept
{
    switch (role) {
    case Role::upscaling:
        return "upscaling";
    case Role::frame_generation:
        return "frame-generation";
    case Role::latency:
        return "latency";
    }
    return "unknown";
}

std::string_view availability_name(const Availability availability) noexcept
{
    switch (availability) {
    case Availability::unknown:
        return "unknown";
    case Availability::available:
        return "available";
    case Availability::disabled_by_configuration:
        return "disabled-by-configuration";
    case Availability::runtime_missing:
        return "runtime-missing";
    case Availability::runtime_version_mismatch:
        return "runtime-version-mismatch";
    case Availability::runtime_rejected_signature:
        return "runtime-signature-rejected";
    case Availability::adapter_unsupported:
        return "adapter-unsupported";
    case Availability::shader_model_unsupported:
        return "shader-model-unsupported";
    case Availability::operating_system_unsupported:
        return "operating-system-unsupported";
    case Availability::driver_too_old:
        return "driver-too-old";
    case Availability::d3d12_unavailable:
        return "d3d12-unavailable";
    case Availability::context_creation_failed:
        return "context-creation-failed";
    case Availability::required_input_unavailable:
        return "required-input-unavailable";
    case Availability::hudless_boundary_unavailable:
        return "hudless-boundary-unavailable";
    case Availability::swapchain_ownership_conflict:
        return "swapchain-ownership-conflict";
    case Availability::latency_provider_unavailable:
        return "latency-provider-unavailable";
    case Availability::unsupported_generated_frame_count:
        return "unsupported-generated-frame-count";
    case Availability::headers_absent_at_build_time:
        return "sdk-headers-absent-at-build-time";
    }
    return "unknown";
}

std::string_view availability_explanation(const Availability availability) noexcept
{
    switch (availability) {
    case Availability::unknown:
        return "Not yet queried";
    case Availability::available:
        return "Available";
    case Availability::disabled_by_configuration:
        return "Turned off in configuration";
    case Availability::runtime_missing:
        return "Vendor runtime library not installed";
    case Availability::runtime_version_mismatch:
        return "Vendor runtime is the wrong version";
    case Availability::runtime_rejected_signature:
        return "Vendor runtime failed signature verification";
    case Availability::adapter_unsupported:
        return "This graphics adapter is not supported";
    case Availability::shader_model_unsupported:
        return "Required shader model is unavailable";
    case Availability::operating_system_unsupported:
        return "Windows version is too old";
    case Availability::driver_too_old:
        return "Graphics driver is too old";
    case Availability::d3d12_unavailable:
        return "Direct3D 12 is unavailable";
    case Availability::context_creation_failed:
        return "Provider context could not be created";
    case Availability::required_input_unavailable:
        return "A required render input is unavailable";
    case Availability::hudless_boundary_unavailable:
        return "Interface-free image boundary not yet established";
    case Availability::swapchain_ownership_conflict:
        return "Another technology already owns presentation";
    case Availability::latency_provider_unavailable:
        return "Required latency technology is unavailable";
    case Availability::unsupported_generated_frame_count:
        return "Requested generated-frame count is not supported";
    case Availability::headers_absent_at_build_time:
        return "Not compiled in: vendor SDK headers were absent";
    }
    return "Not yet queried";
}

std::string_view quality_mode_key(const QualityMode mode) noexcept
{
    switch (mode) {
    case QualityMode::off:
        return "Off";
    case QualityMode::native_antialiasing:
        return "NativeAA";
    case QualityMode::ultra_quality_plus:
        return "UltraQualityPlus";
    case QualityMode::ultra_quality:
        return "UltraQuality";
    case QualityMode::quality:
        return "Quality";
    case QualityMode::balanced:
        return "Balanced";
    case QualityMode::performance:
        return "Performance";
    case QualityMode::ultra_performance:
        return "UltraPerformance";
    }
    return "Off";
}

std::string_view quality_mode_display_name(const QualityMode mode) noexcept
{
    switch (mode) {
    case QualityMode::off:
        return "Off";
    case QualityMode::native_antialiasing:
        return "Native AA";
    case QualityMode::ultra_quality_plus:
        return "Ultra Quality+";
    case QualityMode::ultra_quality:
        return "Ultra Quality";
    case QualityMode::quality:
        return "Quality";
    case QualityMode::balanced:
        return "Balanced";
    case QualityMode::performance:
        return "Performance";
    case QualityMode::ultra_performance:
        return "Ultra Performance";
    }
    return "Off";
}
}
