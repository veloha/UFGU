#include "config/Settings.hpp"
#include "providers/RuntimeLoader.hpp"

#include <Windows.h>

#include <SKSE/SKSE.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <format>
#include <string>

namespace mfgdlss::config
{
namespace
{
namespace logger = SKSE::log;

constexpr std::array kFixedMultipliers{2U, 3U, 4U, 6U};
constexpr std::uint32_t kMinimumFrameLimit = 20U;
constexpr std::uint32_t kMaximumFrameLimit = 1000U;
constexpr std::wstring_view kConfigurationFilename{L"UFGU.ini"};

struct ProfileCommitVerification
{
    bool file_flushed{};
    bool file_read{};
    bool provider_matches{};
    bool mode_matches{};
    DWORD error{};

    [[nodiscard]] bool durable() const noexcept
    {
        return file_flushed && file_read && provider_matches && mode_matches;
    }
};

[[nodiscard]] std::string_view trim_ascii(std::string_view value) noexcept
{
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] bool ascii_iequals(
    const std::string_view left,
    const std::string_view right) noexcept
{
    if (left.size() != right.size()) {
        return false;
    }
    return std::ranges::equal(left, right, [](const char a, const char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
    });
}

[[nodiscard]] bool ascii_iequals(
    const std::string_view observed,
    const std::wstring_view expected) noexcept
{
    if (observed.size() != expected.size()) {
        return false;
    }
    for (std::size_t index = 0; index < observed.size(); ++index) {
        const auto expected_character = expected[index];
        if (expected_character > 0x7FU ||
            std::tolower(
                static_cast<unsigned char>(observed[index])) !=
                std::tolower(static_cast<unsigned char>(expected_character))) {
            return false;
        }
    }
    return true;
}

struct PhysicalProfileValues
{
    std::string provider;
    std::string mode;
};

[[nodiscard]] bool read_physical_profile_values(
    const HANDLE file,
    PhysicalProfileValues& values,
    DWORD& error) noexcept
{
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) == FALSE || size.QuadPart < 0 ||
        size.QuadPart > 1024 * 1024) {
        error = GetLastError();
        if (error == ERROR_SUCCESS) {
            error = ERROR_FILE_TOO_LARGE;
        }
        return false;
    }

    LARGE_INTEGER start{};
    if (SetFilePointerEx(file, start, nullptr, FILE_BEGIN) == FALSE) {
        error = GetLastError();
        return false;
    }

    std::string contents(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD bytes_read{};
    if (!contents.empty() &&
        (ReadFile(
             file,
             contents.data(),
             static_cast<DWORD>(contents.size()),
             &bytes_read,
             nullptr) == FALSE ||
         bytes_read != contents.size())) {
        error = GetLastError();
        if (error == ERROR_SUCCESS) {
            error = ERROR_READ_FAULT;
        }
        return false;
    }
    contents.resize(bytes_read);

    bool in_upscaling_section{};
    std::size_t position{};
    while (position <= contents.size()) {
        const auto end = contents.find('\n', position);
        auto line = trim_ascii(std::string_view{contents}.substr(
            position,
            end == std::string::npos ? std::string::npos : end - position));

        if (!line.empty() && line.front() == '[' && line.back() == ']') {
            in_upscaling_section = ascii_iequals(
                trim_ascii(line.substr(1, line.size() - 2)), "Upscaling");
        } else if (in_upscaling_section && !line.empty() &&
                   line.front() != ';' && line.front() != '#') {
            const auto separator = line.find('=');
            if (separator != std::string_view::npos) {
                const auto key = trim_ascii(line.substr(0, separator));
                const auto value = trim_ascii(line.substr(separator + 1));
                if (ascii_iequals(key, "Provider")) {
                    values.provider.assign(value);
                } else if (ascii_iequals(key, "Mode")) {
                    values.mode.assign(value);
                }
            }
        }

        if (end == std::string::npos) {
            break;
        }
        position = end + 1;
    }

    if (values.provider.empty() || values.mode.empty()) {
        error = ERROR_NOT_FOUND;
        return false;
    }
    return true;
}

[[nodiscard]] ProfileCommitVerification verify_profile_commit(
    const std::filesystem::path& path,
    const std::wstring_view provider,
    const std::wstring_view mode) noexcept
{
    ProfileCommitVerification result{};

    const auto file = CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        result.error = GetLastError();
        return result;
    }

    result.file_flushed = FlushFileBuffers(file) != FALSE;
    if (!result.file_flushed) {
        result.error = GetLastError();
    }
    if (!result.file_flushed) {
        CloseHandle(file);
        return result;
    }

    PhysicalProfileValues observed{};
    result.file_read =
        read_physical_profile_values(file, observed, result.error);
    CloseHandle(file);
    if (!result.file_read) {
        return result;
    }
    result.provider_matches = ascii_iequals(observed.provider, provider);
    result.mode_matches = ascii_iequals(observed.mode, mode);
    return result;
}

[[nodiscard]] std::filesystem::path configuration_path()
{

    static std::uint8_t module_anchor{};
    HMODULE module{};
    constexpr auto flags =
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    if (GetModuleHandleExW(
            flags,
            reinterpret_cast<LPCWSTR>(&module_anchor),
            &module) == FALSE ||
        module == nullptr) {
        return {};
    }

    std::wstring module_path(32768, L'\0');
    const auto length = GetModuleFileNameW(
        module,
        module_path.data(),
        static_cast<DWORD>(module_path.size()));
    if (length == 0 || length >= module_path.size()) {
        return {};
    }

    const auto visible_directory =
        std::filesystem::path{std::wstring{module_path.data(), length}}
            .parent_path();

    const auto physical_directory =
        providers::RuntimeLoader::physical_plugin_directory();
    return (physical_directory.empty() ? visible_directory :
                                         physical_directory) /
           kConfigurationFilename;
}

[[nodiscard]] constexpr bool valid_frame_limit(
    const std::uint32_t frame_limit) noexcept
{
    return frame_limit == 0U ||
           (frame_limit >= kMinimumFrameLimit &&
            frame_limit <= kMaximumFrameLimit);
}

struct UpscalingModeName
{
    std::wstring_view name;
    UpscalingMode mode;
};

struct ReflexModeName
{
    std::wstring_view name;
    ReflexMode mode;
};

struct DlssPresetName
{
    std::wstring_view name;
    DlssPreset preset;
};

struct DlssTemporalInputsName
{
    std::wstring_view name;
    DlssTemporalInputs inputs;
};

struct TemporalJitterName
{
    std::wstring_view name;
    TemporalJitter jitter;
};

struct SurfaceModelName
{
    std::wstring_view name;
    SurfaceModel model;
};

struct VirtualKeyName
{
    std::wstring_view name;
    std::uint32_t key;
};

constexpr std::array kUpscalingModes{
    UpscalingModeName{L"Off", UpscalingMode::off},
    UpscalingModeName{L"DLAA", UpscalingMode::dlaa},
    UpscalingModeName{L"Quality", UpscalingMode::quality},
    UpscalingModeName{L"Balanced", UpscalingMode::balanced},
    UpscalingModeName{L"Performance", UpscalingMode::performance},
    UpscalingModeName{
        L"UltraPerformance",
        UpscalingMode::ultra_performance}};

constexpr std::array kReflexModes{
    ReflexModeName{L"Off", ReflexMode::off},
    ReflexModeName{L"On", ReflexMode::low_latency},
    ReflexModeName{L"Boost", ReflexMode::low_latency_boost}};

constexpr std::array kDlssPresets{
    DlssPresetName{L"Recommended", DlssPreset::recommended},
    DlssPresetName{L"E", DlssPreset::legacy_e},
    DlssPresetName{L"J", DlssPreset::j},
    DlssPresetName{L"K", DlssPreset::k},
    DlssPresetName{L"L", DlssPreset::l},
    DlssPresetName{L"M", DlssPreset::m}};

constexpr std::array kDlssTemporalInputs{
    DlssTemporalInputsName{
        L"Standard",
        DlssTemporalInputs::standard},
    DlssTemporalInputsName{
        L"NoHints",
        DlssTemporalInputs::no_hints},
    DlssTemporalInputsName{
        L"RawMotion",
        DlssTemporalInputs::raw_motion},
    DlssTemporalInputsName{
        L"RawMotionNoHints",
        DlssTemporalInputs::raw_motion_no_hints}};

constexpr std::array kTemporalJitterModes{
    TemporalJitterName{L"Halton", TemporalJitter::halton},
    TemporalJitterName{L"Off", TemporalJitter::disabled}};

constexpr std::array kSurfaceModels{
    SurfaceModelName{L"CompleteFrame", SurfaceModel::complete_frame},
    SurfaceModelName{L"SubRect", SurfaceModel::sub_rect}};

struct JitterFoldName
{
    std::wstring_view name;
    JitterFold fold;
};

constexpr std::array kJitterFolds{
    JitterFoldName{L"Engine", JitterFold::engine},
    JitterFoldName{L"NopGate", JitterFold::nop_gate}};

constexpr std::array kVirtualKeys{
    VirtualKeyName{L"PageDown", VK_NEXT},
    VirtualKeyName{L"PageUp", VK_PRIOR},
    VirtualKeyName{L"Home", VK_HOME},
    VirtualKeyName{L"End", VK_END},
    VirtualKeyName{L"Insert", VK_INSERT},
    VirtualKeyName{L"Delete", VK_DELETE},
    VirtualKeyName{L"F1", VK_F1},
    VirtualKeyName{L"F2", VK_F2},
    VirtualKeyName{L"F3", VK_F3},
    VirtualKeyName{L"F4", VK_F4},
    VirtualKeyName{L"F5", VK_F5},
    VirtualKeyName{L"F6", VK_F6},
    VirtualKeyName{L"F7", VK_F7},
    VirtualKeyName{L"F8", VK_F8},
    VirtualKeyName{L"F9", VK_F9},
    VirtualKeyName{L"F10", VK_F10},
    VirtualKeyName{L"F11", VK_F11},
    VirtualKeyName{L"F12", VK_F12},
    VirtualKeyName{L"Grave", VK_OEM_3},
    VirtualKeyName{L"Backslash", VK_OEM_5},
    VirtualKeyName{L"Minus", VK_OEM_MINUS},
    VirtualKeyName{L"Equals", VK_OEM_PLUS}};

[[nodiscard]] std::wstring lowercase(std::wstring value)
{
    std::ranges::transform(
        value,
        value.begin(),
        [](const wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
    return value;
}

[[nodiscard]] UpscalingMode read_upscaling_mode(
    const std::filesystem::path& path)
{
    std::array<wchar_t, 64> value{};
    GetPrivateProfileStringW(
        L"Upscaling",
        L"Mode",
        L"Off",
        value.data(),
        static_cast<DWORD>(value.size()),
        path.c_str());
    const auto configured = lowercase(value.data());
    for (const auto& candidate : kUpscalingModes) {
        if (lowercase(std::wstring{candidate.name}) == configured) {
            return candidate.mode;
        }
    }
    logger::warn(
        "Invalid DLSS SR mode in {}; using Off",
        path.string());
    return UpscalingMode::off;
}

[[nodiscard]] providers::Vendor read_upscaling_provider(
    const std::filesystem::path& path)
{
    std::array<wchar_t, 32> value{};
    GetPrivateProfileStringW(
        L"Upscaling",
        L"Provider",
        L"NVIDIA",
        value.data(),
        static_cast<DWORD>(value.size()),
        path.c_str());

    const auto configured = lowercase(value.data());
    if (configured == L"nvidia" || configured == L"dlss" ||
        configured == L"automatic" || configured == L"auto") {
        return providers::Vendor::nvidia;
    }
    if (configured == L"intel" || configured == L"xess") {
        return providers::Vendor::intel;
    }
    if (configured == L"amd" || configured == L"fsr" ||
        configured == L"fidelityfx") {
        return providers::Vendor::amd;
    }

    logger::warn(
        "Invalid or unavailable upscaling provider in {}; using NVIDIA",
        path.string());
    return providers::Vendor::nvidia;
}

[[nodiscard]] ReflexMode read_reflex_mode(
    const std::filesystem::path& path)
{
    std::array<wchar_t, 32> value{};
    GetPrivateProfileStringW(
        L"Reflex",
        L"Mode",
        L"Off",
        value.data(),
        static_cast<DWORD>(value.size()),
        path.c_str());
    const auto configured = lowercase(value.data());
    for (const auto& candidate : kReflexModes) {
        if (lowercase(std::wstring{candidate.name}) == configured) {
            return candidate.mode;
        }
    }
    logger::warn(
        "Invalid NVIDIA Reflex mode in {}; using Off",
        path.string());
    return ReflexMode::off;
}

[[nodiscard]] DlssPreset read_dlss_preset(
    const std::filesystem::path& path)
{
    std::array<wchar_t, 32> value{};
    GetPrivateProfileStringW(
        L"Upscaling",
        L"Preset",
        L"Recommended",
        value.data(),
        static_cast<DWORD>(value.size()),
        path.c_str());
    const auto configured = lowercase(value.data());
    for (const auto& candidate : kDlssPresets) {
        if (lowercase(std::wstring{candidate.name}) == configured) {
            return candidate.preset;
        }
    }
    logger::warn(
        "Invalid DLSS model preset in {}; using Recommended",
        path.string());
    return DlssPreset::recommended;
}

[[nodiscard]] DlssTemporalInputs read_dlss_temporal_inputs(
    const std::filesystem::path& path)
{
    std::array<wchar_t, 32> value{};
    GetPrivateProfileStringW(
        L"Upscaling",
        L"TemporalInputs",
        L"Standard",
        value.data(),
        static_cast<DWORD>(value.size()),
        path.c_str());
    const auto configured = lowercase(value.data());
    for (const auto& candidate : kDlssTemporalInputs) {
        if (lowercase(std::wstring{candidate.name}) == configured) {
            return candidate.inputs;
        }
    }
    logger::warn(
        "Invalid DLSS temporal-input mode in {}; using Standard",
        path.string());
    return DlssTemporalInputs::standard;
}

[[nodiscard]] TemporalJitter read_temporal_jitter(
    const std::filesystem::path& path)
{
    std::array<wchar_t, 32> value{};
    GetPrivateProfileStringW(
        L"Upscaling",
        L"Jitter",
        L"Halton",
        value.data(),
        static_cast<DWORD>(value.size()),
        path.c_str());
    const auto configured = lowercase(value.data());
    for (const auto& candidate : kTemporalJitterModes) {
        if (lowercase(std::wstring{candidate.name}) == configured) {
            return candidate.jitter;
        }
    }
    logger::warn(
        "Invalid DLSS temporal-jitter mode in {}; using Halton",
        path.string());
    return TemporalJitter::halton;
}

[[nodiscard]] SurfaceModel read_surface_model(
    const std::filesystem::path& path)
{
    std::array<wchar_t, 32> value{};
    GetPrivateProfileStringW(
        L"Upscaling",
        L"SurfaceModel",
        L"CompleteFrame",
        value.data(),
        static_cast<DWORD>(value.size()),
        path.c_str());
    const auto configured = lowercase(value.data());
    for (const auto& candidate : kSurfaceModels) {
        if (lowercase(std::wstring{candidate.name}) == configured) {
            return candidate.model;
        }
    }
    logger::warn(
        "Invalid surface model in {}; using CompleteFrame",
        path.string());
    return SurfaceModel::complete_frame;
}

[[nodiscard]] JitterFold read_jitter_fold(
    const std::filesystem::path& path)
{
    std::array<wchar_t, 32> value{};
    GetPrivateProfileStringW(
        L"Upscaling",
        L"JitterFold",
        L"NopGate",
        value.data(),
        static_cast<DWORD>(value.size()),
        path.c_str());
    const auto configured = lowercase(value.data());
    for (const auto& candidate : kJitterFolds) {
        if (lowercase(std::wstring{candidate.name}) == configured) {
            return candidate.fold;
        }
    }
    logger::warn(
        "Invalid jitter-fold mode in {}; using NopGate",
        path.string());
    return JitterFold::nop_gate;
}

[[nodiscard]] MipBiasMode read_mip_bias_mode(
    const std::filesystem::path& path)
{
    std::array<wchar_t, 32> value{};
    GetPrivateProfileStringW(
        L"Upscaling",
        L"MipBias",
        L"Off",
        value.data(),
        static_cast<DWORD>(value.size()),
        path.c_str());
    const auto configured = lowercase(value.data());
    if (configured == L"auto" || configured == L"automatic") {
        return MipBiasMode::automatic;
    }
    if (configured != L"off") {
        logger::warn(
            "Invalid MipBias mode in {}; using Off",
            path.string());
    }
    return MipBiasMode::off;
}

[[nodiscard]] VendorFrameGeneration read_vendor_frame_generation(
    const std::filesystem::path& path)
{
    std::array<wchar_t, 32> value{};
    GetPrivateProfileStringW(
        L"FrameGeneration",
        L"VendorFrameGeneration",
        L"Off",
        value.data(),
        static_cast<DWORD>(value.size()),
        path.c_str());
    const auto configured = lowercase(value.data());
    if (configured == L"intel" || configured == L"xess" ||
        configured == L"xefg") {
        return VendorFrameGeneration::intel;
    }
    if (configured == L"amd" || configured == L"fsr" ||
        configured == L"fsrfg" || configured == L"fidelityfx") {
        return VendorFrameGeneration::amd;
    }
    if (configured != L"off" && configured != L"none" &&
        configured != L"dlssg" && configured != L"nvidia") {
        logger::warn(
            "Invalid vendor frame generator in {}; using Off (Streamline "
            "DLSS-G)",
            path.string());
    }
    return VendorFrameGeneration::off;
}

[[nodiscard]] MultiplierMode read_multiplier_mode(
    const std::filesystem::path& path)
{
    std::array<wchar_t, 32> value{};
    GetPrivateProfileStringW(
        L"FrameGeneration",
        L"MultiplierMode",
        L"Fixed",
        value.data(),
        static_cast<DWORD>(value.size()),
        path.c_str());
    const auto configured = lowercase(value.data());
    if (configured == L"dynamic") {
        return MultiplierMode::dynamic;
    }
    if (configured != L"fixed" && configured != L"static") {
        logger::warn(
            "Invalid multiplier mode in {}; using Fixed",
            path.string());
    }
    return MultiplierMode::fixed;
}

[[nodiscard]] JitterSource read_jitter_source(
    const std::filesystem::path& path)
{
    std::array<wchar_t, 32> value{};
    GetPrivateProfileStringW(
        L"Upscaling",
        L"JitterSource",
        L"Requested",
        value.data(),
        static_cast<DWORD>(value.size()),
        path.c_str());
    const auto configured = lowercase(value.data());
    if (configured == L"measured") {
        return JitterSource::measured;
    }
    if (configured == L"requested") {
        return JitterSource::requested;
    }
    logger::warn(
        "Invalid JitterSource in {}; using Requested",
        path.string());
    return JitterSource::requested;
}

[[nodiscard]] float read_menu_sound_volume(
    const std::filesystem::path& path)
{
    std::array<wchar_t, 32> value{};
    GetPrivateProfileStringW(
        L"Overlay",
        L"MenuSoundVolume",
        L"0.60",
        value.data(),
        static_cast<DWORD>(value.size()),
        path.c_str());
    wchar_t* end{};
    const auto parsed = std::wcstof(value.data(), &end);
    if (end == value.data() || !std::isfinite(parsed)) {
        logger::warn(
            "Invalid [Overlay] MenuSoundVolume in {}; using 0.60",
            path.string());
        return 0.6F;
    }
    return (std::clamp)(parsed, 0.0F, 1.0F);
}

[[nodiscard]] MenuPresentation read_menu_presentation(
    const std::filesystem::path& path)
{
    std::array<wchar_t, 32> value{};
    GetPrivateProfileStringW(
        L"Overlay",
        L"MenuPresentation",
        L"Full",
        value.data(),
        static_cast<DWORD>(value.size()),
        path.c_str());
    const auto configured = lowercase(value.data());
    if (configured == L"full") {
        return MenuPresentation::full;
    }
    if (configured == L"bar") {
        return MenuPresentation::bar;
    }
    if (configured == L"dock") {
        return MenuPresentation::dock;
    }
    if (configured == L"drawer") {
        return MenuPresentation::drawer;
    }
    if (configured == L"card") {
        return MenuPresentation::card;
    }
    if (configured == L"corner") {
        return MenuPresentation::corner;
    }
    logger::warn(
        "[Overlay] MenuPresentation in {} is not one of Full, Bar, Dock, "
        "Drawer, Card or Corner; using Full",
        path.string());
    return MenuPresentation::full;
}

[[nodiscard]] OverlayFps read_overlay_fps_display(
    const std::filesystem::path& path)
{
    std::array<wchar_t, 32> value{};
    GetPrivateProfileStringW(
        L"Overlay",
        L"FpsDisplay",
        L"Both",
        value.data(),
        static_cast<DWORD>(value.size()),
        path.c_str());
    const auto configured = lowercase(value.data());
    if (configured == L"both") {
        return OverlayFps::both;
    }
    if (configured == L"output") {
        return OverlayFps::output;
    }
    if (configured == L"rendered") {
        return OverlayFps::rendered;
    }
    logger::warn(
        "Invalid [Overlay] FpsDisplay in {}; using Both",
        path.string());
    return OverlayFps::both;
}

[[nodiscard]] FsrColorSpace read_fsr_color_space(
    const std::filesystem::path& path)
{
    std::array<wchar_t, 32> value{};
    GetPrivateProfileStringW(
        L"Upscaling",
        L"FsrColorSpace",
        L"NonLinearSRGB",
        value.data(),
        static_cast<DWORD>(value.size()),
        path.c_str());
    const auto configured = lowercase(value.data());
    if (configured == L"linear") {
        return FsrColorSpace::linear;
    }
    if (configured == L"nonlinearsrgb" || configured == L"srgb" ||
        configured == L"nonlinear") {
        return FsrColorSpace::non_linear_srgb;
    }
    logger::warn(
        "Invalid FsrColorSpace in {}; using NonLinearSRGB",
        path.string());
    return FsrColorSpace::non_linear_srgb;
}

[[nodiscard]] MotionDilation read_motion_dilation(
    const std::filesystem::path& path)
{
    std::array<wchar_t, 32> value{};
    GetPrivateProfileStringW(
        L"Upscaling",
        L"MotionDilation",
        L"Standard",
        value.data(),
        static_cast<DWORD>(value.size()),
        path.c_str());
    const auto configured = lowercase(value.data());
    if (configured == L"standard") {
        return MotionDilation::standard;
    }
    if (configured == L"nearfade") {
        return MotionDilation::near_fade;
    }
    if (configured == L"off" || configured == L"none") {
        return MotionDilation::off;
    }
    logger::warn(
        "Invalid [Upscaling] MotionDilation in {}; using Standard",
        path.string());
    return MotionDilation::standard;
}

[[nodiscard]] DepthInvertedOverride read_depth_inverted_override(
    const std::filesystem::path& path)
{
    std::array<wchar_t, 32> value{};
    GetPrivateProfileStringW(
        L"Upscaling",
        L"DepthInverted",
        L"Measured",
        value.data(),
        static_cast<DWORD>(value.size()),
        path.c_str());
    const auto configured = lowercase(value.data());
    if (configured == L"measured") {
        return DepthInvertedOverride::measured;
    }
    if (configured == L"inverted" || configured == L"true") {
        return DepthInvertedOverride::force_inverted;
    }
    if (configured == L"standard" || configured == L"false") {
        return DepthInvertedOverride::force_standard;
    }
    logger::warn(
        "Invalid [Upscaling] DepthInverted in {}; using Measured",
        path.string());
    return DepthInvertedOverride::measured;
}

[[nodiscard]] FsrDepthOverride read_fsr_depth_override(
    const std::filesystem::path& path)
{
    std::array<wchar_t, 32> value{};
    GetPrivateProfileStringW(
        L"Upscaling",
        L"FsrDepth",
        L"Measured",
        value.data(),
        static_cast<DWORD>(value.size()),
        path.c_str());
    const auto configured = lowercase(value.data());
    if (configured == L"measured") {
        return FsrDepthOverride::measured;
    }
    if (configured == L"standardfinite" || configured == L"standard") {
        return FsrDepthOverride::standard_finite;
    }
    if (configured == L"standardinfinite") {
        return FsrDepthOverride::standard_infinite;
    }
    if (configured == L"invertedfinite" || configured == L"inverted") {
        return FsrDepthOverride::inverted_finite;
    }
    if (configured == L"invertedinfinite") {
        return FsrDepthOverride::inverted_infinite;
    }
    logger::warn(
        "Invalid FsrDepth in {}; using Measured",
        path.string());
    return FsrDepthOverride::measured;
}

[[nodiscard]] float read_motion_scale(
    const std::filesystem::path& path,
    const wchar_t* const key)
{
    std::array<wchar_t, 32> value{};
    GetPrivateProfileStringW(
        L"Upscaling",
        key,
        L"1.00",
        value.data(),
        static_cast<DWORD>(value.size()),
        path.c_str());
    wchar_t* end{};
    const auto parsed = std::wcstof(value.data(), &end);
    if (end == value.data() || !std::isfinite(parsed)) {
        logger::warn(
            "Invalid motion-vector scale in {}; using 1.00",
            path.string());
        return 1.0F;
    }

    return (std::clamp)(parsed, -4.0F, 4.0F);
}

[[nodiscard]] float read_sharpness(
    const std::filesystem::path& path)
{
    std::array<wchar_t, 32> value{};
    GetPrivateProfileStringW(
        L"Upscaling",
        L"Sharpness",
        L"0.00",
        value.data(),
        static_cast<DWORD>(value.size()),
        path.c_str());
    wchar_t* end{};
    const auto parsed = std::wcstof(value.data(), &end);
    if (end == value.data() || !std::isfinite(parsed)) {
        logger::warn(
            "Invalid DLSS sharpness in {}; using 0.00",
            path.string());
        return 0.0F;
    }
    return (std::clamp)(parsed, 0.0F, 1.0F);
}

[[nodiscard]] std::uint32_t read_menu_key(
    const std::filesystem::path& path)
{
    std::array<wchar_t, 32> value{};
    GetPrivateProfileStringW(
        L"Input",
        L"MenuKey",
        L"PageDown",
        value.data(),
        static_cast<DWORD>(value.size()),
        path.c_str());
    const auto configured = lowercase(value.data());
    for (const auto& candidate : kVirtualKeys) {
        if (lowercase(std::wstring{candidate.name}) == configured) {
            return candidate.key;
        }
    }
    if (configured.size() == 1) {
        const auto translated = VkKeyScanW(configured.front());
        if (translated != -1) {
            return static_cast<std::uint32_t>(
                translated & 0xFF);
        }
    }
    logger::warn(
        "Invalid graphics-menu key in {}; using PageDown",
        path.string());
    return VK_NEXT;
}

[[nodiscard]] std::string_view mode_name(
    const UpscalingMode mode) noexcept
{
    switch (mode) {
    case UpscalingMode::off:
        return "Off";
    case UpscalingMode::dlaa:
        return "DLAA";
    case UpscalingMode::quality:
        return "Quality";
    case UpscalingMode::balanced:
        return "Balanced";
    case UpscalingMode::performance:
        return "Performance";
    case UpscalingMode::ultra_performance:
        return "UltraPerformance";
    }
    return "Off";
}

[[nodiscard]] std::string_view multiplier_mode_name(
    const MultiplierMode mode) noexcept
{

    switch (mode) {
    case MultiplierMode::dynamic:
        return "up to";
    case MultiplierMode::fixed:
        return "fixed";
    }
    return "up to";
}

[[nodiscard]] std::string_view reflex_mode_name(
    const ReflexMode mode) noexcept
{
    switch (mode) {
    case ReflexMode::off:
        return "Off";
    case ReflexMode::low_latency:
        return "On";
    case ReflexMode::low_latency_boost:
        return "Boost";
    }
    return "On";
}

[[nodiscard]] std::string_view dlss_preset_name(
    const DlssPreset preset) noexcept
{
    switch (preset) {
    case DlssPreset::recommended:
        return "Recommended";
    case DlssPreset::legacy_e:
        return "E";
    case DlssPreset::j:
        return "J";
    case DlssPreset::k:
        return "K";
    case DlssPreset::l:
        return "L";
    case DlssPreset::m:
        return "M";
    }
    return "Recommended";
}

[[nodiscard]] std::string_view dlss_temporal_inputs_name(
    const DlssTemporalInputs inputs) noexcept
{
    switch (inputs) {
    case DlssTemporalInputs::standard:
        return "Standard";
    case DlssTemporalInputs::no_hints:
        return "NoHints";
    case DlssTemporalInputs::raw_motion:
        return "RawMotion";
    case DlssTemporalInputs::raw_motion_no_hints:
        return "RawMotionNoHints";
    }
    return "Standard";
}

[[nodiscard]] std::string_view temporal_jitter_name(
    const TemporalJitter jitter) noexcept
{
    switch (jitter) {
    case TemporalJitter::halton:
        return "Halton";
    case TemporalJitter::disabled:
        return "Off";
    }
    return "Halton";
}

[[nodiscard]] std::string_view surface_model_name(
    const SurfaceModel model) noexcept
{
    switch (model) {
    case SurfaceModel::complete_frame:
        return "CompleteFrame";
    case SurfaceModel::sub_rect:
        return "SubRect";
    }
    return "CompleteFrame";
}
}

Settings& Settings::instance() noexcept
{
    static Settings settings;
    return settings;
}

bool Settings::load()
{
    if (path_.empty()) {
        path_ = configuration_path();
        if (path_.empty()) {
            logger::error(
                "Unable to resolve the configuration path from the plugin "
                "module");
            return false;
        }
    }

    std::error_code existence_error;
    if (!std::filesystem::is_regular_file(path_, existence_error) ||
        existence_error) {
        logger::error(
            "Configuration file is unavailable: {}",
            path_.string());
        return false;
    }

    auto configured = static_cast<std::uint32_t>(
        GetPrivateProfileIntW(
            L"FrameGeneration",
            L"Multiplier",
            2,
            path_.c_str()));
    if (std::ranges::find(kFixedMultipliers, configured) ==
        kFixedMultipliers.end()) {
        logger::warn(
            "Invalid frame-generation multiplier {} in {}; using 2x",
            configured,
            path_.string());
        configured = 2;
    }

    frame_generation_enabled_ =
        GetPrivateProfileIntW(
            L"FrameGeneration",
            L"Enabled",
            0,
            path_.c_str()) != 0;
    frame_generation_multiplier_ = configured;
    multiplier_mode_ = read_multiplier_mode(path_);
    base_frame_limit_ = static_cast<std::uint32_t>(
        GetPrivateProfileIntW(
            L"FrameGeneration",
            L"BaseFrameLimit",
            0,
            path_.c_str()));
    if (!valid_frame_limit(base_frame_limit_)) {
        logger::warn(
            "Invalid base frame limit {} in {}; supported range is {}-{} "
            "FPS or 0 for Off; using Off",
            base_frame_limit_,
            path_.string(),
            kMinimumFrameLimit,
            kMaximumFrameLimit);
        base_frame_limit_ = 0U;
    }
    upscaling_provider_ = read_upscaling_provider(path_);
    upscaling_mode_ = read_upscaling_mode(path_);
    reflex_mode_ = read_reflex_mode(path_);
    frame_limit_ = static_cast<std::uint32_t>(
        GetPrivateProfileIntW(
            L"Reflex",
            L"FrameLimit",
            0,
            path_.c_str()));
    if (!valid_frame_limit(frame_limit_)) {
        logger::warn(
            "Invalid final output frame limit {} in {}; supported range is "
            "{}-{} FPS or 0 for Off; using Off",
            frame_limit_,
            path_.string(),
            kMinimumFrameLimit,
            kMaximumFrameLimit);
        frame_limit_ = 0;
    }
    dlss_preset_ = read_dlss_preset(path_);
    dlss_temporal_inputs_ = read_dlss_temporal_inputs(path_);
    temporal_jitter_ = read_temporal_jitter(path_);

    if (!surface_model_latched_) {
        surface_model_latched_ = true;
        surface_model_ = read_surface_model(path_);
    } else if (!surface_model_change_logged_ &&
               read_surface_model(path_) != surface_model_) {
        surface_model_change_logged_ = true;
        logger::warn(
            "SurfaceModel was changed in UFGU.ini, but it is read once per "
            "session because the render surface is built from it at startup. "
            "Restart Skyrim for the new value to take effect.");
    }

    if (!jitter_fold_latched_) {
        jitter_fold_latched_ = true;
        jitter_fold_ = read_jitter_fold(path_);
        mip_bias_mode_ = read_mip_bias_mode(path_);
    } else if (!jitter_fold_change_logged_ &&
               read_jitter_fold(path_) != jitter_fold_) {
        jitter_fold_change_logged_ = true;
        logger::warn(
            "JitterFold was changed in UFGU.ini, but it is read once per "
            "session because it is resolved against the game's camera setup "
            "at startup. Restart Skyrim for the new value to take effect.");
    }

    if (!vendor_frame_generation_latched_) {
        vendor_frame_generation_latched_ = true;
        vendor_frame_generation_ = read_vendor_frame_generation(path_);
    } else if (!vendor_frame_generation_change_logged_ &&
               read_vendor_frame_generation(path_) !=
                   vendor_frame_generation_) {
        vendor_frame_generation_change_logged_ = true;
        logger::warn(
            "VendorFrameGeneration was changed in UFGU.ini, but it is read "
            "once per session because it decides how the D3D12 device and the "
            "swap chain are created at startup. Restart Skyrim for the new "
            "value to take effect.");
    }
    sharpness_ = read_sharpness(path_);
    motion_scale_x_ = read_motion_scale(path_, L"MotionScaleX");
    motion_scale_y_ = read_motion_scale(path_, L"MotionScaleY");
    jitter_source_ = read_jitter_source(path_);
    fsr_color_space_ = read_fsr_color_space(path_);
    fsr_depth_override_ = read_fsr_depth_override(path_);
    depth_inverted_override_ = read_depth_inverted_override(path_);
    motion_dilation_ = read_motion_dilation(path_);

    allow_tearing_ =
        GetPrivateProfileIntW(
            L"Display",
            L"AllowTearing",
            0,
            path_.c_str()) != 0;

    const auto requested_back_buffers =
        GetPrivateProfileIntW(
            L"Display",
            L"BackBufferCount",
            0,
            path_.c_str());
    if (requested_back_buffers <= 0) {
        back_buffer_count_ = 0U;
    } else if (requested_back_buffers < 3) {
        back_buffer_count_ = 3U;
    } else if (requested_back_buffers > 16) {
        back_buffer_count_ = 16U;
    } else {
        back_buffer_count_ = static_cast<std::uint32_t>(requested_back_buffers);
    }
    overlay_enabled_ =
        GetPrivateProfileIntW(
            L"Overlay",
            L"Enabled",
            1,
            path_.c_str()) != 0;
    d3d12_debug_layer_ =
        GetPrivateProfileIntW(
            L"Debug",
            L"D3D12DebugLayer",
            0,
            path_.c_str()) != 0;
    experimental_features_ =
        GetPrivateProfileIntW(
            L"Experimental",
            L"Enabled",
            0,
            path_.c_str()) != 0;
    show_only_interpolated_frames_ =
        GetPrivateProfileIntW(
            L"Experimental",
            L"ShowOnlyInterpolatedFrames",
            0,
            path_.c_str()) != 0;
    depth_object_separation_ = static_cast<float>(
        GetPrivateProfileIntW(
            L"Upscaling",
            L"DepthObjectSeparationTenths",
            400,
            path_.c_str())) / 10.0F;
    overlay_fps_display_ = read_overlay_fps_display(path_);
    menu_presentation_ = read_menu_presentation(path_);
    menu_sounds_enabled_ = GetPrivateProfileIntW(
        L"Overlay", L"MenuSounds", 1, path_.c_str()) != 0;
    menu_sound_volume_ = read_menu_sound_volume(path_);
    menu_key_ = read_menu_key(path_);

    debug_view_index_ = static_cast<std::uint32_t>(
        GetPrivateProfileIntW(L"Debug", L"View", 0, path_.c_str()));

    if (debug_view_index_ > 7U) {
        logger::warn(
            "[Debug] View={} is out of range; debug views are disabled",
            debug_view_index_);
        debug_view_index_ = 0U;
    }
    if (debug_view_index_ != 0U) {
        logger::warn(
            "[Debug] View={} selects a diagnostic visualization at startup. "
            "This replaces the scene image and is not a release setting.",
            debug_view_index_);
    }
    std::error_code error;
    last_write_time_ = std::filesystem::last_write_time(path_, error);
    loaded_ = true;
    logger::info(
        "Configuration loaded from {}: MFG={} ({} {}x), upscaler={}, "
        "quality={}, "
        "preset={}, temporal-inputs={}, jitter={}, surface-model={}, "
        "sharpness={:.2f}, "
        "Reflex={}, base-frame-limit={}, output-frame-limit={}, overlay={}, "
        "menu-key=0x{:02X}",
        path_.string(),
        frame_generation_enabled_,
        multiplier_mode_name(multiplier_mode_),
        frame_generation_multiplier_,
        providers::vendor_display_name(upscaling_provider_),
        mode_name(upscaling_mode_),
        dlss_preset_name(dlss_preset_),
        dlss_temporal_inputs_name(dlss_temporal_inputs_),
        temporal_jitter_name(temporal_jitter_),
        surface_model_name(surface_model_),
        sharpness_,
        reflex_mode_name(reflex_mode_),
        base_frame_limit_,
        frame_limit_,
        overlay_enabled_,
        menu_key_);
    return true;
}

bool Settings::loaded() const noexcept
{
    return loaded_;
}

bool Settings::reload_if_changed()
{
    if (path_.empty()) {
        return load();
    }

    constexpr auto kChangeCheckInterval = std::chrono::milliseconds(250);
    const auto now = std::chrono::steady_clock::now();
    if (last_change_check_ != std::chrono::steady_clock::time_point{} &&
        now - last_change_check_ < kChangeCheckInterval) {
        return false;
    }
    last_change_check_ = now;

    std::error_code error;
    const auto current_write_time =
        std::filesystem::last_write_time(path_, error);
    if (error || current_write_time == last_write_time_) {
        return false;
    }

    const auto previous_frame_generation =
        frame_generation_enabled_;
    const auto previous_multiplier = frame_generation_multiplier_;
    const auto previous_multiplier_mode = multiplier_mode_;
    const auto previous_provider = upscaling_provider_;
    const auto previous_upscaling = upscaling_mode_;
    const auto previous_reflex = reflex_mode_;
    const auto previous_base_frame_limit = base_frame_limit_;
    const auto previous_frame_limit = frame_limit_;
    const auto previous_preset = dlss_preset_;
    const auto previous_temporal_inputs = dlss_temporal_inputs_;
    const auto previous_jitter = temporal_jitter_;
    const auto previous_sharpness = sharpness_;
    const auto previous_overlay = overlay_enabled_;
    const auto previous_menu_key = menu_key_;
    if (!load()) {
        return frame_generation_enabled_ != previous_frame_generation ||
               frame_generation_multiplier_ != previous_multiplier ||
               multiplier_mode_ != previous_multiplier_mode ||
               upscaling_provider_ != previous_provider ||
               upscaling_mode_ != previous_upscaling ||
               reflex_mode_ != previous_reflex ||
               base_frame_limit_ != previous_base_frame_limit ||
               frame_limit_ != previous_frame_limit ||
               dlss_preset_ != previous_preset ||
               dlss_temporal_inputs_ != previous_temporal_inputs ||
               temporal_jitter_ != previous_jitter ||
               sharpness_ != previous_sharpness ||
               overlay_enabled_ != previous_overlay ||
               menu_key_ != previous_menu_key;
    }
    if (frame_generation_enabled_ != previous_frame_generation ||
        frame_generation_multiplier_ != previous_multiplier ||
        multiplier_mode_ != previous_multiplier_mode ||
        upscaling_provider_ != previous_provider ||
        upscaling_mode_ != previous_upscaling ||
        reflex_mode_ != previous_reflex ||
        base_frame_limit_ != previous_base_frame_limit ||
        frame_limit_ != previous_frame_limit ||
        dlss_preset_ != previous_preset ||
        dlss_temporal_inputs_ != previous_temporal_inputs ||
        temporal_jitter_ != previous_jitter ||
        sharpness_ != previous_sharpness ||
        overlay_enabled_ != previous_overlay ||
        menu_key_ != previous_menu_key) {
        logger::info(
            "Live configuration change detected: MFG={} ({} {}x), "
            "upscaler={}, quality={}, preset={}, temporal-inputs={}, jitter={}, "
            "sharpness={:.2f}, Reflex={}, base-frame-limit={}, "
            "output-frame-limit={}, overlay={}, menu-key=0x{:02X}",
            frame_generation_enabled_,
            multiplier_mode_name(multiplier_mode_),
            frame_generation_multiplier_,
            providers::vendor_display_name(upscaling_provider_),
            mode_name(upscaling_mode_),
            dlss_preset_name(dlss_preset_),
            dlss_temporal_inputs_name(dlss_temporal_inputs_),
            temporal_jitter_name(temporal_jitter_),
            sharpness_,
            reflex_mode_name(reflex_mode_),
            base_frame_limit_,
            frame_limit_,
            overlay_enabled_,
            menu_key_);
        return true;
    }
    return false;
}

bool Settings::frame_generation_enabled() const noexcept
{
    return frame_generation_enabled_;
}

bool Settings::set_frame_generation_enabled(const bool enabled)
{
    if (path_.empty() && !load()) {
        return false;
    }
    if (frame_generation_enabled_ == enabled) {
        return true;
    }

    const auto written =
        WritePrivateProfileStringW(
            L"FrameGeneration",
            L"Enabled",
            enabled ? L"1" : L"0",
            path_.c_str()) != FALSE;
    if (!written) {
        logger::error(
            "Unable to persist frame-generation state {} to {}",
            enabled,
            path_.string());
        return false;
    }
    frame_generation_enabled_ = enabled;
    std::error_code error;
    last_write_time_ = std::filesystem::last_write_time(path_, error);
    logger::info(
        "In-game graphics menu set frame generation {}",
        enabled ? "On" : "Off");
    return true;
}

bool Settings::set_frame_generation_multiplier(
    const std::uint32_t multiplier)
{
    if (std::ranges::find(kFixedMultipliers, multiplier) ==
        kFixedMultipliers.end()) {
        return false;
    }
    if (path_.empty() && !load()) {
        return false;
    }
    if (frame_generation_multiplier_ == multiplier) {
        return true;
    }

    const auto value = std::to_wstring(multiplier);
    const auto written =
        WritePrivateProfileStringW(
            L"FrameGeneration",
            L"Multiplier",
            value.c_str(),
            path_.c_str()) != FALSE;
    if (!written) {
        logger::error(
            "Unable to persist in-game MFG selector value {}x to {}",
            multiplier,
            path_.string());
        return false;
    }
    frame_generation_multiplier_ = multiplier;
    std::error_code error;
    last_write_time_ = std::filesystem::last_write_time(path_, error);
    logger::info(
        "In-game MFG selector applied fixed {}x (persisted=true)",
        multiplier);
    return true;
}

bool Settings::set_multiplier_mode(const MultiplierMode mode)
{
    if (path_.empty() && !load()) {
        return false;
    }
    if (multiplier_mode_ == mode) {
        return true;
    }

    const auto* const value =
        mode == MultiplierMode::dynamic ? L"Dynamic" : L"Fixed";
    const auto written =
        WritePrivateProfileStringW(
            L"FrameGeneration",
            L"MultiplierMode",
            value,
            path_.c_str()) != FALSE;
    if (!written) {
        logger::error(
            "Unable to persist in-game multiplier mode to {}",
            path_.string());
        return false;
    }
    multiplier_mode_ = mode;
    std::error_code error;
    last_write_time_ = std::filesystem::last_write_time(path_, error);
    logger::info(
        "In-game menu set Dynamic Multi Frame Generation {} (persisted=true)",
        mode == MultiplierMode::dynamic ? "On" : "Off");
    return true;
}

bool Settings::set_upscaling_provider(const providers::Vendor provider)
{
    if (provider != providers::Vendor::nvidia &&
        provider != providers::Vendor::amd &&
        provider != providers::Vendor::intel) {
        return false;
    }
    if (path_.empty() && !load()) {
        return false;
    }
    const auto key = providers::vendor_key(provider);
    const std::wstring value{key.begin(), key.end()};
    const auto written =
        WritePrivateProfileStringW(
            L"Upscaling",
            L"Provider",
            value.c_str(),
            path_.c_str()) != FALSE;
    if (!written) {
        logger::error(
            "Unable to persist upscaling provider {} to {}",
            providers::vendor_display_name(provider),
            path_.string());
        return false;
    }

    upscaling_provider_ = provider;
    std::error_code error;
    last_write_time_ = std::filesystem::last_write_time(path_, error);
    logger::info(
        "Universal upscaling provider staged for the next Skyrim launch: {} "
        "(persisted=true)",
        providers::vendor_display_name(provider));
    return true;
}

bool Settings::commit_upscaling_selection(
    const providers::Vendor provider,
    const UpscalingMode mode)
{
    if (provider != providers::Vendor::nvidia &&
        provider != providers::Vendor::amd &&
        provider != providers::Vendor::intel) {
        return false;
    }
    const auto candidate = std::ranges::find_if(
        kUpscalingModes,
        [mode](const UpscalingModeName& value) {
            return value.mode == mode;
        });
    if (candidate == kUpscalingModes.end()) {
        return false;
    }
    if (path_.empty() && !load()) {
        return false;
    }

    std::array<wchar_t, 64> previous_provider{};
    std::array<wchar_t, 64> previous_mode{};
    GetPrivateProfileStringW(
        L"Upscaling",
        L"Provider",
        L"NVIDIA",
        previous_provider.data(),
        static_cast<DWORD>(previous_provider.size()),
        path_.c_str());
    GetPrivateProfileStringW(
        L"Upscaling",
        L"Mode",
        L"Off",
        previous_mode.data(),
        static_cast<DWORD>(previous_mode.size()),
        path_.c_str());

    const auto restore_physical_selection = [&]() {
        const auto provider_restored = WritePrivateProfileStringW(
            L"Upscaling",
            L"Provider",
            previous_provider.data(),
            path_.c_str()) != FALSE;
        const auto mode_restored = WritePrivateProfileStringW(
            L"Upscaling",
            L"Mode",
            previous_mode.data(),
            path_.c_str()) != FALSE;
        const auto rollback_verified = verify_profile_commit(
            path_, previous_provider.data(), previous_mode.data());
        return std::array{
            provider_restored,
            mode_restored,
            rollback_verified.durable()};
    };

    const auto provider_key = providers::vendor_key(provider);
    const std::wstring provider_value{
        provider_key.begin(), provider_key.end()};
    if (WritePrivateProfileStringW(
            L"Upscaling",
            L"Provider",
            provider_value.c_str(),
            path_.c_str()) == FALSE) {
        logger::error(
            "Atomic upscaling selection could not persist provider {}",
            providers::vendor_display_name(provider));
        return false;
    }

    if (WritePrivateProfileStringW(
            L"Upscaling",
            L"Mode",
            candidate->name.data(),
            path_.c_str()) == FALSE) {
        const auto rollback = restore_physical_selection();
        logger::error(
            "Atomic upscaling selection could not persist mode {}; physical "
            "rollback provider={}, mode={}, flushed={}",
            mode_name(mode),
            rollback[0],
            rollback[1],
            rollback[2]);
        return false;
    }

    const auto commit = verify_profile_commit(
        path_, provider_value, candidate->name);
    if (!commit.durable()) {
        const auto rollback = restore_physical_selection();
        logger::error(
            "Atomic upscaling selection could not verify durable storage in "
            "{} (file-flushed={}, file-read={}, provider-match={}, "
            "mode-match={}, win32-error={}); physical rollback provider={}, "
            "mode={}, durable={}",
            path_.string(),
            commit.file_flushed,
            commit.file_read,
            commit.provider_matches,
            commit.mode_matches,
            commit.error,
            rollback[0],
            rollback[1],
            rollback[2]);
        return false;
    }

    upscaling_provider_ = provider;
    upscaling_mode_ = mode;
    std::error_code error;
    last_write_time_ = std::filesystem::last_write_time(path_, error);
    logger::info(
        "Atomic upscaling selection committed after renderer validation: {} "
        "{} (persisted=true, path={})",
        providers::vendor_display_name(provider),
        mode_name(mode),
        path_.string());
    return true;
}

bool Settings::stage_upscaling_for_restart(
    const providers::Vendor provider,
    const UpscalingMode mode)
{
    if (provider != providers::Vendor::nvidia &&
        provider != providers::Vendor::amd &&
        provider != providers::Vendor::intel) {
        return false;
    }
    const auto candidate = std::ranges::find_if(
        kUpscalingModes,
        [mode](const UpscalingModeName& value) {
            return value.mode == mode;
        });
    if (candidate == kUpscalingModes.end()) {
        return false;
    }
    if (path_.empty() && !load()) {
        return false;
    }

    const auto provider_key = providers::vendor_key(provider);
    const std::wstring provider_value{
        provider_key.begin(), provider_key.end()};
    const auto previous_provider_key =
        providers::vendor_key(upscaling_provider_);
    const std::wstring previous_provider_value{
        previous_provider_key.begin(), previous_provider_key.end()};
    const auto previous_mode = std::ranges::find_if(
        kUpscalingModes,
        [this](const UpscalingModeName& value) {
            return value.mode == upscaling_mode_;
        });

    if (WritePrivateProfileStringW(
            L"Upscaling",
            L"Provider",
            provider_value.c_str(),
            path_.c_str()) == FALSE) {
        logger::error(
            "Unable to stage upscaling provider {} for restart in {}",
            providers::vendor_display_name(provider),
            path_.string());
        return false;
    }

    if (WritePrivateProfileStringW(
            L"Upscaling",
            L"Mode",
            candidate->name.data(),
            path_.c_str()) == FALSE) {

        auto restored =
            WritePrivateProfileStringW(
                L"Upscaling",
                L"Provider",
                previous_provider_value.c_str(),
                path_.c_str()) != FALSE;
        if (previous_mode != kUpscalingModes.end()) {
            const auto mode_restored =
                WritePrivateProfileStringW(
                    L"Upscaling",
                    L"Mode",
                    previous_mode->name.data(),
                    path_.c_str()) != FALSE;
            restored = restored && mode_restored;
        }
        logger::error(
            "Unable to stage {} upscaling mode {} for restart in {}; {}",
            providers::vendor_display_name(provider),
            mode_name(mode),
            path_.string(),
            restored
                ? "the previous provider/profile pair was restored"
                : "the previous provider/profile pair could not be restored "
                  "either, so the file may now name a provider without its "
                  "matching mode");
        return false;
    }

    std::error_code error;
    last_write_time_ = std::filesystem::last_write_time(path_, error);
    logger::info(
        "Staged {} upscaling {} for the next Skyrim launch; the active "
        "renderer remains {} {}",
        providers::vendor_display_name(provider),
        mode_name(mode),
        providers::vendor_display_name(upscaling_provider_),
        mode_name(upscaling_mode_));
    return true;
}

bool Settings::set_upscaling_mode(const UpscalingMode mode)
{
    const auto candidate = std::ranges::find_if(
        kUpscalingModes,
        [mode](const UpscalingModeName& value) {
            return value.mode == mode;
        });
    if (candidate == kUpscalingModes.end()) {
        return false;
    }
    if (path_.empty() && !load()) {
        return false;
    }

    const auto written =
        WritePrivateProfileStringW(
            L"Upscaling",
            L"Mode",
            candidate->name.data(),
            path_.c_str()) != FALSE;
    if (!written) {
        logger::error(
            "Unable to persist in-game DLSS SR mode {} to {}",
            mode_name(mode),
            path_.string());
        return false;
    }
    upscaling_mode_ = mode;
    std::error_code error;
    last_write_time_ = std::filesystem::last_write_time(path_, error);
    logger::info(
        "In-game graphics menu applied upscaling mode {} (persisted=true)",
        mode_name(mode));
    return true;
}

bool Settings::set_reflex_mode(const ReflexMode mode)
{
    const auto candidate = std::ranges::find_if(
        kReflexModes,
        [mode](const ReflexModeName& value) {
            return value.mode == mode;
        });
    if (candidate == kReflexModes.end()) {
        return false;
    }
    if (path_.empty() && !load()) {
        return false;
    }
    if (reflex_mode_ == mode) {
        return true;
    }

    const auto written =
        WritePrivateProfileStringW(
            L"Reflex",
            L"Mode",
            candidate->name.data(),
            path_.c_str()) != FALSE;
    if (!written) {
        logger::error(
            "Unable to persist NVIDIA Reflex mode {} to {}",
            reflex_mode_name(mode),
            path_.string());
        return false;
    }
    reflex_mode_ = mode;
    std::error_code error;
    last_write_time_ = std::filesystem::last_write_time(path_, error);
    logger::info(
        "NVIDIA Reflex mode applied: {} (persisted=true)",
        reflex_mode_name(mode));
    return true;
}

bool Settings::set_base_frame_limit(const std::uint32_t frame_limit)
{
    if (!valid_frame_limit(frame_limit)) {
        return false;
    }
    if (path_.empty() && !load()) {
        return false;
    }
    if (base_frame_limit_ == frame_limit) {
        return true;
    }

    const auto value = std::to_wstring(frame_limit);
    const auto written =
        WritePrivateProfileStringW(
            L"FrameGeneration",
            L"BaseFrameLimit",
            value.c_str(),
            path_.c_str()) != FALSE;
    if (!written) {
        logger::error(
            "Unable to persist base frame limit {} to {}",
            frame_limit,
            path_.string());
        return false;
    }
    base_frame_limit_ = frame_limit;
    std::error_code error;
    last_write_time_ = std::filesystem::last_write_time(path_, error);
    logger::info(
        "Base rendered frame limit applied: {}",
        base_frame_limit_ == 0 ?
            "Off" : std::to_string(base_frame_limit_));
    return true;
}

bool Settings::set_frame_limit(const std::uint32_t frame_limit)
{
    if (!valid_frame_limit(frame_limit)) {
        return false;
    }
    if (path_.empty() && !load()) {
        return false;
    }
    if (frame_limit_ == frame_limit) {
        return true;
    }

    const auto value = std::to_wstring(frame_limit);
    const auto written =
        WritePrivateProfileStringW(
            L"Reflex",
            L"FrameLimit",
            value.c_str(),
            path_.c_str()) != FALSE;
    if (!written) {
        logger::error(
            "Unable to persist Reflex frame limit {} to {}",
            frame_limit,
            path_.string());
        return false;
    }
    frame_limit_ = frame_limit;
    std::error_code error;
    last_write_time_ = std::filesystem::last_write_time(path_, error);
    logger::info(
        "Final Streamline output frame limit applied: {}",
        frame_limit_ == 0 ? "Off" : std::to_string(frame_limit_));
    return true;
}

bool Settings::set_sharpness(const float sharpness)
{
    if (!std::isfinite(sharpness)) {
        return false;
    }
    if (path_.empty() && !load()) {
        return false;
    }
    const auto clamped = (std::clamp)(sharpness, 0.0F, 1.0F);
    if (sharpness_ == clamped) {
        return true;
    }
    const auto value = std::format(L"{:.2f}", clamped);
    const auto written =
        WritePrivateProfileStringW(
            L"Upscaling",
            L"Sharpness",
            value.c_str(),
            path_.c_str()) != FALSE;
    if (!written) {
        logger::error(
            "Unable to persist DLSS sharpness {:.2f} to {}",
            clamped,
            path_.string());
        return false;
    }
    sharpness_ = clamped;
    std::error_code error;
    last_write_time_ = std::filesystem::last_write_time(path_, error);
    logger::info(
        "DLSS contrast-adaptive sharpness applied: {:.2f} "
        "(persisted=true)",
        sharpness_);
    return true;
}

bool Settings::set_overlay_fps_display(const OverlayFps display)
{
    if (path_.empty() && !load()) {
        return false;
    }
    if (overlay_fps_display_ == display) {
        return true;
    }

    const auto* const text =
        display == OverlayFps::output ? L"Output" :
        display == OverlayFps::rendered ? L"Rendered" :
        L"Both";
    const auto written =
        WritePrivateProfileStringW(
            L"Overlay",
            L"FpsDisplay",
            text,
            path_.c_str()) != FALSE;
    if (!written) {
        logger::error(
            "Unable to persist FPS-counter display mode to {}",
            path_.string());
        return false;
    }
    overlay_fps_display_ = display;
    std::error_code error;
    last_write_time_ = std::filesystem::last_write_time(path_, error);
    return true;
}

bool Settings::set_overlay_enabled(const bool enabled)
{
    if (path_.empty() && !load()) {
        return false;
    }
    if (overlay_enabled_ == enabled) {
        return true;
    }

    const auto written =
        WritePrivateProfileStringW(
            L"Overlay",
            L"Enabled",
            enabled ? L"1" : L"0",
            path_.c_str()) != FALSE;
    if (!written) {
        logger::error(
            "Unable to persist FPS-counter state {} to {}",
            enabled,
            path_.string());
        return false;
    }
    overlay_enabled_ = enabled;
    std::error_code error;
    last_write_time_ = std::filesystem::last_write_time(path_, error);
    logger::info(
        "In-game graphics menu set FPS counter {}",
        enabled ? "On" : "Off");
    return true;
}

std::uint32_t Settings::frame_generation_multiplier() const noexcept
{
    return frame_generation_multiplier_;
}

MultiplierMode Settings::multiplier_mode() const noexcept
{
    return multiplier_mode_;
}

providers::Vendor Settings::upscaling_provider() const noexcept
{
    return upscaling_provider_;
}

UpscalingMode Settings::upscaling_mode() const noexcept
{
    return upscaling_mode_;
}

ReflexMode Settings::reflex_mode() const noexcept
{
    return reflex_mode_;
}

std::uint32_t Settings::base_frame_limit() const noexcept
{
    return base_frame_limit_;
}

std::uint32_t Settings::frame_limit() const noexcept
{
    return frame_limit_;
}

DlssPreset Settings::dlss_preset() const noexcept
{
    return dlss_preset_;
}

DlssTemporalInputs Settings::dlss_temporal_inputs() const noexcept
{
    return dlss_temporal_inputs_;
}

SurfaceModel Settings::surface_model() const noexcept
{
    return surface_model_;
}

JitterFold Settings::jitter_fold() const noexcept
{
    return jitter_fold_;
}

VendorFrameGeneration Settings::vendor_frame_generation() const noexcept
{
    return vendor_frame_generation_;
}

TemporalJitter Settings::temporal_jitter() const noexcept
{
    return temporal_jitter_;
}

float Settings::sharpness() const noexcept
{
    return sharpness_;
}

JitterSource Settings::jitter_source() const noexcept
{
    return jitter_source_;
}

FsrColorSpace Settings::fsr_color_space() const noexcept
{
    return fsr_color_space_;
}

FsrDepthOverride Settings::fsr_depth_override() const noexcept
{
    return fsr_depth_override_;
}

DepthInvertedOverride Settings::depth_inverted_override() const noexcept
{
    return depth_inverted_override_;
}

MotionDilation Settings::motion_dilation() const noexcept
{
    return motion_dilation_;
}

float Settings::motion_scale_x() const noexcept
{
    return motion_scale_x_;
}

float Settings::motion_scale_y() const noexcept
{
    return motion_scale_y_;
}

std::uint32_t Settings::debug_view_index() const noexcept
{
    return debug_view_index_;
}

std::uint32_t Settings::back_buffer_count() const noexcept
{
    return back_buffer_count_;
}

bool Settings::allow_tearing() const noexcept
{
    return allow_tearing_;
}

bool Settings::d3d12_debug_layer() const noexcept
{
    return d3d12_debug_layer_;
}

bool Settings::show_only_interpolated_frames() const noexcept
{
    return show_only_interpolated_frames_;
}

float Settings::depth_object_separation() const noexcept
{
    return depth_object_separation_;
}

bool Settings::experimental_features() const noexcept
{
    return experimental_features_;
}

MipBiasMode Settings::mip_bias_mode() const noexcept
{
    return mip_bias_mode_;
}

bool Settings::overlay_enabled() const noexcept
{
    return overlay_enabled_;
}

OverlayFps Settings::overlay_fps_display() const noexcept
{
    return overlay_fps_display_;
}

MenuPresentation Settings::menu_presentation() const noexcept
{
    return menu_presentation_;
}

bool Settings::menu_sounds_enabled() const noexcept
{
    return menu_sounds_enabled_;
}

bool Settings::set_menu_sounds_enabled(const bool enabled)
{
    if (path_.empty() && !load()) {
        return false;
    }
    if (menu_sounds_enabled_ == enabled) {
        return true;
    }
    if (WritePrivateProfileStringW(
            L"Overlay",
            L"MenuSounds",
            enabled ? L"1" : L"0",
            path_.c_str()) == FALSE) {
        logger::error(
            "Unable to persist the menu sound setting to {}", path_.string());
        return false;
    }
    menu_sounds_enabled_ = enabled;
    std::error_code error;
    last_write_time_ = std::filesystem::last_write_time(path_, error);
    return true;
}

float Settings::menu_sound_volume() const noexcept
{
    return menu_sound_volume_;
}

bool Settings::set_menu_presentation(const MenuPresentation presentation)
{
    if (path_.empty() && !load()) {
        return false;
    }
    if (menu_presentation_ == presentation) {
        return true;
    }
    const auto* const text =
        presentation == MenuPresentation::bar ? L"Bar" :
        presentation == MenuPresentation::dock ? L"Dock" :
        presentation == MenuPresentation::drawer ? L"Drawer" :
        presentation == MenuPresentation::card ? L"Card" :
        presentation == MenuPresentation::corner ? L"Corner" :
        L"Full";
    if (WritePrivateProfileStringW(
            L"Overlay", L"MenuPresentation", text, path_.c_str()) == FALSE) {
        logger::error(
            "Unable to persist the menu presentation to {}", path_.string());
        return false;
    }
    menu_presentation_ = presentation;
    std::error_code error;
    last_write_time_ = std::filesystem::last_write_time(path_, error);
    return true;
}

std::uint32_t Settings::menu_key() const noexcept
{
    return menu_key_;
}
}
