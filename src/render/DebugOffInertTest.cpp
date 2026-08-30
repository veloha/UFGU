

#include "render/DebugViewCore.hpp"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

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

constexpr UINT kRenderWidth = 2560;
constexpr UINT kRenderHeight = 1440;
constexpr UINT kOutputWidth = 3840;
constexpr UINT kOutputHeight = 2160;
constexpr DXGI_FORMAT kSkyrimDepthFormat = DXGI_FORMAT_R24G8_TYPELESS;
constexpr UINT kSkyrimDepthBindFlags = 0x48U;

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

[[nodiscard]] bool snapshot(
    ID3D11Device* const device,
    ID3D11DeviceContext* const context,
    ID3D11Texture2D* const texture,
    std::vector<unsigned char>& out)
{
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    auto staging = description;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> copy;
    if (FAILED(device->CreateTexture2D(&staging, nullptr, &copy))) {
        return false;
    }
    context->CopyResource(copy.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }
    out.assign(
        static_cast<const unsigned char*>(mapped.pData),
        static_cast<const unsigned char*>(mapped.pData) +
            static_cast<std::size_t>(mapped.RowPitch) * description.Height);
    context->Unmap(copy.Get(), 0);
    return true;
}

struct PipelineState
{
    ComPtr<ID3D11InputLayout> input_layout;
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    ComPtr<ID3D11VertexShader> vertex_shader;
    ComPtr<ID3D11PixelShader> pixel_shader;
    ComPtr<ID3D11Buffer> pixel_constant_buffer;
    ComPtr<ID3D11ShaderResourceView> pixel_resource;
    ComPtr<ID3D11SamplerState> pixel_sampler;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11BlendState> blend;
    float blend_factor[4]{};
    UINT sample_mask{};
    ComPtr<ID3D11DepthStencilState> depth_stencil;
    UINT stencil_reference{};
    std::array<ComPtr<ID3D11RenderTargetView>,
               D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets;
    ComPtr<ID3D11DepthStencilView> depth_view;
    UINT viewport_count{};
    std::array<D3D11_VIEWPORT,
               D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>
        viewports{};
    UINT scissor_count{};
    std::array<D3D11_RECT,
               D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>
        scissors{};
};

void capture(ID3D11DeviceContext* const context, PipelineState& out)
{
    context->IAGetInputLayout(&out.input_layout);
    context->IAGetPrimitiveTopology(&out.topology);
    context->VSGetShader(&out.vertex_shader, nullptr, nullptr);
    context->PSGetShader(&out.pixel_shader, nullptr, nullptr);
    context->PSGetConstantBuffers(0, 1, &out.pixel_constant_buffer);
    context->PSGetShaderResources(0, 1, &out.pixel_resource);
    context->PSGetSamplers(0, 1, &out.pixel_sampler);
    context->RSGetState(&out.rasterizer);
    context->OMGetBlendState(
        &out.blend, out.blend_factor, &out.sample_mask);
    context->OMGetDepthStencilState(
        &out.depth_stencil, &out.stencil_reference);
    std::array<ID3D11RenderTargetView*,
               D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> raw{};
    ID3D11DepthStencilView* raw_depth{};
    context->OMGetRenderTargets(
        D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, raw.data(), &raw_depth);
    for (std::size_t i = 0; i < raw.size(); ++i) {
        out.targets[i].Attach(raw[i]);
    }
    out.depth_view.Attach(raw_depth);
    out.viewport_count = static_cast<UINT>(out.viewports.size());
    context->RSGetViewports(&out.viewport_count, out.viewports.data());
    out.scissor_count = static_cast<UINT>(out.scissors.size());
    context->RSGetScissorRects(&out.scissor_count, out.scissors.data());
}

[[nodiscard]] bool identical(
    const PipelineState& a, const PipelineState& b)
{
    if (a.input_layout.Get() != b.input_layout.Get()) return false;
    if (a.topology != b.topology) return false;
    if (a.vertex_shader.Get() != b.vertex_shader.Get()) return false;
    if (a.pixel_shader.Get() != b.pixel_shader.Get()) return false;
    if (a.pixel_constant_buffer.Get() != b.pixel_constant_buffer.Get())
        return false;
    if (a.pixel_resource.Get() != b.pixel_resource.Get()) return false;
    if (a.pixel_sampler.Get() != b.pixel_sampler.Get()) return false;
    if (a.rasterizer.Get() != b.rasterizer.Get()) return false;
    if (a.blend.Get() != b.blend.Get()) return false;
    if (a.sample_mask != b.sample_mask) return false;
    for (int i = 0; i < 4; ++i) {
        if (a.blend_factor[i] != b.blend_factor[i]) return false;
    }
    if (a.depth_stencil.Get() != b.depth_stencil.Get()) return false;
    if (a.stencil_reference != b.stencil_reference) return false;
    for (std::size_t i = 0; i < a.targets.size(); ++i) {
        if (a.targets[i].Get() != b.targets[i].Get()) return false;
    }
    if (a.depth_view.Get() != b.depth_view.Get()) return false;
    if (a.viewport_count != b.viewport_count) return false;
    for (UINT i = 0; i < a.viewport_count; ++i) {
        if (std::memcmp(
                &a.viewports[i], &b.viewports[i],
                sizeof(D3D11_VIEWPORT)) != 0) {
            return false;
        }
    }
    if (a.scissor_count != b.scissor_count) return false;
    for (UINT i = 0; i < a.scissor_count; ++i) {
        if (std::memcmp(
                &a.scissors[i], &b.scissors[i], sizeof(D3D11_RECT)) != 0) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool bind_production_like_state(
    ID3D11Device* const device,
    ID3D11DeviceContext* const context,
    ComPtr<ID3D11Texture2D>& scene,
    ComPtr<ID3D11RenderTargetView>& scene_view,
    ComPtr<ID3D11RasterizerState>& rasterizer,
    ComPtr<ID3D11BlendState>& blend,
    ComPtr<ID3D11DepthStencilState>& depth_stencil)
{
    if (!make_texture(
            device, kRenderWidth, kRenderHeight,
            DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET, scene)) {
        return false;
    }
    if (FAILED(device->CreateRenderTargetView(
            scene.Get(), nullptr, &scene_view))) {
        return false;
    }
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_WIREFRAME;
    raster.CullMode = D3D11_CULL_FRONT;
    raster.ScissorEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&raster, &rasterizer))) {
        return false;
    }
    D3D11_BLEND_DESC blend_description{};
    blend_description.RenderTarget[0].BlendEnable = TRUE;
    blend_description.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend_description.RenderTarget[0].DestBlend = D3D11_BLEND_DEST_ALPHA;
    blend_description.RenderTarget[0].BlendOp = D3D11_BLEND_OP_SUBTRACT;
    blend_description.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend_description.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    blend_description.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_MAX;
    blend_description.RenderTarget[0].RenderTargetWriteMask = 0x7;
    if (FAILED(device->CreateBlendState(&blend_description, &blend))) {
        return false;
    }
    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = D3D11_COMPARISON_GREATER;
    depth.StencilEnable = TRUE;
    depth.StencilReadMask = 0xAB;
    depth.StencilWriteMask = 0xCD;
    depth.FrontFace.StencilFunc = D3D11_COMPARISON_EQUAL;
    depth.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
    depth.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
    depth.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
    depth.BackFace = depth.FrontFace;
    if (FAILED(
            device->CreateDepthStencilState(&depth, &depth_stencil))) {
        return false;
    }

    ID3D11RenderTargetView* const targets[]{scene_view.Get()};
    context->OMSetRenderTargets(1, targets, nullptr);
    context->IASetPrimitiveTopology(
        D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context->RSSetState(rasterizer.Get());
    const float factor[4]{0.25F, 0.5F, 0.75F, 1.0F};
    context->OMSetBlendState(blend.Get(), factor, 0x0000000FU);
    context->OMSetDepthStencilState(depth_stencil.Get(), 7);

    const D3D11_VIEWPORT viewport{
        0.0F, 0.0F,
        static_cast<float>(kOutputWidth),
        static_cast<float>(kOutputHeight),
        0.0F, 1.0F};
    context->RSSetViewports(1, &viewport);
    const D3D11_RECT scissor{
        0, 0,
        static_cast<LONG>(kRenderWidth),
        static_cast<LONG>(kRenderHeight)};
    context->RSSetScissorRects(1, &scissor);
    return true;
}
}

int main()
{
    std::printf("== the Debug-Off invariant, against a real D3D11 device ==\n\n");

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    const auto created = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1,
        D3D11_SDK_VERSION, &device, nullptr, &context);
    if (FAILED(created) || device == nullptr || context == nullptr) {
        std::printf("  !! could not create a WARP D3D11 device\n");
        return 2;
    }
    std::printf("  WARP D3D11 device created at feature level 11_0\n\n");

    ComPtr<ID3D11Texture2D> output;
    ComPtr<ID3D11Texture2D> colour_source;
    ComPtr<ID3D11Texture2D> depth_source;
    if (!make_texture(
            device.Get(), kOutputWidth, kOutputHeight,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
            output) ||
        !make_texture(
            device.Get(), kRenderWidth, kRenderHeight,
            DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE,
            colour_source) ||
        !make_texture(
            device.Get(), kRenderWidth, kRenderHeight,
            kSkyrimDepthFormat, kSkyrimDepthBindFlags, depth_source)) {
        std::printf("  !! could not create the fixtures\n");
        return 2;
    }

    {
        ComPtr<ID3D11RenderTargetView> view;
        if (FAILED(device->CreateRenderTargetView(
                output.Get(), nullptr, &view))) {
            std::printf("  !! could not create the output view\n");
            return 2;
        }
        const float colour[4]{0.31F, 0.62F, 0.17F, 1.0F};
        context->ClearRenderTargetView(view.Get(), colour);
        context->Flush();
    }

    std::vector<unsigned char> before;
    if (!snapshot(device.Get(), context.Get(), output.Get(), before)) {
        std::printf("  !! could not snapshot the output\n");
        return 2;
    }

    ComPtr<ID3D11Query> statistics;
    D3D11_QUERY_DESC query{};
    query.Query = D3D11_QUERY_PIPELINE_STATISTICS;
    if (FAILED(device->CreateQuery(&query, &statistics))) {
        std::printf("  !! could not create a pipeline-statistics query\n");
        return 2;
    }

    const auto measure_off_call =
        [&](DebugViewRenderer& renderer, const char* const label) {
            ComPtr<ID3D11Texture2D> scene;
            ComPtr<ID3D11RenderTargetView> scene_view;
            ComPtr<ID3D11RasterizerState> rasterizer;
            ComPtr<ID3D11BlendState> blend;
            ComPtr<ID3D11DepthStencilState> depth_stencil;
            if (!bind_production_like_state(
                    device.Get(), context.Get(), scene, scene_view,
                    rasterizer, blend, depth_stencil)) {
                std::printf("  !! could not bind the sentinel state\n");
                ++failures;
                return;
            }

            PipelineState state_before{};
            capture(context.Get(), state_before);

            std::vector<unsigned char> image_before;
            static_cast<void>(snapshot(
                device.Get(), context.Get(), output.Get(), image_before));

            DebugViewParams params{};
            params.view = DebugView::off;
            params.active_width = kRenderWidth;
            params.active_height = kRenderHeight;
            params.reversed_depth = true;

            context->Begin(statistics.Get());
            const auto status = renderer.render(
                device.Get(), context.Get(), output.Get(),
                colour_source.Get(), params);
            context->End(statistics.Get());

            D3D11_QUERY_DATA_PIPELINE_STATISTICS counters{};
            while (context->GetData(
                       statistics.Get(), &counters, sizeof(counters), 0) !=
                   S_OK) {
            }

            PipelineState state_after{};
            capture(context.Get(), state_after);

            std::vector<unsigned char> image_after;
            static_cast<void>(snapshot(
                device.Get(), context.Get(), output.Get(), image_after));

            std::printf("-- %s --\n", label);
            check(
                status == DebugViewStatus::off,
                "the pass reports the off status");
            check(
                !renderer.holds_resources(),
                "no device object is held: nothing was compiled or allocated");
            check(
                counters.IAVertices == 0 && counters.IAPrimitives == 0,
                "the GPU counted ZERO input-assembler vertices and primitives");
            check(
                counters.VSInvocations == 0,
                "the GPU counted ZERO vertex-shader invocations");
            check(
                counters.PSInvocations == 0,
                "the GPU counted ZERO pixel-shader invocations");
            check(
                counters.CInvocations == 0 && counters.CPrimitives == 0,
                "the GPU counted ZERO rasterizer invocations");
            check(
                counters.CSInvocations == 0,
                "the GPU counted ZERO compute-shader invocations");
            check(
                identical(state_before, state_after),
                "every pipeline slot is bit-identical: input layout, topology, "
                "both shaders, PS CB0, PS SRV0, PS sampler0, rasterizer, "
                "blend + factor + mask, depth-stencil + reference, all 8 RTV "
                "slots, the DSV, every viewport and every scissor");
            check(
                state_after.viewport_count == 1 &&
                    state_after.viewports[0].Width ==
                        static_cast<float>(kOutputWidth) &&
                    state_after.viewports[0].Height ==
                        static_cast<float>(kOutputHeight),
                "the 3840x2160 viewport bound over a 2560x1440 surface - the "
                "exact shape of the field failure - is left exactly as found");
            check(
                state_after.targets[0].Get() == scene_view.Get(),
                "the bound render target is still the caller's, not the "
                "debug pass's");
            check(
                image_before == image_after && !image_before.empty(),
                "the production image is byte-for-byte unchanged");
            std::printf("\n");
        };

    {
        DebugViewRenderer renderer;
        measure_off_call(
            renderer, "Off, from a renderer that has never drawn");
    }

    {
        DebugViewRenderer renderer;

        DebugViewParams params{};
        params.view = DebugView::depth;
        params.active_width = kRenderWidth;
        params.active_height = kRenderHeight;
        params.reversed_depth = true;
        const auto drew = renderer.render(
            device.Get(), context.Get(), output.Get(),
            depth_source.Get(), params);

        std::printf("-- a real view genuinely allocates and draws first --\n");
        check(
            drew == DebugViewStatus::drawn,
            "the depth view drew against Skyrim's recorded R24G8_TYPELESS "
            "format and 0x48 bind flags");
        check(
            renderer.holds_resources(),
            "and it is now holding real device objects");
        check(
            renderer.resolved_source_view_format() ==
                DXGI_FORMAT_R24_UNORM_X8_TYPELESS,
            "sampled through a typed R24_UNORM_X8_TYPELESS view");
        std::vector<unsigned char> after_draw;
        static_cast<void>(snapshot(
            device.Get(), context.Get(), output.Get(), after_draw));
        check(
            after_draw != before,
            "the output really changed, so the comparison below is meaningful");
        std::printf("\n");

        measure_off_call(
            renderer, "Off, immediately after that real view drew");
    }

    std::printf("-- selecting and leaving every view returns cleanly --\n");
    {
        DebugViewRenderer renderer;
        const DebugView views[]{
            DebugView::motion_vectors,
            DebugView::depth,
            DebugView::scene_reference,
            DebugView::violations};
        const char* const names[]{
            "motion", "depth", "scene", "violations"};

        auto all_clean = true;
        for (std::size_t i = 0; i < 4; ++i) {
            DebugViewParams params{};
            params.view = views[i];
            params.active_width = kRenderWidth;
            params.active_height = kRenderHeight;
            params.reversed_depth = true;
            params.frame_tainted = (i == 3);
            const auto drew = renderer.render(
                device.Get(), context.Get(), output.Get(),
                views[i] == DebugView::depth ? depth_source.Get()
                                             : colour_source.Get(),
                params);
            const auto allocated = renderer.holds_resources();

            params.view = DebugView::off;
            const auto off = renderer.render(
                device.Get(), context.Get(), output.Get(),
                colour_source.Get(), params);
            const auto released = !renderer.holds_resources();

            const auto clean = drew == DebugViewStatus::drawn && allocated &&
                off == DebugViewStatus::off && released;
            if (!clean) {
                all_clean = false;
            }
            check(
                clean,
                names[i]);
        }
        check(
            all_clean,
            "every view drew, allocated, and released everything on return "
            "to Off");
    }

    std::printf("\n-- Off does not even dereference the device --\n");
    {
        DebugViewRenderer renderer;
        DebugViewParams params{};
        params.view = DebugView::off;
        const auto status =
            renderer.render(nullptr, nullptr, nullptr, nullptr, params);
        check(
            status == DebugViewStatus::off,
            "a null device, context, target and source are all fine while Off");
        check(
            !renderer.holds_resources(),
            "and nothing was allocated");
    }

    std::printf(
        "\n%s\n", failures == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return failures == 0 ? 0 : 1;
}
