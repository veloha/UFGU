#pragma once

#include <cstdint>
#include <memory>

struct ID3D11Texture2D;

namespace mfgdlss::render
{
class DepthRestoration final
{
public:
    [[nodiscard]] static DepthRestoration& instance() noexcept;

    [[nodiscard]] bool restore(
        ID3D11Texture2D* render_depth,
        std::uint32_t output_width,
        std::uint32_t output_height,
        float jitter_x,
        float jitter_y);
    void shutdown() noexcept;

private:
    struct State;

    DepthRestoration();
    ~DepthRestoration();
    DepthRestoration(const DepthRestoration&) = delete;
    DepthRestoration& operator=(const DepthRestoration&) = delete;

    std::unique_ptr<State> state_;
    bool depth_unavailable_logged_{};
    bool depth_extent_logged_{};
    bool first_restore_logged_{};
    bool auxiliary_status_logged_{};
};
}
