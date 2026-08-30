#pragma once

#include "render/ResourceProbe.hpp"

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace mfgdlss::render
{
enum class DepthStage : std::uint32_t
{
    engine_before_copy = 0,
    input_after_copy,
    input_before_evaluate,
    input_after_evaluate,
    count
};

[[nodiscard]] const char* describe(DepthStage stage) noexcept;

class DepthDiagnostics final
{
public:
    [[nodiscard]] static DepthDiagnostics& instance() noexcept;

    void arm() noexcept;
    [[nodiscard]] bool armed() const noexcept;

    void observe(
        DepthStage stage,
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        ID3D11Texture2D* texture,
        bool reversed_depth);

    void collect(ID3D11DeviceContext* context);

    [[nodiscard]] const ProbeStatistics& statistics(
        DepthStage stage) const noexcept;
    [[nodiscard]] ProbeStatus status(DepthStage stage) const noexcept;

    [[nodiscard]] bool round_complete() const noexcept;

    [[nodiscard]] const char* verdict() const noexcept;

    void shutdown() noexcept;

private:
    DepthDiagnostics();
    ~DepthDiagnostics();
    DepthDiagnostics(const DepthDiagnostics&) = delete;
    DepthDiagnostics& operator=(const DepthDiagnostics&) = delete;

    struct State;
    std::unique_ptr<State> state_;
};
}
