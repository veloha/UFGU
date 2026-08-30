#pragma once

#include <cstdint>
#include <memory>

#include <d3d11.h>

namespace mfgdlss::render
{

enum class DebugView : std::uint32_t
{
    off = 0,
    motion_vectors = 1,
    depth = 2,

    scene_reference = 3,

    violations = 4,

    generator_motion = 5,

    generator_depth = 6,

    ui_layer = 7,
};

static_assert(
    static_cast<std::uint32_t>(DebugView::ui_layer) == 7U,
    "DebugView::ui_layer must remain the highest value; "
    "debug_view_from_index rejects anything above it, so inserting a value "
    "below ui_layer would silently disable the views above the new bound");

[[nodiscard]] constexpr DebugView debug_view_from_index(
    const std::uint32_t index) noexcept
{
    return index > static_cast<std::uint32_t>(DebugView::ui_layer) ?
        DebugView::off :
        static_cast<DebugView>(index);
}

enum class DebugViewStatus : std::uint32_t
{
    drawn = 0,
    off,
    no_output_surface,
    no_device,
    no_source,
    shader_compilation_failed,
    resource_creation_failed,
    output_not_renderable,
    source_format_unsupported,
    source_not_shader_readable,
    unsupported_uav_binding,
};

[[nodiscard]] const char* describe(DebugViewStatus status) noexcept;

[[nodiscard]] bool srv_format_for(
    DXGI_FORMAT source_format,
    std::uint32_t bind_flags,
    DXGI_FORMAT& srv_format) noexcept;

[[nodiscard]] float depth_far_epsilon_for(DXGI_FORMAT srv_format) noexcept;

struct DebugViewParams
{
    DebugView view{DebugView::off};

    std::uint32_t active_width{};
    std::uint32_t active_height{};

    bool reversed_depth{true};

    bool frame_tainted{};
};

using DebugViewLogSink = void (*)(const char* message);
void set_debug_view_log_sink(DebugViewLogSink sink) noexcept;

class DebugViewRenderer final
{
public:
    DebugViewRenderer();
    ~DebugViewRenderer();

    DebugViewRenderer(const DebugViewRenderer&) = delete;
    DebugViewRenderer& operator=(const DebugViewRenderer&) = delete;

    [[nodiscard]] DebugViewStatus render(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        ID3D11Texture2D* target,
        ID3D11Texture2D* source,
        const DebugViewParams& params);

    void shutdown() noexcept;

    [[nodiscard]] bool holds_resources() const noexcept;

    [[nodiscard]] DXGI_FORMAT resolved_source_view_format() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};
}
