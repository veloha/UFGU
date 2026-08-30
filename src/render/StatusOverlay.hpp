#pragma once

#include <RE/Skyrim.h>

#include <memory>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace mfgdlss::render
{
class StatusOverlay final :
    public RE::BSTEventSink<RE::InputEvent*>
{
public:
    [[nodiscard]] static StatusOverlay& instance() noexcept;

    [[nodiscard]] bool install_input();
    [[nodiscard]] bool menu_open() const noexcept;
    void draw(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        ID3D11Texture2D* back_buffer);

    void draw_into_ui_layer(
        ID3D11DeviceContext* context,
        ID3D11Texture2D* ui_layer);
    void shutdown() noexcept;

    RE::BSEventNotifyControl ProcessEvent(
        RE::InputEvent* const* events,
        RE::BSTEventSource<RE::InputEvent*>* source) override;

private:
    struct State;
    std::unique_ptr<State> state_;
    bool input_installed_{};
};
}
