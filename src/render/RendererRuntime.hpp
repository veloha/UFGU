#pragma once

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;

namespace mfgdlss::render
{
enum class RendererAdapter
{
    enb,
    vanilla
};

class RendererRuntime final
{
public:
    [[nodiscard]] static RendererRuntime& instance() noexcept;

    [[nodiscard]] bool initialize(
        RendererAdapter adapter,
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        IDXGISwapChain* swap_chain,
        std::uint32_t width,
        std::uint32_t height);
    void begin_frame();
    void end_frame();
    void pre_reset() noexcept;
    void post_reset() noexcept;
    [[nodiscard]] bool resize(
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t back_buffer_count);
    void reset_all_history(const char* reason) noexcept;
    void shutdown() noexcept;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] bool streamline_features_ready() const noexcept;
    [[nodiscard]] bool uses(RendererAdapter adapter) const noexcept;

private:
    RendererAdapter adapter_{RendererAdapter::vanilla};
    bool ready_{};
    bool streamline_features_ready_{};
    bool vendor_generation_suspended_{};
    bool native_frame_open_{};
};
}
