#include "render/UpscalingPass.hpp"

#include "config/Settings.hpp"
#include "providers/FsrUpscaler.hpp"
#include "providers/XessUpscaler.hpp"
#include "render/CameraData.hpp"
#include "render/DebugViewPass.hpp"
#include "render/DepthContract.hpp"
#include "render/DepthDiagnostics.hpp"
#include "render/DepthRestoration.hpp"
#include "render/DynamicResolution.hpp"
#include "render/FsrFrameGeneration.hpp"
#include "render/MainDepthTracker.hpp"
#include "render/XessFrameGeneration.hpp"
#include "render/NativeUiPolicy.hpp"
#include "render/PresentationBridge.hpp"
#include "render/RenderDebug.hpp"
#include "render/RuntimeCompatibilitySkse.hpp"
#include "render/ScaleformBoundary.hpp"
#include "render/SharpeningPass.hpp"
#include "render/SharedResources.hpp"
#include "render/SurfaceBlit.hpp"
#include "render/UiCompositePass.hpp"
#include "streamline/SuperResolution.hpp"
#include "streamline/TemporalInputPolicy.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <SKSE/SKSE.h>

#include <sl_consts.h>

#include <array>
#include <algorithm>
#include <cstddef>
#include <cmath>
#include <string_view>

namespace mfgdlss::render
{
namespace
{
namespace logger = SKSE::log;
using Microsoft::WRL::ComPtr;

void reset_vendor_generation_history(const char* const reason)
{
    const char* generator = nullptr;
    if (XessFrameGeneration::selected() &&
        XessFrameGeneration::instance().owns_presentation()) {
        XessFrameGeneration::instance().reset_history();
        generator = "XeSS-FG";
    } else if (
        FsrFrameGeneration::selected() &&
        FsrFrameGeneration::instance().owns_presentation()) {
        FsrFrameGeneration::instance().reset_history();
        generator = "FidelityFX frame generation";
    }
    if (generator != nullptr) {
        logger::info(
            "{} interpolation history reset because {}",
            generator,
            reason);
    }
}

[[nodiscard]] ID3D11Texture2D* main_depth_texture(
    RE::BSGraphics::RendererData* const renderer) noexcept
{
    const auto* const compatibility = active_runtime_profile();
    if (renderer == nullptr || compatibility == nullptr) {
        return nullptr;
    }
    if (compatibility->version == kSkyrim1597.version) {
        return renderer->depthStencils[
            RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].texture;
    }
    return MainDepthTracker::instance().texture();
}

void set_dirty_states(const bool compute_shader)
{
    using Function = void(bool);
    static REL::Relocation<Function> function{
        REL::RelocationID(75580, 77386)};
    function(compute_shader);
}

void unbind_shader_resources(ID3D11DeviceContext* context)
{
    if (context == nullptr) {
        return;
    }
    constexpr auto count =
        D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
    const std::array<ID3D11ShaderResourceView*, count> empty{};
    context->VSSetShaderResources(0, count, empty.data());
    context->HSSetShaderResources(0, count, empty.data());
    context->DSSetShaderResources(0, count, empty.data());
    context->GSSetShaderResources(0, count, empty.data());
    context->PSSetShaderResources(0, count, empty.data());
    context->CSSetShaderResources(0, count, empty.data());
    constexpr auto unordered_count =
        D3D11_PS_CS_UAV_REGISTER_COUNT;
    const std::array<ID3D11UnorderedAccessView*, unordered_count>
        no_unordered_outputs{};
    context->CSSetUnorderedAccessViews(
        0,
        unordered_count,
        no_unordered_outputs.data(),
        nullptr);
}

[[nodiscard]] bool same_shape(
    const D3D11_TEXTURE2D_DESC& left,
    const D3D11_TEXTURE2D_DESC& right) noexcept
{
    return left.Width == right.Width &&
           left.Height == right.Height &&
           left.MipLevels == right.MipLevels &&
           left.ArraySize == right.ArraySize &&
           left.Format == right.Format &&
           left.SampleDesc.Count == right.SampleDesc.Count &&
           left.SampleDesc.Quality == right.SampleDesc.Quality;
}

[[nodiscard]] bool create_texture(
    ID3D11Device* device,
    D3D11_TEXTURE2D_DESC description,
    const UINT bind_flags,
    ComPtr<ID3D11Texture2D>& texture)
{
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = bind_flags;
    description.CPUAccessFlags = 0;
    description.MiscFlags = 0;
    return SUCCEEDED(
        device->CreateTexture2D(&description, nullptr, &texture));
}

[[nodiscard]] providers::QualityMode provider_quality(
    const config::UpscalingMode mode) noexcept
{
    using config::UpscalingMode;
    using providers::QualityMode;
    switch (mode) {
    case UpscalingMode::dlaa:
        return QualityMode::native_antialiasing;
    case UpscalingMode::quality:
        return QualityMode::quality;
    case UpscalingMode::balanced:
        return QualityMode::balanced;
    case UpscalingMode::performance:
        return QualityMode::performance;
    case UpscalingMode::ultra_performance:
        return QualityMode::ultra_performance;
    case UpscalingMode::off:
        return QualityMode::off;
    }
    return QualityMode::off;
}

}

struct UpscalingPass::State
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> color_input;
    ComPtr<ID3D11Texture2D> depth_input;
    ComPtr<ID3D11Texture2D> motion_input;
    ComPtr<ID3D11Texture2D> bias_input;
    ComPtr<ID3D11Texture2D> transparency_input;
    ComPtr<ID3D11Texture2D> color_output;
    ComPtr<ID3D11UnorderedAccessView> color_output_view;
    D3D11_TEXTURE2D_DESC description{};
    std::uint32_t render_width{};
    std::uint32_t render_height{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};
    bool output_extent_sampled{};
    [[nodiscard]] bool initialize(
        ID3D11Device* source_device,
        ID3D11DeviceContext* source_context,
        const D3D11_TEXTURE2D_DESC& source,
        const std::uint32_t source_render_width,
        const std::uint32_t source_render_height,
        const std::uint32_t source_output_width,
        const std::uint32_t source_output_height)
    {
        if (source_device == nullptr || source_context == nullptr ||
            source.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
            source.Width == 0 ||
            source.Height == 0 ||
            source_render_width == 0 ||
            source_render_height == 0 ||
            source_output_width == 0 ||
            source_output_height == 0 ||
            source_render_width > source.Width ||
            source_render_height > source.Height ||
            source_output_width != source.Width ||
            source_output_height != source.Height ||
            source.SampleDesc.Count != 1) {
            logger::error(
                "Native DLSS pre-postprocess scene texture is incompatible: "
                "resource={}x{}, render={}x{}, output={}x{}, "
                "format={}, samples={}",
                source.Width,
                source.Height,
                source_render_width,
                source_render_height,
                source_output_width,
                source_output_height,
                static_cast<unsigned>(source.Format),
                source.SampleDesc.Count);
            return false;
        }

        device = source_device;
        context = source_context;
        description = source;
        render_width = source_render_width;
        render_height = source_render_height;
        output_width = source_output_width;
        output_height = source_output_height;

        auto input_description = source;
        input_description.Width = render_width;
        input_description.Height = render_height;
        input_description.MipLevels = 1;
        input_description.ArraySize = 1;
        input_description.SampleDesc = {1, 0};

        auto output_description = source;
        output_description.Width = output_width;
        output_description.Height = output_height;
        output_description.MipLevels = 1;
        output_description.ArraySize = 1;
        output_description.SampleDesc = {1, 0};
        if (!create_texture(
                device.Get(),
                input_description,
                D3D11_BIND_SHADER_RESOURCE,
                color_input) ||
            !create_texture(
                device.Get(),
                output_description,
                D3D11_BIND_SHADER_RESOURCE |
                    D3D11_BIND_UNORDERED_ACCESS |
                    D3D11_BIND_RENDER_TARGET,
                color_output) ||
            FAILED(device->CreateUnorderedAccessView(
                color_output.Get(),
                nullptr,
                &color_output_view))) {
            logger::error("Unable to create native DLSS color textures");
            return false;
        }

        logger::info(
            "Native D3D11 HDR DLSS resources ready: exact-input={}x{}, "
            "output={}x{} R16G16B16A16_FLOAT",
            render_width,
            render_height,
            output_width,
            output_height);
        return true;
    }

    [[nodiscard]] bool initialize_present(
        ID3D11Device* source_device,
        ID3D11DeviceContext* source_context,
        const D3D11_TEXTURE2D_DESC& source,
        const D3D11_TEXTURE2D_DESC& output,
        const std::uint32_t source_render_width,
        const std::uint32_t source_render_height,
        const std::uint32_t source_output_width,
        const std::uint32_t source_output_height)
    {
        const auto display_format =
            source.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
            source.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
            source.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
            source.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

        if (source_device == nullptr || source_context == nullptr ||
            !display_format ||
            source_render_width == 0 ||
            source_render_height == 0 ||
            source_render_width > source.Width ||
            source_render_height > source.Height ||
            output.Width != source_output_width ||
            output.Height != source_output_height ||
            source.Format != output.Format ||
            source.SampleDesc.Count != 1 ||
            output.SampleDesc.Count != 1) {
            logger::error(
                "Complete-frame DLSS surfaces are incompatible: "
                "input={}x{} format={}, render extent must fit {}x{}; "
                "output={}x{} format={}, expected={}x{}",
                source.Width,
                source.Height,
                static_cast<unsigned>(source.Format),
                source_render_width,
                source_render_height,
                output.Width,
                output.Height,
                static_cast<unsigned>(output.Format),
                source_output_width,
                source_output_height);
            return false;
        }

        device = source_device;
        context = source_context;
        description = source;
        render_width = source_render_width;
        render_height = source_render_height;
        output_width = source_output_width;
        output_height = source_output_height;

        auto input_description = source;
        input_description.MipLevels = 1;
        input_description.ArraySize = 1;
        input_description.SampleDesc = {1, 0};
        input_description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        input_description.MiscFlags = 0;

        auto output_description = output;
        output_description.MipLevels = 1;
        output_description.ArraySize = 1;
        output_description.SampleDesc = {1, 0};
        output_description.BindFlags =
            D3D11_BIND_SHADER_RESOURCE |
            D3D11_BIND_UNORDERED_ACCESS |
            D3D11_BIND_RENDER_TARGET;
        output_description.MiscFlags = 0;

        if (!create_texture(
                device.Get(),
                input_description,
                input_description.BindFlags,
                color_input) ||
            !create_texture(
                device.Get(),
                output_description,
                output_description.BindFlags,
                color_output) ||
            FAILED(device->CreateUnorderedAccessView(
                color_output.Get(),
                nullptr,
                &color_output_view))) {
            logger::error(
                "Unable to create complete-frame DLSS staging surfaces");
            return false;
        }

        logger::info(
            "Complete-frame DLSS resources ready: {}x{} -> {}x{} on a "
            "{}x{} input surface, format={}",
            render_width,
            render_height,
            output_width,
            output_height,
            source.Width,
            source.Height,
            static_cast<unsigned>(source.Format));
        return true;
    }

    [[nodiscard]] ID3D11Texture2D* copy_active_input(
        ID3D11Texture2D* source,
        ComPtr<ID3D11Texture2D>& destination,
        const char* label)
    {
        if (source == nullptr) {
            return nullptr;
        }

        D3D11_TEXTURE2D_DESC source_description{};
        source->GetDesc(&source_description);

        DepthCopyInputs copy_inputs{};
        copy_inputs.source_width = source_description.Width;
        copy_inputs.source_height = source_description.Height;
        copy_inputs.destination_width = render_width;
        copy_inputs.destination_height = render_height;
        copy_inputs.depth_stencil =
            (source_description.BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0U;
        copy_inputs.sample_count = source_description.SampleDesc.Count;
        const auto plan = copy_plan_for(copy_inputs);
        if (is_refusal(plan)) {

            logger::error(
                "DLSS {} input refused: {}. resource={}x{}, required={}x{}, "
                "samples={}, depth-stencil={}",
                label,
                describe(plan),
                source_description.Width,
                source_description.Height,
                render_width,
                render_height,
                source_description.SampleDesc.Count,
                copy_inputs.depth_stencil);
            return nullptr;
        }

        auto desired = source_description;
        desired.Width = render_width;
        desired.Height = render_height;
        desired.MipLevels = 1;
        desired.ArraySize = 1;
        desired.SampleDesc = {1, 0};

        D3D11_TEXTURE2D_DESC current{};
        if (destination != nullptr) {
            destination->GetDesc(&current);
        }
        if (destination == nullptr || !same_shape(current, desired)) {
            destination.Reset();
            if (!create_texture(
                    device.Get(),
                    desired,
                    desired.BindFlags,
                    destination)) {
                logger::error(
                    "Unable to create exact-size DLSS {} input "
                    "({}x{}, format={})",
                    label,
                    render_width,
                    render_height,
                    static_cast<unsigned>(desired.Format));
                return nullptr;
            }
        }

        if (plan == DepthCopyPlan::whole_subresource) {

            context->CopySubresourceRegion(
                destination.Get(), 0, 0, 0, 0, source, 0, nullptr);
        } else {
            const D3D11_BOX active_region{
                0,
                0,
                0,
                render_width,
                render_height,
                1};
            context->CopySubresourceRegion(
                destination.Get(),
                0,
                0,
                0,
                0,
                source,
                0,
                &active_region);
        }
        return destination.Get();
    }

    [[nodiscard]] bool matches(
        const D3D11_TEXTURE2D_DESC& source,
        const std::uint32_t source_render_width,
        const std::uint32_t source_render_height,
        const std::uint32_t source_output_width,
        const std::uint32_t source_output_height) const noexcept
    {
        return same_shape(description, source) &&
               render_width == source_render_width &&
               render_height == source_render_height &&
               output_width == source_output_width &&
               output_height == source_output_height;
    }

    void sample_output_extent(
        ID3D11Texture2D* output,
        const std::string_view path)
    {
        if (output_extent_sampled || output == nullptr ||
            context == nullptr || device == nullptr) {
            return;
        }
        output_extent_sampled = true;

        D3D11_TEXTURE2D_DESC source{};
        output->GetDesc(&source);
        const auto bytes_per_pixel =
            source.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8U :
            source.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                    source.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                    source.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                    source.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ?
                4U :
                0U;
        constexpr UINT patch_size = 16;
        const auto has_right_region =
            output_width > render_width &&
            output_width - render_width >= patch_size &&
            output_height >= patch_size;
        const auto has_bottom_region =
            output_height > render_height &&
            output_height - render_height >= patch_size &&
            output_width >= patch_size;
        const auto patch_count =
            static_cast<UINT>(has_right_region) +
            static_cast<UINT>(has_bottom_region);
        if (bytes_per_pixel == 0 || patch_count == 0) {
            logger::warn(
                "DLSS output extent diagnostic skipped on {} path: "
                "format={}, render={}x{}, output={}x{}",
                path,
                static_cast<unsigned>(source.Format),
                render_width,
                render_height,
                output_width,
                output_height);
            return;
        }

        D3D11_TEXTURE2D_DESC staging{};
        staging.Width = patch_size * patch_count;
        staging.Height = patch_size;
        staging.MipLevels = 1;
        staging.ArraySize = 1;
        staging.Format = source.Format;
        staging.SampleDesc = {1, 0};
        staging.Usage = D3D11_USAGE_STAGING;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> sample;
        if (FAILED(device->CreateTexture2D(
                &staging,
                nullptr,
                &sample))) {
            logger::warn(
                "Unable to create one-time DLSS output extent diagnostic "
                "surface");
            return;
        }

        UINT destination_x{};
        if (has_right_region) {
            const auto source_y =
                (std::min)(
                    output_height - patch_size,
                    output_height / 2U);
            const D3D11_BOX box{
                output_width - patch_size,
                source_y,
                0,
                output_width,
                source_y + patch_size,
                1};
            context->CopySubresourceRegion(
                sample.Get(),
                0,
                destination_x,
                0,
                0,
                output,
                0,
                &box);
            destination_x += patch_size;
        }
        if (has_bottom_region) {
            const auto source_x =
                (std::min)(
                    output_width - patch_size,
                    output_width / 2U);
            const D3D11_BOX box{
                source_x,
                output_height - patch_size,
                0,
                source_x + patch_size,
                output_height,
                1};
            context->CopySubresourceRegion(
                sample.Get(),
                0,
                destination_x,
                0,
                0,
                output,
                0,
                &box);
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(
                sample.Get(),
                0,
                D3D11_MAP_READ,
                0,
                &mapped))) {
            logger::warn(
                "Unable to map one-time DLSS output extent diagnostic "
                "surface");
            return;
        }
        auto populated = false;
        const auto row_bytes =
            staging.Width * bytes_per_pixel;
        for (UINT y = 0; y < staging.Height && !populated; ++y) {
            const auto* row =
                static_cast<const std::byte*>(mapped.pData) +
                static_cast<std::size_t>(y) * mapped.RowPitch;
            populated = std::any_of(
                row,
                row + row_bytes,
                [](const std::byte value) {
                    return value != std::byte{};
                });
        }
        context->Unmap(sample.Get(), 0);
        logger::info(
            "DLSS output extent diagnostic on {} path: pixels outside "
            "{}x{} input are {} across the {}x{} output",
            path,
            render_width,
            render_height,
            populated ? "populated" : "still cleared",
            output_width,
            output_height);
    }
};

UpscalingPass::UpscalingPass() = default;
UpscalingPass::~UpscalingPass() = default;

UpscalingPass& UpscalingPass::instance() noexcept
{
    static UpscalingPass pass;
    return pass;
}

bool UpscalingPass::install()
{
    if (installed_) {
        return true;
    }
    const auto* const compatibility = active_runtime_profile();
    if (compatibility == nullptr) {
        return false;
    }

    const auto main_draw =
        REL::ID(compatibility->address_ids.main_world_draw).address() +
        compatibility->hooks.main_draw_call;
    const auto post_processing =
        REL::ID(compatibility->address_ids.post_processing).address() +
        compatibility->hooks.post_processing_call;
    const auto main_draw_preflight = preflight_direct_call(
        main_draw,
        compatibility->hooks.main_draw_call_signature);
    const auto post_processing_preflight = preflight_direct_call(
        post_processing,
        compatibility->hooks.post_processing_call_signature);
    const auto module_base = REL::Module::get().base();
    logger::info(
        "Hook preflight on {}: main draw call at rva 0x{:X} -> {} (target "
        "rva 0x{:X}), post-processing call at rva 0x{:X} -> {} (target rva "
        "0x{:X})",
        compatibility->name,
        main_draw - module_base,
        hook_validation_failure_name(main_draw_preflight.failure),
        main_draw_preflight.target != 0U ?
            main_draw_preflight.target - module_base : 0U,
        post_processing - module_base,
        hook_validation_failure_name(post_processing_preflight.failure),
        post_processing_preflight.target != 0U ?
            post_processing_preflight.target - module_base : 0U);
    if (!atomic_patch_allowed(
            main_draw_preflight.valid(),
            post_processing_preflight.valid())) {
        logger::error(
            "Main-draw and post-processing hooks refused on {}: the bytes at "
            "the call sites do not match this runtime profile, so patching "
            "them could corrupt the game's code. Nothing was patched",
            compatibility->name);
        return false;
    }

    auto& trampoline = SKSE::GetTrampoline();
    original_main_draw_ = trampoline.write_call<5>(
        main_draw,
        main_draw_thunk);
    original_post_processing_ = trampoline.write_call<5>(
        post_processing,
        post_processing_thunk);

    installed_ = true;
    logger::info(
        "Native HDR pre-postprocess DLSS and pre-UI capture hooks installed");
    return true;
}

ID3D11Texture2D* UpscalingPass::debug_motion_source() const noexcept
{
    if (active_upscaling_provider_ == providers::Vendor::intel ||
        active_upscaling_provider_ == providers::Vendor::amd) {
        return SharedResources::instance().vendor_motion_d3d11();
    }

    const auto* const evaluated_state =
        PresentationBridge::instance().uses_virtual_render_surface() ?
            present_state_.get() : state_.get();
    return evaluated_state != nullptr ?
        evaluated_state->motion_input.Get() : nullptr;
}

ID3D11Texture2D* UpscalingPass::debug_depth_source() const noexcept
{
    if (active_upscaling_provider_ == providers::Vendor::intel ||
        active_upscaling_provider_ == providers::Vendor::amd) {
        return SharedResources::instance().depth_d3d11();
    }
    const auto* const evaluated_state =
        PresentationBridge::instance().uses_virtual_render_surface() ?
            present_state_.get() : state_.get();
    return evaluated_state != nullptr ?
        evaluated_state->depth_input.Get() : nullptr;
}

ID3D11Texture2D* UpscalingPass::debug_raw_motion_source() const noexcept
{
    const auto* const renderer = RE::BSGraphics::Renderer::GetRendererData();
    if (renderer == nullptr) {
        return nullptr;
    }
    return renderer->renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR].texture;
}

ID3D11Texture2D* UpscalingPass::debug_generator_motion_source() const noexcept
{
    return SharedResources::instance().generator_motion_d3d11();
}

ID3D11Texture2D* UpscalingPass::debug_color_source() const noexcept
{
    if (active_upscaling_provider_ == providers::Vendor::intel ||
        active_upscaling_provider_ == providers::Vendor::amd) {
        return SharedResources::instance().hudless_color_d3d11();
    }
    const auto* const evaluated_state =
        PresentationBridge::instance().uses_virtual_render_surface() ?
            present_state_.get() : state_.get();
    return evaluated_state != nullptr ?
        evaluated_state->color_input.Get() : nullptr;
}

void UpscalingPass::shutdown() noexcept
{

    DebugViewPass::instance().shutdown();
    DepthDiagnostics::instance().shutdown();
    DepthRestoration::instance().shutdown();
    SharpeningPass::instance().shutdown();
    UiCompositePass::instance().shutdown();
    providers::FsrUpscaler::instance().shutdown();
    providers::XessUpscaler::instance().shutdown();
    state_.reset();
    present_state_.reset();
    reset_next_evaluation_ = true;
    evaluation_verified_ = false;
    evaluation_failed_ = false;
    first_evaluation_logged_ = false;
    first_callsite_logged_ = false;
    presentation_prepared_ = false;
    presentation_fallback_logged_ = false;
    attempted_this_frame_ = false;
    active_upscaling_provider_ = providers::Vendor::none;
    provider_change_logged_ = false;
    provider_failure_logged_ = false;
    provider_contract_logged_ = false;
    switch_recovery_armed_ = false;
    switch_recovery_provider_ = providers::Vendor::none;
    switch_recovery_mode_ = config::UpscalingMode::off;
    switch_recovery_render_width_ = 0;
    switch_recovery_render_height_ = 0;
    switch_recovery_frames_remaining_ = 0;
    diagnostic_mode_ = 0;
    diagnostic_viewport_width_ = 0;
    diagnostic_viewport_height_ = 0;
    retry_frames_remaining_ = 0;
}

bool UpscalingPass::evaluation_verified() const noexcept
{
    return evaluation_verified_;
}

bool UpscalingPass::evaluation_failed() noexcept
{
    if (retry_frames_remaining_ != 0) {
        --retry_frames_remaining_;
        if (retry_frames_remaining_ == 0) {
            evaluation_failed_ = false;
        }
        return true;
    }
    return evaluation_failed_;
}

void UpscalingPass::begin_frame() noexcept
{
    attempted_this_frame_ = false;
    presentation_prepared_ = false;
    scene_finalized_this_frame_ = false;
    scene_resolve_pending_ = false;
    CameraData::set_capture_frozen(false);

    static_cast<void>(CameraData::instance().measure_depth_contract());

    note_switch_recovery();

    auto& scaleform_boundary = ScaleformBoundary::instance();
    scaleform_boundary.begin_frame();
    const auto native_ui_policy = make_native_ui_policy(
        config::Settings::instance().frame_generation_enabled(),
        PresentationBridge::instance().uses_virtual_render_surface(),
        SharedResources::instance().ui_rendering());
    if (native_ui_policy.install_scaleform_boundary) {
        static_cast<void>(scaleform_boundary.ensure_installed());
    }
}

void UpscalingPass::note_switch_recovery() noexcept
{
    if (!switch_recovery_armed_) {
        return;
    }

    if (evaluation_verified_) {
        switch_recovery_armed_ = false;
        switch_recovery_provider_ = providers::Vendor::none;
        switch_recovery_mode_ = config::UpscalingMode::off;
        switch_recovery_render_width_ = 0;
        switch_recovery_render_height_ = 0;
        switch_recovery_frames_remaining_ = 0;
        logger::info(
            "Live provider switch confirmed by a completed evaluation: {} {} "
            "is rendering the visible frame",
            providers::vendor_display_name(active_upscaling_provider_),
            static_cast<std::uint32_t>(
                streamline::SuperResolution::instance().mode()));
        return;
    }

    if (switch_recovery_frames_remaining_ != 0) {
        --switch_recovery_frames_remaining_;
        return;
    }

    const auto failed_provider = active_upscaling_provider_;
    const auto restore_provider = switch_recovery_provider_;
    const auto restore_mode = switch_recovery_mode_;
    const auto restore_width = switch_recovery_render_width_;
    const auto restore_height = switch_recovery_render_height_;
    switch_recovery_armed_ = false;
    switch_recovery_provider_ = providers::Vendor::none;
    switch_recovery_mode_ = config::UpscalingMode::off;
    switch_recovery_render_width_ = 0;
    switch_recovery_render_height_ = 0;

    auto& super_resolution = streamline::SuperResolution::instance();
    std::uint32_t reprepared_width{};
    std::uint32_t reprepared_height{};
    const auto restored =
        super_resolution.prepare_provider(
            restore_provider,
            restore_mode,
            reprepared_width,
            reprepared_height) &&
        reprepared_width == restore_width &&
        reprepared_height == restore_height &&
        super_resolution.commit_prepared_provider(
            restore_provider, restore_mode, restore_width, restore_height) &&
        config::Settings::instance().commit_upscaling_selection(
            restore_provider, restore_mode);
    if (restored) {
        active_upscaling_provider_ = restore_provider;
        reset_vendor_generation_history(
            "a failed provider switch was rolled back to the previous "
            "upscaler");
        reset_next_evaluation_ = true;
        provider_change_logged_ = false;
        provider_failure_logged_ = false;
        provider_contract_logged_ = false;
        first_evaluation_logged_ = false;
        logger::error(
            "{} was selected but completed no evaluation within {} frames; "
            "the previous provider {} {} has been restored and written back "
            "to the INI",
            providers::vendor_display_name(failed_provider),
            kSwitchRecoveryFrameBudget,
            providers::vendor_display_name(restore_provider),
            static_cast<std::uint32_t>(restore_mode));
        return;
    }

    logger::critical(
        "{} completed no evaluation within {} frames and the previous "
        "provider {} {} could not be restored either; rendering continues on "
        "the spatial fallback until a new selection is applied",
        providers::vendor_display_name(failed_provider),
        kSwitchRecoveryFrameBudget,
        providers::vendor_display_name(restore_provider),
        static_cast<std::uint32_t>(restore_mode));
}

bool UpscalingPass::presentation_prepared() const noexcept
{
    return presentation_prepared_;
}

bool UpscalingPass::scene_ever_finalized() const noexcept
{
    return scene_ever_finalized_;
}

void UpscalingPass::reset_history() noexcept
{
    reset_next_evaluation_ = true;
    evaluation_verified_ = false;
    evaluation_failed_ = false;
    first_evaluation_logged_ = false;
    retry_frames_remaining_ = 0;
}

bool UpscalingPass::evaluate_present_surface(
    ID3D11Texture2D* color,
    ID3D11Texture2D* output)
{
    auto& super_resolution =
        streamline::SuperResolution::instance();
    if (active_upscaling_provider_ == providers::Vendor::none) {
        active_upscaling_provider_ = super_resolution.provider();
        if (active_upscaling_provider_ == providers::Vendor::none) {
            active_upscaling_provider_ =
                config::Settings::instance().upscaling_provider();
        }
        logger::info(
            "Production upscaling provider selected for this renderer "
            "lifetime: {}",
            providers::vendor_display_name(active_upscaling_provider_));
    }

    return evaluate_provider_surface(
        color,
        output,
        active_upscaling_provider_,
        super_resolution.mode(),
        super_resolution.render_width(),
        super_resolution.render_height(),
        super_resolution.output_width(),
        super_resolution.output_height());
}

bool UpscalingPass::request_same_extent_provider_switch(
    const providers::Vendor provider,
    const config::UpscalingMode mode,
    bool& restart_required,
    std::string& detail)
{
    restart_required = false;
    detail.clear();

    auto& super_resolution =
        streamline::SuperResolution::instance();
    const auto previous_provider = super_resolution.provider();
    const auto previous_mode = super_resolution.mode();
    if (!super_resolution.ready() ||
        previous_provider == providers::Vendor::none ||
        previous_mode == config::UpscalingMode::off) {
        detail = "the active upscaling contract is not ready";
        return false;
    }
    if (provider == providers::Vendor::none ||
        mode == config::UpscalingMode::off) {
        detail = "the requested provider/profile is not a temporal upscaler";
        return false;
    }
    if (provider == previous_provider && mode == previous_mode) {
        detail = "the requested provider/profile is already active";
        return false;
    }

    const auto render_width = super_resolution.render_width();
    const auto render_height = super_resolution.render_height();
    const auto output_width = super_resolution.output_width();
    const auto output_height = super_resolution.output_height();
    auto& presentation = PresentationBridge::instance();

    if (render_width == 0 || render_height == 0 ||
        output_width != presentation.output_width() ||
        output_height != presentation.output_height() ||
        render_width != presentation.render_width() ||
        render_height != presentation.render_height()) {
        restart_required = true;
        detail = "the active render and presentation extents disagree";
        logger::info(
            "Live provider switch deferred to the extent-changing path: "
            "super-resolution {}x{} vs presentation {}x{} inside {}x{} "
            "output; no runtime was prepared and nothing was released",
            render_width,
            render_height,
            presentation.render_width(),
            presentation.render_height(),
            output_width,
            output_height);
        return false;
    }

    if (!super_resolution.prepare_provider_at_extent(
            provider, mode, render_width, render_height)) {
        restart_required = true;
        detail =
            "the target runtime will not render at the active extent; the "
            "selection is saved for the next launch";
        logger::info(
            "Live provider switch to {} {} refused the active {}x{} extent; "
            "deferring to the next launch, incumbent {} {} untouched",
            providers::vendor_display_name(provider),
            static_cast<std::uint32_t>(mode),
            render_width,
            render_height,
            providers::vendor_display_name(previous_provider),
            static_cast<std::uint32_t>(previous_mode));
        return false;
    }

    if (!super_resolution.commit_prepared_provider(
            provider, mode, render_width, render_height)) {
        detail = "the prepared contract could not be made active";
        logger::error(
            "Live provider switch to {} {} prepared successfully but could "
            "not be committed; the incumbent {} {} is untouched",
            providers::vendor_display_name(provider),
            static_cast<std::uint32_t>(mode),
            providers::vendor_display_name(previous_provider),
            static_cast<std::uint32_t>(previous_mode));
        return false;
    }

    if (!config::Settings::instance().commit_upscaling_selection(
            provider, mode)) {
        const auto contract_restored = super_resolution.commit_prepared_provider(
            previous_provider,
            previous_mode,
            super_resolution.render_width(),
            super_resolution.render_height());
        detail = "the selection could not be written to the INI";
        logger::error(
            "Live provider switch to {} could not persist its selection; "
            "incumbent {} contract rollback={}",
            providers::vendor_display_name(provider),
            providers::vendor_display_name(previous_provider),
            contract_restored);
        return false;
    }

    reset_next_evaluation_ = true;
    evaluation_verified_ = false;
    active_upscaling_provider_ = provider;
    provider_change_logged_ = false;
    provider_failure_logged_ = false;
    provider_contract_logged_ = false;
    first_evaluation_logged_ = false;
    switch_recovery_armed_ = true;
    switch_recovery_provider_ = previous_provider;
    switch_recovery_mode_ = previous_mode;
    switch_recovery_render_width_ = render_width;
    switch_recovery_render_height_ = render_height;
    switch_recovery_frames_remaining_ = kSwitchRecoveryFrameBudget;
    reset_vendor_generation_history(
        "the upscaling provider changed and it is what writes the motion "
        "vectors the generator interpolates from");
    logger::info(
        "Live same-extent provider switch applied: {} {} -> {} {}, {}x{} -> "
        "{}x{}; proxy, swap chain and render extent identities unchanged",
        providers::vendor_display_name(previous_provider),
        static_cast<std::uint32_t>(previous_mode),
        providers::vendor_display_name(provider),
        static_cast<std::uint32_t>(mode),
        render_width,
        render_height,
        output_width,
        output_height);
    return true;
}

bool UpscalingPass::evaluate_provider_surface(
    ID3D11Texture2D* color,
    ID3D11Texture2D* output,
    const providers::Vendor provider,
    const config::UpscalingMode mode,
    const std::uint32_t render_width,
    const std::uint32_t render_height,
    const std::uint32_t output_width,
    const std::uint32_t output_height)
{
    auto& presentation = PresentationBridge::instance();
    auto& super_resolution =
        streamline::SuperResolution::instance();
    const auto use_xess =
        provider == providers::Vendor::intel;
    const auto use_fsr =
        provider == providers::Vendor::amd;
    const auto use_vendor_upscaler = use_xess || use_fsr;
    if (provider != providers::Vendor::nvidia &&
        !use_vendor_upscaler) {
        return false;
    }
    if (!presentation.uses_virtual_render_surface() ||
        color == nullptr ||
        output == nullptr ||
        !super_resolution.enabled() ||
        !CameraData::instance().temporal_inputs_valid()) {
        return false;
    }

    auto* renderer =
        RE::BSGraphics::Renderer::GetRendererData();
    auto* device = presentation.d3d11_device();
    auto* context = presentation.d3d11_context();
    if (renderer == nullptr || device == nullptr || context == nullptr) {
        return false;
    }

    D3D11_TEXTURE2D_DESC color_description{};
    D3D11_TEXTURE2D_DESC output_description{};
    color->GetDesc(&color_description);
    output->GetDesc(&output_description);
    if (!use_vendor_upscaler &&
        (present_state_ == nullptr ||
         !present_state_->matches(
             color_description,
             render_width,
             render_height,
             output_width,
             output_height))) {
        auto state = std::make_unique<State>();
        if (!state->initialize_present(
                device,
                context,
                color_description,
                output_description,
                render_width,
                render_height,
                output_width,
                output_height)) {
            return false;
        }
        present_state_ = std::move(state);
        reset_next_evaluation_ = true;
    }

    auto* const depth_texture = main_depth_texture(renderer);
    const auto& motion =
        renderer->renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
    if (depth_texture == nullptr || motion.texture == nullptr) {
        return false;
    }

    auto& resources = SharedResources::instance();
    if (!resources.ready()) {
        static_cast<void>(resources.initialize());
    }
    const auto temporal_inputs_ready =
        resources.prepare_temporal_inputs(
            render_width,
            render_height,
            use_vendor_upscaler);
    const auto temporal_mode =
        config::Settings::instance().dlss_temporal_inputs();
    const auto temporal_policy =
        streamline::select_temporal_inputs(
            temporal_mode,
            temporal_inputs_ready,
            config::Settings::instance().motion_dilation() !=
                config::MotionDilation::off);
    auto* source_motion =
        temporal_policy.use_dilated_motion ?
            resources.temporal_motion_d3d11() :
            motion.texture;
    auto* source_bias =
        temporal_policy.use_hints ?
            resources.reactive_mask_d3d11() : nullptr;
    auto* source_transparency =
        temporal_policy.use_hints ?
            resources.transparency_mask_d3d11() : nullptr;

    auto* scene_color = resources.hudless_color_d3d11();
    if (scene_color == nullptr) {
        scene_color = color;
    }

    set_dirty_states(false);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    unbind_shader_resources(context);

    const auto depth_contract = CameraData::instance().depth_contract();

    const auto reversed_depth =
        depth_contract.orientation == DepthOrientation::reversed;

    const auto raster_jitter = CameraData::instance().frame_jitter();
    ID3D11Texture2D* provider_output{};
    if (use_xess) {
        if (!temporal_inputs_ready ||
            resources.color() == nullptr ||
            resources.vendor_motion_vectors() == nullptr ||
            resources.depth() == nullptr ||
            resources.reactive_mask() == nullptr ||
            resources.upscaled_output() == nullptr) {
            return false;
        }

        if (!depth_contract.determinate()) {

            ++depth_indeterminate_frames_;
            if (!depth_refusal_logged_ &&
                depth_indeterminate_frames_ > kDepthContractGraceFrames) {
                depth_refusal_logged_ = true;
                logger::error(
                    "Intel XeSS evaluation refused for {} consecutive frames: "
                    "Skyrim's depth convention is {} / {}, so the provider "
                    "cannot be told which end of the range is near. See the "
                    "'Projection form measured' line for the matrix this was "
                    "decided from.",
                    depth_indeterminate_frames_,
                    describe(depth_contract.orientation),
                    describe(depth_contract.range));
            }
            return false;
        }

        auto& xess = providers::XessUpscaler::instance();
        const auto quality = provider_quality(mode);
        const auto configured = xess.configure(
            quality,
            render_width,
            render_height,
            output_width,
            output_height,
            depth_contract.reversed());
        const providers::XessDispatch dispatch{
            resources.color(),
            resources.vendor_motion_vectors(),
            resources.depth(),
            resources.reactive_mask(),
            resources.upscaled_output(),
            render_width,
            render_height,
            output_width,
            output_height,
            raster_jitter.pixels_x,
            raster_jitter.pixels_y,
            reset_next_evaluation_,
            depth_contract.reversed()};
        if (!configured || !xess.evaluate(dispatch)) {
            if (!provider_failure_logged_) {
                provider_failure_logged_ = true;
                logger::error(
                    "Intel XeSS evaluation unavailable: {} ({})",
                    xess.detail(),
                    providers::availability_name(xess.availability()));
            }
            return false;
        }
        provider_failure_logged_ = false;
        depth_refusal_logged_ = false;
        depth_indeterminate_frames_ = 0;
        provider_output = resources.upscaled_output_d3d11();
    } else if (use_fsr) {
        sl::Constants camera{};

        auto* const vendor_reactive =
            temporal_policy.use_hints ? resources.reactive_mask() : nullptr;
        auto* const vendor_transparency =
            temporal_policy.use_hints ?
                resources.transparency_mask() : nullptr;
        if (!temporal_inputs_ready ||
            !CameraData::instance().build_constants(
                camera, reset_next_evaluation_, false) ||
            resources.color() == nullptr ||
            resources.vendor_motion_vectors() == nullptr ||
            resources.depth() == nullptr ||
            resources.upscaled_output() == nullptr) {
            return false;
        }

        auto& fsr = providers::FsrUpscaler::instance();
        if (!depth_contract.determinate()) {

            ++depth_indeterminate_frames_;
            if (!depth_refusal_logged_ &&
                depth_indeterminate_frames_ > kDepthContractGraceFrames) {
                depth_refusal_logged_ = true;
                logger::error(
                    "AMD FidelityFX evaluation refused for {} consecutive "
                    "frames: Skyrim's depth convention is {} / {}, so the "
                    "provider cannot be told which end of the range is near or "
                    "whether the far plane is finite. See the 'Projection form "
                    "measured' line for the matrix this was decided from.",
                    depth_indeterminate_frames_,
                    describe(depth_contract.orientation),
                    describe(depth_contract.range));
            }
            return false;
        }

        auto declared_reversed_depth = depth_contract.reversed();
        auto infinite_depth = depth_contract.infinite();
        switch (config::Settings::instance().fsr_depth_override()) {
        case config::FsrDepthOverride::measured:
            break;
        case config::FsrDepthOverride::standard_finite:
            declared_reversed_depth = false;
            infinite_depth = false;
            break;
        case config::FsrDepthOverride::standard_infinite:
            declared_reversed_depth = false;
            infinite_depth = true;
            break;
        case config::FsrDepthOverride::inverted_finite:
            declared_reversed_depth = true;
            infinite_depth = false;
            break;
        case config::FsrDepthOverride::inverted_infinite:
            declared_reversed_depth = true;
            infinite_depth = true;
            break;
        }
        if (!provider_contract_logged_) {
            provider_contract_logged_ = true;

            logger::info(
                "AMD FidelityFX input contract: {}x{} -> {}x{}, depth={}, "
                "range={}, reversed={}, infinite={}, reactive-mask={}, "
                "transparency-mask={}, jitter={:.4f},{:.4f} px "
                "(coherent={}) measured from the raster. Every value on this "
                "line comes from one snapshot taken this frame.",
                render_width,
                render_height,
                output_width,
                output_height,
                describe(depth_contract.orientation),
                describe(depth_contract.range),
                declared_reversed_depth,
                infinite_depth,
                vendor_reactive != nullptr,
                vendor_transparency != nullptr,
                raster_jitter.pixels_x,
                raster_jitter.pixels_y,
                raster_jitter.coherent);
        }
        const auto quality = provider_quality(mode);
        const auto configured = fsr.configure(
            quality,
            render_width,
            render_height,
            output_width,
            output_height,
            declared_reversed_depth,
            infinite_depth);
        const providers::FsrDispatch dispatch{
            resources.color(),
            resources.vendor_motion_vectors(),
            resources.depth(),
            vendor_reactive,
            vendor_transparency,
            resources.upscaled_output(),
            render_width,
            render_height,
            output_width,
            output_height,
            raster_jitter.pixels_x,
            raster_jitter.pixels_y,
            camera.cameraNear,
            camera.cameraFar,
            camera.cameraFOV,
            reset_next_evaluation_,
            declared_reversed_depth,
            infinite_depth};
        if (!configured || !fsr.evaluate(dispatch)) {
            if (!provider_failure_logged_) {
                provider_failure_logged_ = true;
                logger::error(
                    "AMD FidelityFX evaluation unavailable: {} ({})",
                    fsr.detail(),
                    providers::availability_name(fsr.availability()));
            }
            return false;
        }
        provider_failure_logged_ = false;
        depth_refusal_logged_ = false;
        depth_indeterminate_frames_ = 0;
        provider_output = resources.upscaled_output_d3d11();
    } else {
        context->CopyResource(
            present_state_->color_input.Get(),
            scene_color);

        auto& depth_diagnostics = DepthDiagnostics::instance();
        depth_diagnostics.observe(
            DepthStage::engine_before_copy,
            device,
            context,
            depth_texture,
            reversed_depth);

        auto* depth_input = present_state_->copy_active_input(
            depth_texture,
            present_state_->depth_input,
            "present-depth");

        depth_diagnostics.observe(
            DepthStage::input_after_copy,
            device,
            context,
            depth_input,
            reversed_depth);

        auto* motion_input = present_state_->copy_active_input(
            source_motion,
            present_state_->motion_input,
            "present-motion");
        auto* bias_input =
            source_bias != nullptr ?
                present_state_->copy_active_input(
                    source_bias,
                    present_state_->bias_input,
                    "present-reactive-mask") :
                nullptr;
        auto* transparency_input =
            source_transparency != nullptr ?
                present_state_->copy_active_input(
                    source_transparency,
                    present_state_->transparency_input,
                    "present-transparency-mask") :
                nullptr;
        if (depth_input == nullptr || motion_input == nullptr ||
            (source_bias != nullptr && bias_input == nullptr) ||
            (source_transparency != nullptr &&
             transparency_input == nullptr)) {
            return false;
        }

        constexpr float clear[4]{0.0F, 0.0F, 0.0F, 0.0F};
        context->ClearUnorderedAccessViewFloat(
            present_state_->color_output_view.Get(),
            clear);

        depth_diagnostics.observe(
            DepthStage::input_before_evaluate,
            device,
            context,
            depth_input,
            reversed_depth);

        if (!super_resolution.evaluate(
                present_state_->color_input.Get(),
                present_state_->color_output.Get(),
                depth_input,
                motion_input,
                bias_input,
                transparency_input,

                raster_jitter.pixels_x,
                raster_jitter.pixels_y,
                reset_next_evaluation_)) {
            return false;
        }

        depth_diagnostics.observe(
            DepthStage::input_after_evaluate,
            device,
            context,
            depth_input,
            reversed_depth);
        depth_diagnostics.collect(context);
        present_state_->sample_output_extent(
            present_state_->color_output.Get(),
            "complete-frame");
        provider_output = present_state_->color_output.Get();
    }

    if (provider_output == nullptr) {
        return false;
    }

    unbind_shader_resources(context);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context->CopyResource(output, provider_output);
    const D3D11_VIEWPORT viewport{
        0.0F,
        0.0F,
        static_cast<float>(output_width),
        static_cast<float>(output_height),
        0.0F,
        1.0F};
    const D3D11_RECT scissor{
        0,
        0,
        static_cast<LONG>(output_width),
        static_cast<LONG>(output_height)};
    context->RSSetViewports(1, &viewport);
    context->RSSetScissorRects(1, &scissor);
    static_cast<void>(SharpeningPass::instance().apply(
        output,
        config::Settings::instance().sharpness()));

    if (DebugViewPass::armed()) {
        static_cast<void>(DebugViewPass::instance().render(output));
    }
    static_cast<void>(
        resources.capture_frame_generation_hudless(output));
    set_dirty_states(false);

    reset_next_evaluation_ = false;
    evaluation_verified_ = true;
    evaluation_failed_ = false;
    retry_frames_remaining_ = 0;
    if (!first_evaluation_logged_) {
        first_evaluation_logged_ = true;
        if (use_xess) {
            logger::info(
                "Native complete-frame Intel XeSS evaluation verified at "
                "the production pre-UI boundary: {}x{} -> {}x{}, "
                "motion=undilated, responsive-mask=true",
                render_width,
                render_height,
                output_width,
                output_height);
        } else if (use_fsr) {
            logger::info(
                "Native complete-frame AMD FidelityFX evaluation verified "
                "at the production pre-UI boundary: {}x{} -> {}x{}, "
                "motion=undilated, reactive-mask=true, "
                "transparency-mask=true",
                render_width,
                render_height,
                output_width,
                output_height);
        } else {
            logger::info(
                "Native D3D11 complete-frame DLSS evaluation verified: "
                "{}x{} -> {}x{}, motion={}, hints={}",
                render_width,
                render_height,
                output_width,
                output_height,
                temporal_policy.use_dilated_motion ?
                    "depth-aware-dilated" : "raw",
                temporal_policy.use_hints);
        }
    }
    return true;
}

void UpscalingPass::main_draw_thunk(
    const std::int64_t renderer,
    const int unknown)
{
    auto& pass = instance();
    auto& presentation = PresentationBridge::instance();
    if (!presentation.uses_virtual_render_surface()) {
        DynamicResolution::instance().prepare_ui();
        pass.capture_hudless_back_buffer();
        original_main_draw_(renderer, unknown);
        return;
    }

    original_main_draw_(renderer, unknown);

}

void UpscalingPass::post_processing_thunk(
    RE::ImageSpaceManager* manager,
    const std::uint32_t unknown,
    const RE::RENDER_TARGET target,
    void* data,
    const bool flag)
{
    auto& pass = instance();
    auto& super_resolution =
        streamline::SuperResolution::instance();
    auto& presentation = PresentationBridge::instance();
    auto evaluated = false;
    auto guard_full_resolution_post_processing = false;

    const auto virtual_surface = presentation.uses_virtual_render_surface();
    const auto scene_finalization =
        virtual_surface &&
        target == RE::RENDER_TARGET::kFRAMEBUFFER;
    if (scene_finalization && pass.scene_finalized_this_frame_) {

        if (!pass.extra_framebuffer_pass_logged_) {
            pass.extra_framebuffer_pass_logged_ = true;
            logger::warn(
                "A second image-space framebuffer pass occurred after scene "
                "finalization; it lands on the reduced proxy while the resolve "
                "is still pending, and on the native surface once the UI phase "
                "has opened (resolve-pending={})",
                pass.scene_resolve_pending_);
        }
    }

    if (!virtual_surface &&
        super_resolution.enabled() &&
        DynamicResolution::instance().should_evaluate() &&
        !pass.attempted_this_frame_) {
        pass.attempted_this_frame_ = true;
        if (!pass.first_callsite_logged_) {
            pass.first_callsite_logged_ = true;
            logger::info(
                "Native DLSS pre-postprocess HDR boundary selected");
        }
        evaluated = pass.evaluate_pre_postprocessing_scene();
    }
    if (evaluated) {
        const auto reduced_resolution =
            super_resolution.render_width() <
                super_resolution.output_width() ||
            super_resolution.render_height() <
                super_resolution.output_height();
        guard_full_resolution_post_processing = reduced_resolution;
        if (reduced_resolution &&
            pass.state_ != nullptr &&
            !DepthRestoration::instance().restore(
                pass.state_->depth_input.Get(),
                super_resolution.output_width(),
                super_resolution.output_height(),
                DynamicResolution::instance().jitter_x(),
                DynamicResolution::instance().jitter_y())) {
            logger::error(
                "DLSS color completed, but full-resolution depth restoration "
                "failed; returning subsequent frames to native resolution");
            pass.evaluation_failed_ = true;
            pass.retry_frames_remaining_ = 300;
            pass.reset_next_evaluation_ = true;
        }
        DynamicResolution::instance().prepare_ui();
        set_dirty_states(false);
    }
    if (guard_full_resolution_post_processing) {
        DynamicResolution::instance().
            begin_full_resolution_post_processing();
    }
    auto* renderer_data = RE::BSGraphics::Renderer::GetRendererData();
    auto* saved_texture = static_cast<ID3D11Texture2D*>(nullptr);
    auto* saved_view = static_cast<ID3D11RenderTargetView*>(nullptr);
    auto* saved_resource_view =
        static_cast<ID3D11ShaderResourceView*>(nullptr);
    const auto redirect_scene_blit =
        scene_finalization &&
        renderer_data != nullptr &&
        presentation.d3d11_render_buffer() != nullptr &&
        presentation.d3d11_render_target_view() != nullptr;
    if (redirect_scene_blit) {
        auto& framebuffer =
            renderer_data->renderTargets[
                RE::RENDER_TARGETS::kFRAMEBUFFER];
        saved_texture = framebuffer.texture;
        saved_view = framebuffer.RTV;
        saved_resource_view = framebuffer.SRV;
        framebuffer.texture = presentation.d3d11_render_buffer();
        framebuffer.RTV = presentation.d3d11_render_target_view();
        framebuffer.SRV = presentation.d3d11_render_resource_view();
    }

    const auto suppress_vanilla_resolve =
        target == RE::RENDER_TARGET::kFRAMEBUFFER;
    if (suppress_vanilla_resolve) {
        DynamicResolution::instance().suppress_vanilla_taa_resolve();
    }
    original_post_processing_(
        manager,
        unknown,
        target,
        data,
        flag);
    if (suppress_vanilla_resolve) {
        DynamicResolution::instance().
            enable_vanilla_taa_for_world_render();
    }
    if (redirect_scene_blit) {
        auto& framebuffer =
            renderer_data->renderTargets[
                RE::RENDER_TARGETS::kFRAMEBUFFER];
        framebuffer.texture = saved_texture;
        framebuffer.RTV = saved_view;
        framebuffer.SRV = saved_resource_view;

        set_dirty_states(false);
    }
    if (scene_finalization && !pass.scene_finalized_this_frame_) {

        pass.scene_finalized_this_frame_ = true;
        pass.scene_ever_finalized_ = true;
        pass.scene_resolve_pending_ = true;
        CameraData::set_capture_frozen(true);

        static_cast<void>(ScaleformBoundary::instance().ensure_installed());
    }
    if (guard_full_resolution_post_processing) {
        DynamicResolution::instance().
            end_full_resolution_post_processing();
    }
}

bool UpscalingPass::scene_resolve_pending() const noexcept
{
    return scene_resolve_pending_;
}

bool UpscalingPass::commit_pending_scene_resolve(const char* const boundary)
{
    if (!scene_resolve_pending_) {
        return presentation_prepared_;
    }

    scene_resolve_pending_ = false;

    auto& presentation = PresentationBridge::instance();
    auto& super_resolution = streamline::SuperResolution::instance();
    auto* reduced_scene = presentation.d3d11_render_buffer();
    auto* full_resolution = presentation.d3d11_back_buffer();

    capture_hudless_back_buffer();
    auto prepared = evaluate_present_surface(reduced_scene, full_resolution);
    if (prepared && presentation_fallback_logged_) {
        presentation_fallback_logged_ = false;
        logger::info(
            "Scene-finalization DLSS inputs are ready; temporal upscaling "
            "resumed");
    }
    if (!prepared && reduced_scene != nullptr && full_resolution != nullptr) {

        prepared = SurfaceBlit::instance().apply(
            presentation.d3d11_device(),
            presentation.d3d11_context(),
            reduced_scene,
            full_resolution,
            super_resolution.render_width(),
            super_resolution.render_height());
        if (prepared) {
            static_cast<void>(SharpeningPass::instance().apply(
                full_resolution,
                config::Settings::instance().sharpness()));
            static_cast<void>(
                SharedResources::instance().
                    capture_frame_generation_hudless(full_resolution));
            reset_history();
            if (!presentation_fallback_logged_) {
                presentation_fallback_logged_ = true;
                logger::warn(
                    "SPATIAL FALLBACK: no temporal reconstruction ran this "
                    "frame. The reduced scene is being stretched to the "
                    "output extent. This is NOT the selected provider's "
                    "output, and image quality must not be judged from it.");
            }
        }
    }
    if (!prepared) {
        return false;
    }

    set_dirty_states(false);
    presentation_prepared_ = true;

    DynamicResolution::instance().begin_full_resolution_ui();
    if (!native_ui_phase_logged_) {
        native_ui_phase_logged_ = true;
        logger::info(
            "Native UI phase opened at the {} boundary: reduced {}x{} resolved "
            "to native {}x{}; Skyrim's UI rasterises at the output extent from "
            "here",
            boundary,
            super_resolution.render_width(),
            super_resolution.render_height(),
            super_resolution.output_width(),
            super_resolution.output_height());
    }
    return true;
}

void UpscalingPass::capture_hudless_back_buffer()
{
    auto& presentation = PresentationBridge::instance();
    auto* back_buffer =
        presentation.uses_virtual_render_surface() ?
            presentation.d3d11_render_buffer() :
            presentation.d3d11_back_buffer();
    if (back_buffer != nullptr) {

        if (!presentation.uses_virtual_render_surface() &&
            streamline::SuperResolution::instance().enabled()) {
            static_cast<void>(SharpeningPass::instance().apply(
                back_buffer,
                config::Settings::instance().sharpness()));

            set_dirty_states(false);
        }
        static_cast<void>(
            SharedResources::instance().capture_hudless(back_buffer));
        if (!presentation.uses_virtual_render_surface()) {
            static_cast<void>(
                SharedResources::instance().
                    capture_frame_generation_hudless(back_buffer));
        }
    }
}

bool UpscalingPass::evaluate_pre_postprocessing_scene()
{
    auto& super_resolution =
        streamline::SuperResolution::instance();
    if (!super_resolution.enabled() ||
        !DynamicResolution::instance().should_evaluate() ||
        !CameraData::instance().temporal_inputs_valid()) {
        return false;
    }

    if (active_upscaling_provider_ != providers::Vendor::nvidia) {
        if (!foreign_provider_route_logged_) {
            foreign_provider_route_logged_ = true;
            logger::error(
                "{} selected the full-extent pre-postprocess route, which is "
                "NVIDIA NGX/DLSS-only: this configuration has no reduced "
                "virtual render surface, so there is no {} evaluator on this "
                "path. Refusing rather than running DLSS and attributing it "
                "to {}. Select an upscaling mode with a reduced render extent "
                "for this provider, or use NVIDIA for full-extent DLAA.",
                providers::vendor_display_name(active_upscaling_provider_),
                providers::vendor_display_name(active_upscaling_provider_),
                providers::vendor_display_name(active_upscaling_provider_));
        }
        return false;
    }
    foreign_provider_route_logged_ = false;

    auto* renderer =
        RE::BSGraphics::Renderer::GetRendererData();
    auto* device =
        PresentationBridge::instance().d3d11_device();
    auto* context =
        PresentationBridge::instance().d3d11_context();
    if (renderer == nullptr || device == nullptr || context == nullptr) {
        evaluation_failed_ = true;
        retry_frames_remaining_ = 60;
        DynamicResolution::instance().prepare_ui();
        return false;
    }
    const auto fail = [this](const std::uint32_t retry_frames) {
        evaluation_failed_ = true;
        retry_frames_remaining_ = retry_frames;

        DynamicResolution::instance().prepare_ui();
        return false;
    };

    auto* main_scene =
        renderer->renderTargets[
            RE::RENDER_TARGETS::kMAIN].texture;
    if (main_scene == nullptr) {
        return fail(60);
    }
    D3D11_TEXTURE2D_DESC main_description{};
    main_scene->GetDesc(&main_description);
    const auto mode =
        static_cast<std::uint32_t>(super_resolution.mode());
    if (diagnostic_mode_ != mode ||
        diagnostic_viewport_width_ !=
            super_resolution.render_width() ||
        diagnostic_viewport_height_ !=
            super_resolution.render_height()) {
        diagnostic_mode_ = mode;
        diagnostic_viewport_width_ =
            super_resolution.render_width();
        diagnostic_viewport_height_ =
            super_resolution.render_height();
        logger::info(
            "DLSS pre-postprocess boundary state: kMAIN={}x{} format={}, "
            "active-render={}x{}, output={}x{}",
            main_description.Width,
            main_description.Height,
            static_cast<unsigned>(main_description.Format),
            super_resolution.render_width(),
            super_resolution.render_height(),
            super_resolution.output_width(),
            super_resolution.output_height());
    }
    const auto reduced_resolution =
        super_resolution.render_width() <
            super_resolution.output_width() ||
        super_resolution.render_height() <
            super_resolution.output_height();
    if (reduced_resolution &&
        !DynamicResolution::instance().scene_viewport_verified()) {
        logger::error(
            "Skyrim did not render kMAIN at the requested {}x{} scene "
            "extent; rejecting pre-postprocess DLSS evaluation",
            super_resolution.render_width(),
            super_resolution.render_height());
        return fail(60);
    }

    auto* const depth_texture = main_depth_texture(renderer);
    const auto& motion =
        renderer->renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
    if (depth_texture == nullptr ||
        motion.texture == nullptr) {
        return fail(60);
    }

    if (state_ == nullptr ||
        !state_->matches(
            main_description,
            super_resolution.render_width(),
            super_resolution.render_height(),
            super_resolution.output_width(),
            super_resolution.output_height())) {
        auto state = std::make_unique<State>();
        if (!state->initialize(
                device,
                context,
                main_description,
                super_resolution.render_width(),
                super_resolution.render_height(),
                super_resolution.output_width(),
                super_resolution.output_height())) {
            return fail(300);
        }
        state_ = std::move(state);
        reset_next_evaluation_ = true;
    }

    auto& resources = SharedResources::instance();
    if (!resources.ready()) {
        static_cast<void>(resources.initialize());
    }
    const auto temporal_inputs_ready =
        resources.prepare_temporal_inputs(
            super_resolution.render_width(),
            super_resolution.render_height(),
            false);
    const auto temporal_mode =
        config::Settings::instance().dlss_temporal_inputs();
    const auto temporal_policy =
        streamline::select_temporal_inputs(
            temporal_mode,
            temporal_inputs_ready,
            config::Settings::instance().motion_dilation() !=
                config::MotionDilation::off);
    auto* source_motion =
        temporal_policy.use_dilated_motion ?
            resources.temporal_motion_d3d11() :
            motion.texture;
    auto* source_bias =
        temporal_policy.use_hints ?
            resources.reactive_mask_d3d11() : nullptr;
    auto* source_transparency =
        temporal_policy.use_hints ?
            resources.transparency_mask_d3d11() :
            nullptr;

    set_dirty_states(false);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    unbind_shader_resources(context);
    const D3D11_BOX active_color_region{
        0,
        0,
        0,
        super_resolution.render_width(),
        super_resolution.render_height(),
        1};
    context->CopySubresourceRegion(
        state_->color_input.Get(),
        0,
        0,
        0,
        0,
        main_scene,
        0,
        &active_color_region);
    auto* depth_input = state_->copy_active_input(
        depth_texture,
        state_->depth_input,
        "depth");
    auto* motion_input = state_->copy_active_input(
        source_motion,
        state_->motion_input,
        "motion");
    auto* bias_input =
        source_bias != nullptr ?
            state_->copy_active_input(
                source_bias,
                state_->bias_input,
                "reactive-mask") :
            nullptr;
    auto* transparency_input =
        source_transparency != nullptr ?
            state_->copy_active_input(
                source_transparency,
                state_->transparency_input,
                "transparency-mask") :
            nullptr;
    if (depth_input == nullptr || motion_input == nullptr ||
        (source_bias != nullptr && bias_input == nullptr) ||
        (source_transparency != nullptr &&
         transparency_input == nullptr)) {
        return fail(60);
    }
    constexpr float cleared_output[4]{0.0F, 0.0F, 0.0F, 0.0F};
    context->ClearUnorderedAccessViewFloat(
        state_->color_output_view.Get(),
        cleared_output);

    const auto pending_raster_jitter =
        CameraData::instance().frame_jitter();
    const auto evaluated = super_resolution.evaluate(
        state_->color_input.Get(),
        state_->color_output.Get(),
        depth_input,
        motion_input,
        bias_input,
        transparency_input,
        pending_raster_jitter.pixels_x,
        pending_raster_jitter.pixels_y,
        reset_next_evaluation_);
    if (!evaluated) {
        return fail(60);
    }
    state_->sample_output_extent(
        state_->color_output.Get(),
        "pre-postprocess");

    unbind_shader_resources(context);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context->CopyResource(
        main_scene,
        state_->color_output.Get());
    const D3D11_VIEWPORT output_viewport{
        0.0F,
        0.0F,
        static_cast<float>(super_resolution.output_width()),
        static_cast<float>(super_resolution.output_height()),
        0.0F,
        1.0F};
    const D3D11_RECT output_scissor{
        0,
        0,
        static_cast<LONG>(super_resolution.output_width()),
        static_cast<LONG>(super_resolution.output_height())};
    context->RSSetViewports(1, &output_viewport);
    context->RSSetScissorRects(1, &output_scissor);

    if (DebugViewPass::armed()) {
        static_cast<void>(DebugViewPass::instance().render(main_scene));
    }
    set_dirty_states(false);

    reset_next_evaluation_ = false;
    evaluation_verified_ = true;
    evaluation_failed_ = false;
    retry_frames_remaining_ = 0;
    if (!first_evaluation_logged_) {
        first_evaluation_logged_ = true;
        logger::info(
            "Native D3D11 DLSS {} evaluation verified on HDR kMAIN before "
            "post-processing: {}x{} -> {}x{}, motion={}, hints={}, "
            "main scene depth",
            super_resolution.mode() == config::UpscalingMode::dlaa ?
                "DLAA" :
                "upscaling",
            super_resolution.render_width(),
            super_resolution.render_height(),
            super_resolution.output_width(),
            super_resolution.output_height(),
            temporal_policy.use_dilated_motion ?
                "depth-aware-dilated" : "raw",
            temporal_policy.use_hints);
    }
    return true;
}
}
