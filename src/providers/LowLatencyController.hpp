#pragma once

#include "providers/LowLatency.hpp"

#include <cstdint>
#include <string>

struct ID3D11Device;

namespace mfgdlss::providers
{

class LowLatencyController final
{
public:
    [[nodiscard]] static LowLatencyController& instance() noexcept;

    void initialize(ID3D11Device* device) noexcept;
    void shutdown() noexcept;

    void update_before_input_sampling(std::uint32_t max_fps) noexcept;

    void set_mode(LatencyMode mode) noexcept;
    void refresh_mode_from_settings() noexcept;
    [[nodiscard]] LatencyMode mode() const noexcept;

    [[nodiscard]] LatencyBackend backend() const noexcept;
    [[nodiscard]] LatencyUnavailableReason reason() const noexcept;
    [[nodiscard]] bool available() const noexcept;

    [[nodiscard]] std::string describe() const noexcept;

private:
    LowLatencyController() = default;
    ~LowLatencyController();
    LowLatencyController(const LowLatencyController&) = delete;
    LowLatencyController& operator=(const LowLatencyController&) = delete;
    LowLatencyController(LowLatencyController&&) = delete;
    LowLatencyController& operator=(LowLatencyController&&) = delete;

    struct State;
    State* state_{};
};
}
