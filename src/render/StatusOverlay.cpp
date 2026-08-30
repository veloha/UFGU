#include "render/StatusOverlay.hpp"

#include "render/MenuBarLayout.hpp"
#include "render/MultiplierProjection.hpp"
#include "render/RuntimeCompatibilitySkse.hpp"

#include "audio/MenuAudio.hpp"

#include "config/Settings.hpp"
#include "providers/FsrUpscaler.hpp"
#include "providers/LowLatencyController.hpp"
#include "providers/XessUpscaler.hpp"
#include "diagnostics/FrameTimeSeries.hpp"
#include "render/ApplyOutcome.hpp"
#include "render/CameraData.hpp"
#include "render/D3D12Backend.hpp"
#include "render/DebugViewPass.hpp"
#include "render/DynamicResolution.hpp"
#include "render/FrameGenerationChoicePolicy.hpp"
#include "render/MenuGeometry.hpp"
#include "render/OutputCapControls.hpp"
#include "render/PresentationBridge.hpp"
#include "render/ProfileChangeController.hpp"
#include "render/FsrFrameGeneration.hpp"
#include "render/RendererRuntime.hpp"
#include "render/XessFrameGeneration.hpp"
#include "render/UpscalingPass.hpp"
#include "streamline/FeatureSupport.hpp"
#include "streamline/FrameGeneration.hpp"
#include "streamline/SuperResolution.hpp"

#include <Windows.h>
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <d2d1helper.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <SKSE/SKSE.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace mfgdlss::render
{
namespace
{
namespace logger = SKSE::log;
using Microsoft::WRL::ComPtr;

constexpr std::size_t kMenuRowCount = 12;
constexpr std::size_t kFrameGenerationRow = 0;

constexpr std::size_t kDynamicMfgRow = 1;
constexpr std::size_t kBaseCapRow = 2;
constexpr std::size_t kProviderRow = 3;
constexpr std::size_t kUpscalingRow = 4;
constexpr std::size_t kSharpnessRow = 5;
constexpr std::size_t kReflexRow = 6;
constexpr std::size_t kOutputCapRow = 7;
constexpr std::size_t kOverlayRow = 8;

constexpr std::size_t kDebugRow = 9;
constexpr std::size_t kNtcRow = 10;
constexpr std::size_t kMenuSoundsRow = 11;

constexpr float kValueGlyphUnits = 8.15F;
constexpr float kValueWidthFloorUnits = 40.0F;
constexpr std::array<std::size_t, kMenuRowCount> kWidestValueGlyphs{
    3U,
    11U,
    8U,
    12U,
    17U,
    10U,
    10U,
    8U,
    8U,
    18U,
    11U,
    3U};

[[nodiscard]] constexpr float value_width_units(
    const std::size_t row) noexcept
{
    const auto glyphs = row < kWidestValueGlyphs.size() ?
        kWidestValueGlyphs[row] : 12U;
    const auto width = static_cast<float>(glyphs) * kValueGlyphUnits;
    return width < kValueWidthFloorUnits ? kValueWidthFloorUnits : width;
}

static_assert(
    value_width_units(kFrameGenerationRow) < 60.0F,
    "the frame generation row shows at most Off or 6x, so its value box "
    "must be narrow or the left caret is stranded far from the text");
static_assert(
    value_width_units(kUpscalingRow) > 120.0F,
    "the upscaling quality row must still fit Ultra Performance");
static_assert(
    value_width_units(kDebugRow) > 130.0F,
    "the debug view row must still fit Motion (generator)");
static_assert(
    value_width_units(kDynamicMfgRow) > 80.0F,
    "the dynamic MFG row must still fit Unavailable");
static_assert(
    value_width_units(kOutputCapRow) > 60.0F,
    "the output cap row must still fit 1000 FPS and Type FPS");

constexpr std::size_t kSectionCount = 4;
constexpr std::size_t kMaxSectionRows = 4;

struct SectionDefinition final
{
    std::wstring_view name;
    std::wstring_view summary;
    std::array<std::size_t, kMaxSectionRows> rows;
    std::size_t row_count;
};

constexpr std::array<SectionDefinition, kSectionCount> kSections{
    SectionDefinition{
        L"Generation",
        L"Frames created between the ones Skyrim renders.",
        {kFrameGenerationRow, kBaseCapRow, kDynamicMfgRow, kOutputCapRow},
        4},
    SectionDefinition{
        L"Upscaling",
        L"Rendering below output resolution, then reconstructing.",
        {kProviderRow, kUpscalingRow, kSharpnessRow, 0},
        3},
    SectionDefinition{
        L"Latency",
        L"Keeping input response close to the rendered rate.",
        {kReflexRow, 0, 0, 0},
        1},
    SectionDefinition{
        L"Diagnostics",
        L"On-screen readouts and renderer debugging.",
        {kOverlayRow, kDebugRow, kMenuSoundsRow, 0},
        3}};

[[nodiscard]] constexpr std::size_t section_of_row(
    const std::size_t row) noexcept
{
    for (std::size_t section = 0; section < kSectionCount; ++section) {
        for (std::size_t index = 0;
             index < kSections[section].row_count;
             ++index) {
            if (kSections[section].rows[index] == row) {
                return section;
            }
        }
    }
    return 0;
}

[[nodiscard]] constexpr std::size_t row_slot_in_section(
    const std::size_t row) noexcept
{
    const auto section = section_of_row(row);
    for (std::size_t index = 0;
         index < kSections[section].row_count;
         ++index) {
        if (kSections[section].rows[index] == row) {
            return index;
        }
    }
    return 0;
}

constexpr std::array kFpsCounterChoices{
    config::OverlayFps::both,
    config::OverlayFps::output,
    config::OverlayFps::rendered};

[[nodiscard]] constexpr std::wstring_view fps_counter_name(
    const std::size_t index) noexcept
{
    switch (index) {
    case 1: return L"Both";
    case 2: return L"Output";
    case 3: return L"Rendered";
    default: break;
    }
    return L"Off";
}

constexpr std::array kDebugViewChoices{
    DebugView::off,
    DebugView::motion_vectors,
    DebugView::generator_motion,
    DebugView::depth,
    DebugView::generator_depth,
    DebugView::scene_reference,
    DebugView::ui_layer,
    DebugView::violations};

[[nodiscard]] constexpr std::wstring_view debug_view_name(
    const DebugView view) noexcept
{
    switch (view) {
    case DebugView::off: return L"Off";
    case DebugView::motion_vectors: return L"Motion";
    case DebugView::generator_motion: return L"Motion (generator)";
    case DebugView::depth: return L"Depth";
    case DebugView::generator_depth: return L"Depth (generator)";
    case DebugView::scene_reference: return L"Scene";
    case DebugView::ui_layer: return L"UI layer";
    case DebugView::violations: return L"Violations";
    }
    return L"Off";
}

[[nodiscard]] constexpr std::wstring_view debug_view_description(
    const DebugView view) noexcept
{
    switch (view) {
    case DebugView::off:
        return L"No debug view. Nothing is compiled, allocated or drawn.";
    case DebugView::motion_vectors:
        return L"What the UPSCALER consumes. Hue = direction (right red, down "
               L"green, left cyan, up blue). Grey = zero. White = past range.";
    case DebugView::generator_motion:
        return L"What FRAME GENERATION consumes - a different texture, same "
               L"colour rule. Compare against Motion; they should agree.";
    case DebugView::depth:
        return L"What the UPSCALER consumes. Warm/bright = near, deep blue = "
               L"far. Magenta = cleared depth / no geometry.";
    case DebugView::generator_depth:
        return L"What FRAME GENERATION consumes - a different texture, same "
               L"colour rule. Compare against Depth; they should agree.";
    case DebugView::scene_reference:
        return L"The exact colour the upscaler consumed, unmodified.";
    case DebugView::ui_layer:
        return L"The layer composited over the upscaled scene. Dark = "
               L"transparent, correct. Seeing the SCENE here is the defect.";
    case DebugView::violations:
        return L"Green = no oversized viewport or scissor was observed and "
               L"corrected. Red = one was; the reason and rectangle follow.";
    }
    return L"";
}

[[nodiscard]] std::wstring to_wide(const std::string_view text)
{
    std::wstring wide;
    wide.reserve(text.size());
    for (const auto character : text) {
        wide.push_back(static_cast<wchar_t>(
            static_cast<unsigned char>(character)));
    }
    return wide;
}

[[nodiscard]] std::wstring to_wide(const char* const text)
{
    return text == nullptr ?
        std::wstring{} : to_wide(std::string_view{text});
}
constexpr std::array kFrameGenerationChoices{0U, 2U, 3U, 4U, 6U};

struct ConflictingModule
{
    std::string_view module;
    std::string_view name;
    std::string_view collision;
    std::string_view extra;
};

[[nodiscard]] const std::vector<ConflictingModule>&
conflicting_injectors() noexcept
{
    static const std::vector<ConflictingModule> detected =
        []() -> std::vector<ConflictingModule> {
        constexpr std::array kKnown{
            ConflictingModule{
                "SkyrimUpscaler.dll",
                "Skyrim Upscaler - DLSS FSR2 XeSS",
                "both upscale",
                " If you also have PureDark's Patreon build of the same mod "
                "installed, disable that one as well; it is the same plugin "
                "with frame generation added and it conflicts the same way."},
            ConflictingModule{
                "PDPerfPlugin.dll",
                "Upscaler Base Plugin",
                "it is the hook Skyrim Upscaler upscales through",
                " If you also have PureDark's Patreon build installed, "
                "disable that one as well."},
            ConflictingModule{
                "ENBFrameGeneration.dll",
                "ENB Frame Generation",
                "both generate frames",
                ""},
            ConflictingModule{
                "ENBAntiAliasing.dll",
                "ENB Anti-Aliasing - AMD FSR 3.1 - NVIDIA DLAA",
                "both upscale and anti-alias",
                ""},
            ConflictingModule{
                "NVIDIA_Reflex.dll",
                "NVIDIA Reflex Support",
                "both drive Reflex",
                ""},
            ConflictingModule{
                "dlss-enabler.asi",
                "DLSS Enabler",
                "two hooks on one swap chain",
                ""},
            ConflictingModule{
                "dlss-enabler-upscaler.dll",
                "DLSS Enabler",
                "two hooks on one swap chain",
                ""},
            ConflictingModule{
                "OptiScaler.asi",
                "OptiScaler",
                "two hooks on one swap chain",
                ""},
            ConflictingModule{
                "OptiScaler.dll",
                "OptiScaler",
                "two hooks on one swap chain",
                ""},
            ConflictingModule{
                "dlssg_to_fsr3_amd_is_better.dll",
                "dlssg to FSR3",
                "two hooks on one swap chain",
                ""},
            ConflictingModule{
                "dlssg_to_fsr3.asi",
                "dlssg to FSR3",
                "two hooks on one swap chain",
                ""},
            ConflictingModule{
                "fakenvapi.dll",
                "fakenvapi",
                "both answer for NVAPI",
                ""}};
        std::vector<ConflictingModule> found;
        for (const auto& entry : kKnown) {
            if (GetModuleHandleA(std::string{entry.module}.c_str()) !=
                nullptr) {

                found.push_back(entry);
            }
        }
        return found;
    }();
    return detected;
}

struct PresentHookModule
{
    std::string_view module;
    std::string_view name;
};

[[nodiscard]] const std::vector<PresentHookModule>&
present_hook_modules() noexcept
{
    static const std::vector<PresentHookModule> detected =
        []() -> std::vector<PresentHookModule> {
        constexpr std::array kKnown{
            PresentHookModule{
                "gameoverlayrenderer64.dll", "Steam overlay"},
            PresentHookModule{
                "graphicshook64.dll", "OBS game capture"},
            PresentHookModule{
                "obs-vulkan64.dll", "OBS Vulkan capture"},
            PresentHookModule{
                "RTSSHooks64.dll",
                "RivaTuner Statistics Server, which ships with MSI "
                "Afterburner"},
            PresentHookModule{
                "EOSOVH-Win64-Shipping.dll", "Epic Online Services "
                "overlay"},
            PresentHookModule{
                "GameOverlayRenderer64.dll", "Steam overlay"},
            PresentHookModule{
                "DiscordHook64.dll", "Discord overlay"},
            PresentHookModule{
                "NvCameraWhitelisting64.dll",
                "the NVIDIA app overlay"}};
        std::vector<PresentHookModule> found;
        for (const auto& entry : kKnown) {
            if (GetModuleHandleA(std::string{entry.module}.c_str()) !=
                nullptr) {
                const auto duplicate = std::any_of(
                    found.begin(),
                    found.end(),
                    [&entry](const PresentHookModule& seen) {
                        return seen.name == entry.name;
                    });
                if (!duplicate) {
                    found.push_back(entry);
                }
            }
        }
        return found;
    }();
    return detected;
}

[[nodiscard]] bool frame_generation_available() noexcept
{
    if (RendererRuntime::instance().streamline_features_ready()) {
        return true;
    }
    if (XessFrameGeneration::selected()) {
        return XessFrameGeneration::instance().state() !=
            XessGenerationState::failed;
    }
    if (FsrFrameGeneration::selected()) {
        return FsrFrameGeneration::instance().state() !=
            FsrGenerationState::failed;
    }
    return false;
}

[[nodiscard]] std::uint32_t vendor_presented_per_frame() noexcept
{
    if (XessFrameGeneration::selected() &&
        XessFrameGeneration::instance().owns_presentation()) {
        return XessFrameGeneration::instance().presented_frame_count();
    }
    if (FsrFrameGeneration::selected() &&
        FsrFrameGeneration::instance().owns_presentation()) {

        const auto generated =
            FsrFrameGeneration::instance().generated_frames();
        return generated > 0U ? generated + 1U : 0U;
    }
    return 0U;
}

[[nodiscard]] bool low_latency_available() noexcept
{
    const auto& controller = providers::LowLatencyController::instance();
    if (controller.backend() == providers::LatencyBackend::nvidia_reflex) {
        return RendererRuntime::instance().streamline_features_ready();
    }
    return controller.available();
}

[[nodiscard]] bool dynamic_mfg_available() noexcept
{
    if (XessFrameGeneration::selected() || FsrFrameGeneration::selected()) {
        return false;
    }
    return RendererRuntime::instance().streamline_features_ready() &&
           streamline::FeatureSupport::instance().dynamic_mfg_supported();
}

void report_multiplier_ceiling(
    const char* const provider,
    const std::uint32_t ceiling) noexcept
{
    static const char* reported_provider = nullptr;
    static std::uint32_t reported_ceiling = 0U;
    if (reported_provider == provider && reported_ceiling == ceiling) {
        return;
    }
    reported_provider = provider;
    reported_ceiling = ceiling;
    if (ceiling >= 4U) {
        logger::info(
            "{} offers frame generation multipliers up to {}x, so the menu "
            "lists every multiplier this plugin supports",
            provider,
            ceiling);
        return;
    }
    logger::info(
        "{} offers frame generation multipliers up to {}x, so the menu lists "
        "nothing above that. This is the runtime's own ceiling rather than a "
        "limit of this plugin or of the graphics card: AMD's frame "
        "interpolation kernel takes no phase input and can only place a "
        "generated frame at the midpoint between two rendered ones. Selecting "
        "an NVIDIA or Intel frame generation provider, where the hardware "
        "allows it, is what raises the ceiling",
        provider,
        ceiling);
}

[[nodiscard]] std::size_t usable_frame_generation_choices() noexcept
{
    std::uint32_t ceiling = 0U;
    const char* provider = nullptr;
    if (FsrFrameGeneration::selected()) {
        provider = "AMD FidelityFX";
        ceiling = FsrFrameGeneration::maximum_multiplier();
    } else if (XessFrameGeneration::selected()) {
        provider = "Intel XeSS-FG";
        auto& xess = XessFrameGeneration::instance();
        ceiling = xess.maximum_multiplier();
        if (ceiling == 0U &&
            xess.state() != XessGenerationState::failed) {

            static_cast<void>(xess.probe_capability());
            ceiling = xess.maximum_multiplier();
        }
        if (ceiling == 0U) {
            ceiling = 2U;
        }
    }
    if (ceiling == 0U && streamline::FeatureSupport::instance().ready()) {
        provider = "NVIDIA DLSS Frame Generation";
        ceiling = streamline::FeatureSupport::instance().maximum_multiplier();
    }
    if (provider != nullptr && ceiling != 0U) {
        report_multiplier_ceiling(provider, ceiling);
    }
    return usable_choice_count(kFrameGenerationChoices, ceiling);
}
constexpr std::array kUpscalingProviders{
    providers::Vendor::nvidia,
    providers::Vendor::amd,
    providers::Vendor::intel};
constexpr std::array kUpscalingModes{
    config::UpscalingMode::off,
    config::UpscalingMode::dlaa,
    config::UpscalingMode::quality,
    config::UpscalingMode::balanced,
    config::UpscalingMode::performance,
    config::UpscalingMode::ultra_performance};
constexpr std::array kReflexModes{
    config::ReflexMode::off,
    config::ReflexMode::low_latency,
    config::ReflexMode::low_latency_boost};

struct MenuLayout
{
    bool compact{};
    float scale{};
    float corner_radius{};
    Rectangle screen;
    Rectangle panel;
    Rectangle header;
    Rectangle rail;
    Rectangle main;
    Rectangle detail;
    bool detail_visible{};
    std::array<Rectangle, kSectionCount> sections;
    Rectangle rail_footer;
    Rectangle section_title;
    Rectangle section_summary;
    std::array<Rectangle, kMenuRowCount> rows;
    std::array<Rectangle, kMenuRowCount> left_buttons;
    std::array<Rectangle, kMenuRowCount> right_buttons;
    std::array<Rectangle, kMenuRowCount> values;
    Rectangle footer;
    Rectangle status;
    Rectangle detail_kicker;
    Rectangle detail_title;
    Rectangle detail_body;
    Rectangle detail_options;
    Rectangle detail_effect;
    Rectangle detail_note;
    Rectangle debug_panel;
    Rectangle sharpness_track;
    Rectangle apply;
};

[[nodiscard]] D2D1_RECT_F to_d2d(const Rectangle& rectangle) noexcept
{
    return {
        rectangle.left,
        rectangle.top,
        rectangle.right,
        rectangle.bottom};
}

[[nodiscard]] D2D1_ROUNDED_RECT rounded(
    const Rectangle& rectangle,
    const float radius) noexcept
{
    return {to_d2d(rectangle), radius, radius};
}

[[nodiscard]] constexpr PanelAnchor anchor_for(
    const config::MenuPresentation presentation) noexcept
{
    switch (presentation) {
    case config::MenuPresentation::dock: return PanelAnchor::bottom;
    case config::MenuPresentation::drawer: return PanelAnchor::left;
    case config::MenuPresentation::card: return PanelAnchor::centre;
    case config::MenuPresentation::corner: return PanelAnchor::corner;
    case config::MenuPresentation::bar:
    case config::MenuPresentation::full: break;
    }
    return PanelAnchor::top;
}

[[nodiscard]] constexpr const wchar_t* presentation_name(
    const config::MenuPresentation presentation) noexcept
{
    switch (presentation) {
    case config::MenuPresentation::bar: return L"Top Bar";
    case config::MenuPresentation::dock: return L"Bottom Dock";
    case config::MenuPresentation::drawer: return L"Left Drawer";
    case config::MenuPresentation::card: return L"Centre Card";
    case config::MenuPresentation::corner: return L"Corner HUD";
    case config::MenuPresentation::full: break;
    }
    return L"Full Panel";
}

[[nodiscard]] MenuLayout make_menu_layout(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::size_t active_section = 0,
    const bool compact = false,
    const PanelAnchor anchor = PanelAnchor::top) noexcept
{
    const auto screen_width = static_cast<float>(width);
    const auto screen_height = static_cast<float>(height);
    const auto scale = (std::clamp)(
        screen_height / 720.0F,
        0.9F,
        4.0F);

    const auto content_width =
        (std::min)(screen_width, screen_height * 16.0F / 9.0F);
    const auto content_left = (screen_width - content_width) * 0.5F;

    auto rail_width = 206.0F * scale;
    auto detail_width = 336.0F * scale;
    const auto minimum_main = 470.0F * scale;
    const auto detail_visible =
        content_width - rail_width - detail_width >= minimum_main;
    if (!detail_visible) {
        detail_width = 0.0F;
    }
    if (content_width - rail_width < minimum_main) {
        rail_width = (std::max)(0.0F, content_width - minimum_main);
    }
    const auto main_width = content_width - rail_width - detail_width;

    MenuLayout layout{};
    layout.scale = scale;
    layout.corner_radius = 6.0F * scale;
    layout.detail_visible = detail_visible;
    layout.screen = {0.0F, 0.0F, screen_width, screen_height};
    layout.panel = {
        content_left,
        0.0F,
        content_left + content_width,
        screen_height};
    layout.rail = {
        content_left,
        0.0F,
        content_left + rail_width,
        screen_height};
    layout.main = {
        layout.rail.right,
        0.0F,
        layout.rail.right + main_width,
        screen_height};
    layout.detail = {
        layout.main.right,
        0.0F,
        layout.main.right + detail_width,
        screen_height};

    const auto rail_pad_left = 28.0F * scale;
    const auto rail_pad_right = 14.0F * scale;
    const auto top_pad = 30.0F * scale;
    layout.header = {
        layout.rail.left + rail_pad_left,
        top_pad,
        layout.rail.right - rail_pad_right,
        top_pad + 44.0F * scale};

    const auto section_height = 33.0F * scale;
    const auto section_top = layout.header.bottom + 24.0F * scale;
    for (std::size_t section = 0; section < kSectionCount; ++section) {
        const auto top =
            section_top + static_cast<float>(section) * section_height;
        layout.sections[section] = {
            layout.rail.left + rail_pad_left - 11.0F * scale,
            top,
            layout.rail.right - rail_pad_right,
            top + 30.0F * scale};
    }
    layout.rail_footer = {
        layout.rail.left + rail_pad_left,
        screen_height - 78.0F * scale,
        layout.rail.right - rail_pad_right,
        screen_height - 22.0F * scale};

    const auto main_pad = 30.0F * scale;
    const auto main_left = layout.main.left + main_pad;
    const auto main_right = layout.main.right - main_pad;
    layout.section_title = {
        main_left,
        top_pad,
        main_right,
        top_pad + 24.0F * scale};
    layout.section_summary = {
        main_left,
        layout.section_title.bottom + 4.0F * scale,
        main_right,
        layout.section_title.bottom + 22.0F * scale};

    const auto row_height = 43.0F * scale;
    const auto rows_top = layout.section_summary.bottom + 18.0F * scale;
    const auto arrow_width = 22.0F * scale;
    const auto& definition = kSections[
        (std::min)(active_section, kSectionCount - 1U)];
    for (std::size_t row = 0; row < kMenuRowCount; ++row) {
        layout.rows[row] = {};
        layout.left_buttons[row] = {};
        layout.values[row] = {};
        layout.right_buttons[row] = {};
    }
    for (std::size_t slot = 0; slot < definition.row_count; ++slot) {
        const auto row = definition.rows[slot];
        const auto top =
            rows_top + static_cast<float>(slot) * row_height;
        layout.rows[row] = {
            main_left,
            top,
            main_right,
            top + 40.0F * scale};
        const auto centre = top + 20.0F * scale;
        const auto content_right = main_right - 16.0F * scale;
        layout.right_buttons[row] = {
            content_right - arrow_width,
            centre - 12.0F * scale,
            content_right,
            centre + 12.0F * scale};
        layout.values[row] = {
            layout.right_buttons[row].left -
                value_width_units(row) * scale,
            centre - 12.0F * scale,
            layout.right_buttons[row].left,
            centre + 12.0F * scale};
        layout.left_buttons[row] = {
            layout.values[row].left - arrow_width,
            centre - 12.0F * scale,
            layout.values[row].left,
            centre + 12.0F * scale};
    }

    layout.footer = {
        main_left,
        screen_height - 62.0F * scale,
        main_right,
        screen_height - 26.0F * scale};
    const auto apply_width = 84.0F * scale;
    const auto apply_height = 26.0F * scale;
    const auto footer_centre =
        (layout.footer.top + layout.footer.bottom) * 0.5F;
    layout.apply = {
        main_right - apply_width,
        footer_centre - apply_height * 0.5F,
        main_right,
        footer_centre + apply_height * 0.5F};
    layout.status = {
        main_left + main_width * 0.45F,
        layout.footer.top,
        layout.apply.left - 24.0F * scale,
        layout.footer.bottom};

    const auto detail_pad = 26.0F * scale;
    const auto detail_left = layout.detail.left + detail_pad;
    const auto detail_right = layout.detail.right - 28.0F * scale;
    layout.detail_kicker = {
        detail_left,
        top_pad,
        detail_right,
        top_pad + 14.0F * scale};
    layout.detail_title = {
        detail_left,
        layout.detail_kicker.bottom + 9.0F * scale,
        detail_right,
        layout.detail_kicker.bottom + 31.0F * scale};
    layout.detail_body = {
        detail_left,
        layout.detail_title.bottom + 9.0F * scale,
        detail_right,
        layout.detail_title.bottom + 115.0F * scale};
    layout.detail_options = {
        detail_left,
        layout.detail_body.bottom + 12.0F * scale,
        detail_right,
        layout.detail_body.bottom + 132.0F * scale};
    layout.detail_effect = {
        detail_left,
        layout.detail_options.bottom + 12.0F * scale,
        detail_right,
        layout.detail_options.bottom + 116.0F * scale};
    layout.detail_note = {
        detail_left,
        screen_height - 84.0F * scale,
        detail_right,
        screen_height - 26.0F * scale};

    layout.sharpness_track = {
        layout.values[kSharpnessRow].left + 4.0F * scale,
        (layout.values[kSharpnessRow].top +
         layout.values[kSharpnessRow].bottom) * 0.5F -
            2.0F * scale,
        layout.values[kSharpnessRow].right - 34.0F * scale,
        (layout.values[kSharpnessRow].top +
         layout.values[kSharpnessRow].bottom) * 0.5F +
            2.0F * scale};
    layout.debug_panel = {
        main_left,
        rows_top + static_cast<float>(definition.row_count) * row_height +
            12.0F * scale,
        main_right,
        layout.footer.top - 14.0F * scale};

    if (compact) {
        layout.compact = true;
        layout.detail_visible = false;
        const auto rows = definition.row_count;
        const auto sheet = compute_panel_rect(
            anchor, screen_width, screen_height, scale, rows);
        const BarRect bar{
            sheet.left,
            sheet.top,
            sheet.right,
            sheet.top + compute_bar_height(scale)};
        const auto side = kBarSidePaddingUnits * scale;
        layout.panel = {sheet.left, sheet.top, sheet.right, sheet.bottom};
        layout.main = layout.panel;
        layout.rail = {};
        layout.detail = {};
        layout.rail_footer = {};
        layout.section_summary = {};
        layout.footer = {};
        layout.status = {};
        layout.detail_kicker = {};
        layout.detail_title = {};
        layout.detail_body = {};
        layout.detail_options = {};
        layout.detail_effect = {};
        layout.detail_note = {};
        for (auto& entry : layout.sections) {
            entry = {};
        }
        const auto bar_centre = (bar.top + bar.bottom) * 0.5F;
        layout.header = {
            sheet.left + side,
            bar_centre - 13.0F * scale,
            sheet.left + side + 92.0F * scale,
            bar_centre + 13.0F * scale};
        layout.section_title = {
            layout.header.right + 18.0F * scale,
            bar_centre - 12.0F * scale,
            sheet.right - side - 96.0F * scale,
            bar_centre + 12.0F * scale};
        const auto bar_apply_width = 84.0F * scale;
        const auto bar_apply_height = 26.0F * scale;
        layout.apply = {
            sheet.right - side - bar_apply_width,
            bar_centre - bar_apply_height * 0.5F,
            sheet.right - side,
            bar_centre + bar_apply_height * 0.5F};
        for (std::size_t slot = 0; slot < rows; ++slot) {
            const auto row = definition.rows[slot];
            const auto strip = compute_panel_row(
                anchor, screen_width, screen_height, scale, rows, slot);
            layout.rows[row] = {
                strip.left, strip.top, strip.right, strip.bottom};
            const auto centre = (strip.top + strip.bottom) * 0.5F;
            const auto columns = compute_row_columns(strip, scale);
            layout.right_buttons[row] = {
                columns.value_right,
                centre - 12.0F * scale,
                columns.right_arrow_right,
                centre + 12.0F * scale};
            layout.values[row] = {
                columns.value_left,
                centre - 12.0F * scale,
                columns.value_right,
                centre + 12.0F * scale};
            layout.left_buttons[row] = {
                columns.left_arrow_left,
                centre - 12.0F * scale,
                columns.value_left,
                centre + 12.0F * scale};
        }
    }
    return layout;
}

[[nodiscard]] constexpr std::wstring_view upscaling_mode_name(
    const config::UpscalingMode mode,
    const providers::Vendor provider) noexcept
{
    switch (mode) {
    case config::UpscalingMode::off: return L"Off";
    case config::UpscalingMode::dlaa:
        return provider == providers::Vendor::nvidia ?
            L"DLAA" : L"Native AA";
    case config::UpscalingMode::quality: return L"Quality";
    case config::UpscalingMode::balanced: return L"Balanced";
    case config::UpscalingMode::performance: return L"Performance";
    case config::UpscalingMode::ultra_performance:
        return L"Ultra Performance";
    }
    return L"Off";
}

[[nodiscard]] constexpr providers::QualityMode provider_quality(
    const config::UpscalingMode mode) noexcept
{
    switch (mode) {
    case config::UpscalingMode::dlaa:
        return providers::QualityMode::native_antialiasing;
    case config::UpscalingMode::quality:
        return providers::QualityMode::quality;
    case config::UpscalingMode::balanced:
        return providers::QualityMode::balanced;
    case config::UpscalingMode::performance:
        return providers::QualityMode::performance;
    case config::UpscalingMode::ultra_performance:
        return providers::QualityMode::ultra_performance;
    case config::UpscalingMode::off:
        return providers::QualityMode::off;
    }
    return providers::QualityMode::off;
}

[[nodiscard]] constexpr std::wstring_view upscaling_provider_name(
    const providers::Vendor provider) noexcept
{
    switch (provider) {
    case providers::Vendor::nvidia: return L"DLSS 4.5";
    case providers::Vendor::amd: return L"FSR 4.1";
    case providers::Vendor::intel: return L"XeSS-SR 2.0";
    case providers::Vendor::automatic: return L"Automatic";
    case providers::Vendor::none: return L"Off";
    }
    return L"Off";
}

[[nodiscard]] constexpr std::wstring_view reflex_mode_name(
    const config::ReflexMode mode) noexcept
{
    switch (mode) {
    case config::ReflexMode::off: return L"Off";
    case config::ReflexMode::low_latency: return L"On";
    case config::ReflexMode::low_latency_boost: return L"Boost";
    }
    return L"Off";
}

[[nodiscard]] constexpr providers::LatencyMode latency_mode_of(
    const config::ReflexMode mode) noexcept
{
    switch (mode) {
    case config::ReflexMode::off:
        return providers::LatencyMode::off;
    case config::ReflexMode::low_latency:
        return providers::LatencyMode::on;
    case config::ReflexMode::low_latency_boost:
        return providers::LatencyMode::boost;
    }
    return providers::LatencyMode::off;
}

[[nodiscard]] std::wstring low_latency_value(
    const config::ReflexMode reflex_mode)
{
    if (!low_latency_available()) {
        return L"Unavailable";
    }
    const auto backend =
        providers::LowLatencyController::instance().backend();
    if (backend == providers::LatencyBackend::nvidia_reflex) {
        return std::wstring{L"NVIDIA Reflex "} +
               std::wstring{reflex_mode_name(reflex_mode)};
    }
    return to_wide(providers::backend_display_name(backend)) + L" " +
           to_wide(providers::mode_display_name(
               backend, latency_mode_of(reflex_mode)));
}

[[nodiscard]] std::wstring fps_text(
    const float rendered,
    const float output,
    const config::OverlayFps display,
    const bool generation_enabled)
{
    const auto rendered_value =
        std::to_wstring(static_cast<int>(std::lround(rendered)));
    const auto output_value =
        std::to_wstring(static_cast<int>(std::lround(output)));
    switch (display) {
    case config::OverlayFps::output:

        return output_value +
               (generation_enabled ? L" FG FPS" : L" FPS");
    case config::OverlayFps::rendered:
        return rendered_value + L" FPS";
    case config::OverlayFps::both:
        break;
    }
    return rendered_value + L" / " + output_value;
}

[[nodiscard]] std::wstring percentage_text(
    const std::size_t step)
{
    return std::to_wstring(step * 5U) + L"%";
}

[[nodiscard]] std::wstring frame_limit_text(
    const std::uint32_t frame_limit)
{
    return frame_limit == 0U ?
               std::wstring{L"Off"} :
               std::to_wstring(frame_limit) + L" FPS";
}
}

struct StatusOverlay::State
{
    ComPtr<ID2D1Factory1> d2d_factory;
    ComPtr<ID2D1Device> d2d_device;
    ComPtr<ID2D1DeviceContext> d2d_context;
    ComPtr<IDWriteFactory> write_factory;
    ComPtr<ID2D1Bitmap1> target_bitmap;
    ComPtr<ID2D1Bitmap1> snapshot_bitmap;
    ComPtr<ID2D1Effect> blur_effect;
    ComPtr<ID2D1Layer> blur_clip_layer;
    ComPtr<ID2D1RoundedRectangleGeometry> blur_clip_geometry;
    ComPtr<ID2D1PathGeometry> cursor_geometry;
    ComPtr<ID3D11Texture2D> snapshot_texture;

    ComPtr<ID2D1SolidColorBrush> white_brush;
    ComPtr<ID2D1SolidColorBrush> muted_brush;
    ComPtr<ID2D1SolidColorBrush> accent_brush;
    ComPtr<ID2D1SolidColorBrush> accent_soft_brush;
    ComPtr<ID2D1SolidColorBrush> glass_brush;
    ComPtr<ID2D1SolidColorBrush> glass_border_brush;
    ComPtr<ID2D1SolidColorBrush> scrim_brush;
    ComPtr<ID2D1SolidColorBrush> row_brush;
    ComPtr<ID2D1SolidColorBrush> control_brush;
    ComPtr<ID2D1SolidColorBrush> control_hover_brush;
    ComPtr<ID2D1SolidColorBrush> cursor_shadow_brush;
    ComPtr<ID2D1SolidColorBrush> label_brush;
    ComPtr<ID2D1SolidColorBrush> value_brush;
    ComPtr<ID2D1SolidColorBrush> faint_brush;
    ComPtr<ID2D1SolidColorBrush> disabled_brush;
    ComPtr<ID2D1SolidColorBrush> separator_brush;
    ComPtr<ID2D1SolidColorBrush> warn_brush;
    ComPtr<ID2D1SolidColorBrush> warn_fill_brush;
    ComPtr<ID2D1SolidColorBrush> section_active_brush;

    ComPtr<IDWriteTextFormat> title_format;
    ComPtr<IDWriteTextFormat> label_format;
    ComPtr<IDWriteTextFormat> value_format;
    ComPtr<IDWriteTextFormat> detail_format;
    ComPtr<IDWriteTextFormat> detail_body_format;
    ComPtr<IDWriteTextFormat> button_format;
    ComPtr<IDWriteTextFormat> counter_format;
    ComPtr<IDWriteTextFormat> brand_format;
    ComPtr<IDWriteTextFormat> version_format;
    ComPtr<IDWriteTextFormat> section_format;
    ComPtr<IDWriteTextFormat> section_count_format;
    ComPtr<IDWriteTextFormat> heading_format;
    ComPtr<IDWriteTextFormat> summary_format;
    ComPtr<IDWriteTextFormat> kicker_format;
    ComPtr<IDWriteTextFormat> option_format;
    ComPtr<IDWriteTextFormat> option_value_format;
    ComPtr<IDWriteTextFormat> key_format;
    ComPtr<IDWriteTextFormat> apply_format;

    ID3D11Texture2D* target{};
    std::uint32_t target_width{};
    std::uint32_t target_height{};
    float format_scale{};

    ComPtr<ID2D1Bitmap1> ui_layer_bitmap;
    ID3D11Texture2D* ui_layer_target{};
    std::uint32_t ui_layer_width{};
    std::uint32_t ui_layer_height{};
    bool ui_layer_failure_logged{};

    std::chrono::steady_clock::time_point sample_start{
        std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point last_frame_sample{
        std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point selector_activity{};
    bool selector_dirty{};
    std::uint32_t sample_frames{};
    std::uint64_t last_presented_frames_total{};
    float rendered_fps{};
    float output_fps{};
    std::size_t selector_row{};
    bool keyboard_navigation{};
    std::size_t selector_section{};
    std::size_t animated_section_from{};
    std::chrono::steady_clock::time_point menu_opened_at{};
    static constexpr std::uint32_t kOverlayTextFormatCount = 18U;
    std::chrono::steady_clock::time_point menu_closed_at{};
    bool overlay_cost_reported{};
    [[nodiscard]] float closing_fade() const noexcept
    {
        if (selector_open ||
            menu_closed_at == std::chrono::steady_clock::time_point{}) {

            return 0.0F;
        }
        const auto elapsed =
            std::chrono::duration<float, std::milli>(
                std::chrono::steady_clock::now() - menu_closed_at).count();
        constexpr auto kCloseMilliseconds = 190.0F;
        if (elapsed >= kCloseMilliseconds) {
            return 0.0F;
        }
        return 1.0F - elapsed / kCloseMilliseconds;
    }
    [[nodiscard]] bool closing_active() const noexcept
    {
        return closing_fade() > 0.0F;
    }
    std::chrono::steady_clock::time_point section_changed_at{};
    std::array<std::chrono::steady_clock::time_point, kMenuRowCount>
        value_changed_at{};
    std::array<std::wstring, kMenuRowCount> previous_values{};
    diagnostics::FrameTimeSeries frame_series{};
    double series_clock_seconds{};

    std::size_t selector_debug_index{};

    ApplyOutcome apply_outcome{ApplyOutcome::none};
    std::wstring apply_message;
    int apply_frames_remaining{};

    providers::Vendor applied_provider{providers::Vendor::none};
    config::UpscalingMode applied_mode{config::UpscalingMode::off};
    std::size_t selector_frame_generation_index{};

    std::size_t selector_dynamic_mfg_index{};
    std::size_t selector_provider_index{};
    std::size_t selector_upscaling_index{};
    std::size_t selector_sharpness_step{};
    std::size_t selector_reflex_index{};
    std::uint32_t selector_base_frame_limit{};
    std::uint32_t selector_custom_base_frame_limit{40U};
    std::uint32_t selector_frame_limit{};
    std::uint32_t selector_custom_frame_limit{240U};
    std::size_t selector_overlay_index{};
    std::size_t selector_menu_sounds_index{};
    bool selector_open{};
    bool selector_base_frame_limit_custom{};
    bool selector_frame_limit_custom{};
    bool frame_limit_editing{};
    std::size_t frame_limit_editing_row{kOutputCapRow};
    std::string frame_limit_input;
    bool menu_key_was_down{};
    bool up_was_down{};
    bool tab_was_down{};
    bool space_was_down{};
    bool down_was_down{};
    bool left_was_down{};
    bool right_was_down{};
    bool enter_was_down{};
    bool escape_was_down{};
    bool backspace_was_down{};
    std::array<bool, 10> digit_was_down{};
    std::array<bool, 10> numpad_digit_was_down{};
    bool sharpness_dragging{};
    bool presented_total_initialized{};
    bool snapshot_valid{};
    bool first_draw_logged{};
    bool rendering_failure_logged{};
    std::atomic_bool input_capture{};
    std::atomic_int mouse_delta_x{};
    std::atomic_int mouse_delta_y{};
    std::atomic_bool mouse_click_pending{};
    std::array<Rectangle, kMenuRowCount> caret_left_rects{};
    std::atomic_bool mouse_button_down{};
    float cursor_x{};
    float cursor_y{};

    [[nodiscard]] bool initialize(
        ID3D11Device* device,
        ID3D11Texture2D* back_buffer);
    [[nodiscard]] bool create_device_resources(
        ID3D11Device* device);
    [[nodiscard]] bool create_target_resources(
        ID3D11Device* device,
        ID3D11Texture2D* back_buffer);
    [[nodiscard]] bool create_text_formats(float scale);
    void update_metrics();
    [[nodiscard]] bool apply_selector();
    void update_selector(std::uint32_t width, std::uint32_t height);
    void render(
        ID3D11DeviceContext* context,
        ID3D11Texture2D* back_buffer);
    [[nodiscard]] bool prepare_ui_layer_target(ID3D11Texture2D* ui_layer);
    void render_into_ui_layer(
        ID3D11DeviceContext* context,
        ID3D11Texture2D* ui_layer);
    void draw_menu(const MenuLayout& menu);
    void draw_counter(std::uint32_t width, std::uint32_t height);
    void draw_cursor(float scale);
    void release_target_resources() noexcept;
};

bool StatusOverlay::State::create_device_resources(
    ID3D11Device* device)
{
    if (d2d_context != nullptr) {
        return true;
    }

    D2D1_FACTORY_OPTIONS factory_options{};
    auto result = D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory1),
        &factory_options,
        reinterpret_cast<void**>(d2d_factory.GetAddressOf()));
    if (FAILED(result)) {
        logger::error(
            "Direct2D factory creation failed: 0x{:08X}",
            static_cast<unsigned>(result));
        return false;
    }

    ComPtr<IDXGIDevice> dxgi_device;
    result = device->QueryInterface(IID_PPV_ARGS(&dxgi_device));
    if (SUCCEEDED(result)) {
        result = d2d_factory->CreateDevice(
            dxgi_device.Get(),
            &d2d_device);
    }
    if (SUCCEEDED(result)) {
        result = d2d_device->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
            &d2d_context);
    }
    if (FAILED(result)) {
        logger::error(
            "Direct2D device initialization failed: 0x{:08X}",
            static_cast<unsigned>(result));
        return false;
    }

    result = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(
            write_factory.GetAddressOf()));
    if (FAILED(result)) {
        logger::error(
            "DirectWrite factory creation failed: 0x{:08X}",
            static_cast<unsigned>(result));
        return false;
    }

    const auto create_brush =
        [this](
            const D2D1_COLOR_F color,
            ComPtr<ID2D1SolidColorBrush>& brush) {
            return d2d_context->CreateSolidColorBrush(
                color,
                &brush);
        };
    result = create_brush(
        D2D1::ColorF(1.0F, 1.0F, 1.0F, 1.0F),
        white_brush);
    if (SUCCEEDED(result)) {
        result = create_brush(
            D2D1::ColorF(0.541F, 0.541F, 0.573F, 1.0F),
            muted_brush);
    }
    if (SUCCEEDED(result)) {
        result = create_brush(
            D2D1::ColorF(1.0F, 0.310F, 0.639F, 1.0F),
            accent_brush);
    }
    if (SUCCEEDED(result)) {
        result = create_brush(
            D2D1::ColorF(1.0F, 0.310F, 0.639F, 0.16F),
            accent_soft_brush);
    }
    if (SUCCEEDED(result)) {
        result = create_brush(
            D2D1::ColorF(0.035F, 0.039F, 0.047F, 0.80F),
            glass_brush);
    }
    if (SUCCEEDED(result)) {
        result = create_brush(
            D2D1::ColorF(1.0F, 1.0F, 1.0F, 0.07F),
            glass_border_brush);
    }
    if (SUCCEEDED(result)) {
        result = create_brush(
            D2D1::ColorF(0.005F, 0.009F, 0.016F, 0.14F),
            scrim_brush);
    }
    if (SUCCEEDED(result)) {
        result = create_brush(
            D2D1::ColorF(1.0F, 1.0F, 1.0F, 0.055F),
            row_brush);
    }
    if (SUCCEEDED(result)) {
        result = create_brush(
            D2D1::ColorF(1.0F, 1.0F, 1.0F, 0.0F),
            control_brush);
    }
    if (SUCCEEDED(result)) {
        result = create_brush(
            D2D1::ColorF(1.0F, 1.0F, 1.0F, 0.025F),
            control_hover_brush);
    }
    if (SUCCEEDED(result)) {
        result = create_brush(
            D2D1::ColorF(0.0F, 0.0F, 0.0F, 0.78F),
            cursor_shadow_brush);
    }
    if (SUCCEEDED(result)) {
        result = create_brush(
            D2D1::ColorF(0.847F, 0.847F, 0.867F, 1.0F),
            label_brush);
    }
    if (SUCCEEDED(result)) {
        result = create_brush(
            D2D1::ColorF(0.690F, 0.690F, 0.722F, 1.0F),
            value_brush);
    }
    if (SUCCEEDED(result)) {
        result = create_brush(
            D2D1::ColorF(0.361F, 0.361F, 0.400F, 1.0F),
            faint_brush);
    }
    if (SUCCEEDED(result)) {
        result = create_brush(
            D2D1::ColorF(0.329F, 0.329F, 0.369F, 1.0F),
            disabled_brush);
    }
    if (SUCCEEDED(result)) {
        result = create_brush(
            D2D1::ColorF(1.0F, 1.0F, 1.0F, 0.07F),
            separator_brush);
    }
    if (SUCCEEDED(result)) {
        result = create_brush(
            D2D1::ColorF(0.910F, 0.451F, 0.478F, 1.0F),
            warn_brush);
    }
    if (SUCCEEDED(result)) {
        result = create_brush(
            D2D1::ColorF(0.839F, 0.235F, 0.235F, 0.09F),
            warn_fill_brush);
    }
    if (SUCCEEDED(result)) {
        result = create_brush(
            D2D1::ColorF(1.0F, 1.0F, 1.0F, 0.06F),
            section_active_brush);
    }
    if (FAILED(result)) {
        logger::error(
            "Acrylic UI brush creation failed: 0x{:08X}",
            static_cast<unsigned>(result));
        return false;
    }

    result = d2d_factory->CreatePathGeometry(&cursor_geometry);
    if (SUCCEEDED(result)) {
        ComPtr<ID2D1GeometrySink> sink;
        result = cursor_geometry->Open(&sink);
        if (SUCCEEDED(result)) {
            sink->BeginFigure(
                D2D1::Point2F(0.0F, 0.0F),
                D2D1_FIGURE_BEGIN_FILLED);
            const std::array points{
                D2D1::Point2F(0.0F, 20.0F),
                D2D1::Point2F(5.0F, 15.0F),
                D2D1::Point2F(9.0F, 23.0F),
                D2D1::Point2F(13.0F, 21.0F),
                D2D1::Point2F(9.0F, 14.0F),
                D2D1::Point2F(16.0F, 14.0F)};
            sink->AddLines(
                points.data(),
                static_cast<UINT32>(points.size()));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            result = sink->Close();
        }
    }
    if (FAILED(result)) {
        logger::error(
            "Acrylic UI cursor geometry creation failed: 0x{:08X}",
            static_cast<unsigned>(result));
        return false;
    }
    return true;
}

bool StatusOverlay::State::create_text_formats(const float scale)
{
    if (write_factory == nullptr) {
        return false;
    }
    if (title_format != nullptr &&
        std::abs(format_scale - scale) < 0.01F) {
        return true;
    }

    title_format.Reset();
    label_format.Reset();
    value_format.Reset();
    detail_format.Reset();
    detail_body_format.Reset();
    button_format.Reset();
    counter_format.Reset();
    brand_format.Reset();
    version_format.Reset();
    section_format.Reset();
    section_count_format.Reset();
    heading_format.Reset();
    summary_format.Reset();
    kicker_format.Reset();
    option_format.Reset();
    option_value_format.Reset();
    key_format.Reset();
    apply_format.Reset();

    const auto create_format =
        [this, scale](
            const float size,
            const DWRITE_FONT_WEIGHT weight,
            const DWRITE_TEXT_ALIGNMENT alignment,
            ComPtr<IDWriteTextFormat>& format) {
            auto result = write_factory->CreateTextFormat(
                L"Segoe UI Variable",
                nullptr,
                weight,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                size * scale,
                L"en-us",
                &format);
            if (SUCCEEDED(result)) {
                result = format->SetTextAlignment(alignment);
            }
            if (SUCCEEDED(result)) {
                result = format->SetParagraphAlignment(
                    DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
            if (SUCCEEDED(result)) {
                result = format->SetWordWrapping(
                    DWRITE_WORD_WRAPPING_NO_WRAP);
            }
            return result;
        };

    const auto create_mono =
        [this, scale](
            const float size,
            const DWRITE_FONT_WEIGHT weight,
            const DWRITE_TEXT_ALIGNMENT alignment,
            ComPtr<IDWriteTextFormat>& format) {
            auto created = write_factory->CreateTextFormat(
                L"Consolas",
                nullptr,
                weight,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                size * scale,
                L"en-us",
                &format);
            if (SUCCEEDED(created)) {
                created = format->SetTextAlignment(alignment);
            }
            if (SUCCEEDED(created)) {
                created = format->SetParagraphAlignment(
                    DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
            if (SUCCEEDED(created)) {
                created = format->SetWordWrapping(
                    DWRITE_WORD_WRAPPING_NO_WRAP);
            }
            return created;
        };

    auto result = create_format(
        28.0F,
        DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_TEXT_ALIGNMENT_LEADING,
        title_format);
    if (SUCCEEDED(result)) {
        result = create_format(
            13.5F,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_TEXT_ALIGNMENT_LEADING,
            label_format);
    }
    if (SUCCEEDED(result)) {
        result = create_mono(
            13.5F,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_TEXT_ALIGNMENT_TRAILING,
            value_format);
    }
    if (SUCCEEDED(result)) {
        result = create_mono(
            11.0F,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_TEXT_ALIGNMENT_CENTER,
            button_format);
    }
    if (SUCCEEDED(result)) {
        result = create_format(
            12.5F,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_TEXT_ALIGNMENT_LEADING,
            detail_format);
        if (SUCCEEDED(result) && detail_format != nullptr) {
            result = detail_format->SetWordWrapping(
                DWRITE_WORD_WRAPPING_WRAP);
        }
    }
    if (SUCCEEDED(result)) {
        result = create_format(
            12.5F,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_TEXT_ALIGNMENT_LEADING,
            detail_body_format);
        if (SUCCEEDED(result) && detail_body_format != nullptr) {
            result = detail_body_format->SetWordWrapping(
                DWRITE_WORD_WRAPPING_WRAP);
            if (SUCCEEDED(result)) {
                result = detail_body_format->SetParagraphAlignment(
                    DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }
        }
    }
    if (SUCCEEDED(result)) {
        result = create_format(
            18.0F,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_TEXT_ALIGNMENT_TRAILING,
            counter_format);
    }

    if (SUCCEEDED(result)) {
        result = create_format(
            17.0F,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_TEXT_ALIGNMENT_LEADING,
            brand_format);
    }
    if (SUCCEEDED(result)) {
        result = create_mono(
            11.0F,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_TEXT_ALIGNMENT_LEADING,
            version_format);
    }
    if (SUCCEEDED(result)) {
        result = create_format(
            13.0F,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_TEXT_ALIGNMENT_LEADING,
            section_format);
    }
    if (SUCCEEDED(result)) {
        result = create_mono(
            10.5F,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_TEXT_ALIGNMENT_TRAILING,
            section_count_format);
    }
    if (SUCCEEDED(result)) {
        result = create_format(
            17.0F,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_TEXT_ALIGNMENT_LEADING,
            heading_format);
    }
    if (SUCCEEDED(result)) {
        result = create_format(
            12.5F,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_TEXT_ALIGNMENT_LEADING,
            summary_format);
    }
    if (SUCCEEDED(result)) {
        result = create_format(
            11.0F,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_TEXT_ALIGNMENT_LEADING,
            kicker_format);
    }
    if (SUCCEEDED(result)) {
        result = create_mono(
            12.5F,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_TEXT_ALIGNMENT_LEADING,
            option_format);
    }
    if (SUCCEEDED(result)) {
        result = create_mono(
            12.5F,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_TEXT_ALIGNMENT_TRAILING,
            option_value_format);
    }
    if (SUCCEEDED(result)) {
        result = create_format(
            11.5F,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_TEXT_ALIGNMENT_LEADING,
            key_format);
    }
    if (SUCCEEDED(result)) {
        result = create_format(
            12.5F,
            DWRITE_FONT_WEIGHT_MEDIUM,
            DWRITE_TEXT_ALIGNMENT_CENTER,
            apply_format);
    }
    if (FAILED(result)) {
        logger::error(
            "Segoe UI DirectWrite format creation failed: 0x{:08X}",
            static_cast<unsigned>(result));
        return false;
    }
    format_scale = scale;
    return true;
}

void StatusOverlay::State::release_target_resources() noexcept
{
    if (d2d_context != nullptr) {
        d2d_context->SetTarget(nullptr);
    }
    if (blur_effect != nullptr) {
        blur_effect->SetInput(0, nullptr);
    }
    blur_effect.Reset();
    blur_clip_layer.Reset();
    blur_clip_geometry.Reset();
    snapshot_bitmap.Reset();
    target_bitmap.Reset();
    snapshot_texture.Reset();
    snapshot_valid = false;
    target = nullptr;
    target_width = 0;
    target_height = 0;
}

bool StatusOverlay::State::create_target_resources(
    ID3D11Device* device,
    ID3D11Texture2D* back_buffer)
{
    D3D11_TEXTURE2D_DESC description{};
    back_buffer->GetDesc(&description);
    if (target == back_buffer &&
        target_width == description.Width &&
        target_height == description.Height &&
        target_bitmap != nullptr &&
        snapshot_bitmap != nullptr) {
        return create_text_formats(
            make_menu_layout(
                description.Width,
                description.Height).scale);
    }
    release_target_resources();

    ComPtr<IDXGISurface> target_surface;
    auto result =
        back_buffer->QueryInterface(IID_PPV_ARGS(&target_surface));
    if (FAILED(result)) {
        logger::error(
            "Acrylic UI target is not a DXGI surface: 0x{:08X}",
            static_cast<unsigned>(result));
        return false;
    }
    const auto pixel_format = D2D1::PixelFormat(
        description.Format,
        D2D1_ALPHA_MODE_IGNORE);
    const auto target_properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET |
            D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        pixel_format,
        96.0F,
        96.0F);
    result = d2d_context->CreateBitmapFromDxgiSurface(
        target_surface.Get(),
        &target_properties,
        &target_bitmap);
    if (FAILED(result)) {
        logger::error(
            "Acrylic UI target bitmap creation failed: 0x{:08X}",
            static_cast<unsigned>(result));
        return false;
    }

    auto snapshot_description = description;
    snapshot_description.Usage = D3D11_USAGE_DEFAULT;
    snapshot_description.BindFlags =
        D3D11_BIND_SHADER_RESOURCE |
        D3D11_BIND_RENDER_TARGET;
    snapshot_description.CPUAccessFlags = 0;
    snapshot_description.MiscFlags = 0;
    result = device->CreateTexture2D(
        &snapshot_description,
        nullptr,
        &snapshot_texture);
    if (FAILED(result)) {
        logger::error(
            "Acrylic scene snapshot creation failed: 0x{:08X}",
            static_cast<unsigned>(result));
        return false;
    }

    ComPtr<IDXGISurface> snapshot_surface;
    result = snapshot_texture.As(&snapshot_surface);
    if (SUCCEEDED(result)) {
        const auto snapshot_properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            pixel_format,
            96.0F,
            96.0F);
        result = d2d_context->CreateBitmapFromDxgiSurface(
            snapshot_surface.Get(),
            &snapshot_properties,
            &snapshot_bitmap);
    }
    if (SUCCEEDED(result)) {
        result = d2d_context->CreateEffect(
            CLSID_D2D1GaussianBlur,
            &blur_effect);
    }
    const auto menu = make_menu_layout(
        description.Width,
        description.Height,
        0,
        false);
    if (SUCCEEDED(result)) {
        result = d2d_context->CreateLayer(&blur_clip_layer);
    }
    if (SUCCEEDED(result)) {
        result = d2d_factory->CreateRoundedRectangleGeometry(
            rounded(menu.panel, menu.corner_radius),
            &blur_clip_geometry);
    }
    if (FAILED(result)) {
        logger::error(
            "Acrylic blur resource creation failed: 0x{:08X}",
            static_cast<unsigned>(result));
        return false;
    }
    blur_effect->SetInput(0, snapshot_bitmap.Get());
    static_cast<void>(blur_effect->SetValue(
        D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION,
        D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED));
    static_cast<void>(blur_effect->SetValue(
        D2D1_GAUSSIANBLUR_PROP_BORDER_MODE,
        D2D1_BORDER_MODE_HARD));

    target = back_buffer;
    target_width = description.Width;
    target_height = description.Height;
    d2d_context->SetTarget(target_bitmap.Get());
    return create_text_formats(
        make_menu_layout(
            description.Width,
            description.Height).scale);
}

bool StatusOverlay::State::initialize(
    ID3D11Device* device,
    ID3D11Texture2D* back_buffer)
{
    static auto injector_checked = false;
    if (!injector_checked) {
        injector_checked = true;
        if (present_hook_modules().empty()) {
            logger::info(
                "No overlay or capture software is hooking Present in "
                "this process, so frame pacing measurements from this "
                "session are clean");
        } else {
            for (const auto& entry : present_hook_modules()) {
                logger::warn(
                    "PRESENT HOOK DETECTED: {} ({}). This is NOT a "
                    "conflict and the mod works normally, but it costs "
                    "time on EVERY PRESENTED frame, so at a multiplier "
                    "of N it is paid N times per rendered frame. A "
                    "constant per-present overhead therefore reads as a "
                    "multiplier-specific stutter that looks like a "
                    "generator defect. Close it before reporting any "
                    "pacing problem, and before trusting any Present "
                    "timing in this log",
                    entry.name,
                    entry.module);
            }
        }
        if (conflicting_injectors().empty()) {
            logger::info(
                "No conflicting upscaling, frame generation or latency mod is "
                "loaded in this process");
        } else {
            for (const auto& entry : conflicting_injectors()) {
                logger::warn(
                    "Conflicting mod detected: {} ({}), because {}. This is "
                    "not a compatibility gap that will be closed: it owns the "
                    "same hook UFGU owns, so disable one of the two before "
                    "treating any UFGU behaviour as a defect.{}",
                    entry.name,
                    entry.module,
                    entry.collision,
                    entry.extra);
            }
        }
    }
    if (device == nullptr || back_buffer == nullptr) {
        return false;
    }

    const auto before_device = std::chrono::steady_clock::now();
    const auto device_ready = create_device_resources(device);
    const auto before_target = std::chrono::steady_clock::now();
    const auto target_ready =
        device_ready && create_target_resources(device, back_buffer);
    const auto after_target = std::chrono::steady_clock::now();

    const auto milliseconds = [](
        const std::chrono::steady_clock::time_point from,
        const std::chrono::steady_clock::time_point to) {
        return std::chrono::duration<double, std::milli>(to - from).count();
    };
    const auto device_ms = milliseconds(before_device, before_target);
    const auto target_ms = milliseconds(before_target, after_target);
    if ((device_ms + target_ms) >= 4.0 &&
        !overlay_cost_reported) {
        overlay_cost_reported = true;
        logger::info(
            "Overlay lazy initialisation cost {:.2f}ms this frame: Direct2D "
            "and DirectWrite factories and brushes {:.2f}ms, render target, "
            "snapshot bitmap, blur effect and {} text formats {:.2f}ms. This "
            "is the hitch seen on the first menu open and on any resolution "
            "change, and it is measured here rather than guessed at so the "
            "expensive half is known before anything is pre-warmed",
            device_ms + target_ms,
            device_ms,
            kOverlayTextFormatCount,
            target_ms);
    }
    return target_ready;
}

void StatusOverlay::State::update_metrics()
{
    const auto now = std::chrono::steady_clock::now();
    const auto& frame_generation =
        streamline::FrameGeneration::instance();
    const auto presented_frames_total =
        frame_generation.presented_frames_total();
    if (!presented_total_initialized ||
        presented_frames_total < last_presented_frames_total) {
        last_presented_frames_total = presented_frames_total;
        presented_total_initialized = true;
    }
    const auto frame_seconds =
        std::chrono::duration<float>(now - last_frame_sample).count();
    last_frame_sample = now;
    if (frame_seconds > 0.0F && frame_seconds < 1.0F) {
        series_clock_seconds += static_cast<double>(frame_seconds);
        frame_series.push(frame_seconds * 1000.0F, series_clock_seconds);
    }
    ++sample_frames;
    const auto elapsed =
        std::chrono::duration<float>(now - sample_start).count();
    if (elapsed < 0.5F) {
        return;
    }

    const auto average_fps =
        static_cast<float>(sample_frames) / elapsed;
    rendered_fps = average_fps;
    const auto presented_delta =
        presented_frames_total - last_presented_frames_total;

    const auto vendor_multiplier = vendor_presented_per_frame();
    if (vendor_multiplier > 1U) {
        output_fps = rendered_fps * static_cast<float>(vendor_multiplier);
    } else {
        output_fps =
            frame_generation.enabled() && presented_delta != 0U ?
                static_cast<float>(presented_delta) / elapsed :
                rendered_fps;
    }
    last_presented_frames_total = presented_frames_total;
    sample_start = now;
    sample_frames = 0;
}

bool StatusOverlay::State::apply_selector()
{
    auto& settings = config::Settings::instance();

    const auto streamline_available =
        frame_generation_available();
    const auto streamline_runtime =
        RendererRuntime::instance().streamline_features_ready();
    const auto frame_generation =
        kFrameGenerationChoices[selector_frame_generation_index];
    const auto frame_generation_enabled = frame_generation != 0U;
    const auto provider =
        kUpscalingProviders[selector_provider_index];
    const auto mode = kUpscalingModes[selector_upscaling_index];
    const auto sharpness =
        static_cast<float>(selector_sharpness_step) * 0.05F;
    const auto reflex = kReflexModes[selector_reflex_index];
    const auto base_frame_limit = selector_base_frame_limit_custom ?
        selector_custom_base_frame_limit : selector_base_frame_limit;
    const auto frame_limit = selector_frame_limit_custom ?
        selector_custom_frame_limit : selector_frame_limit;
    const auto overlay_enabled = selector_overlay_index != 0U;
    const auto menu_sounds_enabled = selector_menu_sounds_index != 0U;

    const auto overlay_fps_display =
        overlay_enabled ?
            kFpsCounterChoices[selector_overlay_index - 1U] :
            settings.overlay_fps_display();

    const auto previous_frame_generation_enabled =
        settings.frame_generation_enabled();
    const auto previous_provider = settings.upscaling_provider();
    const auto previous_mode = settings.upscaling_mode();
    const auto previous_multiplier =
        settings.frame_generation_multiplier();

    const auto multiplier_mode =
        dynamic_mfg_available() ?
            (selector_dynamic_mfg_index != 0U ?
                 config::MultiplierMode::dynamic :
                 config::MultiplierMode::fixed) :
            settings.multiplier_mode();
    const auto previous_multiplier_mode = settings.multiplier_mode();
    const auto previous_sharpness = settings.sharpness();
    const auto previous_reflex = settings.reflex_mode();
    const auto previous_base_frame_limit = settings.base_frame_limit();
    const auto previous_frame_limit = settings.frame_limit();
    const auto previous_overlay = settings.overlay_enabled();
    const auto previous_overlay_fps = settings.overlay_fps_display();

    const auto previous_debug_view = DebugViewPass::selected_view();
    const auto requested_debug_view =
        kDebugViewChoices[selector_debug_index];

    const auto previous_runtime_mode =
        streamline::SuperResolution::instance().mode();
    auto& frame_generation_runtime =
        streamline::FrameGeneration::instance();

    const auto restore_everything = [&]() {
        auto restored = settings.set_upscaling_provider(previous_provider);
        restored = settings.set_upscaling_mode(previous_mode) && restored;
        restored = settings.set_reflex_mode(previous_reflex) && restored;
        restored =
            settings.set_base_frame_limit(previous_base_frame_limit) &&
            restored;
        restored = settings.set_frame_limit(previous_frame_limit) && restored;
        restored = settings.set_sharpness(previous_sharpness) && restored;
        restored = settings.set_overlay_enabled(previous_overlay) && restored;
        restored =
            settings.set_overlay_fps_display(previous_overlay_fps) && restored;
        restored =
            settings.set_frame_generation_multiplier(previous_multiplier) &&
            restored;
        restored =
            settings.set_multiplier_mode(previous_multiplier_mode) && restored;
        restored =
            settings.set_frame_generation_enabled(
                previous_frame_generation_enabled) &&
            restored;
        if (streamline_runtime) {
            restored =
                frame_generation_runtime.set_reflex_mode(previous_reflex) &&
                restored;
            restored =
                frame_generation_runtime.set_output_target_fps(
                    previous_frame_limit) &&
                restored;
        }
        DebugViewPass::select(previous_debug_view);
        return restored;
    };

    const auto persist_everything = [&]() {
        const auto latency_persisted =
            !low_latency_available() || settings.set_reflex_mode(reflex);
        const auto common_persisted =
            settings.set_upscaling_provider(provider) &&
            settings.set_upscaling_mode(mode) &&
            settings.set_sharpness(sharpness) &&
            settings.set_base_frame_limit(base_frame_limit) &&
            settings.set_overlay_enabled(overlay_enabled) &&
            settings.set_overlay_fps_display(overlay_fps_display) &&
            settings.set_menu_sounds_enabled(menu_sounds_enabled) &&
            latency_persisted;
        if (!common_persisted || !streamline_available) {
            return common_persisted;
        }
        return settings.set_frame_limit(frame_limit) &&
               (!frame_generation_enabled ||
                (settings.set_frame_generation_multiplier(frame_generation) &&
                 settings.set_multiplier_mode(multiplier_mode))) &&
               settings.set_frame_generation_enabled(
                   frame_generation_enabled);
    };

    const auto latched_provider =
        streamline::SuperResolution::instance().provider();
    const auto active_provider =
        latched_provider == providers::Vendor::none ?
            previous_provider : latched_provider;

    const auto stage_restart_selection = [&]() {
        if (streamline_runtime &&
            (!frame_generation_runtime.set_reflex_mode(reflex) ||
             !frame_generation_runtime.set_output_target_fps(frame_limit))) {
            restore_everything();
            logger::error(
                "Could not apply the independent runtime settings while "
                "staging an upscaling restart");
            return false;
        }
        DebugViewPass::select(requested_debug_view);

        const auto latency_staged =
            !low_latency_available() || settings.set_reflex_mode(reflex);
        auto persisted_non_provider =
            settings.set_sharpness(sharpness) &&
            settings.set_base_frame_limit(base_frame_limit) &&
            settings.set_overlay_enabled(overlay_enabled) &&
            settings.set_overlay_fps_display(overlay_fps_display) &&
            latency_staged;
        if (persisted_non_provider && streamline_available) {
            persisted_non_provider =
                settings.set_frame_limit(frame_limit) &&
                (!frame_generation_enabled ||
                 (settings.set_frame_generation_multiplier(frame_generation) &&
                  settings.set_multiplier_mode(multiplier_mode))) &&
                settings.set_frame_generation_enabled(
                    frame_generation_enabled);
        }
        const auto staged =
            persisted_non_provider &&
            settings.stage_upscaling_for_restart(provider, mode);
        if (!staged) {
            auto restored = settings.stage_upscaling_for_restart(
                previous_provider, previous_mode);
            restored = restore_everything() && restored;
            if (restored) {
                logger::error(
                    "Could not stage the requested upscaling "
                    "provider/profile; the active and persisted settings "
                    "were restored");
            } else {
                logger::error(
                    "Could not stage the requested upscaling "
                    "provider/profile, and restoring the previous settings "
                    "also failed. UFGU.ini may not match the running state; "
                    "check that it is writable");
            }
            return false;
        }

        apply_outcome = ApplyOutcome::staged_for_restart;
        apply_message =
            std::wstring{upscaling_provider_name(provider)} + L" " +
            std::wstring{upscaling_mode_name(mode, provider)} +
            L" is saved for the next Skyrim launch. Independent controls "
            L"were applied; the current renderer remains " +
            std::wstring{upscaling_provider_name(active_provider)} + L" " +
            std::wstring{upscaling_mode_name(
                previous_runtime_mode, active_provider)} + L".";
        apply_frames_remaining = 600;
        snapshot_valid = false;
        logger::info(
            "Apply outcome {}: staged {} {} for the next Skyrim launch "
            "without mutating the active {} renderer; independent controls "
            "were applied",
            outcome_name(apply_outcome),
            providers::vendor_display_name(provider),
            static_cast<std::uint32_t>(mode),
            providers::vendor_display_name(active_provider));
        return true;
    };

    if (provider != active_provider) {

        if (mode != config::UpscalingMode::off) {
            const auto output_width =
                PresentationBridge::instance().output_width();
            const auto output_height =
                PresentationBridge::instance().output_height();
            std::uint32_t render_width{};
            std::uint32_t render_height{};
            std::string provider_failure;
            auto provider_ready = output_width != 0U && output_height != 0U;
            if (!provider_ready) {
                provider_failure =
                    "the physical output extent is not available";
            } else if (provider == providers::Vendor::amd) {
                auto& fsr = providers::FsrUpscaler::instance();
                provider_ready = fsr.query_render_resolution(
                    provider_quality(mode),
                    output_width,
                    output_height,
                    render_width,
                    render_height);
                if (!provider_ready) {
                    provider_failure = fsr.detail();
                }
            } else if (provider == providers::Vendor::intel) {
                auto& xess = providers::XessUpscaler::instance();
                provider_ready = xess.query_render_resolution(
                    provider_quality(mode),
                    output_width,
                    output_height,
                    render_width,
                    render_height);
                if (!provider_ready) {
                    provider_failure = xess.detail();
                }
            } else if (provider == providers::Vendor::nvidia) {
                constexpr std::uint32_t kNvidiaVendorId = 0x10DEU;
                provider_ready =
                    D3D12Backend::instance().adapter_vendor_id() ==
                    kNvidiaVendorId;
                if (!provider_ready) {
                    provider_failure =
                        "the active display adapter is not NVIDIA";
                }
            } else {
                provider_ready = false;
                provider_failure = "the requested provider is invalid";
            }

            if (!provider_ready) {

                apply_outcome = ApplyOutcome::refused;
                apply_message =
                    std::wstring{upscaling_provider_name(provider)} +
                    L" was not saved: " +
                    to_wide(provider_failure.empty() ?
                        "the provider runtime rejected the request" :
                        provider_failure);
                apply_frames_remaining = 600;
                const auto active_selection = std::ranges::find(
                    kUpscalingProviders, active_provider);
                if (active_selection != kUpscalingProviders.end()) {
                    selector_provider_index = static_cast<std::size_t>(
                        std::distance(
                            kUpscalingProviders.begin(), active_selection));
                }
                snapshot_valid = false;
                logger::warn(
                    "The in-game graphics menu refused {} {} before "
                    "changing anything: {}",
                    providers::vendor_display_name(provider),
                    static_cast<std::uint32_t>(mode),
                    provider_failure.empty() ?
                        "provider runtime rejection" : provider_failure);
                return false;
            }

            if (provider == providers::Vendor::nvidia) {
                logger::info(
                    "Provider restart preflight accepted NVIDIA on adapter "
                    "vendor 0x10DE; NGX feature creation remains a startup "
                    "validation");
            } else {
                logger::info(
                    "Provider restart preflight accepted {} {}: {}x{} -> "
                    "{}x{}",
                    providers::vendor_display_name(provider),
                    static_cast<std::uint32_t>(mode),
                    render_width,
                    render_height,
                    output_width,
                    output_height);
            }
        }

        if (mode == config::UpscalingMode::off) {
            static_cast<void>(stage_restart_selection());
            return false;
        }

        if (streamline_runtime &&
            (!frame_generation_runtime.set_reflex_mode(reflex) ||
             !frame_generation_runtime.set_output_target_fps(frame_limit))) {
            restore_everything();
            apply_outcome = ApplyOutcome::failed;
            apply_message =
                L"The provider was not changed because an independent "
                L"runtime control could not be applied.";
            apply_frames_remaining = 600;
            return false;
        }

        const auto latency_independent =
            !low_latency_available() || settings.set_reflex_mode(reflex);
        auto persisted_independent =
            settings.set_sharpness(sharpness) &&
            settings.set_base_frame_limit(base_frame_limit) &&
            settings.set_overlay_enabled(overlay_enabled) &&
            settings.set_overlay_fps_display(overlay_fps_display) &&
            latency_independent;
        if (persisted_independent && streamline_available) {
            persisted_independent =
                settings.set_frame_limit(frame_limit) &&
                (!frame_generation_enabled ||
                 (settings.set_frame_generation_multiplier(frame_generation) &&
                  settings.set_multiplier_mode(multiplier_mode))) &&
                settings.set_frame_generation_enabled(
                    frame_generation_enabled);
        }
        if (!persisted_independent) {
            restore_everything();
            apply_outcome = ApplyOutcome::failed;
            apply_message =
                L"The provider was not changed because the independent "
                L"controls could not be persisted.";
            apply_frames_remaining = 600;
            return false;
        }

        bool restart_required{};
        std::string switch_detail;
        auto& upscaling_pass = UpscalingPass::instance();
        if (upscaling_pass.request_same_extent_provider_switch(
                provider, mode, restart_required, switch_detail)) {
            DebugViewPass::select(requested_debug_view);
            apply_outcome = ApplyOutcome::committed;
            applied_provider = provider;
            applied_mode = mode;

            apply_message =
                std::wstring{upscaling_provider_name(provider)} + L" " +
                std::wstring{upscaling_mode_name(mode, provider)} +
                L" is now rendering.";
            apply_frames_remaining = 600;
            sharpness_dragging = false;
            logger::info(
                "Apply outcome {}: live same-extent provider switch to {} {} "
                "applied and persisted",
                outcome_name(apply_outcome),
                providers::vendor_display_name(provider),
                static_cast<std::uint32_t>(mode));
            return true;
        }

        if (restart_required) {
            static_cast<void>(stage_restart_selection());
            return false;
        }

        apply_outcome = ApplyOutcome::refused;
        apply_message =
            std::wstring{upscaling_provider_name(provider)} +
            L" could not begin live validation: " +
            to_wide(switch_detail.empty() ?
                "the target runtime rejected the request" : switch_detail);
        apply_frames_remaining = 600;
        const auto active_selection = std::ranges::find(
            kUpscalingProviders, active_provider);
        if (active_selection != kUpscalingProviders.end()) {
            selector_provider_index = static_cast<std::size_t>(
                std::distance(
                    kUpscalingProviders.begin(), active_selection));
        }
        snapshot_valid = false;
        logger::warn(
            "Live same-extent provider switch refused without mutation: {}",
            switch_detail.empty() ? "target runtime rejection" :
                                    switch_detail);
        return false;
    }

    std::uint32_t requested_width{};
    std::uint32_t requested_height{};
    const auto requested_extent_resolved =
        streamline::SuperResolution::instance().optimal_render_resolution(
            mode, requested_width, requested_height);
    const auto& bridge = PresentationBridge::instance();
    const auto extent_changes =
        requested_extent_resolved &&
        (requested_width != bridge.render_width() ||
         requested_height != bridge.render_height());
    if (extent_changes) {
        logger::info(
            "The requested {} profile changes the active render extent from "
            "{}x{} to {}x{}; staging it for restart so the stable Skyrim/ENB "
            "proxy identity remains unchanged",
            static_cast<std::uint32_t>(mode),
            bridge.render_width(),
            bridge.render_height(),
            requested_width,
            requested_height);
        static_cast<void>(stage_restart_selection());
        return false;
    }

    ProfileChangeCallbacks callbacks{};

    callbacks.preflight =
        [](const std::uint32_t requested_mode, const char*& why) {
            bool user_facing = false;
            return PresentationBridge::instance()
                .evaluate_profile_change_preflight(
                    static_cast<config::UpscalingMode>(requested_mode),
                    why,
                    user_facing);
        };

    callbacks.stage_transaction = []() {};

    callbacks.apply_runtime_settings = [&]() {
        if (streamline_runtime) {
            if (!frame_generation_runtime.set_reflex_mode(reflex)) {
                return false;
            }
            if (!frame_generation_runtime.set_output_target_fps(frame_limit)) {
                static_cast<void>(
                    frame_generation_runtime.set_reflex_mode(
                        previous_reflex));
                return false;
            }
        }

        DebugViewPass::select(requested_debug_view);
        return true;
    };

    callbacks.persist_settings = [&]() {
        return persist_everything();
    };

    callbacks.reset_history = []() {
        UpscalingPass::instance().reset_history();
    };

    callbacks.reconfigure = [&]() {
        return PresentationBridge::instance()
            .request_upscaling_reconfiguration(mode);
    };

    callbacks.rollback = restore_everything;

    callbacks.report_refusal = [&](const char* const why) {

        if (stage_restart_selection()) {
            logger::info(
                "A live upscaling profile change was refused because a "
                "restart is required: {}. The selection has been saved for "
                "the next Skyrim launch rather than discarded. Previously the "
                "whole Apply was abandoned and the requested mode never "
                "reached UFGU.ini, so selecting it in the menu did nothing at "
                "all and the only way to change it was to edit the file by "
                "hand",
                why != nullptr ? why : "unknown");
            return;
        }

        const auto active = std::ranges::find(
            kUpscalingModes,
            streamline::SuperResolution::instance().mode());
        if (active != kUpscalingModes.end()) {
            selector_upscaling_index = static_cast<std::size_t>(
                std::distance(kUpscalingModes.begin(), active));
        }
        apply_outcome = ApplyOutcome::refused;
        apply_message = to_wide(why != nullptr ? why : "unknown");
        apply_frames_remaining = 600;
        snapshot_valid = false;
        logger::warn(
            "The in-game graphics menu refused the requested DLSS profile "
            "and could not stage it for the next launch either: {}. Nothing "
            "was changed and nothing was saved.",
            why != nullptr ? why : "unknown");
    };

    ProfileChangeRequest request{};
    request.requested_mode = static_cast<std::uint32_t>(mode);
    request.active_runtime_mode =
        static_cast<std::uint32_t>(previous_runtime_mode);

    const auto outcome = run_profile_change(request, callbacks);
    switch (outcome) {
    case ProfileChangeOutcome::applied:
    case ProfileChangeOutcome::applied_without_profile_change:

        logger::info(
            "Apply outcome {}: provider={} mode={} sharpness={:.2f} "
            "reflex={} base-frame-limit={} output-frame-limit={} "
            "multiplier={}x generation={} "
            "overlay={} fps-display={} debug-view={}",
            describe(outcome),
            providers::vendor_display_name(provider),
            static_cast<std::uint32_t>(mode),
            sharpness,
            static_cast<std::uint32_t>(reflex),
            base_frame_limit,
            frame_limit,
            frame_generation,
            frame_generation_enabled,
            overlay_enabled,
            static_cast<std::uint32_t>(overlay_fps_display),
            static_cast<std::uint32_t>(requested_debug_view));
        break;

    case ProfileChangeOutcome::refused_by_preflight:

        return false;

    case ProfileChangeOutcome::failed_runtime_settings:
    case ProfileChangeOutcome::failed_persist:
    case ProfileChangeOutcome::failed_reconfiguration:
    case ProfileChangeOutcome::aborted_incomplete_callbacks:
        logger::error(
            "In-game graphics menu could not apply the selection: {}",
            describe(outcome));
        return false;
    }

    sharpness_dragging = false;
    snapshot_valid = false;
    return true;
}

void StatusOverlay::State::update_selector(
    const std::uint32_t width,
    const std::uint32_t height)
{

    if (applied_provider != providers::Vendor::none &&
        apply_frames_remaining != 0) {
        const auto& super_resolution =
            streamline::SuperResolution::instance();
        if (super_resolution.provider() != applied_provider ||
            super_resolution.mode() != applied_mode) {
            apply_outcome = ApplyOutcome::failed;
            apply_message =
                std::wstring{upscaling_provider_name(applied_provider)} +
                L" completed no frame and was rolled back to " +
                std::wstring{upscaling_provider_name(
                    super_resolution.provider())} + L".";
            logger::error(
                "In-game graphics menu observed {} {} being rolled back to "
                "{} by the switch recovery watchdog",
                providers::vendor_display_name(applied_provider),
                static_cast<std::uint32_t>(applied_mode),
                providers::vendor_display_name(super_resolution.provider()));
            applied_provider = providers::Vendor::none;
            applied_mode = config::UpscalingMode::off;
            apply_frames_remaining = 600;
            snapshot_valid = false;
        } else if (UpscalingPass::instance().evaluation_verified()) {

            applied_provider = providers::Vendor::none;
            applied_mode = config::UpscalingMode::off;
        }
    }

    const auto pressed = [](const int key, bool& was_down) {
        const auto down = (GetAsyncKeyState(key) & 0x8000) != 0;
        const auto edge = down && !was_down;
        was_down = down;
        return edge;
    };
    const auto now = std::chrono::steady_clock::now();
    const auto menu_key = pressed(
        static_cast<int>(config::Settings::instance().menu_key()),
        menu_key_was_down);
    const auto up = pressed(VK_UP, up_was_down);
    const auto down = pressed(VK_DOWN, down_was_down);
    const auto left = pressed(VK_LEFT, left_was_down);
    const auto right = pressed(VK_RIGHT, right_was_down);
    const auto enter = pressed(VK_RETURN, enter_was_down);
    const auto escape = pressed(VK_ESCAPE, escape_was_down);
    const auto backspace = pressed(VK_BACK, backspace_was_down);
    const auto tab = pressed(VK_TAB, tab_was_down);
    const auto space = pressed(VK_SPACE, space_was_down);
    auto typed_digit = -1;
    for (auto digit = 0; digit != 10; ++digit) {
        const auto top_row = pressed(
            '0' + digit,
            digit_was_down[static_cast<std::size_t>(digit)]);
        const auto numpad = pressed(
            VK_NUMPAD0 + digit,
            numpad_digit_was_down[static_cast<std::size_t>(digit)]);
        if (top_row || numpad) {
            typed_digit = digit;
        }
    }
    const auto mouse_clicked =
        mouse_click_pending.exchange(false, std::memory_order_acq_rel);

    if (menu_key && !selector_open) {
        auto& settings = config::Settings::instance();
        if (!settings.frame_generation_enabled()) {
            selector_frame_generation_index = 0U;
        } else {

            const auto usable = usable_frame_generation_choices();
            const auto multiplier = std::ranges::find(
                kFrameGenerationChoices,
                settings.frame_generation_multiplier());
            selector_frame_generation_index =
                multiplier != kFrameGenerationChoices.end() ?
                    static_cast<std::size_t>(std::distance(
                        kFrameGenerationChoices.begin(),
                        multiplier)) :
                    1U;
            if (usable != 0U && selector_frame_generation_index >= usable) {
                selector_frame_generation_index = usable - 1U;
            }
        }
        selector_dynamic_mfg_index =
            settings.multiplier_mode() == config::MultiplierMode::dynamic ?
                1U : 0U;
        const auto upscaling = std::ranges::find(
            kUpscalingModes,
            settings.upscaling_mode());
        selector_upscaling_index =
            upscaling != kUpscalingModes.end() ?
                static_cast<std::size_t>(
                    std::distance(
                        kUpscalingModes.begin(),
                        upscaling)) :
                0U;
        const auto provider = std::ranges::find(
            kUpscalingProviders,
            settings.upscaling_provider());
        selector_provider_index =
            provider != kUpscalingProviders.end() ?
                static_cast<std::size_t>(std::distance(
                    kUpscalingProviders.begin(), provider)) :
                0U;
        selector_sharpness_step = static_cast<std::size_t>(
            (std::clamp)(
                std::lround(settings.sharpness() * 20.0F),
                0L,
                20L));
        const auto reflex = std::ranges::find(
            kReflexModes,
            settings.reflex_mode());
        selector_reflex_index =
            reflex != kReflexModes.end() ?
                static_cast<std::size_t>(
                    std::distance(
                        kReflexModes.begin(),
                        reflex)) :
                1U;
        selector_base_frame_limit = settings.base_frame_limit();
        selector_base_frame_limit_custom =
            !output_cap_is_preset(selector_base_frame_limit);
        selector_custom_base_frame_limit =
            selector_base_frame_limit_custom ? selector_base_frame_limit :
            (selector_base_frame_limit == 0U ?
                 40U : selector_base_frame_limit);
        selector_frame_limit = settings.frame_limit();
        selector_frame_limit_custom =
            !output_cap_is_preset(selector_frame_limit);
        selector_custom_frame_limit =
            selector_frame_limit_custom ? selector_frame_limit :
            (selector_frame_limit == 0U ? 240U : selector_frame_limit);
        selector_menu_sounds_index =
            settings.menu_sounds_enabled() ? 1U : 0U;
        if (!settings.overlay_enabled()) {
            selector_overlay_index = 0U;
        } else {
            const auto found = std::ranges::find(
                kFpsCounterChoices, settings.overlay_fps_display());
            selector_overlay_index =
                found != kFpsCounterChoices.end() ?
                    static_cast<std::size_t>(std::distance(
                        kFpsCounterChoices.begin(), found)) + 1U :
                    1U;
        }
        {
            const auto current = DebugViewPass::selected_view();
            const auto found =
                std::ranges::find(kDebugViewChoices, current);
            selector_debug_index =
                found != kDebugViewChoices.end() ?
                    static_cast<std::size_t>(std::distance(
                        kDebugViewChoices.begin(), found)) :
                    0U;
        }
        selector_row = kSections[0].rows[0];
        selector_section = 0;
        animated_section_from = 0;
        selector_open = true;
        selector_dirty = false;
        snapshot_valid = false;
        selector_activity = now;
        menu_opened_at = now;
        section_changed_at = now;
        value_changed_at.fill({});
        previous_values.fill(std::wstring{});
        cursor_x = static_cast<float>(width) * 0.5F;
        cursor_y = static_cast<float>(height) * 0.5F;
        mouse_delta_x.store(0, std::memory_order_relaxed);
        mouse_delta_y.store(0, std::memory_order_relaxed);
        mouse_click_pending.store(false, std::memory_order_relaxed);
        mouse_button_down.store(false, std::memory_order_relaxed);
        sharpness_dragging = false;
        frame_limit_editing = false;
        frame_limit_input.clear();
        input_capture.store(true, std::memory_order_release);
        return;
    }
    if (!selector_open) {
        return;
    }
    if (escape && frame_limit_editing) {
        frame_limit_editing = false;
        frame_limit_input.clear();
        selector_activity = now;
        return;
    }
    if (menu_key || escape) {
        selector_open = false;
        menu_closed_at = std::chrono::steady_clock::now();
        sharpness_dragging = false;
        frame_limit_editing = false;
        frame_limit_input.clear();
        snapshot_valid = false;
        mouse_button_down.store(false, std::memory_order_relaxed);
        input_capture.store(false, std::memory_order_release);
        return;
    }

    const auto cursor_step_x =
        mouse_delta_x.exchange(0, std::memory_order_acq_rel);
    const auto cursor_step_y =
        mouse_delta_y.exchange(0, std::memory_order_acq_rel);
    if (cursor_step_x != 0 || cursor_step_y != 0) {
        keyboard_navigation = false;
    }
    cursor_x += static_cast<float>(cursor_step_x);
    cursor_y += static_cast<float>(cursor_step_y);
    cursor_x = (std::clamp)(
        cursor_x,
        0.0F,
        static_cast<float>(width - 1U));
    cursor_y = (std::clamp)(
        cursor_y,
        0.0F,
        static_cast<float>(height - 1U));

    const auto layout = make_menu_layout(
        width,
        height,
        selector_section,
        config::Settings::instance().menu_presentation() !=
            config::MenuPresentation::full,
        anchor_for(config::Settings::instance().menu_presentation()));
    const auto commit_custom_frame_limit = [&]() {
        const auto parsed = parse_output_cap_digits(frame_limit_input);
        if (!parsed.has_value()) {
            apply_outcome = ApplyOutcome::refused;
            apply_message =
                L"Enter Off as 0, or a whole-number frame cap from 20 "
                L"through 1000 FPS.";
            apply_frames_remaining = 300;
            selector_activity = now;
            return false;
        }
        if (frame_limit_editing_row == kBaseCapRow) {
            selector_custom_base_frame_limit = *parsed;
        } else {
            const auto active_multiplier = kFrameGenerationChoices[
                (std::min)(
                    selector_frame_generation_index,
                    kFrameGenerationChoices.size() - 1U)];
            if (active_multiplier >= 2U && *parsed != 0U) {
                const auto derived = base_for_output_preset(
                    *parsed, active_multiplier);
                if (derived == 0U) {
                    apply_outcome = ApplyOutcome::refused;
                    apply_message =
                        L"At " +
                        std::to_wstring(active_multiplier) +
                        L"x the final output is the base cap times " +
                        L"the multiplier, so it has to divide by " +
                        std::to_wstring(active_multiplier) +
                        L" and leave a base of at least 20 FPS. " +
                        L"Try " +
                        std::to_wstring(active_multiplier * 20U) +
                        L" or higher, or set the Base FPS Cap instead.";
                    apply_frames_remaining = 300;
                    selector_activity = now;
                    return false;
                }
                selector_base_frame_limit = derived;
                selector_base_frame_limit_custom =
                    !output_cap_is_preset(derived);
                selector_custom_base_frame_limit = derived;
                selector_frame_limit = *parsed;
                selector_frame_limit_custom =
                    !output_cap_is_preset(*parsed);
                selector_custom_frame_limit = *parsed;
                value_changed_at[kBaseCapRow] = now;
            } else {
                selector_custom_frame_limit = *parsed;
            }
        }
        frame_limit_editing = false;
        frame_limit_input.clear();
        selector_activity = now;
        return true;
    };

    if (frame_limit_editing) {
        if (typed_digit >= 0 && frame_limit_input.size() < 4U) {
            frame_limit_input.push_back(
                static_cast<char>('0' + typed_digit));
            selector_activity = now;
        }
        if (backspace && !frame_limit_input.empty()) {
            frame_limit_input.pop_back();
            selector_activity = now;
        }
        if (enter) {
            static_cast<void>(commit_custom_frame_limit());
            return;
        }
    }

    for (std::size_t section = 0; section < kSectionCount; ++section) {
        if (layout.sections[section].contains(cursor_x, cursor_y) &&
            mouse_clicked) {
            selector_section = section;
            selector_row = kSections[section].rows[0];
            selector_activity = now;
            keyboard_navigation = false;
            break;
        }
    }
    if (!frame_limit_editing && space) {
        auto& settings = config::Settings::instance();
        const auto next = static_cast<config::MenuPresentation>(
            (static_cast<std::uint32_t>(settings.menu_presentation()) + 1U) %
            config::kMenuPresentationCount);
        if (!settings.set_menu_presentation(next)) {
            logger::warn(
                "The menu presentation could not be persisted, so the change "
                "applies to this session only");
        }
        selector_activity = now;
        keyboard_navigation = true;
        audio::MenuAudio::instance().play(audio::MenuCue::section);
    }
    if (!frame_limit_editing && tab) {
        const auto& definition = kSections[selector_section];
        auto current = definition.row_count;
        for (std::size_t slot = 0; slot < definition.row_count; ++slot) {
            if (definition.rows[slot] == selector_row) {
                current = slot;
                break;
            }
        }
        if (current == definition.row_count) {
            current = 0U;
        }
        const auto backwards = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const auto next = backwards ?
            (current == 0U ? definition.row_count - 1U : current - 1U) :
            (current + 1U) % definition.row_count;
        selector_row = definition.rows[next];
        selector_activity = now;
        keyboard_navigation = true;
        audio::MenuAudio::instance().play(audio::MenuCue::move);
    }
    if (!frame_limit_editing && (up || down)) {
        animated_section_from = selector_section;
        selector_section = up ?
            (selector_section == 0U ?
                kSectionCount - 1U :
                selector_section - 1U) :
            (selector_section + 1U) % kSectionCount;
        selector_row = kSections[selector_section].rows[0];
        selector_activity = now;
        section_changed_at = now;
        keyboard_navigation = true;
        audio::MenuAudio::instance().play(audio::MenuCue::section);
    }

    for (std::size_t row = 0; !keyboard_navigation && row < kMenuRowCount;
         ++row) {
        if (layout.rows[row].right <= layout.rows[row].left ||
            layout.rows[row].bottom <= layout.rows[row].top) {
            continue;
        }
        if (layout.rows[row].contains(cursor_x, cursor_y)) {
            selector_row = row;
            break;
        }
    }
    if (section_of_row(selector_row) != selector_section) {
        selector_row = kSections[selector_section].rows[0];
    }

    const auto cycle = [](
        std::size_t& index,
        const std::size_t count,
        const int amount) {
        if (amount < 0) {
            index = index == 0U ? count - 1U : index - 1U;
        } else {
            index = (index + 1U) % count;
        }
    };
    const auto adjust_frame_cap = [&](
        std::uint32_t& preset,
        std::uint32_t& custom,
        bool& custom_selected,
        const int amount) {

        const auto control =
            (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        const auto shift =
            (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        const auto custom_step = control ? 1U : (shift ? 10U : 0U);
        frame_limit_editing = false;
        frame_limit_input.clear();
        if (custom_step != 0U) {
            custom = adjust_output_cap(
                custom_selected ? custom : preset,
                amount,
                custom_step);
            custom_selected = true;
        } else if (custom_selected) {
            custom_selected = false;
            preset = amount > 0 ?
                kOutputCapPresets.front() : kOutputCapPresets.back();
        } else if (
            (amount > 0 && preset == kOutputCapPresets.back()) ||
            (amount < 0 && preset == kOutputCapPresets.front())) {
            custom_selected = true;
        } else {
            preset = adjust_output_cap(preset, amount);
        }
    };
    const auto adjust_selected = [&](const int amount) {
        switch (selector_row) {
        case kFrameGenerationRow:
            if (!frame_generation_available()) {
                break;
            }
            cycle(
                selector_frame_generation_index,
                usable_frame_generation_choices(),
                amount);
            break;
        case kDynamicMfgRow:
            if (!dynamic_mfg_available()) {
                break;
            }
            cycle(selector_dynamic_mfg_index, 2U, amount);
            break;
        case kBaseCapRow:
            adjust_frame_cap(
                selector_base_frame_limit,
                selector_custom_base_frame_limit,
                selector_base_frame_limit_custom,
                amount);
            break;
        case kProviderRow:
            cycle(
                selector_provider_index,
                kUpscalingProviders.size(),
                amount);
            break;
        case kUpscalingRow:
            cycle(
                selector_upscaling_index,
                kUpscalingModes.size(),
                amount);
            break;
        case kSharpnessRow:
            if (amount < 0 && selector_sharpness_step != 0U) {
                --selector_sharpness_step;
            } else if (
                amount > 0 && selector_sharpness_step < 20U) {
                ++selector_sharpness_step;
            }
            static_cast<void>(
                config::Settings::instance().set_sharpness(
                    static_cast<float>(
                        selector_sharpness_step) *
                    0.05F));
            break;
        case kReflexRow:
            if (!low_latency_available()) {
                break;
            }
            cycle(
                selector_reflex_index,
                kReflexModes.size(),
                amount);
            break;
        case kOutputCapRow:
        {
            if (!frame_generation_available()) {
                break;
            }
            const auto active_multiplier = kFrameGenerationChoices[
                (std::min)(
                    selector_frame_generation_index,
                    kFrameGenerationChoices.size() - 1U)];
            if (active_multiplier >= 2U) {
                const auto current_base =
                    selector_base_frame_limit_custom ?
                        selector_custom_base_frame_limit :
                        selector_base_frame_limit;
                const auto next_base = adjust_base_by_output(
                    current_base, active_multiplier, amount);
                if (next_base != current_base) {
                    selector_base_frame_limit = next_base;
                    selector_base_frame_limit_custom = false;
                    selector_frame_limit =
                        next_base * active_multiplier;
                    selector_frame_limit_custom = false;
                    value_changed_at[kBaseCapRow] = now;
                }
                break;
            }
            adjust_frame_cap(
                selector_frame_limit,
                selector_custom_frame_limit,
                selector_frame_limit_custom,
                amount);
            break;
        }
        case kOverlayRow:
            cycle(
                selector_overlay_index,
                kFpsCounterChoices.size() + 1U,
                amount);
            break;
        case kMenuSoundsRow:
            cycle(selector_menu_sounds_index, 2U, amount);
            break;
        case kDebugRow:
            cycle(
                selector_debug_index,
                kDebugViewChoices.size(),
                amount);

            break;
        default:
            break;
        }
    };

    const auto direction = right ? 1 : (left ? -1 : 0);
    if (!frame_limit_editing && direction != 0) {
        adjust_selected(direction);
        selector_activity = now;
        selector_dirty = true;
    }

    const Rectangle slider_hit_area{
        layout.sharpness_track.left - 12.0F * layout.scale,
        layout.values[kSharpnessRow].top,
        layout.values[kSharpnessRow].right,
        layout.values[kSharpnessRow].bottom};
    const auto mouse_held =
        mouse_button_down.load(std::memory_order_acquire) ||
        (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (mouse_clicked &&
        slider_hit_area.contains(cursor_x, cursor_y)) {
        sharpness_dragging = true;
    }
    if (!mouse_held && !mouse_clicked) {
        sharpness_dragging = false;
    }
    if (sharpness_dragging) {
        selector_row = kSharpnessRow;
        const auto fraction = (std::clamp)(
            (cursor_x - layout.sharpness_track.left) /
                (layout.sharpness_track.right -
                 layout.sharpness_track.left),
            0.0F,
            1.0F);
        selector_sharpness_step =
            static_cast<std::size_t>(
                std::lround(fraction * 20.0F));
        static_cast<void>(
            config::Settings::instance().set_sharpness(
                static_cast<float>(selector_sharpness_step) *
                0.05F));
        selector_activity = now;
    } else if (mouse_clicked) {
        auto handled = false;
        if (selector_base_frame_limit_custom &&
            layout.values[kBaseCapRow].contains(cursor_x, cursor_y)) {
            selector_row = kBaseCapRow;
            frame_limit_editing = true;
            frame_limit_editing_row = kBaseCapRow;
            frame_limit_input.clear();
            handled = true;
        }
        if (selector_frame_limit_custom &&
            !handled &&
            layout.values[kOutputCapRow].contains(cursor_x, cursor_y)) {
            selector_row = kOutputCapRow;
            frame_limit_editing = true;
            frame_limit_editing_row = kOutputCapRow;
            frame_limit_input.clear();
            handled = true;
        }
        for (std::size_t row = 0;
             !handled && row < kMenuRowCount;
             ++row) {
            const auto& caret_left = caret_left_rects[row].right >
                    caret_left_rects[row].left ?
                caret_left_rects[row] : layout.left_buttons[row];
            if (row != kSharpnessRow &&
                caret_left.contains(cursor_x, cursor_y)) {
                selector_row = row;
                adjust_selected(-1);
                audio::MenuAudio::instance().play(audio::MenuCue::move);
                handled = true;
                break;
            }
            if (row != kSharpnessRow &&
                layout.right_buttons[row].contains(
                    cursor_x,
                    cursor_y)) {
                selector_row = row;
                adjust_selected(1);
                audio::MenuAudio::instance().play(audio::MenuCue::move);
                handled = true;
                break;
            }
        }
        if (handled) {
            selector_activity = now;
        } else if (
            layout.apply.contains(cursor_x, cursor_y)) {
            if (frame_limit_editing &&
                !commit_custom_frame_limit()) {
                return;
            }
            static_cast<void>(apply_selector());
            selector_activity = now;
        }
    }

    if (enter && !frame_limit_editing) {
        static_cast<void>(apply_selector());
        selector_activity = now;
        selector_dirty = false;
    }
    const auto idle_seconds =
        std::chrono::duration<float>(now - selector_activity).count();
    if (menu_should_auto_close(selector_dirty, idle_seconds)) {
        selector_open = false;
        menu_closed_at = std::chrono::steady_clock::now();
        sharpness_dragging = false;
        frame_limit_editing = false;
        frame_limit_input.clear();
        snapshot_valid = false;
        mouse_button_down.store(false, std::memory_order_relaxed);
        input_capture.store(false, std::memory_order_release);
    }
}

void StatusOverlay::State::draw_counter(
    const std::uint32_t width,
    const std::uint32_t height)
{
    if (!config::Settings::instance().overlay_enabled()) {
        return;
    }
    const auto scale = make_menu_layout(width, height).scale;

    const auto text = fps_text(
        rendered_fps,
        output_fps,
        config::Settings::instance().overlay_fps_display(),
        streamline::FrameGeneration::instance().enabled() ||
            vendor_presented_per_frame() > 1U);
    const D2D1_RECT_F rectangle{
        static_cast<float>(width) - 290.0F * scale,
        12.0F * scale,
        static_cast<float>(width) - 18.0F * scale,
        46.0F * scale};
    d2d_context->DrawText(
        text.c_str(),
        static_cast<UINT32>(text.size()),
        counter_format.Get(),
        rectangle,
        white_brush.Get(),
        D2D1_DRAW_TEXT_OPTIONS_NO_SNAP,
        DWRITE_MEASURING_MODE_NATURAL);
}

void StatusOverlay::State::draw_menu(const MenuLayout& source_layout)
{
    MenuLayout menu = source_layout;
    const auto text_options = menu.compact ?
        static_cast<D2D1_DRAW_TEXT_OPTIONS>(
            D2D1_DRAW_TEXT_OPTIONS_NO_SNAP | D2D1_DRAW_TEXT_OPTIONS_CLIP) :
        D2D1_DRAW_TEXT_OPTIONS_NO_SNAP;
    const auto draw_text = [this, text_options](
        const std::wstring_view text,
        const Rectangle& rectangle,
        IDWriteTextFormat* format,
        ID2D1Brush* brush) {
        d2d_context->DrawText(
            text.data(),
            static_cast<UINT32>(text.size()),
            format,
            to_d2d(rectangle),
            brush,
            text_options,
            DWRITE_MEASURING_MODE_NATURAL);
    };
    const auto control_radius = 9.0F * menu.scale;
    const auto cursor_over = [this](const Rectangle& rectangle) {
        return rectangle.contains(cursor_x, cursor_y);
    };

    const auto animation_now = std::chrono::steady_clock::now();
    const auto progress = [animation_now](
        const std::chrono::steady_clock::time_point start,
        const float milliseconds) {
        if (start.time_since_epoch().count() == 0) {
            return 1.0F;
        }
        const auto elapsed =
            std::chrono::duration<float, std::milli>(animation_now - start)
                .count();
        return (std::clamp)(elapsed / milliseconds, 0.0F, 1.0F);
    };
    const auto ease_out = [](const float t) {
        const auto inverse = 1.0F - t;
        return 1.0F - inverse * inverse * inverse;
    };
    const auto mix = [](const float a, const float b, const float t) {
        return a + (b - a) * t;
    };

    const auto fade = closing_fade();
    const auto open_progress = fade > 0.0F ?
        fade :
        ease_out(progress(menu_opened_at, 260.0F));
    const auto section_progress =
        ease_out(progress(section_changed_at, 200.0F));

    if (!menu.compact) {
        d2d_context->FillRectangle(
            D2D1::RectF(
                0.0F,
                0.0F,
                static_cast<float>(target_width),
                static_cast<float>(target_height)),
            scrim_brush.Get());
    }

    D2D1_LAYER_PARAMETERS1 blur_clip{};
    blur_clip.contentBounds = to_d2d(menu.panel);
    blur_clip.geometricMask = blur_clip_geometry.Get();
    blur_clip.maskAntialiasMode = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
    blur_clip.maskTransform = D2D1::Matrix3x2F::Identity();
    blur_clip.opacity = 1.0F;
    blur_clip.layerOptions = D2D1_LAYER_OPTIONS1_NONE;
    if (!menu.compact) {
        d2d_context->PushLayer(blur_clip, blur_clip_layer.Get());
        static_cast<void>(blur_effect->SetValue(
            D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION,
            14.0F * menu.scale));
        const auto blur_origin = D2D1::Point2F(0.0F, 0.0F);
        d2d_context->DrawImage(
            blur_effect.Get(),
            &blur_origin,
            nullptr,
            D2D1_INTERPOLATION_MODE_LINEAR,
            D2D1_COMPOSITE_MODE_SOURCE_OVER);
        d2d_context->PopLayer();
    }

    glass_brush->SetOpacity(open_progress);
    d2d_context->FillRectangle(
        to_d2d(menu.compact ? menu.panel : menu.screen), glass_brush.Get());
    glass_brush->SetOpacity(1.0F);

    D2D1_LAYER_PARAMETERS1 fade_layer{};
    fade_layer.contentBounds = to_d2d(menu.screen);
    fade_layer.geometricMask = nullptr;
    fade_layer.maskAntialiasMode = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
    fade_layer.maskTransform = D2D1::Matrix3x2F::Identity();
    fade_layer.opacity = open_progress;
    fade_layer.layerOptions = D2D1_LAYER_OPTIONS1_NONE;
    d2d_context->PushLayer(fade_layer, nullptr);
    d2d_context->SetTransform(
        D2D1::Matrix3x2F::Translation(
            0.0F,
            (1.0F - open_progress) * 18.0F * menu.scale));

    const auto hairline = [this, &menu](
        const float x0,
        const float y0,
        const float x1,
        const float y1) {
        d2d_context->DrawLine(
            D2D1::Point2F(x0, y0),
            D2D1::Point2F(x1, y1),
            separator_brush.Get(),
            1.0F * menu.scale);
    };

    if (!menu.compact) {
        hairline(
            menu.rail.right,
            menu.screen.top + menu.screen.bottom * 0.06F,
            menu.rail.right,
            menu.screen.bottom * 0.94F);
    }
    if (menu.detail_visible) {
        hairline(
            menu.detail.left,
            menu.screen.top + menu.screen.bottom * 0.06F,
            menu.detail.left,
            menu.screen.bottom * 0.94F);
    }

    draw_text(
        L"UFGU",
        {
            menu.header.left,
            menu.header.top,
            menu.header.right,
            menu.header.top + 24.0F * menu.scale},
        brand_format.Get(),
        white_brush.Get());
    if (!menu.compact) {
        draw_text(
            L"0.1.0",
            {
                menu.header.left,
                menu.header.top + 22.0F * menu.scale,
                menu.header.right,
                menu.header.bottom},
            version_format.Get(),
            faint_brush.Get());
    }

    if (!menu.compact) {
        const auto& from = menu.sections[
            (std::min)(animated_section_from, kSectionCount - 1U)];
        const auto& to = menu.sections[selector_section];
        const Rectangle sliding{
            mix(from.left, to.left, section_progress),
            mix(from.top, to.top, section_progress),
            mix(from.right, to.right, section_progress),
            mix(from.bottom, to.bottom, section_progress)};
        d2d_context->FillRoundedRectangle(
            rounded(sliding, 5.0F * menu.scale),
            section_active_brush.Get());
    }

    for (std::size_t section = 0;
         !menu.compact && section < kSectionCount;
         ++section) {
        const auto active = section == selector_section;
        const auto hovered =
            !active && menu.sections[section].contains(cursor_x, cursor_y);
        if (active) {
            static_cast<void>(active);
        } else if (hovered) {
            d2d_context->FillRoundedRectangle(
                rounded(menu.sections[section], 5.0F * menu.scale),
                control_hover_brush.Get());
        }
        const Rectangle text_area{
            menu.sections[section].left + 11.0F * menu.scale,
            menu.sections[section].top,
            menu.sections[section].right - 8.0F * menu.scale,
            menu.sections[section].bottom};
        draw_text(
            kSections[section].name,
            text_area,
            section_format.Get(),
            active ? white_brush.Get() : muted_brush.Get());
        const auto count =
            std::to_wstring(kSections[section].row_count);
        draw_text(
            count,
            text_area,
            section_count_format.Get(),
            active ? muted_brush.Get() : faint_brush.Get());
    }

    if (!menu.compact) {
        std::wstring footer_text{std::to_wstring(target_width)};
        footer_text += L" x ";
        footer_text += std::to_wstring(target_height);
        const auto* const runtime_profile = active_runtime_profile();
        if (runtime_profile != nullptr) {
            constexpr std::string_view kRuntimePrefix{"Skyrim "};
            auto runtime_name = runtime_profile->name;
            if (runtime_name.starts_with(kRuntimePrefix)) {
                runtime_name.remove_prefix(kRuntimePrefix.size());
            }
            footer_text += L"  ";
            for (const auto letter : runtime_name) {
                footer_text.push_back(static_cast<wchar_t>(letter));
            }
        }
        draw_text(
            footer_text,
            menu.rail_footer,
            version_format.Get(),
            disabled_brush.Get());
    }

    if (!conflicting_injectors().empty()) {
        const Rectangle banner{
            menu.section_title.left,
            menu.screen.bottom - 108.0F * menu.scale,
            menu.main.right - 30.0F * menu.scale,
            menu.screen.bottom - 74.0F * menu.scale};
        d2d_context->FillRoundedRectangle(
            rounded(banner, menu.corner_radius),
            warn_fill_brush.Get());
        const Rectangle banner_text{
            banner.left + 12.0F * menu.scale,
            banner.top,
            banner.right - 12.0F * menu.scale,
            banner.bottom};
        const auto& first_conflict = conflicting_injectors().front();
        const auto extra_conflicts = conflicting_injectors().size() - 1U;
        draw_text(
            L"Conflicting mod: " + to_wide(first_conflict.name) +
                (extra_conflicts != 0U ?
                     L" and " + std::to_wstring(extra_conflicts) + L" more" :
                     std::wstring{}) +
                L". Disable it; " + to_wide(first_conflict.collision) + L".",
            banner_text,
            detail_format.Get(),
            warn_brush.Get());
    }

    draw_text(
        kSections[selector_section].name,
        menu.section_title,
        heading_format.Get(),
        white_brush.Get());
    if (menu.compact) {
        std::wstring compact_hint{
            std::to_wstring(selector_section + 1U)};
        compact_hint += L" of ";
        compact_hint += std::to_wstring(kSectionCount);
        compact_hint += L"   Up/Down section   Tab setting   ";
        compact_hint += L"Space layout";
        draw_text(
            compact_hint,
            menu.section_title,
            section_count_format.Get(),
            muted_brush.Get());
    }
    if (!menu.compact) {
        draw_text(
            kSections[selector_section].summary,
            menu.section_summary,
            summary_format.Get(),
            muted_brush.Get());
    }

    constexpr std::array<std::wstring_view, kMenuRowCount> labels{
        L"Frame Generation",

        L"Dynamic Multi Frame Generation",
        L"Base FPS Cap",
        L"Upscaling Provider",
        L"Upscaling Quality",
        L"Sharpness",

        L"Low Latency",
        L"Final Output",
        L"FPS Counter",
        L"Debug View",
        L"Neural Texture Compression",
        L"Menu Sounds"};
    const auto frame_generation =
        kFrameGenerationChoices[
            selector_frame_generation_index];
    const auto streamline_available =
        frame_generation_available();

    const auto output_cap_value = [&]() {
        if (!selector_frame_limit_custom) {
            const auto configured_base = selector_base_frame_limit_custom ?
                selector_custom_base_frame_limit : selector_base_frame_limit;
            const auto effective = reflex_limit_preserving_base(
                selector_frame_limit,
                configured_base,
                static_cast<std::uint32_t>(frame_generation));
            if (effective > selector_frame_limit) {
                return std::to_wstring(effective) + L" FPS";
            }
            return frame_limit_text(selector_frame_limit);
        }
        if (!frame_limit_editing ||
            frame_limit_editing_row != kOutputCapRow) {
            return std::wstring{L"Custom"};
        }
        return frame_limit_input.empty() ?
            std::wstring{L"Type FPS"} :
            to_wide(frame_limit_input) + L"|";
    }();
    const auto base_cap_value = [&]() {
        if (!selector_base_frame_limit_custom) {
            return frame_limit_text(selector_base_frame_limit);
        }
        if (!frame_limit_editing ||
            frame_limit_editing_row != kBaseCapRow) {
            return std::wstring{L"Custom"};
        }
        return frame_limit_input.empty() ?
            std::wstring{L"Type FPS"} :
            to_wide(frame_limit_input) + L"|";
    }();
    const std::array<std::wstring, kMenuRowCount> values{
        !streamline_available ?
            std::wstring{L"Unavailable"} :
        frame_generation == 0U ?
            std::wstring{L"Off"} :

            (dynamic_mfg_available() && selector_dynamic_mfg_index != 0U) ?
                std::wstring{L"Auto"} :
                std::to_wstring(frame_generation) + L"x",

        !dynamic_mfg_available() ?
            std::wstring{L"Unavailable"} :
            std::wstring{
                selector_dynamic_mfg_index != 0U ? L"On" : L"Off"},
        base_cap_value,
        std::wstring{upscaling_provider_name(
            kUpscalingProviders[selector_provider_index])},
        std::wstring{upscaling_mode_name(
            kUpscalingModes[selector_upscaling_index],
            kUpscalingProviders[selector_provider_index])},
        percentage_text(selector_sharpness_step),

        low_latency_value(kReflexModes[selector_reflex_index]),
        !streamline_available ?
            std::wstring{L"Unavailable"} :
            output_cap_value,
        std::wstring{fps_counter_name(selector_overlay_index)},
        std::wstring{debug_view_name(
            kDebugViewChoices[selector_debug_index])},
        std::wstring{L"Unavailable"},
        std::wstring{selector_menu_sounds_index != 0U ? L"On" : L"Off"}};

    const auto measure_value = [&](const std::size_t row) {
        const auto& text = values[row];
        if (text.empty() || write_factory == nullptr ||
            value_format == nullptr) {
            return 0.0F;
        }
        ComPtr<IDWriteTextLayout> layout_measure;
        if (FAILED(write_factory->CreateTextLayout(
                text.c_str(),
                static_cast<UINT32>(text.size()),
                value_format.Get(),
                4096.0F,
                256.0F,
                layout_measure.GetAddressOf())) ||
            layout_measure == nullptr) {
            return 0.0F;
        }
        DWRITE_TEXT_METRICS metrics{};
        if (FAILED(layout_measure->GetMetrics(&metrics))) {
            return 0.0F;
        }
        return metrics.widthIncludingTrailingWhitespace;
    };

    const auto caret_left_for = [&](const std::size_t row) {
        auto rect = menu.left_buttons[row];
        const auto box = menu.values[row].right - menu.values[row].left;
        const auto text = measure_value(row);
        const auto slack = box - text;
        if (slack > 0.0F) {
            rect.left += slack;
            rect.right += slack;
        } else if (slack < 0.0F) {
            rect.left += slack;
            rect.right += slack;
        }
        caret_left_rects[row] = rect;
        return rect;
    };

    for (std::size_t row = 0; row < kMenuRowCount; ++row) {
        if (previous_values[row] != values[row]) {
            if (!previous_values[row].empty()) {
                value_changed_at[row] = animation_now;
            }
            previous_values[row] = values[row];
        }
    }

    const auto& active_section = kSections[selector_section];
    for (std::size_t slot = 0; slot < active_section.row_count; ++slot) {
        const auto row = active_section.rows[slot];
        const auto focused = row == selector_row;
        const auto unavailable = values[row] == L"Unavailable";
        if (focused) {
            d2d_context->FillRoundedRectangle(
                rounded(menu.rows[row], menu.corner_radius),
                row_brush.Get());
        } else if (menu.rows[row].contains(cursor_x, cursor_y)) {
            d2d_context->FillRoundedRectangle(
                rounded(menu.rows[row], menu.corner_radius),
                control_hover_brush.Get());
        }
        if (slot != 0 && !focused) {
            d2d_context->DrawLine(
                D2D1::Point2F(menu.rows[row].left, menu.rows[row].top),
                D2D1::Point2F(menu.rows[row].right, menu.rows[row].top),
                separator_brush.Get(),
                1.0F * menu.scale);
        }
        const Rectangle label_rectangle{
            menu.rows[row].left + 14.0F * menu.scale,
            menu.rows[row].top,
            menu.left_buttons[row].left - 10.0F * menu.scale,
            menu.rows[row].bottom};
        draw_text(
            labels[row],
            label_rectangle,
            label_format.Get(),
            unavailable ? disabled_brush.Get() :
                focused ? white_brush.Get() : label_brush.Get());

        const auto row_hovered = menu.rows[row].contains(cursor_x, cursor_y);
        const auto carets_visible =
            !unavailable && (focused || row_hovered);

        if (row == kSharpnessRow) {
            const auto fraction =
                static_cast<float>(selector_sharpness_step) / 20.0F;
            const auto track_centre =
                (menu.sharpness_track.top +
                 menu.sharpness_track.bottom) * 0.5F;
            d2d_context->DrawLine(
                D2D1::Point2F(menu.sharpness_track.left, track_centre),
                D2D1::Point2F(menu.sharpness_track.right, track_centre),
                separator_brush.Get(),
                2.0F * menu.scale);
            if (fraction > 0.0F) {
                d2d_context->DrawLine(
                    D2D1::Point2F(menu.sharpness_track.left, track_centre),
                    D2D1::Point2F(
                        menu.sharpness_track.left +
                            (menu.sharpness_track.right -
                             menu.sharpness_track.left) * fraction,
                        track_centre),
                    focused ? accent_brush.Get() : value_brush.Get(),
                    2.0F * menu.scale);
            }
            if (carets_visible) {
                draw_text(
                    L"<",
                    caret_left_for(row),
                    button_format.Get(),
                    focused ? muted_brush.Get() : faint_brush.Get());
                draw_text(
                    L">",
                    menu.right_buttons[row],
                    button_format.Get(),
                    focused ? muted_brush.Get() : faint_brush.Get());
            }
            draw_text(
                values[row],
                menu.values[row],
                value_format.Get(),
                focused ? accent_brush.Get() : value_brush.Get());
            continue;
        }

        if (carets_visible) {
            const auto left_hover = cursor_over(caret_left_for(row));
            const auto right_hover = cursor_over(menu.right_buttons[row]);
            draw_text(
                L"<",
                caret_left_for(row),
                button_format.Get(),
                left_hover ? white_brush.Get() :
                    focused ? muted_brush.Get() : faint_brush.Get());
            draw_text(
                L">",
                menu.right_buttons[row],
                button_format.Get(),
                right_hover ? white_brush.Get() :
                    focused ? muted_brush.Get() : faint_brush.Get());
        }
        const auto change_progress =
            ease_out(progress(value_changed_at[row], 220.0F));
        const auto lift = (1.0F - change_progress) * 7.0F * menu.scale;
        const Rectangle animated_value{
            menu.values[row].left,
            menu.values[row].top - lift,
            menu.values[row].right,
            menu.values[row].bottom - lift};
        auto* const value_paint =
            unavailable ? disabled_brush.Get() :
                focused ? accent_brush.Get() : value_brush.Get();
        value_paint->SetOpacity(mix(0.15F, 1.0F, change_progress));
        draw_text(
            values[row],
            animated_value,
            value_format.Get(),
            value_paint);
        value_paint->SetOpacity(1.0F);
    }
    static_cast<void>(control_radius);

    const auto fps_number = [](const float value) {
        return std::to_wstring(
            static_cast<int>(std::lround((std::max)(value, 0.0F))));
    };


    if (!menu.compact) {
        const Rectangle keys_area{
            menu.footer.left,
            menu.footer.top,
            menu.footer.left + (menu.footer.right - menu.footer.left) * 0.6F,
            menu.footer.bottom};
        d2d_context->DrawLine(
            D2D1::Point2F(menu.footer.left, menu.footer.top - 12.0F * menu.scale),
            D2D1::Point2F(menu.footer.right, menu.footer.top - 12.0F * menu.scale),
            separator_brush.Get(),
            1.0F * menu.scale);
        {
            struct KeyHint final
            {
                const wchar_t* key;
                const wchar_t* action;
            };
            constexpr std::array<KeyHint, 6> hints{{
                {L"Up/Down", L"section"},
                {L"Tab", L"setting"},
                {L"Left/Right", L"value"},
                {L"Enter", L"apply"},
                {L"Space", L"collapse"},
                {L"Esc", L"close"}}};
            const auto column_width =
                (keys_area.right - keys_area.left) / 3.0F;
            const auto row_height =
                (keys_area.bottom - keys_area.top) / 2.0F;
            for (std::size_t index = 0U; index < hints.size(); ++index) {
                const auto column = static_cast<float>(index % 3U);
                const auto row = static_cast<float>(index / 3U);
                const Rectangle cell{
                    keys_area.left + column * column_width,
                    keys_area.top + row * row_height,
                    keys_area.left + (column + 1.0F) * column_width,
                    keys_area.top + (row + 1.0F) * row_height};
                std::wstring hint{hints[index].key};
                hint += L"  ";
                hint += hints[index].action;
                draw_text(hint, cell, key_format.Get(), faint_brush.Get());
            }
        }

        std::wstring status_text{fps_number(rendered_fps)};
        status_text += L" -> ";
        status_text += fps_number(output_fps);
        status_text += L" fps";
        draw_text(
            status_text,
            menu.status,
            option_value_format.Get(),
            muted_brush.Get());
    }

    if (menu.detail_visible) {
        constexpr std::array<std::wstring_view, kMenuRowCount> descriptions{
            L"Creates frames between the ones Skyrim renders. Higher "
            L"multipliers do not add latency on their own, but they only "
            L"look right if the base rate holds steady.",
            L"Targets a frame rate instead of a fixed multiplier. Requires "
            L"a runtime that reports dynamic support.",
            L"Caps the rendered rate. Choose a value your heaviest area can "
            L"sustain, because generated frames inherit its stability.",
            L"Selects which vendor runtime performs upscaling. The switch "
            L"happens immediately when the new runtime accepts the current "
            L"render extent, and asks for a restart only when it will not.",
            L"Trades internal resolution for performance. The full resolution "
            L"option, named DLAA on NVIDIA and Native AA elsewhere, only "
            L"removes aliasing.",
            L"Sharpening applied after upscaling. Zero disables the pass "
            L"entirely.",
            L"Reduces the queue between input and display. Which runtime is "
            L"available depends on the GPU and its driver, not on the "
            L"upscaling provider.",
            L"Caps the final presented rate, after generated frames. This "
            L"is not the base cap, and it is applied by the active low "
            L"latency runtime.",
            L"Chooses which rate the on-screen counter reports.",
            L"Renders an internal buffer instead of the scene. For "
            L"diagnosing renderer faults only.",
            L"Textures stay compressed in video memory and the graphics "
            L"card unpacks them as it draws, cutting VRAM from 6.5GB to "
            L"970MB in NVIDIA's demonstration, with the same visual detail. "
            L"That would leave far more room for high quality texture mods. "
            L"Not available yet. I am looking into whether it can work in "
            L"Skyrim at all."};

        draw_text(
            L"SELECTED",
            menu.detail_kicker,
            kicker_format.Get(),
            faint_brush.Get());
        draw_text(
            labels[selector_row],
            menu.detail_title,
            heading_format.Get(),
            white_brush.Get());
        const auto selected_unavailable = values[selector_row] == L"Unavailable";
        std::wstring body{descriptions[selector_row]};
        if (selector_row == kFrameGenerationRow) {
            const auto usable = usable_frame_generation_choices();
            if (usable != 0U && usable < kFrameGenerationChoices.size()) {
                body += L"\n\nThis adapter tops out at ";
                body += std::to_wstring(kFrameGenerationChoices[usable - 1U]);
                body += L"x. ";
                if (FsrFrameGeneration::selected()) {
                    body += L"FSR frame generation produces one generated "
                            L"frame, so higher multipliers are not offered.";
                } else if (XessFrameGeneration::selected()) {
                    body += L"That is the ceiling the XeSS runtime reports "
                            L"for this GPU.";
                } else {
                    body += L"That is the ceiling this GPU and driver report "
                            L"for DLSS frame generation.";
                }
            }
        }
        if (selector_row == kOutputCapRow) {
            const auto selected_output_cap =
                selector_frame_limit_custom ?
                    selector_custom_frame_limit : selector_frame_limit;
            if (selected_output_cap == 0U) {
                if (RendererRuntime::instance().streamline_features_ready() &&
                    streamline::FrameGeneration::instance()
                            .display_refresh_hz() != 0U) {
                    body += L"\n\nOff still paces, to the reported display "
                            L"refresh.";
                } else {
                    body += L"\n\nOff leaves presentation unpaced here.";
                }
            }
        }
        if (selected_unavailable) {
            body += L"\n\n";
            const auto vendor_generation =
                FsrFrameGeneration::selected() ||
                XessFrameGeneration::selected();
            if (selector_row == kReflexRow) {
                const auto* const latency_reason =
                    providers::unavailable_reason_text(
                        providers::LowLatencyController::instance().reason());
                if (latency_reason != nullptr && latency_reason[0] != 0) {
                    body += L"Unavailable: ";
                    body += to_wide(latency_reason);
                    body += L".";
                } else {
                    body += L"Unavailable: the NVIDIA Streamline runtime did "
                            L"not initialise, so Reflex cannot be driven from "
                            L"this session.";
                }
            } else if (selector_row == kDynamicMfgRow) {
                if (vendor_generation) {
                    body += L"Unavailable: Dynamic Multi Frame Generation is "
                            L"a DLSS feature. The selected frame generator "
                            L"does not offer it.";
                } else {
                    body += L"Unavailable: this GPU and driver do not report "
                            L"Dynamic Multi Frame Generation support.";
                }
            } else if (vendor_generation) {
                body += L"Unavailable: the selected frame generator did not "
                        L"start. Check UFGU.log for the reason it refused.";
            } else if (!RendererRuntime::instance()
                            .streamline_features_ready()) {
                body += L"Unavailable: DLSS frame generation did not "
                        L"initialise here. If this card cannot run DLSS-G, "
                        L"set VendorFrameGeneration in UFGU.ini to AMD or "
                        L"Intel.";
            } else {
                body += L"Unavailable: the active provider did not report "
                        L"this capability on this adapter.";
            }
        }
        auto* const body_format = detail_body_format != nullptr ?
            detail_body_format.Get() : detail_format.Get();
        if (write_factory != nullptr && body_format != nullptr &&
            !body.empty()) {
            const auto body_width =
                menu.detail_body.right - menu.detail_body.left;
            ComPtr<IDWriteTextLayout> body_measure;
            if (body_width > 0.0F &&
                SUCCEEDED(write_factory->CreateTextLayout(
                    body.c_str(),
                    static_cast<UINT32>(body.size()),
                    body_format,
                    body_width,
                    4096.0F,
                    body_measure.GetAddressOf())) &&
                body_measure != nullptr) {
                DWRITE_TEXT_METRICS body_metrics{};
                if (SUCCEEDED(body_measure->GetMetrics(&body_metrics))) {
                    const auto reserved =
                        menu.detail_body.bottom - menu.detail_body.top;
                    const auto overflow = body_metrics.height - reserved;
                    if (overflow > 0.0F) {
                        menu.detail_body.bottom += overflow;
                        menu.detail_options.top += overflow;
                        menu.detail_options.bottom += overflow;
                        menu.detail_effect.top += overflow;
                        menu.detail_effect.bottom += overflow;
                    }
                }
            }
        }
        draw_text(
            body,
            menu.detail_body,
            body_format,
            selected_unavailable ? warn_brush.Get() : muted_brush.Get());

        if (selector_row == kBaseCapRow) {
            const auto refresh =
                streamline::FrameGeneration::instance().display_refresh_hz();
            const auto multiplier =
                frame_generation == 0U ? 1U : frame_generation;
            d2d_context->DrawLine(
                D2D1::Point2F(
                    menu.detail_effect.left,
                    menu.detail_effect.top),
                D2D1::Point2F(
                    menu.detail_effect.right,
                    menu.detail_effect.top),
                separator_brush.Get(),
                1.0F * menu.scale);
            const auto effect_height = 21.0F * menu.scale;
            const auto cap_line = [&](
                const std::size_t index,
                const std::wstring_view name,
                const std::wstring& reading,
                ID2D1Brush* paint) {
                const Rectangle line{
                    menu.detail_effect.left,
                    menu.detail_effect.top + 8.0F * menu.scale +
                        static_cast<float>(index) * effect_height,
                    menu.detail_effect.right,
                    menu.detail_effect.top + 8.0F * menu.scale +
                        static_cast<float>(index + 1U) * effect_height};
                draw_text(name, line, summary_format.Get(), muted_brush.Get());
                draw_text(reading, line, option_value_format.Get(), paint);
            };
            cap_line(
                0U,
                L"Display refresh",
                refresh == 0U ?
                    std::wstring{L"unknown"} :
                    std::to_wstring(refresh) + L" Hz",
                label_brush.Get());
            const auto configured_base = selector_base_frame_limit_custom ?
                selector_custom_base_frame_limit : selector_base_frame_limit;
            const auto configured_cap = selector_frame_limit_custom ?
                selector_custom_frame_limit : selector_frame_limit;
            const auto submitted = reflex_limit_preserving_base(
                configured_cap,
                configured_base,
                static_cast<std::uint32_t>(multiplier));
            const auto raised = submitted > configured_cap;
            cap_line(
                1U,
                L"Output at this base",
                submitted == 0U ?
                    std::wstring{L"uncapped"} :
                    std::to_wstring(submitted) + L" fps",
                raised ? accent_brush.Get() : label_brush.Get());
            if (raised) {
                cap_line(
                    2U,
                    L"Output cap raised",
                    L"was " + std::to_wstring(configured_cap) + L" fps",
                    accent_brush.Get());
            } else if (refresh != 0U && submitted > refresh) {
                cap_line(
                    2U,
                    L"Above refresh",
                    std::to_wstring(submitted - refresh) + L" fps not shown",
                    warn_brush.Get());
            }
        }

        if (selector_row == kFrameGenerationRow) {
            d2d_context->DrawLine(
                D2D1::Point2F(
                    menu.detail_options.left,
                    menu.detail_options.top),
                D2D1::Point2F(
                    menu.detail_options.right,
                    menu.detail_options.top),
                separator_brush.Get(),
                1.0F * menu.scale);
            const auto ceiling = usable_frame_generation_choices();
            const auto option_height = 21.0F * menu.scale;
            for (std::size_t index = 0;
                 index < kFrameGenerationChoices.size();
                 ++index) {
                const auto multiplier = kFrameGenerationChoices[index];
                const auto supported = index < ceiling;
                const auto current =
                    index == selector_frame_generation_index;
                const Rectangle line{
                    menu.detail_options.left,
                    menu.detail_options.top + 8.0F * menu.scale +
                        static_cast<float>(index) * option_height,
                    menu.detail_options.right,
                    menu.detail_options.top + 8.0F * menu.scale +
                        static_cast<float>(index + 1U) * option_height};
                const auto name =
                    multiplier == 0U ?
                        std::wstring{L"Off"} :
                        std::to_wstring(multiplier) + L"x";
                auto* const brush =
                    !supported ? disabled_brush.Get() :
                        current ? accent_brush.Get() : value_brush.Get();
                draw_text(name, line, option_format.Get(), brush);
                const auto base_known = projection_is_measurable(rendered_fps);
                const auto active_cap = selector_frame_limit_custom ?
                    selector_custom_frame_limit : selector_frame_limit;
                const auto active_base_cap = selector_base_frame_limit_custom ?
                    selector_custom_base_frame_limit : selector_base_frame_limit;
                const auto reference = reference_base_fps(
                    rendered_fps,
                    static_cast<std::uint32_t>(frame_generation),
                    reflex_limit_preserving_base(
                        static_cast<std::uint32_t>(active_cap),
                        static_cast<std::uint32_t>(active_base_cap),
                        static_cast<std::uint32_t>(frame_generation)),
                    static_cast<std::uint32_t>(active_base_cap));
                const auto projection = project_multiplier(
                    reference,
                    static_cast<std::uint32_t>(multiplier),
                    reflex_limit_preserving_base(
                        static_cast<std::uint32_t>(active_cap),
                        static_cast<std::uint32_t>(active_base_cap),
                        static_cast<std::uint32_t>(multiplier)));
                std::wstring reading =
                    fps_number(projection.output_fps) + L" fps";
                const auto implied_differs =
                    projection.implied_base_fps !=
                    static_cast<std::uint32_t>(active_base_cap);
                if (projection.cap_binds && multiplier > 1U &&
                    implied_differs) {
                    reading += L"  base ";
                    reading += fps_number(projection.implied_base_fps);
                }
                draw_text(
                    !supported ? std::wstring{L"not supported"} :
                        base_known ? reading : std::wstring{L"--"},
                    line,
                    option_value_format.Get(),
                    projection.base_is_thin && supported ? warn_brush.Get() :
                        brush);
            }

            d2d_context->DrawLine(
                D2D1::Point2F(
                    menu.detail_effect.left,
                    menu.detail_effect.top),
                D2D1::Point2F(
                    menu.detail_effect.right,
                    menu.detail_effect.top),
                separator_brush.Get(),
                1.0F * menu.scale);
            const auto effect_height = 21.0F * menu.scale;
            const auto effect_line = [&](
                const std::size_t index,
                const std::wstring_view name,
                const std::wstring& reading) {
                const Rectangle line{
                    menu.detail_effect.left,
                    menu.detail_effect.top + 8.0F * menu.scale +
                        static_cast<float>(index) * effect_height,
                    menu.detail_effect.right,
                    menu.detail_effect.top + 8.0F * menu.scale +
                        static_cast<float>(index + 1U) * effect_height};
                draw_text(name, line, summary_format.Get(), muted_brush.Get());
                draw_text(
                    reading,
                    line,
                    option_value_format.Get(),
                    label_brush.Get());
            };
            const auto base_measured = rendered_fps >= 5.0F;
            effect_line(
                0U,
                L"Current base framerate",
                base_measured ?
                    fps_number(rendered_fps) + L" fps" :
                    std::wstring{L"measuring"});
            const auto latency_ms =
                base_measured ? 1000.0F / rendered_fps : 0.0F;
            std::wstring latency_text{
                std::to_wstring(static_cast<int>(latency_ms * 10.0F) / 10)};
            latency_text += L".";
            latency_text += std::to_wstring(
                static_cast<int>(latency_ms * 10.0F) % 10);
            latency_text += L" ms";
            effect_line(
                1U,
                frame_generation == 0U ? L"Latency" : L"Added latency",
                !base_measured ? std::wstring{L"--"} :
                    frame_generation == 0U ?
                        std::wstring{L"none"} : latency_text);

            const auto memory = D3D12Backend::instance().video_memory_status();
            const auto gigabytes = [](const std::uint64_t bytes) {
                const auto tenths = (bytes * 10ULL) / (1024ULL * 1024ULL * 1024ULL);
                return std::to_wstring(tenths / 10ULL) + L"." +
                    std::to_wstring(tenths % 10ULL);
            };
            const auto free_bytes =
                memory.budget_bytes > memory.current_usage_bytes
                    ? memory.budget_bytes - memory.current_usage_bytes
                    : 0ULL;
            effect_line(
                2U,
                L"Video memory free",
                !memory.available ?
                    std::wstring{L"--"} :
                    gigabytes(free_bytes) + L" of " +
                        gigabytes(memory.budget_bytes) + L" GB");

            const auto pacing =
                frame_series.statistics(series_clock_seconds, 2.0F);
            effect_line(
                3U,
                L"Pacing",
                pacing.sample_count < 30U ? std::wstring{L"measuring"} :
                    pacing.pacing_deviation_ratio <= 0.25F ?
                        std::wstring{L"even"} :
                        std::wstring{L"uneven"});

            auto& presentation = PresentationBridge::instance();
            const auto render_w = presentation.render_width();
            const auto render_h = presentation.render_height();
            const auto output_w = presentation.output_width();
            const auto output_h = presentation.output_height();
            const auto extent = [](const std::uint32_t width,
                                   const std::uint32_t height) {
                return std::to_wstring(width) + L"x" + std::to_wstring(height);
            };
            effect_line(
                4U,
                L"Resolution",
                (output_w == 0U || output_h == 0U) ?
                    std::wstring{L"--"} :
                    (render_w == 0U || render_h == 0U ||
                     (render_w == output_w && render_h == output_h)) ?
                        extent(output_w, output_h) + L" native" :
                        extent(render_w, render_h) + L" to " +
                            extent(output_w, output_h));
        }
    }

    if (selector_section == section_of_row(kDebugRow)) {
        d2d_context->DrawLine(
            D2D1::Point2F(
                menu.debug_panel.left,
                menu.debug_panel.top - 10.0F * menu.scale),
            D2D1::Point2F(
                menu.debug_panel.right,
                menu.debug_panel.top - 10.0F * menu.scale),
            separator_brush.Get(),
            1.0F * menu.scale);
    }
    if (selector_section == section_of_row(kDebugRow)) {
        const auto selected_view = kDebugViewChoices[selector_debug_index];
        const auto& debug = DebugViewPass::instance();
        const auto info = debug.source_info();
        const auto* reason = debug.unavailable_reason();
        const auto& dynamic_resolution = DynamicResolution::instance();

        const auto pair = [](const std::uint32_t a, const std::uint32_t b) {
            return std::to_wstring(a) + L"x" + std::to_wstring(b);
        };

        const auto active_view = DebugViewPass::selected_view();
        const auto staged = selected_view != active_view;

        std::array<std::wstring, 12> lines{};
        std::size_t count = 0;
        const auto append_line = [&lines, &count](std::wstring line) {
            if (count < lines.size()) {
                lines[count++] = std::move(line);
            }
        };

        int flagged_line = -1;

        const auto streamline_control =
            selector_row == kFrameGenerationRow ||
            selector_row == kReflexRow ||
            selector_row == kOutputCapRow;
        if (streamline_control && !streamline_available) {
            append_line(
                L"Unavailable: Streamline presentation features did not "
                L"initialize on this adapter. Native FSR/XeSS upscaling "
                L"remains available.");
            append_line(
                L"This control is disabled and Apply preserves its saved "
                L"value for a future compatible NVIDIA session.");
        } else if (selector_row == kBaseCapRow) {
            append_line(
                L"Caps Skyrim's real rendered FPS for stable Fixed Frame "
                L"Generation pacing. Choose a rate the game can maintain in "
                L"its heaviest areas.");
            const auto selected_base_limit =
                selector_base_frame_limit_custom ?
                    selector_custom_base_frame_limit :
                    selector_base_frame_limit;
            if (selected_base_limit == 0U) {
                append_line(L"Off: Skyrim's real render loop is uncapped.");
            } else if (selector_dynamic_mfg_index != 0U) {
                append_line(
                    L"Dynamic MFG bypasses this cap automatically while its "
                    L"runtime-controlled cadence is active.");
            } else {
                append_line(
                    L"The cap applies to real frames only; generated output "
                    L"frames remain controlled separately.");
            }
        } else if (selector_row == kOutputCapRow) {
            append_line(
                L"Final display cap. Arrows select presets or Custom. "
                L"Click Custom, type a value, then press Enter or Apply.");
            const auto selected_frame_limit =
                selector_frame_limit_custom ?
                    selector_custom_frame_limit : selector_frame_limit;
            if (selected_frame_limit == 0U) {
                append_line(
                    L"Uncapped: presentation cadence is not limited by this "
                    L"plugin.");
            }

            {
                const auto& super_resolution =
                    streamline::SuperResolution::instance();
                const auto render_width = super_resolution.render_width();
                const auto render_height = super_resolution.render_height();
                const auto output_width =
                    PresentationBridge::instance().output_width();
                const auto output_height =
                    PresentationBridge::instance().output_height();
                if (render_width != 0U && render_height != 0U &&
                    output_width != 0U && output_height != 0U) {
                    append_line(
                        L"Rendering " + std::to_wstring(render_width) +
                        L" x " + std::to_wstring(render_height) +
                        L"  ->  " + std::to_wstring(output_width) +
                        L" x " + std::to_wstring(output_height) +
                        (render_width == output_width &&
                                 render_height == output_height ?
                             L" (native, no upscale)" :
                             L""));
                }
            }
        } else if (selector_row == kReflexRow) {

            const auto& controller =
                providers::LowLatencyController::instance();
            if (!controller.available()) {
                append_line(
                    std::wstring{L"Unavailable: "} +
                    to_wide(providers::unavailable_reason_text(
                        controller.reason())));
            }
        } else if (selector_row == kProviderRow) {
            const auto selected_provider =
                kUpscalingProviders[selector_provider_index];
            const auto active_provider =
                streamline::SuperResolution::instance().provider();
            append_line(
                L"Provider changes are saved for the next Skyrim launch; "
                L"the active renderer and its resources are never replaced "
                L"mid-session.");
            append_line(
                std::wstring{L"Selected: "} +
                std::wstring{upscaling_provider_name(selected_provider)} +
                L"   Active: " +
                std::wstring{upscaling_provider_name(active_provider)});
        } else {
            append_line(std::wstring{debug_view_description(selected_view)});
        }

        if (staged) {
            append_line(
                std::wstring{L"STAGED: press Apply to activate. Currently "
                             L"active: "} +
                std::wstring{debug_view_name(active_view)} + L".");
        }

        if (selected_view == DebugView::off) {
            append_line(
                L"Inactive: no shader, no resource and no GPU work. The "
                L"production render path is untouched.");
        } else if (staged) {

        } else if (!info.valid) {
            append_line(
                std::wstring{L"Unavailable: "} +
                (reason != nullptr ?
                     to_wide(reason) : std::wstring{L"not drawn yet"}));
        } else {
            append_line(
                L"Source " + pair(info.source_width, info.source_height) +
                L"  DXGI format " + std::to_wstring(info.source_format) +
                L" sampled as " + std::to_wstring(info.sampled_format));
            append_line(
                L"Render extent " +
                pair(info.active_width, info.active_height) +
                L"   Output " +
                pair(info.output_width, info.output_height));
            append_line(
                L"Motion scale " +
                std::to_wstring(static_cast<int>(info.motion_scale_x)) +
                L", " +
                std::to_wstring(static_cast<int>(info.motion_scale_y)) +
                (info.reversed_depth ?
                     L"   Depth: reversed-Z (1.0 at the near plane)" :
                     L"   Depth: forward-Z (0.0 at the near plane)"));
            append_line(
                std::wstring{L"Depth convention measured from Skyrim's "
                             L"projection: "} +
                to_wide(describe(CameraData::instance().depth_orientation())));

            if (selected_view == DebugView::motion_vectors) {
                const auto& motion = debug.motion_statistics();
                if (!motion.valid) {
                    append_line(L"Vector statistics: measuring...");
                } else {
                    const auto sampled =
                        motion.sampled_pixels == 0 ? 1U : motion.sampled_pixels;
                    const auto near_zero_percent =
                        100.0 * static_cast<double>(motion.near_zero_pixels) /
                        static_cast<double>(sampled);
                    append_line(
                        L"Vectors: max " +
                        to_wide(std::format("{:.5f}", motion.maximum)) +
                        L"  p50 " +
                        to_wide(std::format(
                            "{:.5f}",
                            motion.bucket_percentile(0.5F) *
                                0.05F)) +
                        L"  p99 " +
                        to_wide(std::format(
                            "{:.5f}",
                            motion.bucket_percentile(0.99F) * 0.05F)) +
                        L"  (screen-space units, histogram-derived)");
                    append_line(
                        L"Stationary " +
                        to_wide(std::format("{:.1f}%", near_zero_percent)) +
                        L"   non-finite " +
                        std::to_wstring(motion.nonfinite_pixels) +
                        L"   direction R/D " +
                        std::to_wstring(motion.quadrant_right_down) +
                        L" R/U " + std::to_wstring(motion.quadrant_right_up) +
                        L" L/D " + std::to_wstring(motion.quadrant_left_down) +
                        L" L/U " + std::to_wstring(motion.quadrant_left_up));
                }
            }
        }

        if (!dynamic_resolution.extent_observation_armed()) {
            append_line(
                L"Extent observation: off (armed only while a debug view is "
                L"selected).");
        } else if (dynamic_resolution.extent_violation_observed()) {
            const auto* violation =
                dynamic_resolution.extent_violation_reason();
            std::int32_t left{};
            std::int32_t top{};
            std::int32_t right{};
            std::int32_t bottom{};
            auto text = std::wstring{L"CORRECTED: "} +
                (violation != nullptr ?
                     to_wide(violation) : std::wstring{L"unspecified"});
            if (dynamic_resolution.extent_violation_rectangle(
                    left, top, right, bottom)) {
                text += L"  rect " + std::to_wstring(left) + L"," +
                    std::to_wstring(top) + L".." + std::to_wstring(right) +
                    L"," + std::to_wstring(bottom);
            }

            text += L"  (within " +
                std::to_wstring(debug.frames_since_select()) +
                L" frames of selecting this view)";
            append_line(std::move(text));
        } else {
            append_line(
                L"Extent observation: armed, nothing corrected in the " +
                std::to_wstring(debug.frames_since_select()) +
                L" frames since this view was selected.");
        }

        if (apply_frames_remaining > 0 && count < lines.size() &&
            apply_outcome != ApplyOutcome::none) {

            append_line(
                std::wstring{banner_prefix(apply_outcome)} + apply_message);
            flagged_line = is_flagged(apply_outcome) ?
                static_cast<int>(count - 1U) :
                flagged_line;
            --apply_frames_remaining;
        }

        const auto line_height = 19.0F * menu.scale;
        const auto left = menu.debug_panel.left + 14.0F * menu.scale;
        const auto right = menu.debug_panel.right - 14.0F * menu.scale;
        auto top = menu.debug_panel.top + 9.0F * menu.scale;

        draw_text(
            lines[0],
            {left, top, right, top + line_height * 2.0F},
            detail_format.Get(),
            muted_brush.Get());
        top += line_height * 2.0F + 4.0F * menu.scale;

        const auto approximate_character_width = 8.5F * menu.scale;
        const auto wrapped_rows = [&](const std::wstring& text) {
            const auto available = right - left;
            if (available <= 0.0F || text.empty()) {
                return 1.0F;
            }
            const auto needed =
                static_cast<float>(text.size()) * approximate_character_width;
            const auto rows = std::ceil(needed / available);
            return (std::clamp)(rows, 1.0F, 4.0F);
        };

        for (std::size_t line = 1; line < count; ++line) {
            const auto rows = wrapped_rows(lines[line]);
            const auto height = line_height * rows;
            if (top + height > menu.debug_panel.bottom) {
                break;
            }

            const auto flagged =
                lines[line].rfind(L"CORRECTED", 0) == 0 ||
                static_cast<int>(line) == flagged_line;
            draw_text(
                lines[line],
                {left, top, right, top + height},
                detail_format.Get(),
                flagged ? accent_brush.Get() : label_brush.Get());
            top += height + 2.0F * menu.scale;
        }
    }

    const auto apply_hover = cursor_over(menu.apply);
    d2d_context->FillRoundedRectangle(
        rounded(menu.apply, menu.corner_radius),
        apply_hover ?
            section_active_brush.Get() :
            control_hover_brush.Get());
    d2d_context->DrawRoundedRectangle(
        rounded(menu.apply, menu.corner_radius),
        separator_brush.Get(),
        1.0F * menu.scale);
    draw_text(
        L"Apply",
        menu.apply,
        apply_format.Get(),
        apply_hover ? white_brush.Get() : label_brush.Get());

    d2d_context->SetTransform(D2D1::Matrix3x2F::Identity());
    d2d_context->PopLayer();
}

void StatusOverlay::State::draw_cursor(const float scale)
{
    D2D1_MATRIX_3X2_F previous{};
    d2d_context->GetTransform(&previous);
    d2d_context->SetTransform(
        D2D1::Matrix3x2F::Scale(scale, scale) *
        D2D1::Matrix3x2F::Translation(
            cursor_x + 2.0F * scale,
            cursor_y + 2.0F * scale));
    d2d_context->FillGeometry(
        cursor_geometry.Get(),
        cursor_shadow_brush.Get());
    d2d_context->SetTransform(
        D2D1::Matrix3x2F::Scale(scale, scale) *
        D2D1::Matrix3x2F::Translation(cursor_x, cursor_y));
    d2d_context->FillGeometry(
        cursor_geometry.Get(),
        white_brush.Get());
    d2d_context->DrawGeometry(
        cursor_geometry.Get(),
        glass_border_brush.Get(),
        0.75F);
    d2d_context->SetTransform(previous);
}

bool StatusOverlay::State::prepare_ui_layer_target(
    ID3D11Texture2D* const ui_layer)
{
    D3D11_TEXTURE2D_DESC description{};
    ui_layer->GetDesc(&description);
    if (ui_layer_target == ui_layer &&
        ui_layer_width == description.Width &&
        ui_layer_height == description.Height &&
        ui_layer_bitmap != nullptr) {
        return true;
    }
    ui_layer_bitmap.Reset();
    ui_layer_target = nullptr;
    ui_layer_width = 0U;
    ui_layer_height = 0U;

    ComPtr<IDXGISurface> surface;
    auto result = ui_layer->QueryInterface(IID_PPV_ARGS(&surface));
    if (SUCCEEDED(result)) {

        const auto properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(
                description.Format, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0F,
            96.0F);
        result = d2d_context->CreateBitmapFromDxgiSurface(
            surface.Get(), &properties, &ui_layer_bitmap);
    }
    if (FAILED(result) || ui_layer_bitmap == nullptr) {
        ui_layer_bitmap.Reset();
        if (!ui_layer_failure_logged) {
            ui_layer_failure_logged = true;
            logger::warn(
                "The overlay could not target the UI colour/alpha layer "
                "(0x{:08X}); the frame counter will be absent from generated "
                "frames on generators that interpolate the HUD-less colour",
                static_cast<unsigned>(result));
        }
        return false;
    }
    ui_layer_target = ui_layer;
    ui_layer_width = description.Width;
    ui_layer_height = description.Height;
    return true;
}

void StatusOverlay::State::render_into_ui_layer(
    ID3D11DeviceContext* const context,
    ID3D11Texture2D* const ui_layer)
{
    if (!config::Settings::instance().overlay_enabled() && !selector_open &&
        !closing_active()) {

        return;
    }
    if (!prepare_ui_layer_target(ui_layer)) {
        return;
    }

    ComPtr<ID3D11RenderTargetView> previous_target;
    ComPtr<ID3D11DepthStencilView> previous_depth;
    ID3D11RenderTargetView* previous_target_pointer{};
    ID3D11DepthStencilView* previous_depth_pointer{};
    context->OMGetRenderTargets(
        1, &previous_target_pointer, &previous_depth_pointer);
    previous_target.Attach(previous_target_pointer);
    previous_depth.Attach(previous_depth_pointer);
    context->OMSetRenderTargets(0, nullptr, nullptr);

    d2d_context->SetTarget(ui_layer_bitmap.Get());
    d2d_context->BeginDraw();
    d2d_context->SetTransform(D2D1::Matrix3x2F::Identity());
    draw_counter(ui_layer_width, ui_layer_height);
    const auto result = d2d_context->EndDraw();
    d2d_context->SetTarget(nullptr);

    previous_target_pointer = previous_target.Get();
    previous_depth_pointer = previous_depth.Get();
    context->OMSetRenderTargets(
        previous_target_pointer != nullptr ? 1U : 0U,
        previous_target_pointer != nullptr ? &previous_target_pointer : nullptr,
        previous_depth_pointer);

    if (result == D2DERR_RECREATE_TARGET) {
        ui_layer_bitmap.Reset();
        ui_layer_target = nullptr;
    }
}

void StatusOverlay::State::render(
    ID3D11DeviceContext* context,
    ID3D11Texture2D* back_buffer)
{
    update_metrics();
    update_selector(target_width, target_height);
    if (!selector_open && !closing_active() &&
        !config::Settings::instance().overlay_enabled()) {
        return;
    }

    ComPtr<ID3D11RenderTargetView> previous_target;
    ComPtr<ID3D11DepthStencilView> previous_depth;
    ID3D11RenderTargetView* previous_target_pointer{};
    ID3D11DepthStencilView* previous_depth_pointer{};
    context->OMGetRenderTargets(
        1,
        &previous_target_pointer,
        &previous_depth_pointer);
    previous_target.Attach(previous_target_pointer);
    previous_depth.Attach(previous_depth_pointer);
    context->OMSetRenderTargets(0, nullptr, nullptr);

    if (selector_open && snapshot_texture != nullptr) {
        context->CopyResource(
            snapshot_texture.Get(),
            back_buffer);
        snapshot_valid = true;
    }

    d2d_context->SetTarget(target_bitmap.Get());
    d2d_context->BeginDraw();
    d2d_context->SetTransform(D2D1::Matrix3x2F::Identity());
    if (selector_open || closing_active()) {
        const auto menu = make_menu_layout(
            target_width,
            target_height,
            selector_section,
            config::Settings::instance().menu_presentation() !=
                config::MenuPresentation::full,
            anchor_for(config::Settings::instance().menu_presentation()));
        draw_menu(menu);
        if (selector_open) {
            draw_cursor((std::max)(menu.scale * 0.75F, 0.8F));
        }
    } else {
        draw_counter(target_width, target_height);
    }
    const auto result = d2d_context->EndDraw();

    previous_target_pointer = previous_target.Get();
    previous_depth_pointer = previous_depth.Get();
    context->OMSetRenderTargets(
        previous_target_pointer != nullptr ? 1U : 0U,
        previous_target_pointer != nullptr ?
            &previous_target_pointer :
            nullptr,
        previous_depth_pointer);

    if (result == D2DERR_RECREATE_TARGET) {
        release_target_resources();
    } else if (FAILED(result) && !rendering_failure_logged) {
        rendering_failure_logged = true;
        logger::error(
            "Native acrylic UI draw failed: 0x{:08X}",
            static_cast<unsigned>(result));
    } else if (SUCCEEDED(result)) {
        rendering_failure_logged = false;
    }

    if (!first_draw_logged && SUCCEEDED(result)) {
        first_draw_logged = true;
        logger::info(
            "Native Direct2D acrylic UFGU UI rendered at {}x{} "
            "with Segoe UI Variable",
            target_width,
            target_height);
    }
}

StatusOverlay& StatusOverlay::instance() noexcept
{
    static StatusOverlay overlay;
    return overlay;
}

bool StatusOverlay::install_input()
{
    if (input_installed_) {
        return true;
    }
    auto* input = RE::BSInputDeviceManager::GetSingleton();
    if (input == nullptr) {
        return false;
    }
    if (state_ == nullptr) {
        state_ = std::make_unique<State>();
    }
    input->AddEventSink(this);
    input_installed_ = true;
    logger::info(
        "Native acrylic settings raw-mouse input sink installed");
    return true;
}

bool StatusOverlay::menu_open() const noexcept
{
    return state_ != nullptr &&
           state_->input_capture.load(std::memory_order_acquire);
}

RE::BSEventNotifyControl StatusOverlay::ProcessEvent(
    RE::InputEvent* const* events,
    RE::BSTEventSource<RE::InputEvent*>*)
{
    if (state_ == nullptr ||
        !state_->input_capture.load(std::memory_order_acquire) ||
        events == nullptr) {
        return RE::BSEventNotifyControl::kContinue;
    }

    for (auto* event = *events; event != nullptr;
         event = event->next) {
        if (event->GetDevice() != RE::INPUT_DEVICE::kMouse) {
            continue;
        }
        if (const auto* motion = event->AsMouseMoveEvent();
            motion != nullptr) {
            state_->mouse_delta_x.fetch_add(
                motion->mouseInputX,
                std::memory_order_relaxed);
            state_->mouse_delta_y.fetch_add(
                motion->mouseInputY,
                std::memory_order_relaxed);
        } else if (
            const auto* button = event->AsButtonEvent();
            button != nullptr &&
            button->GetIDCode() == 0U) {
            state_->mouse_button_down.store(
                button->IsDown(),
                std::memory_order_release);
            if (button->IsDown()) {
                state_->mouse_click_pending.store(
                    true,
                    std::memory_order_release);
            }
        }
    }
    return RE::BSEventNotifyControl::kStop;
}

void StatusOverlay::draw(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* back_buffer)
{
    if (device == nullptr ||
        context == nullptr ||
        back_buffer == nullptr) {
        return;
    }
    if (state_ == nullptr) {
        state_ = std::make_unique<State>();
    }
    if (state_->initialize(device, back_buffer)) {
        state_->render(context, back_buffer);
    }
}

void StatusOverlay::draw_into_ui_layer(
    ID3D11DeviceContext* context,
    ID3D11Texture2D* ui_layer)
{

    if (context == nullptr || ui_layer == nullptr || state_ == nullptr) {
        return;
    }
    state_->render_into_ui_layer(context, ui_layer);
}

void StatusOverlay::shutdown() noexcept
{
    if (input_installed_) {
        if (auto* input =
                RE::BSInputDeviceManager::GetSingleton();
            input != nullptr) {
            input->RemoveEventSink(this);
        }
        input_installed_ = false;
    }
    state_.reset();
}
}
