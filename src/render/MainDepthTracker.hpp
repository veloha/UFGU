#pragma once

#include <d3d11.h>
#include <wrl/client.h>

namespace mfgdlss::render
{

class MainDepthTracker final
{
public:
    [[nodiscard]] static MainDepthTracker& instance() noexcept;

    void observe_binding(
        UINT target_count,
        ID3D11RenderTargetView* const* targets,
        ID3D11DepthStencilView* depth) noexcept;
    void reset() noexcept;

    [[nodiscard]] ID3D11Texture2D* texture() const noexcept;
    [[nodiscard]] ID3D11DepthStencilView* writable_view() const noexcept;
    [[nodiscard]] ID3D11ShaderResourceView* shader_resource_view()
        const noexcept;
    [[nodiscard]] bool captured_from_binding() const noexcept;

private:
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> writable_view_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> shader_resource_view_;
    unsigned contract_rejection_budget_{8U};
    unsigned format_rejection_budget_{8U};
    unsigned device_rejection_budget_{8U};
    bool capture_logged_{};
};
}
