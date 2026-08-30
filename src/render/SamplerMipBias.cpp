#include "render/SamplerMipBias.hpp"

#include "config/Settings.hpp"
#include "streamline/SuperResolution.hpp"

#include <Windows.h>
#include <d3d11.h>

#include <REL/Relocation.h>
#include <SKSE/SKSE.h>

#include <atomic>
#include <cmath>

namespace mfgdlss::render
{
namespace
{
namespace logger = SKSE::log;

using CreateSamplerStateFunction = HRESULT(STDMETHODCALLTYPE*)(
    ID3D11Device*,
    const D3D11_SAMPLER_DESC*,
    ID3D11SamplerState**);

CreateSamplerStateFunction original_create_sampler_state{};

std::atomic<float> active_bias{0.0F};
std::atomic_uint32_t biased_count{};
std::atomic_uint32_t untouched_count{};
std::atomic_bool first_bias_logged{};

HRESULT STDMETHODCALLTYPE create_sampler_state_hook(
    ID3D11Device* const device,
    const D3D11_SAMPLER_DESC* const description,
    ID3D11SamplerState** const sampler)
{
    const auto& super_resolution = streamline::SuperResolution::instance();
    const auto bias = super_resolution.enabled()
        ? mip_bias_for_scale(
              super_resolution.render_width(), super_resolution.output_width())
        : 0.0F;
    active_bias.store(bias, std::memory_order_relaxed);
    if (description == nullptr || bias == 0.0F) {
        return original_create_sampler_state(device, description, sampler);
    }

    if (!sampler_carries_material_mip_chain(
            static_cast<std::uint32_t>(description->Filter),
            description->MipLODBias,
            description->MaxLOD)) {
        untouched_count.fetch_add(1, std::memory_order_relaxed);
        return original_create_sampler_state(device, description, sampler);
    }

    auto biased = *description;
    biased.MipLODBias = bias;
    const auto count = biased_count.fetch_add(1, std::memory_order_relaxed) + 1U;
    if (!first_bias_logged.exchange(true, std::memory_order_relaxed)) {
        logger::info(
            "Sampler mip bias applied to the first material sampler: filter "
            "0x{:04X}, anisotropy {}, MipLODBias 0.0 becomes {:.4f}. AMD and "
            "Intel both document the same formula, log2(render/display) minus "
            "one, and neither this plugin nor Skyrim applied any bias before. "
            "Only samplers with a full mip chain and no existing bias are "
            "touched, so shadow comparison samplers and single mip UI "
            "samplers are left exactly as the engine created them",
            static_cast<std::uint32_t>(description->Filter),
            description->MaxAnisotropy,
            bias);
    }
    if (count == 32U || count == 256U || count % 2048U == 0U) {
        logger::info(
            "Sampler mip bias census: {} material samplers biased at {:.4f}, "
            "{} left untouched because they were comparison, single mip or "
            "already biased. A count that stays at zero after gameplay starts "
            "would mean the selection rule matches nothing this load order "
            "creates, which is a real answer and not a silent no-op",
            count,
            bias,
            untouched_count.load(std::memory_order_relaxed));
    }
    return original_create_sampler_state(device, &biased, sampler);
}
}

void install_sampler_mip_bias(void* d3d11_device)
{
    if (d3d11_device == nullptr ||
        original_create_sampler_state != nullptr) {
        return;
    }
    const auto& settings = config::Settings::instance();
    if (!settings.experimental_features()) {
        return;
    }
    if (settings.mip_bias_mode() != config::MipBiasMode::automatic) {
        logger::info(
            "Sampler mip bias is available but [Upscaling] MipBias is Off. "
            "Set it to Auto to apply the vendor formula, which is "
            "log2(render/display) minus one. It is a separate key from the "
            "experimental master switch so this one feature can be turned off "
            "on its own if it makes distant textures shimmer");
        return;
    }

    const auto vtable_address =
        *reinterpret_cast<std::uintptr_t*>(d3d11_device);
    REL::Relocation<std::uintptr_t> vtable{vtable_address};
    original_create_sampler_state =
        reinterpret_cast<CreateSamplerStateFunction>(
            vtable.write_vfunc(23, create_sampler_state_hook));
    if (original_create_sampler_state == nullptr) {
        logger::error(
            "Sampler mip bias could not hook ID3D11Device::CreateSamplerState");
        return;
    }

    logger::info(
        "Sampler mip bias hook installed. This is EXPERIMENTAL. The bias is "
        "computed at each sampler creation from the upscaler's live render "
        "and output extents, not once at startup: at this point the upscaler "
        "still reports render equal to output with the mode off, so a bias "
        "computed here would be zero and the feature would never arm. "
        "Samplers Skyrim already created keep their existing bias, so the "
        "effect grows as new ones are created and is not retroactive");
}
}
