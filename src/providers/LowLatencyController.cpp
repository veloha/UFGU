#include "providers/LowLatencyController.hpp"

#include "config/Settings.hpp"

#include <Windows.h>

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <SKSE/SKSE.h>

#include <ffx_antilag2_dx11.h>

namespace mfgdlss::providers
{
namespace
{
namespace logger = SKSE::log;
using Microsoft::WRL::ComPtr;

constexpr UINT kVendorNvidia = 0x10DEU;
constexpr UINT kVendorAmd = 0x1002U;
constexpr UINT kVendorIntel = 0x8086U;

[[nodiscard]] UINT adapter_vendor_of(ID3D11Device* const device) noexcept
{
    if (device == nullptr) {
        return 0U;
    }
    ComPtr<IDXGIDevice> dxgi_device;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device)))) {
        return 0U;
    }
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgi_device->GetAdapter(&adapter)) || adapter == nullptr) {
        return 0U;
    }
    DXGI_ADAPTER_DESC description{};
    if (FAILED(adapter->GetDesc(&description))) {
        return 0U;
    }
    return description.VendorId;
}
}

struct LowLatencyController::State
{
    LatencyBackend backend{LatencyBackend::none};
    LatencyUnavailableReason reason{
        LatencyUnavailableReason::no_supported_gpu};
    LatencyMode mode{LatencyMode::off};

    AMD::AntiLag2DX11::Context antilag_context{};
    bool antilag_initialized{};
    bool update_failure_logged{};
    bool reported_enable{};
    std::uint32_t reported_max_fps{0xFFFFFFFFU};

    ~State()
    {
        if (antilag_initialized) {
            static_cast<void>(
                AMD::AntiLag2DX11::DeInitialize(&antilag_context));
            antilag_initialized = false;
        }
    }
};

LowLatencyController::~LowLatencyController()
{
    delete state_;
    state_ = nullptr;
}

LowLatencyController& LowLatencyController::instance() noexcept
{
    static LowLatencyController controller;
    return controller;
}

void LowLatencyController::initialize(ID3D11Device* const device) noexcept
{
    if (state_ == nullptr) {

#pragma warning(suppress : 28182)
        state_ = new (std::nothrow) State{};
        if (state_ == nullptr) {
            return;
        }
    }

    const auto vendor = adapter_vendor_of(device);

    LatencyEnvironment environment;
    environment.nvidia_gpu = vendor == kVendorNvidia;
    environment.amd_gpu = vendor == kVendorAmd;
    environment.intel_gpu = vendor == kVendorIntel;

    environment.reflex_runtime =
        environment.nvidia_gpu &&
        GetModuleHandleW(L"sl.interposer.dll") != nullptr;

    if (environment.amd_gpu && device != nullptr) {
        const auto hr =
            AMD::AntiLag2DX11::Initialize(&state_->antilag_context);
        state_->antilag_initialized = SUCCEEDED(hr);
        environment.antilag2_runtime = state_->antilag_initialized;
        if (!state_->antilag_initialized) {
            logger::info(
                "AMD Anti-Lag 2 is unavailable on this adapter (HRESULT "
                "0x{:08X})",
                static_cast<std::uint32_t>(hr));
        } else {
            logger::warn(
                "AMD Anti-Lag 2 initialised, but read this before measuring "
                "latency with it. Its entire mechanism is a delay, and AMD's "
                "header states the update must be called once per frame just "
                "before the game polls input, because that is what shortens "
                "the gap between a key press and the frame that reflects it. "
                "This plugin drives it from the renderer's frame start, which "
                "under ENB is the start of ENB's rendering and without ENB is "
                "inside Present. Both are after Skyrim has already sampled "
                "input for that frame. Anti-Lag is therefore genuinely "
                "enabled and its delay is not where AMD intends, so a latency "
                "measurement here is a measurement of this placement rather "
                "than of what Anti-Lag can do. Correcting it needs a hook on "
                "Skyrim's input polling, which this plugin does not currently "
                "have");
        }
    }

    environment.xell_runtime = environment.intel_gpu;
    environment.d3d12_presentation = true;
    environment.xess_frame_generation_active =
        config::Settings::instance().vendor_frame_generation() ==
        config::VendorFrameGeneration::intel;
    environment.fsr_frame_generation_active =
        config::Settings::instance().vendor_frame_generation() ==
        config::VendorFrameGeneration::amd;

    const auto selection = select_latency_backend(environment);
    state_->backend = selection.backend;
    state_->reason = selection.reason;
    state_->mode = clamp_mode(state_->backend, state_->mode);

    if (state_->backend != LatencyBackend::amd_antilag2 &&
        state_->antilag_initialized) {
        static_cast<void>(AMD::AntiLag2DX11::DeInitialize(
            &state_->antilag_context));
        state_->antilag_initialized = false;
    }

    logger::info("Low latency backend: {}", describe());
}

void LowLatencyController::shutdown() noexcept
{
    if (state_ == nullptr) {
        return;
    }
    if (state_->antilag_initialized) {
        static_cast<void>(
            AMD::AntiLag2DX11::DeInitialize(&state_->antilag_context));
        state_->antilag_initialized = false;
    }
    state_->backend = LatencyBackend::none;
    state_->reason = LatencyUnavailableReason::no_supported_gpu;
    state_->mode = LatencyMode::off;
    state_->update_failure_logged = false;
    state_->reported_enable = false;
    state_->reported_max_fps = 0xFFFFFFFFU;
}

void LowLatencyController::update_before_input_sampling(
    const std::uint32_t max_fps) noexcept
{
    if (state_ == nullptr ||
        state_->backend != LatencyBackend::amd_antilag2 ||
        !state_->antilag_initialized) {
        return;
    }

    const auto enable = state_->mode != LatencyMode::off;
    if (state_->reported_enable != enable ||
        state_->reported_max_fps != max_fps) {

        state_->reported_enable = enable;
        state_->reported_max_fps = max_fps;
        logger::info(
            "AMD Anti-Lag 2 update: enable={}, maxFPS={}. AMD's header "
            "documents maxFPS as a framerate limit in its own right and sends "
            "it whatever the mode is, so a cap of {} should apply even with "
            "latency reduction off. If the observed frame rate does not match "
            "it, the driver is ignoring the limit while disabled and the "
            "Final Output Cap row is right to stay gated",
            enable,
            max_fps,
            max_fps);
    }
    const auto hr = AMD::AntiLag2DX11::Update(
        &state_->antilag_context,
        enable,
        max_fps);
    if (FAILED(hr) && !state_->update_failure_logged) {
        state_->update_failure_logged = true;
        logger::warn(
            "AMD Anti-Lag 2 update failed (HRESULT 0x{:08X}), so latency "
            "reduction is not being applied for as long as this keeps "
            "failing. This is reported once per initialisation rather than "
            "once per frame, so the absence of further lines does not mean it "
            "recovered",
            static_cast<std::uint32_t>(hr));
    }
}

void LowLatencyController::set_mode(const LatencyMode mode) noexcept
{
    if (state_ == nullptr) {
        return;
    }
    const auto clamped = clamp_mode(state_->backend, mode);
    if (clamped == state_->mode) {
        return;
    }
    state_->mode = clamped;
    if (state_->backend != LatencyBackend::none) {
        logger::info(
            "Low latency mode is now {} on {}",
            mode_display_name(state_->backend, state_->mode),
            backend_display_name(state_->backend));
    }
}

void LowLatencyController::refresh_mode_from_settings() noexcept
{
    switch (config::Settings::instance().reflex_mode()) {
    case config::ReflexMode::off:
        set_mode(LatencyMode::off);
        return;
    case config::ReflexMode::low_latency:
        set_mode(LatencyMode::on);
        return;
    case config::ReflexMode::low_latency_boost:
        set_mode(LatencyMode::boost);
        return;
    }
    set_mode(LatencyMode::off);
}

LatencyMode LowLatencyController::mode() const noexcept
{
    return state_ != nullptr ? state_->mode : LatencyMode::off;
}

LatencyBackend LowLatencyController::backend() const noexcept
{
    return state_ != nullptr ? state_->backend : LatencyBackend::none;
}

LatencyUnavailableReason LowLatencyController::reason() const noexcept
{
    return state_ != nullptr ? state_->reason :
                               LatencyUnavailableReason::no_supported_gpu;
}

bool LowLatencyController::available() const noexcept
{
    return state_ != nullptr && state_->backend != LatencyBackend::none &&
           state_->reason == LatencyUnavailableReason::available;
}

std::string LowLatencyController::describe() const noexcept
{
    if (available()) {
        return std::string{backend_display_name(backend())} + " (" +
               mode_display_name(backend(), mode()) + ")";
    }
    return std::string{"unavailable: "} + unavailable_reason_text(reason());
}
}
