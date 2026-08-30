#include "render/DepthRestoration.hpp"

#include "render/MainDepthTracker.hpp"

#include "render/DepthContract.hpp"
#include "render/PresentationBridge.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace mfgdlss::render
{
namespace
{
namespace logger = SKSE::log;
using Microsoft::WRL::ComPtr;

[[nodiscard]] bool compile_shader(
    const std::string_view source,
    const char* entry_point,
    const char* target,
    ComPtr<ID3DBlob>& bytecode)
{
    ComPtr<ID3DBlob> errors;
    const auto result = D3DCompile(
        source.data(),
        source.size(),
        "MFGDLSS_DepthRestoration",
        nullptr,
        nullptr,
        entry_point,
        target,
        D3DCOMPILE_OPTIMIZATION_LEVEL3 |
            D3DCOMPILE_WARNINGS_ARE_ERRORS,
        0,
        &bytecode,
        &errors);
    if (SUCCEEDED(result)) {
        return true;
    }
    const auto message = errors != nullptr
        ? std::string_view(
              static_cast<const char*>(errors->GetBufferPointer()),
              errors->GetBufferSize())
        : std::string_view{"no compiler diagnostics"};
    logger::error(
        "Depth-restoration {} compilation failed: 0x{:08X}: {}",
        target,
        static_cast<unsigned>(result),
        message);
    return false;
}

struct SurfaceCopy
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    ID3D11Texture2D* source{};
    std::uint32_t width{};
    std::uint32_t height{};

    [[nodiscard]] bool prepare(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        ID3D11Texture2D* source_texture,
        const std::uint32_t active_width,
        const std::uint32_t active_height)
    {
        if (device == nullptr || context == nullptr ||
            source_texture == nullptr ||
            active_width == 0 || active_height == 0) {
            return false;
        }

        D3D11_TEXTURE2D_DESC source_description{};
        source_texture->GetDesc(&source_description);
        if (active_width > source_description.Width ||
            active_height > source_description.Height ||
            source_description.SampleDesc.Count != 1) {
            return false;
        }

        if (texture == nullptr ||
            source != source_texture ||
            width != active_width ||
            height != active_height) {
            texture.Reset();
            view.Reset();
            auto copy_description = source_description;
            copy_description.Width = active_width;
            copy_description.Height = active_height;
            copy_description.MipLevels = 1;
            copy_description.ArraySize = 1;
            copy_description.SampleDesc = {1, 0};
            copy_description.Usage = D3D11_USAGE_DEFAULT;
            copy_description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            copy_description.CPUAccessFlags = 0;
            copy_description.MiscFlags = 0;
            auto result = device->CreateTexture2D(
                &copy_description,
                nullptr,
                &texture);
            if (SUCCEEDED(result)) {
                result = device->CreateShaderResourceView(
                    texture.Get(),
                    nullptr,
                    &view);
            }
            if (FAILED(result)) {
                texture.Reset();
                view.Reset();
                return false;
            }
            source = source_texture;
            width = active_width;
            height = active_height;
        }

        DepthCopyInputs copy_inputs{};
        copy_inputs.source_width = source_description.Width;
        copy_inputs.source_height = source_description.Height;
        copy_inputs.destination_width = active_width;
        copy_inputs.destination_height = active_height;
        copy_inputs.depth_stencil =
            (source_description.BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0U;
        copy_inputs.sample_count = source_description.SampleDesc.Count;
        const auto plan = copy_plan_for(copy_inputs);
        if (is_refusal(plan)) {
            logger::error(
                "Depth restoration source refused: {}. resource={}x{}, "
                "active={}x{}",
                describe(plan),
                source_description.Width,
                source_description.Height,
                active_width,
                active_height);
            return false;
        }
        if (plan == DepthCopyPlan::whole_subresource) {
            context->CopySubresourceRegion(
                texture.Get(), 0, 0, 0, 0, source_texture, 0, nullptr);
        } else {
            const D3D11_BOX region{
                0,
                0,
                0,
                active_width,
                active_height,
                1};
            context->CopySubresourceRegion(
                texture.Get(),
                0,
                0,
                0,
                0,
                source_texture,
                0,
                &region);
        }
        return true;
    }
};
}

struct DepthRestoration::State
{
    ComPtr<ID3D11VertexShader> vertex_shader;
    ComPtr<ID3D11PixelShader> pixel_shader;
    ComPtr<ID3D11PixelShader> surface_pixel_shader;
    ComPtr<ID3D11PixelShader> depth_surface_pixel_shader;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11BlendState> blend_state;
    ComPtr<ID3D11DepthStencilState> depth_state;
    ComPtr<ID3D11Buffer> sampling_constants;
    ComPtr<ID3D11ShaderResourceView> source_view;
    ID3D11Texture2D* source_texture{};
    std::array<SurfaceCopy, 3> auxiliary;

    [[nodiscard]] bool initialize(
        ID3D11Device* device,
        ID3D11Texture2D* source)
    {
        if (device == nullptr || source == nullptr) {
            return false;
        }

        constexpr std::string_view vertex_source = R"(
struct output_data
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

output_data main(uint vertex_id : SV_VertexID)
{
    output_data output;
    output.texcoord = float2(
        (vertex_id << 1) & 2,
        vertex_id & 2);
    output.position = float4(
        output.texcoord * float2(2.0, -2.0) +
            float2(-1.0, 1.0),
        0.0,
        1.0);
    return output;
}
)";
        constexpr std::string_view pixel_source = R"(
Texture2D<float> source_depth : register(t0);
SamplerState linear_clamp : register(s0);

cbuffer sampling_data : register(b0)
{
    float2 jitter_uv;
    float2 padding;
};

struct input_data
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

float main(input_data input) : SV_Depth
{
    return source_depth.SampleLevel(
        linear_clamp,
        saturate(input.texcoord - jitter_uv),
        0);
}
)";
        constexpr std::string_view surface_pixel_source = R"(
Texture2D<float4> source_surface : register(t0);
SamplerState linear_clamp : register(s0);

cbuffer sampling_data : register(b0)
{
    float2 jitter_uv;
    float2 padding;
};

struct input_data
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

float4 main(input_data input) : SV_Target
{
    return source_surface.SampleLevel(
        linear_clamp,
        saturate(input.texcoord - jitter_uv),
        0);
}
)";
        constexpr std::string_view depth_surface_pixel_source = R"(
Texture2D<float> source_depth : register(t0);
SamplerState linear_clamp : register(s0);

cbuffer sampling_data : register(b0)
{
    float2 jitter_uv;
    float2 padding;
};

struct input_data
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

float main(input_data input) : SV_Target
{
    return source_depth.SampleLevel(
        linear_clamp,
        saturate(input.texcoord - jitter_uv),
        0);
}
)";

        ComPtr<ID3DBlob> vertex_bytecode;
        ComPtr<ID3DBlob> pixel_bytecode;
        ComPtr<ID3DBlob> surface_pixel_bytecode;
        ComPtr<ID3DBlob> depth_surface_pixel_bytecode;
        if (!compile_shader(
                vertex_source,
                "main",
                "vs_5_0",
                vertex_bytecode) ||
            !compile_shader(
                pixel_source,
                "main",
                "ps_5_0",
                pixel_bytecode) ||
            !compile_shader(
                surface_pixel_source,
                "main",
                "ps_5_0",
                surface_pixel_bytecode) ||
            !compile_shader(
                depth_surface_pixel_source,
                "main",
                "ps_5_0",
                depth_surface_pixel_bytecode)) {
            return false;
        }

        auto result = device->CreateVertexShader(
            vertex_bytecode->GetBufferPointer(),
            vertex_bytecode->GetBufferSize(),
            nullptr,
            &vertex_shader);
        if (SUCCEEDED(result)) {
            result = device->CreatePixelShader(
                pixel_bytecode->GetBufferPointer(),
                pixel_bytecode->GetBufferSize(),
                nullptr,
                &pixel_shader);
        }
        if (SUCCEEDED(result)) {
            result = device->CreatePixelShader(
                surface_pixel_bytecode->GetBufferPointer(),
                surface_pixel_bytecode->GetBufferSize(),
                nullptr,
                &surface_pixel_shader);
        }
        if (SUCCEEDED(result)) {
            result = device->CreatePixelShader(
                depth_surface_pixel_bytecode->GetBufferPointer(),
                depth_surface_pixel_bytecode->GetBufferSize(),
                nullptr,
                &depth_surface_pixel_shader);
        }

        D3D11_TEXTURE2D_DESC source_description{};
        source->GetDesc(&source_description);
        D3D11_SHADER_RESOURCE_VIEW_DESC view_description{};
        switch (source_description.Format) {
        case DXGI_FORMAT_R24G8_TYPELESS:
            view_description.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            break;
        case DXGI_FORMAT_R32_TYPELESS:
            view_description.Format = DXGI_FORMAT_R32_FLOAT;
            break;
        default:
            logger::error(
                "Unsupported depth-restoration source format {}",
                static_cast<unsigned>(source_description.Format));
            return false;
        }
        view_description.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        view_description.Texture2D.MipLevels = 1;
        if (SUCCEEDED(result)) {
            result = device->CreateShaderResourceView(
                source,
                &view_description,
                &source_view);
        }

        D3D11_SAMPLER_DESC sampler_description{};
        sampler_description.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler_description.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_description.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_description.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_description.MaxLOD = D3D11_FLOAT32_MAX;
        if (SUCCEEDED(result)) {
            result = device->CreateSamplerState(
                &sampler_description,
                &sampler);
        }

        D3D11_RASTERIZER_DESC rasterizer_description{};
        rasterizer_description.FillMode = D3D11_FILL_SOLID;
        rasterizer_description.CullMode = D3D11_CULL_NONE;
        rasterizer_description.DepthClipEnable = FALSE;
        if (SUCCEEDED(result)) {
            result = device->CreateRasterizerState(
                &rasterizer_description,
                &rasterizer);
        }

        D3D11_BLEND_DESC blend_description{};
        blend_description.RenderTarget[0].RenderTargetWriteMask =
            D3D11_COLOR_WRITE_ENABLE_ALL;
        if (SUCCEEDED(result)) {
            result = device->CreateBlendState(
                &blend_description,
                &blend_state);
        }

        D3D11_DEPTH_STENCIL_DESC depth_description{};
        depth_description.DepthEnable = TRUE;
        depth_description.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        depth_description.DepthFunc = D3D11_COMPARISON_ALWAYS;
        depth_description.StencilEnable = FALSE;
        if (SUCCEEDED(result)) {
            result = device->CreateDepthStencilState(
                &depth_description,
                &depth_state);
        }

        D3D11_BUFFER_DESC constant_description{};
        constant_description.ByteWidth = 16;
        constant_description.Usage = D3D11_USAGE_DEFAULT;
        constant_description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (SUCCEEDED(result)) {
            result = device->CreateBuffer(
                &constant_description,
                nullptr,
                &sampling_constants);
        }
        if (FAILED(result)) {
            logger::error(
                "Creating depth-restoration state failed: 0x{:08X}",
                static_cast<unsigned>(result));
            return false;
        }

        source_texture = source;
        return true;
    }
};

DepthRestoration::DepthRestoration() = default;
DepthRestoration::~DepthRestoration() = default;

DepthRestoration& DepthRestoration::instance() noexcept
{
    static DepthRestoration restoration;
    return restoration;
}

bool DepthRestoration::restore(
    ID3D11Texture2D* render_depth,
    const std::uint32_t output_width,
    const std::uint32_t output_height,
    const float jitter_x,
    const float jitter_y)
{
    if (render_depth == nullptr ||
        output_width == 0 ||
        output_height == 0) {
        return false;
    }

    D3D11_TEXTURE2D_DESC source_description{};
    render_depth->GetDesc(&source_description);
    if (source_description.Width == output_width &&
        source_description.Height == output_height) {
        return true;
    }

    auto* renderer = RE::BSGraphics::Renderer::GetRendererData();
    auto* device = PresentationBridge::instance().d3d11_device();
    auto* context = PresentationBridge::instance().d3d11_context();
    if (renderer == nullptr || device == nullptr || context == nullptr) {
        return false;
    }
    constexpr auto resource_slot_count =
        D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
    const std::array<
        ID3D11ShaderResourceView*,
        resource_slot_count> no_resources{};
    context->VSSetShaderResources(
        0,
        resource_slot_count,
        no_resources.data());
    context->HSSetShaderResources(
        0,
        resource_slot_count,
        no_resources.data());
    context->DSSetShaderResources(
        0,
        resource_slot_count,
        no_resources.data());
    context->GSSetShaderResources(
        0,
        resource_slot_count,
        no_resources.data());
    context->PSSetShaderResources(
        0,
        resource_slot_count,
        no_resources.data());
    context->CSSetShaderResources(
        0,
        resource_slot_count,
        no_resources.data());
    constexpr auto unordered_count =
        D3D11_PS_CS_UAV_REGISTER_COUNT;
    const std::array<ID3D11UnorderedAccessView*, unordered_count>
        no_unordered_outputs{};
    context->CSSetUnorderedAccessViews(
        0,
        unordered_count,
        no_unordered_outputs.data(),
        nullptr);
    auto* const output_texture = MainDepthTracker::instance().texture();
    auto* const output_view = MainDepthTracker::instance().writable_view();
    if (output_texture == nullptr || output_view == nullptr) {
        if (!depth_unavailable_logged_) {
            depth_unavailable_logged_ = true;
            logger::error(
                "Skyrim main depth is unavailable for full-resolution "
                "restoration");
        }
        return false;
    }

    D3D11_TEXTURE2D_DESC output_description{};
    output_texture->GetDesc(&output_description);
    if (output_description.Width != output_width ||
        output_description.Height != output_height ||
        output_description.SampleDesc.Count != 1) {
        if (!depth_extent_logged_) {
            depth_extent_logged_ = true;
            logger::error(
                "Skyrim main depth extent is incompatible: {}x{}, "
                "expected {}x{}, samples={}",
                output_description.Width,
                output_description.Height,
                output_width,
                output_height,
                output_description.SampleDesc.Count);
        }
        return false;
    }

    if (state_ == nullptr || state_->source_texture != render_depth) {
        auto state = std::make_unique<State>();
        if (!state->initialize(device, render_depth)) {
            return false;
        }
        state_ = std::move(state);
    }

    const std::array sampling_data{
        jitter_x / static_cast<float>(source_description.Width),
        jitter_y / static_cast<float>(source_description.Height),
        0.0F,
        0.0F};
    context->UpdateSubresource(
        state_->sampling_constants.Get(),
        0,
        nullptr,
        sampling_data.data(),
        0,
        0);
    auto* sampling_constants = state_->sampling_constants.Get();
    context->PSSetConstantBuffers(0, 1, &sampling_constants);

    const D3D11_VIEWPORT viewport{
        0.0F,
        0.0F,
        static_cast<float>(output_width),
        static_cast<float>(output_height),
        0.0F,
        1.0F};
    const D3D11_RECT scissor{
        0,
        0,
        static_cast<LONG>(output_width),
        static_cast<LONG>(output_height)};
    context->RSSetViewports(1, &viewport);
    context->RSSetScissorRects(1, &scissor);
    context->RSSetState(state_->rasterizer.Get());
    context->OMSetBlendState(
        state_->blend_state.Get(),
        nullptr,
        0xFFFFFFFFU);
    context->IASetInputLayout(nullptr);
    context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
    context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(state_->vertex_shader.Get(), nullptr, 0);
    context->PSSetShader(state_->pixel_shader.Get(), nullptr, 0);
    auto* sampler = state_->sampler.Get();
    context->PSSetSamplers(0, 1, &sampler);
    auto* source_view = state_->source_view.Get();
    context->PSSetShaderResources(0, 1, &source_view);
    context->OMSetDepthStencilState(state_->depth_state.Get(), 0);
    context->OMSetRenderTargets(0, nullptr, output_view);
    context->Draw(3, 0);

    ID3D11ShaderResourceView* no_resource{};
    context->PSSetShaderResources(0, 1, &no_resource);
    context->OMSetRenderTargets(0, nullptr, nullptr);

    const auto restore_surface =
        [&](const std::size_t index,
            ID3D11Texture2D* source,
            ID3D11RenderTargetView* destination) {
            if (source == nullptr || destination == nullptr) {
                return false;
            }
            D3D11_TEXTURE2D_DESC description{};
            source->GetDesc(&description);
            if (description.Width == 0 || description.Height == 0 ||
                description.SampleDesc.Count != 1) {
                return false;
            }
            const auto active_width = (std::max)(
                1U,
                static_cast<std::uint32_t>(std::lround(
                    static_cast<double>(description.Width) *
                    static_cast<double>(source_description.Width) /
                    static_cast<double>(output_width))));
            const auto active_height = (std::max)(
                1U,
                static_cast<std::uint32_t>(std::lround(
                    static_cast<double>(description.Height) *
                    static_cast<double>(source_description.Height) /
                    static_cast<double>(output_height))));
            context->OMSetRenderTargets(0, nullptr, nullptr);
            if (!state_->auxiliary[index].prepare(
                    device,
                    context,
                    source,
                    active_width,
                    active_height)) {
                return false;
            }

            const D3D11_VIEWPORT surface_viewport{
                0.0F,
                0.0F,
                static_cast<float>(description.Width),
                static_cast<float>(description.Height),
                0.0F,
                1.0F};
            const D3D11_RECT surface_scissor{
                0,
                0,
                static_cast<LONG>(description.Width),
                static_cast<LONG>(description.Height)};
            context->RSSetViewports(1, &surface_viewport);
            context->RSSetScissorRects(1, &surface_scissor);
            context->OMSetDepthStencilState(nullptr, 0);
            context->PSSetShader(
                state_->surface_pixel_shader.Get(),
                nullptr,
                0);
            auto* surface_view =
                state_->auxiliary[index].view.Get();
            context->PSSetShaderResources(0, 1, &surface_view);
            context->OMSetRenderTargets(
                1,
                &destination,
                nullptr);
            context->Draw(3, 0);
            context->PSSetShaderResources(0, 1, &no_resource);
            context->OMSetRenderTargets(0, nullptr, nullptr);
            return true;
        };
    const auto restore_depth_surface =
        [&](ID3D11Texture2D* destination_texture,
            ID3D11RenderTargetView* destination) {
            if (destination_texture == nullptr || destination == nullptr) {
                return false;
            }
            D3D11_TEXTURE2D_DESC description{};
            destination_texture->GetDesc(&description);
            if (description.Width == 0 || description.Height == 0 ||
                description.SampleDesc.Count != 1) {
                return false;
            }
            const D3D11_VIEWPORT surface_viewport{
                0.0F,
                0.0F,
                static_cast<float>(description.Width),
                static_cast<float>(description.Height),
                0.0F,
                1.0F};
            const D3D11_RECT surface_scissor{
                0,
                0,
                static_cast<LONG>(description.Width),
                static_cast<LONG>(description.Height)};
            context->RSSetViewports(1, &surface_viewport);
            context->RSSetScissorRects(1, &surface_scissor);
            context->OMSetDepthStencilState(nullptr, 0);
            context->PSSetShader(
                state_->depth_surface_pixel_shader.Get(),
                nullptr,
                0);
            auto* depth_view = state_->source_view.Get();
            context->PSSetShaderResources(0, 1, &depth_view);
            context->OMSetRenderTargets(1, &destination, nullptr);
            context->Draw(3, 0);
            context->PSSetShaderResources(0, 1, &no_resource);
            context->OMSetRenderTargets(0, nullptr, nullptr);
            return true;
        };

    const auto& refraction =
        renderer->renderTargets[
            RE::RENDER_TARGETS::kREFRACTION_NORMALS];
    const auto& sao_camera_z =
        renderer->renderTargets[
            RE::RENDER_TARGETS::kSAO_CAMERAZ];
    const auto& underwater =
        renderer->renderTargets[
            RE::RENDER_TARGETS::kUNDERWATER_MASK];
    const std::array auxiliary_restored{
        restore_surface(
            0,
            refraction.texture,
            refraction.RTV),
        restore_depth_surface(
            sao_camera_z.texture,
            sao_camera_z.RTV),
        restore_surface(
            2,
            underwater.texture,
            underwater.RTV)};
    ID3D11Buffer* no_constant{};
    context->PSSetConstantBuffers(0, 1, &no_constant);

    depth_unavailable_logged_ = false;
    depth_extent_logged_ = false;
    if (!first_restore_logged_) {
        first_restore_logged_ = true;
        logger::info(
            "Main depth restored before post-processing: {}x{} -> {}x{}, "
            "bilinear, unjittered by {:.4f},{:.4f} pixels. The filter is "
            "worth knowing: bilinear across a depth discontinuity produces "
            "values that belong to no surface, and a frame generator reads "
            "depth to decide disocclusion, so silhouette artefacts on the "
            "vendor paths would start here",
            source_description.Width,
            source_description.Height,
            output_width,
            output_height,
            jitter_x,
            jitter_y);
    }
    if (!auxiliary_status_logged_) {
        auxiliary_status_logged_ = true;
        logger::info(
            "Pre-postprocess auxiliary restoration: refraction={}, "
            "SAO-camera-Z={}, underwater={}",
            auxiliary_restored[0],
            auxiliary_restored[1],
            auxiliary_restored[2]);
    }
    return true;
}

void DepthRestoration::shutdown() noexcept
{
    state_.reset();
    depth_unavailable_logged_ = false;
    depth_extent_logged_ = false;
    first_restore_logged_ = false;
    auxiliary_status_logged_ = false;
}
}
