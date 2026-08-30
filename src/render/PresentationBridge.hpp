#pragma once

#include "config/Settings.hpp"

#include <dxgiformat.h>

#include <memory>
#include <cstdint>

struct IDXGISwapChain;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11DepthStencilView;
struct ID3D11RenderTargetView;
struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;
struct DXGI_SWAP_CHAIN_DESC;

namespace mfgdlss::render
{
class SwapChainProxy;

class PresentationBridge final
{
public:
    [[nodiscard]] static PresentationBridge& instance() noexcept;

    [[nodiscard]] bool create(
        ID3D11Device* d3d11_device,
        const DXGI_SWAP_CHAIN_DESC& requested_description);
    void shutdown() noexcept;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] IDXGISwapChain* proxy() const noexcept;

    [[nodiscard]] void* presentation_swap_chain() const noexcept;

    [[nodiscard]] void* begin_presentation_handoff();

    [[nodiscard]] bool abort_presentation_handoff();

    [[nodiscard]] bool adopt_presentation_swap_chain(void* chain);
    void release_presentation_swap_chain() noexcept;
    [[nodiscard]] ID3D11Device* d3d11_device() const noexcept;
    [[nodiscard]] ID3D11DeviceContext* d3d11_context() const noexcept;
    [[nodiscard]] ID3D11Texture2D* d3d11_back_buffer() const noexcept;
    [[nodiscard]] ID3D11Texture2D* d3d11_render_buffer() const noexcept;
    [[nodiscard]] ID3D11RenderTargetView*
        d3d11_render_target_view() const noexcept;
    [[nodiscard]] ID3D11ShaderResourceView*
        d3d11_render_resource_view() const noexcept;
    [[nodiscard]] ID3D11RenderTargetView*
        full_resolution_ui_target(
            ID3D11RenderTargetView* requested) const noexcept;
    [[nodiscard]] bool full_resolution_ui_source(
        ID3D11RenderTargetView* requested) const noexcept;
    [[nodiscard]] bool full_resolution_ui_target_compatible(
        ID3D11RenderTargetView* requested) const noexcept;
    [[nodiscard]] ID3D11DepthStencilView* full_resolution_ui_depth(
        ID3D11DepthStencilView* requested);
    [[nodiscard]] ID3D11DepthStencilView*
        mapped_full_resolution_ui_clear_depth(
            ID3D11DepthStencilView* requested) const noexcept;
    [[nodiscard]] bool uses_virtual_render_surface() const noexcept;

    [[nodiscard]] bool uses_sub_rect_render_surface() const noexcept;
    [[nodiscard]] std::uint32_t render_width() const noexcept;
    [[nodiscard]] std::uint32_t render_height() const noexcept;
    [[nodiscard]] std::uint32_t output_width() const noexcept;
    [[nodiscard]] std::uint32_t output_height() const noexcept;
    [[nodiscard]] DXGI_FORMAT back_buffer_format() const noexcept;

    [[nodiscard]] bool evaluate_profile_change_preflight(
        config::UpscalingMode requested_mode,
        const char*& reason,
        bool& user_facing);

    [[nodiscard]] bool request_upscaling_reconfiguration(
        config::UpscalingMode requested_mode);

private:

    void publish_display_refresh() noexcept;

    friend class SwapChainProxy;
    struct State;
    std::unique_ptr<State> state_;
};

[[nodiscard]] bool install_presentation_bootstrap();
}
