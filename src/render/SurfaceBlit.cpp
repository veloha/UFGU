#include "render/SurfaceBlit.hpp"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <SKSE/SKSE.h>

#include <cstdint>
#include <string_view>

namespace mfgdlss::render
{
namespace
{
namespace logger = SKSE::log;
using Microsoft::WRL::ComPtr;

[[nodiscard]] bool compile_shader(
    const std::string_view source,
    const char* profile,
    ComPtr<ID3DBlob>& bytecode)
{
    ComPtr<ID3DBlob> errors;
    const auto result = D3DCompile(
        source.data(),
        source.size(),
        nullptr,
        nullptr,
        nullptr,
        "main",
        profile,
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &bytecode,
        &errors);
    if (SUCCEEDED(result)) {
        return true;
    }
    logger::error(
        "Presentation fallback shader compilation failed: 0x{:08X}",
        static_cast<unsigned>(result));
    return false;
}
}

struct alignas(16) SourceRegionConstants final
{
    float u_scale{1.0F};
    float v_scale{1.0F};
    float reserved_x{};
    float reserved_y{};
};

struct SurfaceBlit::State
{
    ComPtr<ID3D11ShaderResourceView> source_view;
    ComPtr<ID3D11RenderTargetView> target_view;
    ComPtr<ID3D11VertexShader> vertex_shader;
    ComPtr<ID3D11PixelShader> pixel_shader;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11Buffer> region_constants;
    ID3D11Texture2D* source{};
    ID3D11Texture2D* target{};
    std::uint32_t target_width{};
    std::uint32_t target_height{};
    SourceRegionConstants region{};
    bool region_written{};
};

SurfaceBlit::SurfaceBlit() = default;
SurfaceBlit::~SurfaceBlit() = default;

SurfaceBlit& SurfaceBlit::instance() noexcept
{
    static SurfaceBlit blit;
    return blit;
}

bool SurfaceBlit::apply(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* source,
    ID3D11Texture2D* target,
    const std::uint32_t source_width,
    const std::uint32_t source_height)
{
    if (device == nullptr || context == nullptr ||
        source == nullptr || target == nullptr) {
        return false;
    }

    D3D11_TEXTURE2D_DESC source_description{};
    D3D11_TEXTURE2D_DESC target_description{};
    source->GetDesc(&source_description);
    target->GetDesc(&target_description);
    if (source_description.Width == 0 ||
        source_description.Height == 0 ||
        target_description.Width == 0 ||
        target_description.Height == 0 ||
        source_description.SampleDesc.Count != 1 ||
        target_description.SampleDesc.Count != 1) {
        return false;
    }

    const auto region_width =
        source_width != 0 && source_width <= source_description.Width ?
            source_width :
            source_description.Width;
    const auto region_height =
        source_height != 0 && source_height <= source_description.Height ?
            source_height :
            source_description.Height;
    const SourceRegionConstants region{
        static_cast<float>(region_width) /
            static_cast<float>(source_description.Width),
        static_cast<float>(region_height) /
            static_cast<float>(source_description.Height),
        0.0F,
        0.0F};

    const auto compatible =
        state_ != nullptr &&
        state_->source == source &&
        state_->target == target &&
        state_->target_width == target_description.Width &&
        state_->target_height == target_description.Height;
    if (!compatible) {
        state_.reset();
        auto state = std::make_unique<State>();
        state->source = source;
        state->target = target;
        state->target_width = target_description.Width;
        state->target_height = target_description.Height;

        auto result = device->CreateShaderResourceView(
            source,
            nullptr,
            &state->source_view);
        if (SUCCEEDED(result)) {
            result = device->CreateRenderTargetView(
                target,
                nullptr,
                &state->target_view);
        }

        constexpr std::string_view vertex_source = R"(
struct vertex_output
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

vertex_output main(uint vertex_id : SV_VertexID)
{
    vertex_output output;
    output.texcoord = float2((vertex_id << 1) & 2, vertex_id & 2);
    output.position = float4(
        output.texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0),
        0.0,
        1.0);
    return output;
}
)";
        constexpr std::string_view pixel_source = R"(
Texture2D<float4> source_color : register(t0);
SamplerState linear_sampler : register(s0);

cbuffer source_region : register(b0)
{
    float2 uv_scale;
    float2 reserved;
};

float4 main(float4 position : SV_Position, float2 texcoord : TEXCOORD0)
    : SV_Target
{

    return source_color.SampleLevel(linear_sampler, texcoord * uv_scale, 0.0);
}
)";
        ComPtr<ID3DBlob> vertex_bytecode;
        ComPtr<ID3DBlob> pixel_bytecode;
        if (FAILED(result) ||
            !compile_shader(vertex_source, "vs_5_0", vertex_bytecode) ||
            !compile_shader(pixel_source, "ps_5_0", pixel_bytecode)) {
            failure_logged_ = true;
            return false;
        }
        result = device->CreateVertexShader(
            vertex_bytecode->GetBufferPointer(),
            vertex_bytecode->GetBufferSize(),
            nullptr,
            &state->vertex_shader);
        if (SUCCEEDED(result)) {
            result = device->CreatePixelShader(
                pixel_bytecode->GetBufferPointer(),
                pixel_bytecode->GetBufferSize(),
                nullptr,
                &state->pixel_shader);
        }

        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        if (SUCCEEDED(result)) {
            result = device->CreateSamplerState(&sampler, &state->sampler);
        }

        D3D11_RASTERIZER_DESC rasterizer{};
        rasterizer.FillMode = D3D11_FILL_SOLID;
        rasterizer.CullMode = D3D11_CULL_NONE;
        rasterizer.DepthClipEnable = FALSE;
        if (SUCCEEDED(result)) {
            result = device->CreateRasterizerState(
                &rasterizer,
                &state->rasterizer);
        }

        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].RenderTargetWriteMask =
            D3D11_COLOR_WRITE_ENABLE_ALL;
        if (SUCCEEDED(result)) {
            result = device->CreateBlendState(&blend, &state->blend);
        }

        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = FALSE;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        if (SUCCEEDED(result)) {
            result = device->CreateDepthStencilState(&depth, &state->depth);
        }

        D3D11_BUFFER_DESC region_buffer{};
        region_buffer.ByteWidth = sizeof(SourceRegionConstants);
        region_buffer.Usage = D3D11_USAGE_DEFAULT;
        region_buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (SUCCEEDED(result)) {
            result = device->CreateBuffer(
                &region_buffer,
                nullptr,
                &state->region_constants);
        }
        if (FAILED(result)) {
            if (!failure_logged_) {
                failure_logged_ = true;
                logger::error(
                    "Presentation fallback state creation failed: 0x{:08X}",
                    static_cast<unsigned>(result));
            }
            return false;
        }
        state_ = std::move(state);
    }

    if (!state_->region_written ||
        state_->region.u_scale != region.u_scale ||
        state_->region.v_scale != region.v_scale) {
        state_->region = region;
        state_->region_written = true;
        context->UpdateSubresource(
            state_->region_constants.Get(),
            0,
            nullptr,
            &state_->region,
            0,
            0);
    }

    context->OMSetRenderTargets(0, nullptr, nullptr);
    const D3D11_VIEWPORT viewport{
        0.0F,
        0.0F,
        static_cast<float>(state_->target_width),
        static_cast<float>(state_->target_height),
        0.0F,
        1.0F};
    context->RSSetViewports(1, &viewport);
    context->RSSetState(state_->rasterizer.Get());
    context->IASetInputLayout(nullptr);
    context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
    context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
    context->IASetPrimitiveTopology(
        D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(state_->vertex_shader.Get(), nullptr, 0);
    context->PSSetShader(state_->pixel_shader.Get(), nullptr, 0);
    auto* source_view = state_->source_view.Get();
    auto* sampler = state_->sampler.Get();
    auto* region_constants = state_->region_constants.Get();
    context->PSSetShaderResources(0, 1, &source_view);
    context->PSSetSamplers(0, 1, &sampler);
    context->PSSetConstantBuffers(0, 1, &region_constants);
    context->OMSetBlendState(state_->blend.Get(), nullptr, 0xFFFFFFFF);
    context->OMSetDepthStencilState(state_->depth.Get(), 0);
    auto* target_view = state_->target_view.Get();
    context->OMSetRenderTargets(1, &target_view, nullptr);
    context->Draw(3, 0);

    ID3D11ShaderResourceView* no_resource{};
    ID3D11Buffer* no_constants{};
    context->PSSetShaderResources(0, 1, &no_resource);
    context->PSSetConstantBuffers(0, 1, &no_constants);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    failure_logged_ = false;
    return true;
}

void SurfaceBlit::shutdown() noexcept
{
    state_.reset();
    failure_logged_ = false;
}
}
