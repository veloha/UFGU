#include "render/SharpeningPass.hpp"

#include "render/PresentationBridge.hpp"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <SKSE/SKSE.h>

#include <algorithm>
#include <cmath>
#include <string_view>

namespace mfgdlss::render
{
namespace
{
namespace logger = SKSE::log;
using Microsoft::WRL::ComPtr;

struct SharpenConstants
{
    float strength{};
    float padding[3]{};
};

static_assert(sizeof(SharpenConstants) == 16);

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
        "MFGDLSS_ContrastAdaptiveSharpen",
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
    const auto message = errors != nullptr ?
        std::string_view(
            static_cast<const char*>(errors->GetBufferPointer()),
            errors->GetBufferSize()) :
        std::string_view{"no compiler diagnostics"};
    logger::error(
        "Sharpening {} compilation failed: 0x{:08X}: {}",
        target,
        static_cast<unsigned>(result),
        message);
    return false;
}
}

struct SharpeningPass::State
{
    ComPtr<ID3D11Texture2D> source_copy;
    ComPtr<ID3D11ShaderResourceView> source_view;
    ComPtr<ID3D11RenderTargetView> target_view;
    ComPtr<ID3D11VertexShader> vertex_shader;
    ComPtr<ID3D11PixelShader> pixel_shader;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> depth;
    D3D11_TEXTURE2D_DESC description{};
    ID3D11Texture2D* target{};
};

SharpeningPass::SharpeningPass() = default;
SharpeningPass::~SharpeningPass() = default;

SharpeningPass& SharpeningPass::instance() noexcept
{
    static SharpeningPass pass;
    return pass;
}

bool SharpeningPass::apply(
    ID3D11Texture2D* target,
    const float sharpness)
{
    if (target == nullptr || !std::isfinite(sharpness)) {
        return false;
    }
    const auto strength =
        (std::clamp)(sharpness, 0.0F, 1.0F);
    if (strength <= 0.0F) {
        return true;
    }

    auto* device = PresentationBridge::instance().d3d11_device();
    auto* context = PresentationBridge::instance().d3d11_context();
    if (device == nullptr || context == nullptr) {
        return false;
    }

    D3D11_TEXTURE2D_DESC description{};
    target->GetDesc(&description);
    const auto compatible =
        state_ != nullptr &&
        state_->target == target &&
        state_->description.Width == description.Width &&
        state_->description.Height == description.Height &&
        state_->description.Format == description.Format;
    if (!compatible) {
        state_.reset();
        auto state = std::make_unique<State>();
        state->description = description;
        state->target = target;

        auto copy_description = description;
        copy_description.Usage = D3D11_USAGE_DEFAULT;
        copy_description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        copy_description.CPUAccessFlags = 0;
        copy_description.MiscFlags = 0;
        copy_description.MipLevels = 1;
        copy_description.ArraySize = 1;
        copy_description.SampleDesc = {1, 0};
        auto result = device->CreateTexture2D(
            &copy_description,
            nullptr,
            &state->source_copy);
        if (SUCCEEDED(result)) {
            result = device->CreateShaderResourceView(
                state->source_copy.Get(),
                nullptr,
                &state->source_view);
        }
        if (SUCCEEDED(result)) {
            result = device->CreateRenderTargetView(
                target,
                nullptr,
                &state->target_view);
        }

        constexpr std::string_view vertex_source = R"(
struct output_data
{
    float4 position : SV_Position;
};

output_data main(uint vertex_id : SV_VertexID)
{
    output_data output;
    const float2 texcoord = float2(
        (vertex_id << 1) & 2,
        vertex_id & 2);
    output.position = float4(
        texcoord * float2(2.0, -2.0) +
            float2(-1.0, 1.0),
        0.0,
        1.0);
    return output;
}
)";
        constexpr std::string_view pixel_source = R"(
Texture2D<float4> source_color : register(t0);

cbuffer sharpen_constants : register(b0)
{
    float strength;
    float3 padding;
};

float luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

float4 main(float4 position : SV_Position) : SV_Target
{
    uint width;
    uint height;
    source_color.GetDimensions(width, height);
    const int2 maximum = int2(width - 1, height - 1);
    const int2 pixel = clamp(int2(position.xy), 0, maximum);
    const float4 center = source_color.Load(int3(pixel, 0));
    const float3 north = source_color.Load(
        int3(clamp(pixel + int2(0, -1), 0, maximum), 0)).rgb;
    const float3 south = source_color.Load(
        int3(clamp(pixel + int2(0, 1), 0, maximum), 0)).rgb;
    const float3 west = source_color.Load(
        int3(clamp(pixel + int2(-1, 0), 0, maximum), 0)).rgb;
    const float3 east = source_color.Load(
        int3(clamp(pixel + int2(1, 0), 0, maximum), 0)).rgb;

    const float center_luma = luminance(center.rgb);
    const float minimum_luma = min(
        center_luma,
        min(min(luminance(north), luminance(south)),
            min(luminance(west), luminance(east))));
    const float maximum_luma = max(
        center_luma,
        max(max(luminance(north), luminance(south)),
            max(luminance(west), luminance(east))));
    const float contrast =
        (maximum_luma - minimum_luma) /
        max(maximum_luma, 0.0001);
    const float adaptive_strength =
        strength * (0.125 + 0.375 * saturate(1.0 - contrast));
    const float3 detail =
        center.rgb * 4.0 - north - south - west - east;

    const float3 neighbourhood_minimum =
        min(center.rgb, min(min(north, south), min(west, east)));
    const float3 neighbourhood_maximum =
        max(center.rgb, max(max(north, south), max(west, east)));
    const float3 sharpened = clamp(
        center.rgb + detail * adaptive_strength,
        neighbourhood_minimum,
        neighbourhood_maximum);
    return float4(max(sharpened, 0.0), center.a);
}
)";

        ComPtr<ID3DBlob> vertex_bytecode;
        ComPtr<ID3DBlob> pixel_bytecode;
        if (FAILED(result) ||
            !compile_shader(
                vertex_source,
                "main",
                "vs_5_0",
                vertex_bytecode) ||
            !compile_shader(
                pixel_source,
                "main",
                "ps_5_0",
                pixel_bytecode)) {
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

        D3D11_BUFFER_DESC constant_description{};
        constant_description.ByteWidth = sizeof(SharpenConstants);
        constant_description.Usage = D3D11_USAGE_DEFAULT;
        constant_description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (SUCCEEDED(result)) {
            result = device->CreateBuffer(
                &constant_description,
                nullptr,
                &state->constants);
        }

        D3D11_RASTERIZER_DESC rasterizer_description{};
        rasterizer_description.FillMode = D3D11_FILL_SOLID;
        rasterizer_description.CullMode = D3D11_CULL_NONE;
        rasterizer_description.DepthClipEnable = FALSE;
        if (SUCCEEDED(result)) {
            result = device->CreateRasterizerState(
                &rasterizer_description,
                &state->rasterizer);
        }

        D3D11_BLEND_DESC blend_description{};
        blend_description.RenderTarget[0].RenderTargetWriteMask =
            D3D11_COLOR_WRITE_ENABLE_ALL;
        if (SUCCEEDED(result)) {
            result = device->CreateBlendState(
                &blend_description,
                &state->blend);
        }

        D3D11_DEPTH_STENCIL_DESC depth_description{};
        depth_description.DepthEnable = FALSE;
        depth_description.DepthWriteMask =
            D3D11_DEPTH_WRITE_MASK_ZERO;
        if (SUCCEEDED(result)) {
            result = device->CreateDepthStencilState(
                &depth_description,
                &state->depth);
        }
        if (FAILED(result)) {
            if (!failure_logged_) {
                logger::error(
                    "Creating DLSS sharpening state failed: 0x{:08X}",
                    static_cast<unsigned>(result));
                failure_logged_ = true;
            }
            return false;
        }
        state_ = std::move(state);
    }

    context->OMSetRenderTargets(0, nullptr, nullptr);
    context->CopyResource(state_->source_copy.Get(), target);
    const SharpenConstants constants{strength};
    context->UpdateSubresource(
        state_->constants.Get(),
        0,
        nullptr,
        &constants,
        0,
        0);

    const D3D11_VIEWPORT viewport{
        0.0F,
        0.0F,
        static_cast<float>(description.Width),
        static_cast<float>(description.Height),
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
    auto* constants_pointer = state_->constants.Get();
    context->PSSetConstantBuffers(0, 1, &constants_pointer);
    auto* source_pointer = state_->source_view.Get();
    context->PSSetShaderResources(0, 1, &source_pointer);
    context->OMSetBlendState(
        state_->blend.Get(),
        nullptr,
        0xFFFFFFFF);
    context->OMSetDepthStencilState(state_->depth.Get(), 0);
    auto* target_pointer = state_->target_view.Get();
    context->OMSetRenderTargets(1, &target_pointer, nullptr);
    context->Draw(3, 0);

    ID3D11ShaderResourceView* no_resource{};
    context->PSSetShaderResources(0, 1, &no_resource);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    failure_logged_ = false;
    if (!first_apply_logged_) {
        first_apply_logged_ = true;
        logger::info(
            "DLSS contrast-adaptive sharpening active at {:.2f}",
            strength);
    }
    return true;
}

void SharpeningPass::shutdown() noexcept
{
    state_.reset();
    failure_logged_ = false;
    first_apply_logged_ = false;
}
}
