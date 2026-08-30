#pragma once

#include "config/Settings.hpp"
#include "providers/ProviderTypes.hpp"

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Resource;
struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;

namespace mfgdlss::streamline
{
enum class DlssColorInput : std::uint32_t
{
    linear_hdr,
    display_ldr
};

class SuperResolution final
{
public:
    [[nodiscard]] static SuperResolution& instance() noexcept;

    [[nodiscard]] bool initialize(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        std::uint32_t output_width,
        std::uint32_t output_height);
    [[nodiscard]] bool reconfigure(
        std::uint32_t output_width,
        std::uint32_t output_height);
    [[nodiscard]] bool optimal_render_resolution(
        config::UpscalingMode mode,
        std::uint32_t& width,
        std::uint32_t& height) const;

    [[nodiscard]] bool query_provider_render_extent(
        providers::Vendor provider,
        config::UpscalingMode mode,
        std::uint32_t& width,
        std::uint32_t& height);

    [[nodiscard]] bool prepare_provider(
        providers::Vendor provider,
        config::UpscalingMode mode,
        std::uint32_t& render_width,
        std::uint32_t& render_height);

    [[nodiscard]] bool prepare_provider_at_extent(
        providers::Vendor provider,
        config::UpscalingMode mode,
        std::uint32_t render_width,
        std::uint32_t render_height);
    [[nodiscard]] bool commit_prepared_provider(
        providers::Vendor provider,
        config::UpscalingMode mode,
        std::uint32_t render_width,
        std::uint32_t render_height) noexcept;
    [[nodiscard]] bool set_mode(config::UpscalingMode mode);
    void set_color_input(DlssColorInput input) noexcept;
    [[nodiscard]] bool evaluate(
        ID3D11Resource* color,
        ID3D11Resource* output,
        ID3D11Resource* depth,
        ID3D11Resource* motion_vectors,
        ID3D11Resource* bias_current_color,
        ID3D11Resource* transparency,
        float jitter_x,
        float jitter_y,
        bool reset);
    void shutdown() noexcept;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] bool enabled() const noexcept;
    [[nodiscard]] providers::Vendor provider() const noexcept;
    [[nodiscard]] config::UpscalingMode mode() const noexcept;
    [[nodiscard]] std::uint32_t render_width() const noexcept;
    [[nodiscard]] std::uint32_t render_height() const noexcept;
    [[nodiscard]] std::uint32_t output_width() const noexcept;
    [[nodiscard]] std::uint32_t output_height() const noexcept;
    [[nodiscard]] float render_scale() const noexcept;

private:
    [[nodiscard]] bool ensure_ngx_controller();
    [[nodiscard]] bool ensure_nvidia_feature(
        config::UpscalingMode mode,
        std::uint32_t render_width,
        std::uint32_t render_height);
    [[nodiscard]] bool optimal_render_resolution_for(
        providers::Vendor provider,
        config::UpscalingMode mode,
        std::uint32_t& width,
        std::uint32_t& height) const;
    void release_ngx() noexcept;
    void release_feature() noexcept;

    ID3D11Device* device_{};
    ID3D11DeviceContext* context_{};
    NVSDK_NGX_Parameter* capability_parameters_{};
    NVSDK_NGX_Parameter* feature_parameters_{};
    NVSDK_NGX_Handle* feature_{};
    config::UpscalingMode feature_mode_{config::UpscalingMode::off};
    std::uint32_t feature_render_width_{};
    std::uint32_t feature_render_height_{};
    std::uint32_t feature_output_width_{};
    std::uint32_t feature_output_height_{};
    DlssColorInput feature_color_input_{DlssColorInput::linear_hdr};
    std::uint32_t render_width_{};
    std::uint32_t render_height_{};
    std::uint32_t output_width_{};
    std::uint32_t output_height_{};
    providers::Vendor provider_{providers::Vendor::none};
    config::UpscalingMode mode_{config::UpscalingMode::off};
    DlssColorInput color_input_{DlssColorInput::linear_hdr};
    bool ngx_initialized_{};
    bool ready_{};
};
}
