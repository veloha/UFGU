#pragma once

#include <cstdint>
#include <memory>

struct ID3D11Texture2D;
struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;

namespace mfgdlss::render
{
class SharedResources final
{
public:
    [[nodiscard]] static SharedResources& instance() noexcept;

    [[nodiscard]] bool initialize();
    void begin_frame() noexcept;
    [[nodiscard]] bool capture_hudless(ID3D11Texture2D* source);
    [[nodiscard]] bool capture_frame_generation_hudless(
        ID3D11Texture2D* source);

    [[nodiscard]] bool begin_ui_rendering();

    void rebind_ui_target() noexcept;
    void bind_output_target_after_ui_display() noexcept;
    void end_ui_rendering() noexcept;
    [[nodiscard]] bool ui_rendering() const noexcept;
    [[nodiscard]] ID3D11RenderTargetView*
        ui_draw_target_view() const noexcept;
    [[nodiscard]] ID3D11DepthStencilView* ui_depth_view() const noexcept;
    [[nodiscard]] bool capture_ui(ID3D11Texture2D* final_color);
    [[nodiscard]] bool copy_frame();
    [[nodiscard]] bool prepare_temporal_inputs(
        std::uint32_t render_width,
        std::uint32_t render_height,
        bool synchronize_for_d3d12);

    [[nodiscard]] bool complete_upscaler_dispatch();
    void shutdown() noexcept;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] void* color() const noexcept;
    [[nodiscard]] void* complete_frame() const noexcept;
    [[nodiscard]] void* frame_generation_hudless() const noexcept;
    [[nodiscard]] void* motion_vectors() const noexcept;
    [[nodiscard]] void* upscaling_motion_vectors() const noexcept;

    [[nodiscard]] void* vendor_motion_vectors() const noexcept;
    [[nodiscard]] void* depth() const noexcept;
    [[nodiscard]] void* reactive_mask() const noexcept;
    [[nodiscard]] void* transparency_mask() const noexcept;
    [[nodiscard]] void* upscaled_output() const noexcept;
    [[nodiscard]] void* ui_color_alpha() const noexcept;
    void log_vendor_input_provenance(const char* consumer) noexcept;
    [[nodiscard]] ID3D11Texture2D* hudless_color_d3d11() const noexcept;
    [[nodiscard]] ID3D11Texture2D*
        frame_generation_hudless_d3d11() const noexcept;
    [[nodiscard]] ID3D11Texture2D* ui_color_alpha_d3d11() const noexcept;
    [[nodiscard]] ID3D11Texture2D* temporal_motion_d3d11() const noexcept;
    [[nodiscard]] ID3D11Texture2D* vendor_motion_d3d11() const noexcept;

    [[nodiscard]] ID3D11Texture2D* generator_motion_d3d11() const noexcept;
    [[nodiscard]] ID3D11Texture2D* depth_d3d11() const noexcept;
    [[nodiscard]] ID3D11Texture2D* reactive_mask_d3d11() const noexcept;
    [[nodiscard]] ID3D11Texture2D*
        transparency_mask_d3d11() const noexcept;
    [[nodiscard]] ID3D11Texture2D* upscaled_output_d3d11() const noexcept;
    [[nodiscard]] std::uint32_t color_format() const noexcept;
    [[nodiscard]] std::uint32_t ui_format() const noexcept;
    [[nodiscard]] bool ui_recomposition_available() const noexcept;
    [[nodiscard]] bool streamline_ui_recomposition_available()
        const noexcept;
    [[nodiscard]] bool hudless_captured() const noexcept;
    [[nodiscard]] bool frame_generation_hudless_captured() const noexcept;
    [[nodiscard]] bool temporal_inputs_prepared() const noexcept;
    [[nodiscard]] bool complete_frame_captured() const noexcept;
    [[nodiscard]] bool ui_captured() const noexcept;
    void mark_ui_captured() noexcept;
    [[nodiscard]] std::uint64_t ready_fence_value() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
    std::uint32_t retry_frames_remaining_{};
    std::uint32_t prepared_width_{};
    std::uint32_t prepared_height_{};
    bool hudless_captured_{};
    bool frame_generation_hudless_captured_{};
    bool complete_frame_captured_{};
    bool ui_captured_{};
    bool temporal_inputs_prepared_{};
    bool temporal_inputs_synchronized_{};
};
}
