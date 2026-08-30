#pragma once

#include <cstdint>
#include <memory>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace mfgdlss::render
{
class SurfaceBlit final
{
public:
    SurfaceBlit();
    ~SurfaceBlit();

    SurfaceBlit(const SurfaceBlit&) = delete;
    SurfaceBlit& operator=(const SurfaceBlit&) = delete;

    [[nodiscard]] static SurfaceBlit& instance() noexcept;

    [[nodiscard]] bool apply(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        ID3D11Texture2D* source,
        ID3D11Texture2D* target,
        std::uint32_t source_width = 0,
        std::uint32_t source_height = 0);
    void shutdown() noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
    bool failure_logged_{};
};
}
