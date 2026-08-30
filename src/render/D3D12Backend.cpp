#include "render/D3D12Backend.hpp"


#include "config/Settings.hpp"
#include "streamline/StreamlineApi.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <SKSE/SKSE.h>

#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>

#include <vector>
#include <cstddef>
#include <string>
#include <array>
#include <string_view>

namespace mfgdlss::render
{
namespace
{
namespace logger = SKSE::log;
using Microsoft::WRL::ComPtr;

struct FeatureCheck
{
    sl::Feature feature;
    std::string_view name;
};

constexpr std::array kRequiredFeatures{
    FeatureCheck{sl::kFeatureDLSS, "DLSS Super Resolution"},
    FeatureCheck{sl::kFeatureDLSS_G, "DLSS Frame Generation"},
    FeatureCheck{sl::kFeatureReflex, "NVIDIA Reflex"}
};

[[nodiscard]] bool succeeded(const sl::Result result) noexcept
{
    return result == sl::Result::eOk;
}

[[nodiscard]] bool same_luid(const LUID& left, const LUID& right) noexcept
{
    return left.HighPart == right.HighPart && left.LowPart == right.LowPart;
}
}

struct D3D12Backend::State
{
    ComPtr<IDXGIFactory7> native_factory;
    ComPtr<IDXGIFactory7> proxy_factory;
    ComPtr<IDXGIAdapter4> adapter;
    ComPtr<ID3D12Device> native_device;
    ComPtr<ID3D12InfoQueue> info_queue;
    ComPtr<ID3D12Device> proxy_device;
    ComPtr<ID3D12CommandQueue> command_queue;
    DXGI_ADAPTER_DESC3 adapter_description{};
    bool streamline_proxy{};
};

D3D12Backend& D3D12Backend::instance() noexcept
{
    static D3D12Backend backend;
    return backend;
}

bool D3D12Backend::initialize(void* d3d11_device)
{
    if (ready()) {
        return true;
    }
    if (d3d11_device == nullptr) {
        logger::error("Cannot initialize D3D12 backend without Skyrim's D3D11 device");
        return false;
    }

    auto state = std::make_unique<State>();
    ComPtr<IDXGIDevice> dxgi_device;
    auto hr = static_cast<ID3D11Device*>(d3d11_device)->QueryInterface(
        IID_PPV_ARGS(&dxgi_device));
    if (FAILED(hr)) {
        logger::error("Skyrim D3D11 device does not expose IDXGIDevice: 0x{:08X}", static_cast<unsigned>(hr));
        return false;
    }

    ComPtr<IDXGIAdapter> d3d11_adapter;
    hr = dxgi_device->GetAdapter(&d3d11_adapter);
    if (FAILED(hr)) {
        logger::error("IDXGIDevice::GetAdapter failed: 0x{:08X}", static_cast<unsigned>(hr));
        return false;
    }

    DXGI_ADAPTER_DESC d3d11_description{};
    hr = d3d11_adapter->GetDesc(&d3d11_description);
    if (FAILED(hr)) {
        logger::error("IDXGIAdapter::GetDesc failed: 0x{:08X}", static_cast<unsigned>(hr));
        return false;
    }

    hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&state->native_factory));
    if (FAILED(hr)) {
        logger::error("CreateDXGIFactory2 failed: 0x{:08X}", static_cast<unsigned>(hr));
        return false;
    }

    hr = state->native_factory->EnumAdapterByLuid(
        d3d11_description.AdapterLuid,
        IID_PPV_ARGS(&state->adapter));
    if (FAILED(hr)) {
        logger::error("Unable to find Skyrim's adapter by LUID: 0x{:08X}", static_cast<unsigned>(hr));
        return false;
    }

    hr = state->adapter->GetDesc3(&state->adapter_description);
    if (FAILED(hr) ||
        !same_luid(state->adapter_description.AdapterLuid, d3d11_description.AdapterLuid)) {
        logger::error("D3D11 and D3D12 adapter identity verification failed");
        return false;
    }

    if (config::Settings::instance().d3d12_debug_layer()) {
        ComPtr<ID3D12Debug> debug_controller;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_controller))) &&
            debug_controller != nullptr) {
            debug_controller->EnableDebugLayer();
            logger::warn(
                "The Direct3D 12 debug layer is enabled by UFGU.ini. This costs "
                "performance and is only meant for investigating a fault.");
        } else {
            logger::warn(
                "The Direct3D 12 debug layer was requested but could not be "
                "created. Install the Graphics Tools optional Windows feature, "
                "or set D3D12DebugLayer back to 0.");
        }
    }

    hr = D3D12CreateDevice(
        state->adapter.Get(),
        D3D_FEATURE_LEVEL_12_0,
        IID_PPV_ARGS(&state->native_device));
    if (FAILED(hr)) {
        logger::error("D3D12CreateDevice failed: 0x{:08X}", static_cast<unsigned>(hr));
        return false;
    }

    if (config::Settings::instance().d3d12_debug_layer()) {
        if (FAILED(state->native_device.As(&state->info_queue))) {
            state->info_queue.Reset();
            logger::warn(
                "The Direct3D 12 debug layer is on but the device would not "
                "provide an info queue, so its messages cannot be logged.");
        }
    }

    D3D12_COMMAND_QUEUE_DESC queue_description{};
    queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_description.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queue_description.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queue_description.NodeMask = 0;

    const auto vendor_generator =
        config::Settings::instance().vendor_frame_generation();
    auto& streamline_api = streamline::Api::instance();
    auto use_streamline =
        streamline_api.initialized() &&
        vendor_generator == config::VendorFrameGeneration::off;
    if (vendor_generator != config::VendorFrameGeneration::off) {
        logger::info(
            "Streamline presentation proxying is disabled because "
            "VendorFrameGeneration selects {} frame generation, which takes "
            "the swap chain over rather than sharing it. DLSS-G and Reflex are "
            "unavailable this session; DLSS super-resolution is not affected.",
            vendor_generator == config::VendorFrameGeneration::intel ?
                "Intel XeSS" : "AMD FidelityFX");
    }
    if (use_streamline) {
        sl::AdapterInfo adapter_info{};
        adapter_info.deviceLUID = reinterpret_cast<std::uint8_t*>(
            &state->adapter_description.AdapterLuid);
        adapter_info.deviceLUIDSizeInBytes =
            sizeof(state->adapter_description.AdapterLuid);
        for (const auto& check : kRequiredFeatures) {
            const auto support = streamline_api.is_feature_supported(
                check.feature, adapter_info);
            logger::info(
                "{} support on selected adapter: {} ({})",
                check.name,
                succeeded(support),
                static_cast<int>(support));
            use_streamline = use_streamline && succeeded(support);
        }
    }

    if (use_streamline) {
        const auto set_device = streamline_api.set_d3d_device(
            state->native_device.Get());
        auto* proxy_device = state->native_device.Get();
        auto* proxy_factory = state->native_factory.Get();
        const auto upgrade_device = succeeded(set_device) ?
            streamline_api.upgrade_interface(
                reinterpret_cast<void**>(&proxy_device)) :
            set_device;
        ComPtr<ID3D12Device> upgraded_device;
        if (succeeded(upgrade_device) && proxy_device != nullptr) {

            upgraded_device.Attach(proxy_device);
        }

        const auto upgrade_factory = upgraded_device != nullptr ?
            streamline_api.upgrade_interface(
                reinterpret_cast<void**>(&proxy_factory)) :
            upgrade_device;
        ComPtr<IDXGIFactory7> upgraded_factory;
        if (succeeded(upgrade_factory) && proxy_factory != nullptr) {
            upgraded_factory.Attach(proxy_factory);
        }

        if (upgraded_device != nullptr && upgraded_factory != nullptr) {
            state->proxy_device = std::move(upgraded_device);
            state->proxy_factory = std::move(upgraded_factory);
            state->streamline_proxy = true;
        } else {
            logger::warn(
                "Streamline proxy creation was unavailable (device={}, "
                "factory={}); continuing through native D3D12",
                static_cast<int>(upgrade_device),
                static_cast<int>(upgrade_factory));
            use_streamline = false;
        }
    }

    if (!use_streamline) {
        state->proxy_device = state->native_device;
        state->proxy_factory = state->native_factory;
        state->streamline_proxy = false;

        logger::info(
            "Native D3D12 presentation selected; NVIDIA Streamline frame "
            "generation and Reflex are unavailable {}",
            vendor_generator != config::VendorFrameGeneration::off ?
                "because a vendor frame generator owns presentation" :
                "on this adapter");
    }

    hr = state->proxy_device->CreateCommandQueue(
        &queue_description,
        IID_PPV_ARGS(&state->command_queue));
    if (FAILED(hr)) {
        logger::error(
            "D3D12 graphics queue creation failed: 0x{:08X}",
            static_cast<unsigned>(hr));
        return false;
    }
    state->command_queue->SetName(
        state->streamline_proxy ?
            L"Universal Upscaling Streamline graphics queue" :
            L"Universal Upscaling native graphics queue");

    std::string adapter_name;
    {
        const auto* const wide = state->adapter_description.Description;
        const auto needed = WideCharToMultiByte(
            CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
        if (needed > 1) {
            adapter_name.resize(static_cast<std::size_t>(needed) - 1U);
            static_cast<void>(WideCharToMultiByte(
                CP_UTF8,
                0,
                wide,
                -1,
                adapter_name.data(),
                needed,
                nullptr,
                nullptr));
        }
    }
    logger::info(
        "D3D12 backend ready on adapter \"{}\" vendor=0x{:04X}, "
        "device=0x{:04X}, subsystem=0x{:08X}, revision=0x{:02X}, "
        "dedicated-video-memory={} MiB",
        adapter_name.empty() ? std::string{"unknown"} : adapter_name,
        state->adapter_description.VendorId,
        state->adapter_description.DeviceId,
        state->adapter_description.SubSysId,
        state->adapter_description.Revision,
        state->adapter_description.DedicatedVideoMemory / (1024ULL * 1024ULL));
    logger::info(
        "D3D12 interfaces: native-device={}, presentation-device={}, "
        "command-queue={}, presentation-factory={}, streamline={}",
        static_cast<void*>(state->native_device.Get()),
        static_cast<void*>(state->proxy_device.Get()),
        static_cast<void*>(state->command_queue.Get()),
        static_cast<void*>(state->proxy_factory.Get()),
        state->streamline_proxy);

    state_ = std::move(state);

    return true;
}

VideoMemoryStatus D3D12Backend::video_memory_status() const noexcept
{
    VideoMemoryStatus status{};
    if (state_ == nullptr || state_->adapter == nullptr) {
        return status;
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (FAILED(state_->adapter->QueryVideoMemoryInfo(
            0U, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
        return status;
    }
    status.budget_bytes = info.Budget;
    status.current_usage_bytes = info.CurrentUsage;
    status.available_for_reservation_bytes = info.AvailableForReservation;
    status.available = info.Budget != 0ULL;
    return status;
}

void D3D12Backend::drain_debug_messages() noexcept
{
    if (state_ == nullptr || state_->info_queue == nullptr) {
        return;
    }
    const auto count = state_->info_queue->GetNumStoredMessages();
    for (UINT64 index = 0; index < count; ++index) {
        SIZE_T length = 0;
        if (FAILED(state_->info_queue->GetMessage(index, nullptr, &length)) ||
            length == 0) {
            continue;
        }
        std::vector<std::byte> storage(length);
        auto* const message =
            reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (FAILED(state_->info_queue->GetMessage(index, message, &length))) {
            continue;
        }
        std::string text(
            message->pDescription, message->DescriptionByteLength);
        while (!text.empty() && text.back() == '\0') {
            text.pop_back();
        }
        if (message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ||
            message->Severity == D3D12_MESSAGE_SEVERITY_ERROR) {
            logger::error("D3D12 debug layer: {}", text);
        } else if (message->Severity == D3D12_MESSAGE_SEVERITY_WARNING) {
            logger::warn("D3D12 debug layer: {}", text);
        } else {
            logger::info("D3D12 debug layer: {}", text);
        }
    }
    state_->info_queue->ClearStoredMessages();
}

void D3D12Backend::shutdown() noexcept
{
    state_.reset();
}

bool D3D12Backend::ready() const noexcept
{
    return state_ != nullptr;
}

void* D3D12Backend::native_device() const noexcept
{
    return state_ != nullptr ? state_->native_device.Get() : nullptr;
}

void* D3D12Backend::proxy_device() const noexcept
{
    return state_ != nullptr ? state_->proxy_device.Get() : nullptr;
}

void* D3D12Backend::command_queue() const noexcept
{
    return state_ != nullptr ? state_->command_queue.Get() : nullptr;
}

void* D3D12Backend::proxy_factory() const noexcept
{
    return state_ != nullptr ? state_->proxy_factory.Get() : nullptr;
}

bool D3D12Backend::streamline_proxy_active() const noexcept
{
    return state_ != nullptr && state_->streamline_proxy;
}

std::uint32_t D3D12Backend::adapter_vendor_id() const noexcept
{
    return state_ != nullptr ? state_->adapter_description.VendorId : 0U;
}
}
