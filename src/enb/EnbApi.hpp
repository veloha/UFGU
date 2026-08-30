#pragma once

#include <cstdint>

namespace mfgdlss::enb
{
enum class Callback : long
{
    end_frame = 1,
    begin_frame = 2,
    pre_save = 3,
    post_load = 4,
    initialize = 5,
    exit = 6,
    pre_reset = 7,
    post_reset = 8
};

using CallbackFunction = void(__stdcall*)(Callback);

struct RenderInfo
{
    void* d3d11_device{};
    void* d3d11_context{};
    void* dxgi_swap_chain{};
    std::uint32_t width{};
    std::uint32_t height{};
};

class Api final
{
public:
    [[nodiscard]] static Api& instance() noexcept;

    [[nodiscard]] bool connect() noexcept;
    void disconnect() noexcept;

    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] long sdk_version() const noexcept;
    [[nodiscard]] long enb_version() const noexcept;
    [[nodiscard]] RenderInfo* render_info() const noexcept;
    [[nodiscard]] bool set_callback(CallbackFunction callback) const noexcept;

private:
    using GetVersion = long(*)();
    using SetCallback = void(*)(CallbackFunction);
    using GetRenderInfo = RenderInfo*(*)();

    void* module_{};
    GetVersion get_sdk_version_{};
    GetVersion get_enb_version_{};
    SetCallback set_callback_{};
    GetRenderInfo get_render_info_{};
};
}
