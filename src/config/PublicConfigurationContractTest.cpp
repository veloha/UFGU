#include <Windows.h>

#include <array>
#include <cstdio>
#include <cwchar>
#include <filesystem>

namespace
{
int failures{};

void require(const bool condition, const char* const message)
{
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) {
        ++failures;
    }
}

void require_integer(
    const std::filesystem::path& path,
    const wchar_t* const section,
    const wchar_t* const key,
    const UINT expected,
    const char* const message)
{
    constexpr UINT missing = UINT_MAX;
    const auto observed = GetPrivateProfileIntW(
        section,
        key,
        missing,
        path.c_str());
    require(observed == expected, message);
}

void require_text(
    const std::filesystem::path& path,
    const wchar_t* const section,
    const wchar_t* const key,
    const wchar_t* const expected,
    const char* const message)
{
    std::array<wchar_t, 64> observed{};
    constexpr wchar_t missing[] = L"<missing>";
    GetPrivateProfileStringW(
        section,
        key,
        missing,
        observed.data(),
        static_cast<DWORD>(observed.size()),
        path.c_str());
    require(_wcsicmp(observed.data(), expected) == 0, message);
}
}

int wmain(const int argc, const wchar_t* const* const argv)
{
    if (argc != 2) {
        std::printf(
            "PublicConfigurationContractTest requires the staged INI path.\n");
        return 2;
    }

    const std::filesystem::path path(argv[1]);
    require(std::filesystem::is_regular_file(path), "public INI exists");

    require_integer(
        path, L"FrameGeneration", L"Enabled", 0,
        "frame generation starts disabled");
    require_integer(
        path, L"FrameGeneration", L"Multiplier", 2,
        "frame-generation multiplier starts at 2x");
    require_text(
        path, L"FrameGeneration", L"MultiplierMode", L"Fixed",
        "dynamic frame generation is opt-in");
    require_integer(
        path, L"FrameGeneration", L"BaseFrameLimit", 0,
        "base render frame limiter starts disabled");
    require_text(
        path, L"FrameGeneration", L"VendorFrameGeneration", L"Off",
        "vendor presentation replacement is opt-in");

    require_text(
        path, L"Upscaling", L"Provider", L"NVIDIA",
        "default provider selection is explicit");
    require_text(
        path, L"Upscaling", L"Mode", L"Off",
        "upscaling starts disabled");
    require_text(
        path, L"Upscaling", L"MipBias", L"Off",
        "the reserved mip bias key keeps its inert value");
    require_text(
        path, L"Upscaling", L"SurfaceModel", L"CompleteFrame",
        "surface model has a deterministic startup value");
    require_text(
        path, L"Upscaling", L"JitterFold", L"NopGate",
        "jitter-fold startup contract is explicit");

    require_integer(
        path, L"Display", L"AllowTearing", 0,
        "tearing starts disabled");
    require_text(
        path, L"Reflex", L"Mode", L"Off",
        "low-latency control starts disabled");
    require_integer(
        path, L"Reflex", L"FrameLimit", 0,
        "output frame limiter starts disabled");
    require_integer(
        path, L"Overlay", L"Enabled", 1,
        "diagnostic overlay starts enabled");
    require_integer(
        path, L"Debug", L"View", 0,
        "debug visualization starts disabled");
    require_integer(
        path, L"Debug", L"D3D12DebugLayer", 0,
        "the D3D12 debug layer starts disabled");
    require_integer(
        path, L"Experimental", L"Enabled", 0,
        "experimental features are off in the shipped configuration, so a "
        "public build behaves identically whether or not the section exists");

    require_text(
        path, L"Upscaling", L"Preset", L"Recommended",
        "DLSS preset defers to the runtime recommendation");
    require_text(
        path, L"Upscaling", L"TemporalInputs", L"Standard",
        "temporal input policy starts at the documented default");
    require_text(
        path, L"Upscaling", L"Jitter", L"Halton",
        "jitter sequence starts at the documented default");
    require_text(
        path, L"Upscaling", L"JitterSource", L"Requested",
        "jitter source starts at the documented default");
    require_text(
        path, L"Upscaling", L"MotionDilation", L"Standard",
        "motion dilation starts at the documented default");
    require_text(
        path, L"Upscaling", L"Sharpness", L"0.00",
        "post-upscale sharpening starts disabled");
    require_text(
        path, L"Upscaling", L"MotionScaleX", L"1.00",
        "horizontal motion scale starts unscaled");
    require_text(
        path, L"Upscaling", L"MotionScaleY", L"1.00",
        "vertical motion scale starts unscaled");
    require_text(
        path, L"Upscaling", L"DepthInverted", L"Measured",
        "the NVIDIA depth override defers to measurement");
    require_text(
        path, L"Upscaling", L"FsrDepth", L"Measured",
        "the AMD depth override defers to measurement");
    require_text(
        path, L"Upscaling", L"FsrColorSpace", L"NonLinearSRGB",
        "the FidelityFX colour space contract is explicit");
    require_text(
        path, L"Overlay", L"FpsDisplay", L"Both",
        "the overlay counter selection is explicit");
    require_text(
        path, L"Input", L"MenuKey", L"PageDown",
        "the documented menu key is what ships");

    return failures == 0 ? 0 : 1;
}
