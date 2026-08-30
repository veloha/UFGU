#include "render/RendererDiagnostics.hpp"

#include "render/MainDepthTracker.hpp"

#include "enb/EnbApi.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <SKSE/SKSE.h>

#include <array>
#include <string_view>

namespace mfgdlss::render
{
namespace
{
namespace logger = SKSE::log;
using Microsoft::WRL::ComPtr;

[[nodiscard]] std::string_view format_name(const DXGI_FORMAT format) noexcept
{
    switch (format) {
    case DXGI_FORMAT_UNKNOWN:
        return "UNKNOWN";
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return "R32G32B32A32_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R32G32_FLOAT:
        return "R32G32_FLOAT";
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return "R11G11B10_FLOAT";
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_R16G16_FLOAT:
        return "R16G16_FLOAT";
    case DXGI_FORMAT_R32_TYPELESS:
        return "R32_TYPELESS";
    case DXGI_FORMAT_D32_FLOAT:
        return "D32_FLOAT";
    case DXGI_FORMAT_R32_FLOAT:
        return "R32_FLOAT";
    case DXGI_FORMAT_R24G8_TYPELESS:
        return "R24G8_TYPELESS";
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return "D24_UNORM_S8_UINT";
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        return "R24_UNORM_X8_TYPELESS";
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return "B8G8R8A8_UNORM_SRGB";
    default:
        return "OTHER";
    }
}

void log_texture(
    const std::string_view name,
    ID3D11Texture2D* texture,
    const bool expected)
{
    if (texture == nullptr) {
        if (expected) {
            logger::warn("{}: unavailable", name);
        } else {
            logger::info(
                "{}: not created yet, which is expected before Skyrim has "
                "drawn a frame",
                name);
        }
        return;
    }

    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    logger::info(
        "{}: resource={}, {}x{}, format={} ({}), mips={}, array={}, samples={}x/q{}, "
        "usage={}, bind=0x{:X}, cpu=0x{:X}, misc=0x{:X}",
        name,
        static_cast<void*>(texture),
        desc.Width,
        desc.Height,
        format_name(desc.Format),
        static_cast<unsigned>(desc.Format),
        desc.MipLevels,
        desc.ArraySize,
        desc.SampleDesc.Count,
        desc.SampleDesc.Quality,
        static_cast<unsigned>(desc.Usage),
        desc.BindFlags,
        desc.CPUAccessFlags,
        desc.MiscFlags);
}

void log_swap_chain(IDXGISwapChain* swap_chain)
{
    if (swap_chain == nullptr) {
        logger::warn("DXGI swap chain: unavailable");
        return;
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    const auto desc_result = swap_chain->GetDesc(&desc);
    if (FAILED(desc_result)) {
        logger::error("IDXGISwapChain::GetDesc failed: 0x{:08X}", static_cast<unsigned>(desc_result));
        return;
    }

    logger::info(
        "DXGI swap chain: object={}, {}x{}, format={} ({}), buffers={}, windowed={}, "
        "swap-effect={}, flags=0x{:X}, refresh={}/{}",
        static_cast<void*>(swap_chain),
        desc.BufferDesc.Width,
        desc.BufferDesc.Height,
        format_name(desc.BufferDesc.Format),
        static_cast<unsigned>(desc.BufferDesc.Format),
        desc.BufferCount,
        desc.Windowed != FALSE,
        static_cast<unsigned>(desc.SwapEffect),
        desc.Flags,
        desc.BufferDesc.RefreshRate.Numerator,
        desc.BufferDesc.RefreshRate.Denominator);

    ComPtr<ID3D11Texture2D> back_buffer;
    const auto buffer_result = swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (SUCCEEDED(buffer_result)) {
        log_texture("DXGI back buffer", back_buffer.Get(), true);
    } else {
        logger::error("IDXGISwapChain::GetBuffer failed: 0x{:08X}", static_cast<unsigned>(buffer_result));
    }

    ComPtr<IDXGIOutput> output;
    if (SUCCEEDED(swap_chain->GetContainingOutput(&output)) && output != nullptr) {
        DXGI_OUTPUT_DESC output_desc{};
        if (SUCCEEDED(output->GetDesc(&output_desc))) {
            const auto width = output_desc.DesktopCoordinates.right - output_desc.DesktopCoordinates.left;
            const auto height = output_desc.DesktopCoordinates.bottom - output_desc.DesktopCoordinates.top;
            logger::info(
                "DXGI output: {}x{}, attached={}, rotation={}",
                width,
                height,
                output_desc.AttachedToDesktop != FALSE,
                static_cast<unsigned>(output_desc.Rotation));
        }
    }

    ComPtr<IDXGIFactory5> factory;
    if (SUCCEEDED(swap_chain->GetParent(IID_PPV_ARGS(&factory))) && factory != nullptr) {
        BOOL allow_tearing{};
        const auto tearing_result = factory->CheckFeatureSupport(
            DXGI_FEATURE_PRESENT_ALLOW_TEARING,
            &allow_tearing,
            sizeof(allow_tearing));
        if (SUCCEEDED(tearing_result)) {
            logger::info("DXGI present tearing supported={}", allow_tearing != FALSE);
        }
    }
}

void log_device(ID3D11Device* device)
{
    if (device == nullptr) {
        logger::warn("D3D11 device: unavailable");
        return;
    }

    logger::info(
        "D3D11 device: object={}, feature-level=0x{:X}, flags=0x{:X}",
        static_cast<void*>(device),
        static_cast<unsigned>(device->GetFeatureLevel()),
        device->GetCreationFlags());
}

void log_skyrim_targets(const bool expected)
{
    const auto* renderer = RE::BSGraphics::Renderer::GetRendererData();
    if (renderer == nullptr) {
        if (expected) {
            logger::warn("Skyrim renderer data: unavailable");
        } else {
            logger::info(
                "Skyrim renderer data: not published yet, which is expected "
                "before Skyrim has drawn a frame");
        }
        return;
    }

    const auto& main = renderer->renderTargets[RE::RENDER_TARGETS::kMAIN];
    const auto& motion = renderer->renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];

    logger::info(
        "Skyrim display: desired-refresh={}/{}, actual-refresh={}/{}, fullscreen={}, "
        "borderless={}, present-interval={}",
        renderer->desiredRefreshRate.numerator,
        renderer->desiredRefreshRate.denominator,
        renderer->actualRefreshRate.numerator,
        renderer->actualRefreshRate.denominator,
        renderer->fullScreen,
        renderer->borderlessDisplay,
        renderer->presentInterval);
    log_texture("Skyrim MAIN", main.texture, expected);
    log_texture("Skyrim MAIN copy", main.textureCopy, expected);
    log_texture("Skyrim MOTION_VECTOR", motion.texture, expected);
    log_texture(
        "Skyrim MAIN depth",
        MainDepthTracker::instance().texture(),
        expected);
}
}

void log_renderer_diagnostics(const bool skyrim_targets_expected)
{
    const auto* info = enb::Api::instance().render_info();
    if (info == nullptr) {
        logger::warn("ENB render information is not available yet");
        return;
    }

    logger::info(
        "ENB renderer: {}x{}, device={}, context={}, swap-chain={}",
        info->width,
        info->height,
        info->d3d11_device,
        info->d3d11_context,
        info->dxgi_swap_chain);

    log_device(static_cast<ID3D11Device*>(info->d3d11_device));
    log_swap_chain(static_cast<IDXGISwapChain*>(info->dxgi_swap_chain));
    log_skyrim_targets(skyrim_targets_expected);
}
}
