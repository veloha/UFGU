#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11Texture2D;

namespace mfgdlss::render
{

struct SceneBlitProbeState;

struct ScaleformTraceState;

enum class UiCompositeMode : std::uint32_t
{

    signed_delta = 0,

    coverage = 1,

    exact = 2
};

class RenderDebug final
{
public:
    [[nodiscard]] static RenderDebug& instance() noexcept;
    ~RenderDebug();

    RenderDebug(const RenderDebug&) = delete;
    RenderDebug& operator=(const RenderDebug&) = delete;

    void poll_hotkeys();

    [[nodiscard]] UiCompositeMode composite_mode() const noexcept;
    [[nodiscard]] bool capture_pending() const noexcept;

    void dump(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        ID3D11Texture2D* texture,
        std::string_view label);

    void end_capture();

    void log_present_boundary_state(
        ID3D11DeviceContext* context,
        void* window,
        unsigned logical_width,
        unsigned logical_height,
        unsigned physical_width,
        unsigned physical_height);

    [[nodiscard]] bool begin_scene_blit_probe(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        ID3D11Texture2D* description_source,
        unsigned physical_width,
        unsigned physical_height,
        unsigned render_width,
        unsigned render_height);

    void end_scene_blit_probe(ID3D11DeviceContext* context);

    [[nodiscard]] bool scene_blit_probe_armed() const noexcept;

    void scaleform_trace_arm_if_ready();
    [[nodiscard]] bool scaleform_trace_active() const noexcept;

    void scaleform_trace_before_main_draw(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        ID3D11Texture2D* reduced,
        ID3D11Texture2D* native);
    void scaleform_trace_after_main_draw(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        ID3D11Texture2D* reduced);

    void scaleform_trace_observe_target(
        ID3D11DeviceContext* context,
        ID3D11RenderTargetView* requested,
        ID3D11RenderTargetView* applied,
        bool unordered_access_variant);

    void scaleform_trace_end_target_episode(
        ID3D11DeviceContext* context);
    void scaleform_trace_begin_target_episode(
        ID3D11DeviceContext* context,
        ID3D11RenderTargetView* applied);

    void scaleform_trace_report(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        ID3D11Texture2D* reduced_after,
        ID3D11Texture2D* hudless,
        ID3D11Texture2D* native);

private:
    RenderDebug() = default;

    UiCompositeMode composite_mode_{UiCompositeMode::coverage};
    bool capture_pending_{};
    bool capture_key_was_down_{};
    bool mode_key_was_down_{};
    bool directory_failure_logged_{};
    std::uint32_t capture_index_{};
    std::uint32_t files_written_{};
    std::string boundary_signature_;
    std::uint32_t boundary_reports_{};
    bool probe_key_was_down_{};
    bool probe_armed_{};
    std::unique_ptr<SceneBlitProbeState> probe_;
    std::unique_ptr<ScaleformTraceState> trace_;
    std::uint32_t trace_runs_{};
    std::uint32_t trace_ready_frames_{};
    bool trace_key_was_down_{};
    bool trace_rearm_requested_{};
};
}
