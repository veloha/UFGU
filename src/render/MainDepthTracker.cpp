#include "render/MainDepthTracker.hpp"

#include "render/MainDepthBindingPolicy.hpp"
#include "render/RuntimeCompatibilitySkse.hpp"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <utility>

namespace mfgdlss::render
{
namespace
{
namespace logger = SKSE::log;
using Microsoft::WRL::ComPtr;

[[nodiscard]] const void* identity(IUnknown* object) noexcept
{
    if (object == nullptr) {
        return nullptr;
    }
    ComPtr<IUnknown> canonical;
    if (FAILED(object->QueryInterface(
            IID_PPV_ARGS(canonical.GetAddressOf()))) ||
        canonical == nullptr) {
        return object;
    }
    return canonical.Get();
}

[[nodiscard]] bool legacy_renderer_depth_layout() noexcept
{
    const auto* profile = active_runtime_profile();
    return profile != nullptr && profile->version == kSkyrim1597.version;
}

[[nodiscard]] DXGI_FORMAT depth_srv_format(
    const DXGI_FORMAT resource_format,
    const DXGI_FORMAT view_format) noexcept
{
    switch (resource_format) {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default:
        break;
    }
    switch (view_format) {
    case DXGI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_D16_UNORM:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}
}

MainDepthTracker& MainDepthTracker::instance() noexcept
{
    static MainDepthTracker tracker;
    return tracker;
}

void MainDepthTracker::observe_binding(
    const UINT target_count,
    ID3D11RenderTargetView* const* targets,
    ID3D11DepthStencilView* depth) noexcept
{
    if (target_count == 0U ||
        target_count > D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT ||
        targets == nullptr || depth == nullptr) {
        return;
    }

    const auto* renderer = RE::BSGraphics::Renderer::GetRendererData();
    auto* main_texture = renderer != nullptr ?
        renderer->renderTargets[RE::RENDER_TARGETS::kMAIN].texture : nullptr;
    if (main_texture == nullptr) {
        return;
    }

    auto main_bound = false;
    for (UINT index = 0; index < target_count; ++index) {
        if (targets[index] == nullptr) {
            continue;
        }
        ComPtr<ID3D11Resource> resource;
        targets[index]->GetResource(resource.GetAddressOf());
        if (identity(resource.Get()) == identity(main_texture)) {
            main_bound = true;
            break;
        }
    }
    if (!main_bound) {
        return;
    }

    ComPtr<ID3D11Resource> depth_resource;
    depth->GetResource(depth_resource.GetAddressOf());
    ComPtr<ID3D11Texture2D> candidate;
    if (depth_resource == nullptr ||
        FAILED(depth_resource.As(&candidate)) || candidate == nullptr) {
        return;
    }
    if (identity(candidate.Get()) == identity(texture_.Get())) {
        return;
    }

    D3D11_TEXTURE2D_DESC color_description{};
    D3D11_TEXTURE2D_DESC depth_description{};
    D3D11_DEPTH_STENCIL_VIEW_DESC depth_view_description{};
    main_texture->GetDesc(&color_description);
    candidate->GetDesc(&depth_description);
    depth->GetDesc(&depth_view_description);

    const MainDepthCandidate contract{
        color_description.Width,
        color_description.Height,
        depth_description.Width,
        depth_description.Height,
        depth_description.ArraySize,
        depth_description.SampleDesc.Count,
        (depth_description.BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0U,
        (depth_description.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0U};
    if (!valid_main_depth_candidate(contract)) {
        if (contract_rejection_budget_ != 0U) {
            --contract_rejection_budget_;
            logger::warn(
                "Rejected kMAIN output-merger depth candidate: color={}x{}, "
                "depth={}x{}, array={}, samples={}, bind=0x{:X}",
                color_description.Width,
                color_description.Height,
                depth_description.Width,
                depth_description.Height,
                depth_description.ArraySize,
                depth_description.SampleDesc.Count,
                depth_description.BindFlags);
        }
        return;
    }

    ComPtr<ID3D11Device> color_device;
    ComPtr<ID3D11Device> depth_device;
    main_texture->GetDevice(color_device.GetAddressOf());
    candidate->GetDevice(depth_device.GetAddressOf());
    if (identity(color_device.Get()) != identity(depth_device.Get())) {
        if (device_rejection_budget_ != 0U) {
            --device_rejection_budget_;
            logger::warn(
                "Rejected kMAIN depth candidate because its colour and depth "
                "textures belong to different D3D11 devices. Depth cannot be "
                "captured across devices, so upscaling and frame generation "
                "will run without Skyrim's main depth. This is the one "
                "rejection cause that used to be silent");
        }
        return;
    }

    const auto srv_format = depth_srv_format(
        depth_description.Format,
        depth_view_description.Format);
    if (srv_format == DXGI_FORMAT_UNKNOWN || depth_device == nullptr) {
        if (format_rejection_budget_ != 0U) {
            --format_rejection_budget_;
            logger::warn(
                "Rejected kMAIN depth candidate with unsupported resource/DSV "
                "formats {}/{}",
                static_cast<unsigned>(depth_description.Format),
                static_cast<unsigned>(depth_view_description.Format));
        }
        return;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srv_description{};
    srv_description.Format = srv_format;
    srv_description.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_description.Texture2D.MostDetailedMip = 0U;
    srv_description.Texture2D.MipLevels = 1U;
    ComPtr<ID3D11ShaderResourceView> shader_resource_view;
    auto result = depth_device->CreateShaderResourceView(
        candidate.Get(),
        &srv_description,
        shader_resource_view.GetAddressOf());
    if (FAILED(result) || shader_resource_view == nullptr) {
        return;
    }

    depth_view_description.Flags = 0U;
    ComPtr<ID3D11DepthStencilView> writable_view;
    result = depth_device->CreateDepthStencilView(
        candidate.Get(),
        &depth_view_description,
        writable_view.GetAddressOf());
    if (FAILED(result) || writable_view == nullptr) {
        return;
    }

    texture_ = std::move(candidate);
    writable_view_ = std::move(writable_view);
    shader_resource_view_ = std::move(shader_resource_view);
    if (!capture_logged_) {
        capture_logged_ = true;
        logger::info(
            "Validated Skyrim kMAIN depth from its output-merger binding: "
            "{}x{}, resource-format={}, DSV-format={}, SRV-format={}",
            depth_description.Width,
            depth_description.Height,
            static_cast<unsigned>(depth_description.Format),
            static_cast<unsigned>(depth_view_description.Format),
            static_cast<unsigned>(srv_format));
    }
}

void MainDepthTracker::reset() noexcept
{
    shader_resource_view_.Reset();
    writable_view_.Reset();
    texture_.Reset();
    contract_rejection_budget_ = 8U;
    format_rejection_budget_ = 8U;
    device_rejection_budget_ = 8U;
    capture_logged_ = false;
}

ID3D11Texture2D* MainDepthTracker::texture() const noexcept
{
    if (texture_ != nullptr) {
        return texture_.Get();
    }
    const auto* renderer = RE::BSGraphics::Renderer::GetRendererData();
    return legacy_renderer_depth_layout() && renderer != nullptr ?
        renderer->depthStencils[
            RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].texture : nullptr;
}

ID3D11DepthStencilView* MainDepthTracker::writable_view() const noexcept
{
    if (writable_view_ != nullptr) {
        return writable_view_.Get();
    }
    const auto* renderer = RE::BSGraphics::Renderer::GetRendererData();
    return legacy_renderer_depth_layout() && renderer != nullptr ?
        renderer->depthStencils[
            RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].views[0] : nullptr;
}

ID3D11ShaderResourceView* MainDepthTracker::shader_resource_view()
    const noexcept
{
    if (shader_resource_view_ != nullptr) {
        return shader_resource_view_.Get();
    }
    const auto* renderer = RE::BSGraphics::Renderer::GetRendererData();
    return legacy_renderer_depth_layout() && renderer != nullptr ?
        renderer->depthStencils[
            RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].depthSRV : nullptr;
}

bool MainDepthTracker::captured_from_binding() const noexcept
{
    return texture_ != nullptr && writable_view_ != nullptr &&
           shader_resource_view_ != nullptr;
}
}
