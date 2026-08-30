#pragma once

#include <cstdint>
#include <memory>

namespace mfgdlss::render
{
struct VideoMemoryStatus final
{
    std::uint64_t budget_bytes{};
    std::uint64_t current_usage_bytes{};
    std::uint64_t available_for_reservation_bytes{};
    bool available{};
};

class D3D12Backend final
{
public:
    [[nodiscard]] static D3D12Backend& instance() noexcept;

    [[nodiscard]] bool initialize(void* d3d11_device);
    void shutdown() noexcept;
    void drain_debug_messages() noexcept;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] void* native_device() const noexcept;
    [[nodiscard]] void* proxy_device() const noexcept;
    [[nodiscard]] void* command_queue() const noexcept;
    [[nodiscard]] void* proxy_factory() const noexcept;
    [[nodiscard]] bool streamline_proxy_active() const noexcept;
    [[nodiscard]] std::uint32_t adapter_vendor_id() const noexcept;
    [[nodiscard]] VideoMemoryStatus video_memory_status() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};
}
