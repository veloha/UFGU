#pragma once

#include <memory>

struct ID3D11Texture2D;

namespace mfgdlss::render
{
class SharpeningPass final
{
public:
    SharpeningPass();
    ~SharpeningPass();

    SharpeningPass(const SharpeningPass&) = delete;
    SharpeningPass& operator=(const SharpeningPass&) = delete;

    [[nodiscard]] static SharpeningPass& instance() noexcept;

    [[nodiscard]] bool apply(
        ID3D11Texture2D* target,
        float sharpness);
    void shutdown() noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
    bool failure_logged_{};
    bool first_apply_logged_{};
};
}
