

#include "render/DebugViewCore.hpp"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;
using namespace mfgdlss::render;

namespace
{
int failures = 0;

void check(const bool condition, const char* const what)
{
    if (condition) {
        std::printf("  PASS  %s\n", what);
    } else {
        ++failures;
        std::printf("  FAIL  %s\n", what);
    }
}

void sink(const char* const message)
{
    std::printf("        [core] %s\n", message);
}

constexpr DXGI_FORMAT kSkyrimDepthFormat = DXGI_FORMAT_R24G8_TYPELESS;
constexpr UINT kSkyrimDepthBindFlags = 0x48U;

constexpr UINT kActiveWidth = 2227;
constexpr UINT kActiveHeight = 1253;
constexpr UINT kOutputWidth = 2560;
constexpr UINT kOutputHeight = 1440;

[[nodiscard]] bool make_texture(
    ID3D11Device* const device,
    const UINT width,
    const UINT height,
    const DXGI_FORMAT format,
    const UINT bind_flags,
    ComPtr<ID3D11Texture2D>& texture)
{
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = format;
    description.SampleDesc = {1, 0};
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = bind_flags;
    return SUCCEEDED(
        device->CreateTexture2D(&description, nullptr, &texture));
}

[[nodiscard]] ComPtr<ID3DBlob> compile(
    const char* const source,
    const char* const target)
{
    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> errors;
    const auto result = D3DCompile(
        source,
        std::strlen(source),
        "sentinel",
        nullptr,
        nullptr,
        "main",
        target,
        0,
        0,
        &bytecode,
        &errors);
    if (FAILED(result)) {
        std::printf(
            "  !! sentinel %s failed: %s\n",
            target,
            errors != nullptr ?
                static_cast<const char*>(errors->GetBufferPointer()) :
                "(no diagnostics)");
        return {};
    }
    return bytecode;
}

struct Sentinels
{
    ComPtr<ID3D11InputLayout> input_layout;
    ComPtr<ID3D11VertexShader> vertex_shader;
    ComPtr<ID3D11PixelShader> pixel_shader;
    ComPtr<ID3D11Buffer> constant_buffer;
    ComPtr<ID3D11ShaderResourceView> resource;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> depth_stencil;
    ComPtr<ID3D11RenderTargetView> target_a;
    ComPtr<ID3D11RenderTargetView> target_b;
    ComPtr<ID3D11DepthStencilView> depth_view;
    ComPtr<ID3D11Texture2D> scratch_a;
    ComPtr<ID3D11Texture2D> scratch_b;
    ComPtr<ID3D11Texture2D> scratch_depth;
    ComPtr<ID3D11Texture2D> scratch_sampled;

    static constexpr D3D11_PRIMITIVE_TOPOLOGY kTopology =
        D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    static constexpr UINT kStencilReference = 7;
    static constexpr UINT kSampleMask = 0x0000000FU;
    static constexpr float kBlendFactor[4]{0.25F, 0.5F, 0.75F, 1.0F};
};

[[nodiscard]] bool build_sentinels(ID3D11Device* const device, Sentinels& s)
{
    const auto vs = compile(
        "struct I { float4 p : POSITION; };\n"
        "float4 main(I i) : SV_Position { return i.p; }\n",
        "vs_5_0");
    const auto ps = compile(
        "float4 main() : SV_Target { return float4(0,1,0,1); }\n",
        "ps_5_0");
    if (vs == nullptr || ps == nullptr) {
        return false;
    }
    if (FAILED(device->CreateVertexShader(
            vs->GetBufferPointer(), vs->GetBufferSize(), nullptr,
            &s.vertex_shader))) {
        return false;
    }
    if (FAILED(device->CreatePixelShader(
            ps->GetBufferPointer(), ps->GetBufferSize(), nullptr,
            &s.pixel_shader))) {
        return false;
    }

    const D3D11_INPUT_ELEMENT_DESC element{
        "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
        D3D11_INPUT_PER_VERTEX_DATA, 0};
    if (FAILED(device->CreateInputLayout(
            &element, 1, vs->GetBufferPointer(), vs->GetBufferSize(),
            &s.input_layout))) {
        return false;
    }

    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = 256;
    buffer.Usage = D3D11_USAGE_DEFAULT;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&buffer, nullptr, &s.constant_buffer))) {
        return false;
    }

    if (!make_texture(
            device, 64, 64, DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D11_BIND_SHADER_RESOURCE, s.scratch_sampled)) {
        return false;
    }
    if (FAILED(device->CreateShaderResourceView(
            s.scratch_sampled.Get(), nullptr, &s.resource))) {
        return false;
    }

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sampler, &s.sampler))) {
        return false;
    }

    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_WIREFRAME;
    rasterizer.CullMode = D3D11_CULL_FRONT;
    rasterizer.ScissorEnable = TRUE;
    rasterizer.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rasterizer, &s.rasterizer))) {
        return false;
    }

    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_BLEND_FACTOR;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_BLEND_FACTOR;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED;
    if (FAILED(device->CreateBlendState(&blend, &s.blend))) {
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = D3D11_COMPARISON_GREATER;
    depth.StencilEnable = TRUE;
    depth.StencilReadMask = 0xFF;
    depth.StencilWriteMask = 0xFF;
    depth.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
    depth.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_INCR;
    depth.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
    depth.FrontFace.StencilFunc = D3D11_COMPARISON_EQUAL;
    depth.BackFace = depth.FrontFace;
    if (FAILED(device->CreateDepthStencilState(&depth, &s.depth_stencil))) {
        return false;
    }

    if (!make_texture(
            device, 128, 128, DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D11_BIND_RENDER_TARGET, s.scratch_a) ||
        !make_texture(
            device, 128, 128, DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D11_BIND_RENDER_TARGET, s.scratch_b) ||
        !make_texture(
            device, 128, 128, DXGI_FORMAT_R24G8_TYPELESS,
            D3D11_BIND_DEPTH_STENCIL, s.scratch_depth)) {
        return false;
    }
    if (FAILED(device->CreateRenderTargetView(
            s.scratch_a.Get(), nullptr, &s.target_a)) ||
        FAILED(device->CreateRenderTargetView(
            s.scratch_b.Get(), nullptr, &s.target_b))) {
        return false;
    }
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (FAILED(device->CreateDepthStencilView(
            s.scratch_depth.Get(), &dsv, &s.depth_view))) {
        return false;
    }
    return true;
}

const D3D11_VIEWPORT kSentinelViewports[2]{
    {11.0F, 13.0F, 320.0F, 240.0F, 0.25F, 0.75F},
    {31.0F, 37.0F, 160.0F, 120.0F, 0.0F, 1.0F}};
const D3D11_RECT kSentinelScissors[2]{
    {5, 7, 305, 247}, {9, 11, 169, 131}};

void bind_sentinels(ID3D11DeviceContext* const context, Sentinels& s)
{
    context->IASetInputLayout(s.input_layout.Get());
    context->IASetPrimitiveTopology(Sentinels::kTopology);
    context->VSSetShader(s.vertex_shader.Get(), nullptr, 0);
    context->PSSetShader(s.pixel_shader.Get(), nullptr, 0);
    ID3D11Buffer* const buffers[] = {s.constant_buffer.Get()};
    context->PSSetConstantBuffers(0, 1, buffers);
    ID3D11ShaderResourceView* const resources[] = {s.resource.Get()};
    context->PSSetShaderResources(0, 1, resources);
    ID3D11SamplerState* const samplers[] = {s.sampler.Get()};
    context->PSSetSamplers(0, 1, samplers);
    context->RSSetState(s.rasterizer.Get());
    context->RSSetViewports(2, kSentinelViewports);
    context->RSSetScissorRects(2, kSentinelScissors);
    context->OMSetBlendState(
        s.blend.Get(), Sentinels::kBlendFactor, Sentinels::kSampleMask);
    context->OMSetDepthStencilState(
        s.depth_stencil.Get(), Sentinels::kStencilReference);
    ID3D11RenderTargetView* const targets[] = {
        s.target_a.Get(), s.target_b.Get()};
    context->OMSetRenderTargets(2, targets, s.depth_view.Get());
}

void verify_sentinels(ID3D11DeviceContext* const context, Sentinels& s)
{
    {
        ComPtr<ID3D11InputLayout> got;
        context->IAGetInputLayout(&got);
        check(got.Get() == s.input_layout.Get(), "input layout restored");
    }
    {
        D3D11_PRIMITIVE_TOPOLOGY got{};
        context->IAGetPrimitiveTopology(&got);
        check(got == Sentinels::kTopology, "primitive topology restored");
    }
    {
        ComPtr<ID3D11VertexShader> got;
        context->VSGetShader(&got, nullptr, nullptr);
        check(got.Get() == s.vertex_shader.Get(), "vertex shader restored");
    }
    {
        ComPtr<ID3D11PixelShader> got;
        context->PSGetShader(&got, nullptr, nullptr);
        check(got.Get() == s.pixel_shader.Get(), "pixel shader restored");
    }
    {
        ComPtr<ID3D11Buffer> got;
        context->PSGetConstantBuffers(0, 1, &got);
        check(
            got.Get() == s.constant_buffer.Get(),
            "pixel constant buffer slot 0 restored");
    }
    {
        ComPtr<ID3D11ShaderResourceView> got;
        context->PSGetShaderResources(0, 1, &got);
        check(
            got.Get() == s.resource.Get(),
            "pixel shader-resource slot 0 restored");
    }
    {
        ComPtr<ID3D11SamplerState> got;
        context->PSGetSamplers(0, 1, &got);
        check(got.Get() == s.sampler.Get(), "pixel sampler slot 0 restored");
    }
    {
        ComPtr<ID3D11RasterizerState> got;
        context->RSGetState(&got);
        check(got.Get() == s.rasterizer.Get(), "rasterizer state restored");
    }
    {
        UINT count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        D3D11_VIEWPORT got
            [D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        context->RSGetViewports(&count, got);
        check(count == 2, "viewport count restored (2)");
        check(
            count == 2 &&
                std::memcmp(got, kSentinelViewports,
                            sizeof(kSentinelViewports)) == 0,
            "both viewports restored bit-exactly");
    }
    {
        UINT count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        D3D11_RECT got
            [D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        context->RSGetScissorRects(&count, got);
        check(count == 2, "scissor count restored (2)");
        check(
            count == 2 &&
                std::memcmp(got, kSentinelScissors,
                            sizeof(kSentinelScissors)) == 0,
            "both scissor rectangles restored bit-exactly");
    }
    {
        ComPtr<ID3D11BlendState> got;
        float factor[4]{};
        UINT mask{};
        context->OMGetBlendState(&got, factor, &mask);
        check(got.Get() == s.blend.Get(), "blend state restored");
        check(
            std::memcmp(factor, Sentinels::kBlendFactor, sizeof(factor)) == 0,
            "blend factor restored");
        check(mask == Sentinels::kSampleMask, "sample mask restored");
    }
    {
        ComPtr<ID3D11DepthStencilState> got;
        UINT reference{};
        context->OMGetDepthStencilState(&got, &reference);
        check(
            got.Get() == s.depth_stencil.Get(),
            "depth-stencil state restored");
        check(
            reference == Sentinels::kStencilReference,
            "stencil reference restored");
    }
    {
        ID3D11RenderTargetView*
            got[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        ID3D11DepthStencilView* depth{};
        context->OMGetRenderTargets(
            D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, got, &depth);
        check(got[0] == s.target_a.Get(), "render target slot 0 restored");
        check(
            got[1] == s.target_b.Get(),
            "render target slot 1 restored (not just slot 0)");
        check(depth == s.depth_view.Get(), "depth-stencil view restored");
        for (auto* view : got) {
            if (view != nullptr) {
                view->Release();
            }
        }
        if (depth != nullptr) {
            depth->Release();
        }
    }
}

[[nodiscard]] bool create_device(
    ComPtr<ID3D11Device>& device,
    ComPtr<ID3D11DeviceContext>& context)
{
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL obtained{};
    return SUCCEEDED(D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_WARP,
        nullptr,
        0,
        levels,
        1,
        D3D11_SDK_VERSION,
        &device,
        &obtained,
        &context));
}
}

int main()
{
    set_debug_view_log_sink(&sink);

    std::printf("-- typed shader-resource-view format selection --\n");
    {
        DXGI_FORMAT out{};
        check(
            srv_format_for(DXGI_FORMAT_R24G8_TYPELESS, kSkyrimDepthBindFlags,
                           out) &&
                out == DXGI_FORMAT_R24_UNORM_X8_TYPELESS,
            "R24G8_TYPELESS -> R24_UNORM_X8_TYPELESS");
        check(
            srv_format_for(DXGI_FORMAT_D24_UNORM_S8_UINT, 0x48U, out) &&
                out == DXGI_FORMAT_R24_UNORM_X8_TYPELESS,
            "D24_UNORM_S8_UINT -> R24_UNORM_X8_TYPELESS");
        check(
            srv_format_for(DXGI_FORMAT_R32_TYPELESS, 0x48U, out) &&
                out == DXGI_FORMAT_R32_FLOAT,
            "R32_TYPELESS -> R32_FLOAT");
        check(
            srv_format_for(DXGI_FORMAT_D32_FLOAT, 0x48U, out) &&
                out == DXGI_FORMAT_R32_FLOAT,
            "D32_FLOAT -> R32_FLOAT");
        check(
            srv_format_for(DXGI_FORMAT_R32G8X24_TYPELESS, 0x48U, out) &&
                out == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS,
            "R32G8X24_TYPELESS -> R32_FLOAT_X8X24_TYPELESS");
        check(
            srv_format_for(
                DXGI_FORMAT_R16_TYPELESS, D3D11_BIND_DEPTH_STENCIL, out) &&
                out == DXGI_FORMAT_R16_UNORM,
            "R16_TYPELESS with DEPTH_STENCIL -> R16_UNORM");
        check(
            srv_format_for(
                DXGI_FORMAT_R16_TYPELESS, D3D11_BIND_RENDER_TARGET, out) &&
                out == DXGI_FORMAT_R16_FLOAT,
            "R16_TYPELESS without DEPTH_STENCIL -> R16_FLOAT");
        check(
            srv_format_for(DXGI_FORMAT_R16G16_FLOAT, 0x8U, out) &&
                out == DXGI_FORMAT_R16G16_FLOAT,
            "an already-typed format is passed through unchanged");
        check(
            !srv_format_for(DXGI_FORMAT_UNKNOWN, 0x8U, out),
            "DXGI_FORMAT_UNKNOWN is rejected, not guessed");
        check(
            !srv_format_for(DXGI_FORMAT_R32G32B32_TYPELESS, 0x8U, out),
            "an unmapped typeless format is rejected, not guessed");
    }

    std::printf("-- far-plane epsilon matches the storage precision --\n");
    {
        const auto e24 =
            depth_far_epsilon_for(DXGI_FORMAT_R24_UNORM_X8_TYPELESS);
        check(
            e24 > 0.0F && e24 < 1.0F / 16777215.0F,
            "24-bit UNORM epsilon is under one quantisation step");
        check(
            depth_far_epsilon_for(DXGI_FORMAT_R16_UNORM) > e24,
            "16-bit UNORM epsilon is coarser than 24-bit");
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (!create_device(device, context)) {
        std::printf(
            "\nFAILED: no WARP D3D11 device could be created; the device "
            "tests could not run\n");
        return 2;
    }
    std::printf("-- WARP D3D11 device created --\n");

    ComPtr<ID3D11Texture2D> depth_source;
    check(
        make_texture(
            device.Get(), kActiveWidth, kActiveHeight, kSkyrimDepthFormat,
            kSkyrimDepthBindFlags, depth_source),
        "created a texture with Skyrim's recorded depth format and bind flags "
        "(R24G8_TYPELESS, 0x48)");
    ComPtr<ID3D11Texture2D> colour_source;
    check(
        make_texture(
            device.Get(), kActiveWidth, kActiveHeight,
            DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE,
            colour_source),
        "created a colour source");
    ComPtr<ID3D11Texture2D> output;
    check(
        make_texture(
            device.Get(), kOutputWidth, kOutputHeight,
            DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET, output),
        "created an output surface");

    {
        D3D11_SHADER_RESOURCE_VIEW_DESC bad{};
        bad.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        bad.Texture2D.MipLevels = 1;
        bad.Format = kSkyrimDepthFormat;
        ComPtr<ID3D11ShaderResourceView> view;
        check(
            FAILED(device->CreateShaderResourceView(
                depth_source.Get(), &bad, &view)),
            "D3D11 rejects an SRV that uses R24G8_TYPELESS directly");
    }

    DebugViewRenderer renderer;

    std::printf("-- off is inert --\n");
    {
        DebugViewParams params{};
        params.view = DebugView::off;
        const auto status = renderer.render(
            device.Get(), context.Get(), output.Get(), depth_source.Get(),
            params);
        check(status == DebugViewStatus::off, "off reports off");
        check(
            !renderer.holds_resources(),
            "off holds no device resources at all");
    }

    std::printf("-- the depth view works on the real resource --\n");
    {
        DebugViewParams params{};
        params.view = DebugView::depth;
        params.active_width = kActiveWidth;
        params.active_height = kActiveHeight;
        params.reversed_depth = true;
        const auto status = renderer.render(
            device.Get(), context.Get(), output.Get(), depth_source.Get(),
            params);
        check(
            status == DebugViewStatus::drawn,
            "the depth view draws from an R24G8_TYPELESS source");
        check(
            renderer.resolved_source_view_format() ==
                DXGI_FORMAT_R24_UNORM_X8_TYPELESS,
            "it sampled through a typed R24_UNORM_X8_TYPELESS view");
    }

    std::printf("-- the other views draw --\n");
    for (const auto view : {DebugView::motion_vectors,
                            DebugView::scene_reference,
                            DebugView::violations}) {
        DebugViewParams params{};
        params.view = view;
        params.active_width = kActiveWidth;
        params.active_height = kActiveHeight;
        const auto status = renderer.render(
            device.Get(), context.Get(), output.Get(), colour_source.Get(),
            params);
        check(
            status == DebugViewStatus::drawn,
            view == DebugView::motion_vectors ? "motion view draws" :
            view == DebugView::scene_reference ? "scene view draws" :
                                                 "violation view draws");
    }

    std::printf("-- a source without BIND_SHADER_RESOURCE is refused --\n");
    {
        ComPtr<ID3D11Texture2D> unreadable;
        check(
            make_texture(
                device.Get(), 64, 64, DXGI_FORMAT_R8G8B8A8_UNORM,
                D3D11_BIND_RENDER_TARGET, unreadable),
            "created a render-target-only texture");
        DebugViewParams params{};
        params.view = DebugView::scene_reference;
        params.active_width = 64;
        params.active_height = 64;
        const auto status = renderer.render(
            device.Get(), context.Get(), output.Get(), unreadable.Get(),
            params);
        check(
            status == DebugViewStatus::source_not_shader_readable,
            "it is refused with the exact reason, not a crash");
    }

    std::printf("-- pipeline state round-trip --\n");
    {
        Sentinels sentinels;
        if (!build_sentinels(device.Get(), sentinels)) {
            ++failures;
            std::printf("  FAIL  could not build sentinel pipeline state\n");
        } else {
            bind_sentinels(context.Get(), sentinels);
            DebugViewParams params{};
            params.view = DebugView::depth;
            params.active_width = kActiveWidth;
            params.active_height = kActiveHeight;
            params.reversed_depth = true;
            const auto status = renderer.render(
                device.Get(), context.Get(), output.Get(), depth_source.Get(),
                params);
            check(
                status == DebugViewStatus::drawn,
                "the pass drew over the sentinel state");
            verify_sentinels(context.Get(), sentinels);
        }
    }

    std::printf("-- an output-merger UAV is refused, not corrupted --\n");
    {
        ComPtr<ID3D11Texture2D> uav_texture;
        ComPtr<ID3D11UnorderedAccessView> uav;
        if (make_texture(
                device.Get(), 64, 64, DXGI_FORMAT_R32_FLOAT,
                D3D11_BIND_UNORDERED_ACCESS, uav_texture) &&
            SUCCEEDED(device->CreateUnorderedAccessView(
                uav_texture.Get(), nullptr, &uav))) {
            ID3D11UnorderedAccessView* const uavs[] = {uav.Get()};
            const UINT counts[] = {0};
            context->OMSetRenderTargetsAndUnorderedAccessViews(
                0, nullptr, nullptr, 0, 1, uavs, counts);
            DebugViewParams params{};
            params.view = DebugView::depth;
            params.active_width = kActiveWidth;
            params.active_height = kActiveHeight;
            const auto status = renderer.render(
                device.Get(), context.Get(), output.Get(), depth_source.Get(),
                params);
            check(
                status == DebugViewStatus::unsupported_uav_binding,
                "the pass declines rather than destroying a UAV binding");
            ComPtr<ID3D11UnorderedAccessView> still;
            context->OMGetRenderTargetsAndUnorderedAccessViews(
                0, nullptr, nullptr, 0, 1, &still);
            check(
                still.Get() == uav.Get(),
                "the UAV is still bound afterwards");
            ID3D11UnorderedAccessView* const none[] = {nullptr};
            context->OMSetRenderTargetsAndUnorderedAccessViews(
                0, nullptr, nullptr, 0, 1, none, counts);
        } else {
            std::printf("  ....  UAV creation unavailable; check skipped\n");
        }
    }

    std::printf("-- shutdown releases everything --\n");
    {
        check(renderer.holds_resources(), "resources are held before shutdown");
        renderer.shutdown();
        check(!renderer.holds_resources(), "shutdown released them");
        renderer.shutdown();
        check(
            !renderer.holds_resources(),
            "a second shutdown is safe and still holds nothing");
    }

    std::printf("-- reinitialization on a FRESH device --\n");
    {
        ComPtr<ID3D11Device> second_device;
        ComPtr<ID3D11DeviceContext> second_context;
        if (!create_device(second_device, second_context)) {
            ++failures;
            std::printf("  FAIL  could not create a second WARP device\n");
        } else {
            check(
                second_device.Get() != device.Get(),
                "the second device is a distinct object");
            ComPtr<ID3D11Texture2D> second_source;
            ComPtr<ID3D11Texture2D> second_output;
            check(
                make_texture(
                    second_device.Get(), kActiveWidth, kActiveHeight,
                    kSkyrimDepthFormat, kSkyrimDepthBindFlags, second_source) &&
                    make_texture(
                        second_device.Get(), kOutputWidth, kOutputHeight,
                        DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET,
                        second_output),
                "created resources on the second device");
            DebugViewParams params{};
            params.view = DebugView::depth;
            params.active_width = kActiveWidth;
            params.active_height = kActiveHeight;
            params.reversed_depth = true;
            const auto status = renderer.render(
                second_device.Get(), second_context.Get(),
                second_output.Get(), second_source.Get(), params);
            check(
                status == DebugViewStatus::drawn,
                "the pass reinitializes and draws on the fresh device");
        }
    }

    std::printf("-- a changed device is detected without an explicit reset "
                "--\n");
    {

        DebugViewParams params{};
        params.view = DebugView::depth;
        params.active_width = kActiveWidth;
        params.active_height = kActiveHeight;
        params.reversed_depth = true;
        const auto status = renderer.render(
            device.Get(), context.Get(), output.Get(), depth_source.Get(),
            params);
        check(
            status == DebugViewStatus::drawn,
            "switching back to the first device rebuilds and draws, never "
            "reusing an object from the other device");
    }

    std::printf("-- off after use releases resources again --\n");
    {
        DebugViewParams params{};
        params.view = DebugView::off;
        static_cast<void>(renderer.render(
            device.Get(), context.Get(), output.Get(), depth_source.Get(),
            params));
        check(
            !renderer.holds_resources(),
            "returning to off frees every cached device object");
    }

    std::printf(
        "\n%s (%d failure(s))\n",
        failures == 0 ? "ALL PASS" : "FAILED",
        failures);
    return failures == 0 ? 0 : 1;
}
