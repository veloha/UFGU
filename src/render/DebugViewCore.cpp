#include "render/DebugViewCore.hpp"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdarg>
#include <cstdio>
#include <string>
#include <string_view>

namespace mfgdlss::render
{
namespace
{
using Microsoft::WRL::ComPtr;

DebugViewLogSink g_log_sink{};

void report(const char* const message) noexcept
{
    if (g_log_sink != nullptr) {
        g_log_sink(message);
    }
}

void reportf(const char* const format, ...) noexcept
{
    if (g_log_sink == nullptr) {
        return;
    }
    char buffer[512]{};
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    g_log_sink(buffer);
}

struct DebugConstants
{
    float source_width{};
    float source_height{};
    float output_width{};
    float output_height{};

    float active_width{};
    float active_height{};

    float motion_scale_x{};
    float motion_scale_y{};

    float motion_range_pixels{};

    float reversed_depth{};
    float mode{};
    float taint{};

    float depth_far_epsilon{};
    float padding[3]{};
};

static_assert(sizeof(DebugConstants) == 64);
static_assert(sizeof(DebugConstants) % 16 == 0);

constexpr std::string_view kVertexSource = R"(
struct Output
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

Output main(uint id : SV_VertexID)
{
    Output output;
    output.texcoord = float2((id << 1) & 2, id & 2);
    output.position = float4(
        output.texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0),
        0.0,
        1.0);
    return output;
}
)";

constexpr std::string_view kPixelSource = R"(
Texture2D<float4> source_texture : register(t0);
SamplerState point_sampler : register(s0);

cbuffer Constants : register(b0)
{
    float source_width;
    float source_height;
    float output_width;
    float output_height;
    float active_width;
    float active_height;
    float motion_scale_x;
    float motion_scale_y;
    float motion_range_pixels;
    float reversed_depth;
    float mode;
    float taint;
    float depth_far_epsilon;
};

struct Input
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

float3 hue_to_rgb(float hue)
{
    float3 k = frac(hue + float3(0.0, 2.0 / 3.0, 1.0 / 3.0));
    return saturate(abs(k * 6.0 - 3.0) - 1.0);
}

float4 main(Input input) : SV_Target
{

    float source_aspect = source_width / max(source_height, 1.0);
    float output_aspect = output_width / max(output_height, 1.0);
    float2 uv = input.texcoord;
    if (output_aspect > source_aspect) {
        float scale = source_aspect / output_aspect;
        uv.x = (uv.x - 0.5) / scale + 0.5;
    } else {
        float scale = output_aspect / source_aspect;
        uv.y = (uv.y - 0.5) / scale + 0.5;
    }
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {

        return float4(0.05, 0.05, 0.06, 1.0);
    }

    float4 texel = source_texture.SampleLevel(point_sampler, uv, 0);

    if (mode > 4.5) {

        if (texel.a < 0.01) {
            return float4(0.06, 0.06, 0.10, 1.0);
        }
        return float4(texel.rgb, 1.0);
    }

    if (mode > 3.5) {

        float3 tint = taint > 0.5 ?
            float3(0.55, 0.06, 0.06) : float3(0.05, 0.35, 0.10);
        float luma = dot(texel.rgb, float3(0.2126, 0.7152, 0.0722));
        return float4(lerp(tint, tint * (0.4 + luma), 0.75), 1.0);
    }

    if (mode < 0.5) {

        float2 pixels = float2(
            texel.x * motion_scale_x,
            texel.y * motion_scale_y);
        float magnitude = length(pixels);
        if (magnitude < 0.02) {

            return float4(0.5, 0.5, 0.5, 1.0);
        }
        float normalized = saturate(magnitude / max(motion_range_pixels, 0.001));
        float angle = atan2(pixels.y, pixels.x);
        float hue = (angle / 6.2831853) + 0.5;
        float3 rgb = hue_to_rgb(hue);
        rgb = lerp(float3(0.5, 0.5, 0.5), rgb, saturate(normalized * 1.2));
        rgb *= (0.35 + 0.65 * normalized);
        if (magnitude > motion_range_pixels) {
            rgb = lerp(rgb, float3(1.0, 1.0, 1.0), 0.6);
        }
        return float4(rgb, 1.0);
    }

    if (mode < 1.5) {

        float raw = texel.x;
        float nearness = reversed_depth > 0.5 ? raw : (1.0 - raw);
        if (nearness <= depth_far_epsilon) {

            return float4(0.35, 0.02, 0.35, 1.0);
        }
        float shaped = pow(saturate(nearness), 0.25);
        float3 rgb = lerp(
            float3(0.04, 0.10, 0.34),
            float3(1.0, 0.95, 0.75),
            shaped);
        return float4(rgb, 1.0);
    }

    return float4(texel.rgb, 1.0);
}
)";

[[nodiscard]] bool compile_shader(
    const std::string_view source,
    const char* const entry_point,
    const char* const target,
    ComPtr<ID3DBlob>& bytecode)
{
    ComPtr<ID3DBlob> errors;
    const auto result = D3DCompile(
        source.data(),
        source.size(),
        "MFGDLSS_DebugView",
        nullptr,
        nullptr,
        entry_point,
        target,
        D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS,
        0,
        &bytecode,
        &errors);
    if (SUCCEEDED(result)) {
        return true;
    }
    reportf(
        "Debug view %s compilation failed: 0x%08X: %s",
        target,
        static_cast<unsigned>(result),
        errors != nullptr ?
            static_cast<const char*>(errors->GetBufferPointer()) :
            "no compiler diagnostics");
    return false;
}
}

const char* describe(const DebugViewStatus status) noexcept
{
    switch (status) {
    case DebugViewStatus::drawn:
        return "drawn";
    case DebugViewStatus::off:
        return "off";
    case DebugViewStatus::no_output_surface:
        return "no output surface";
    case DebugViewStatus::no_device:
        return "no D3D11 device";
    case DebugViewStatus::no_source:
        return "the upscaler has not been given this input yet";
    case DebugViewStatus::shader_compilation_failed:
        return "debug shader compilation failed";
    case DebugViewStatus::resource_creation_failed:
        return "debug view resources could not be created";
    case DebugViewStatus::output_not_renderable:
        return "the output surface is not renderable";
    case DebugViewStatus::source_format_unsupported:
        return "the source format has no sampleable interpretation";
    case DebugViewStatus::source_not_shader_readable:
        return "the source was not created with D3D11_BIND_SHADER_RESOURCE";
    case DebugViewStatus::unsupported_uav_binding:
        return "an output-merger unordered-access view is bound and could not "
               "be restored faithfully";
    }
    return "unknown";
}

bool srv_format_for(
    const DXGI_FORMAT source_format,
    const std::uint32_t bind_flags,
    DXGI_FORMAT& srv_format) noexcept
{
    const auto depth_capable =
        (bind_flags & D3D11_BIND_DEPTH_STENCIL) != 0U;
    switch (source_format) {

    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        srv_format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        return true;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        srv_format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        return true;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
        srv_format = DXGI_FORMAT_R32_FLOAT;
        return true;
    case DXGI_FORMAT_D16_UNORM:
        srv_format = DXGI_FORMAT_R16_UNORM;
        return true;
    case DXGI_FORMAT_R16_TYPELESS:

        srv_format = depth_capable ?
            DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R16_FLOAT;
        return true;

    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        srv_format = DXGI_FORMAT_R8G8B8A8_UNORM;
        return true;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        srv_format = DXGI_FORMAT_B8G8R8A8_UNORM;
        return true;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        srv_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        return true;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        srv_format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        return true;
    case DXGI_FORMAT_R16G16_TYPELESS:
        srv_format = DXGI_FORMAT_R16G16_FLOAT;
        return true;
    case DXGI_FORMAT_R32G32_TYPELESS:
        srv_format = DXGI_FORMAT_R32G32_FLOAT;
        return true;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        srv_format = DXGI_FORMAT_R10G10B10A2_UNORM;
        return true;
    case DXGI_FORMAT_R8G8_TYPELESS:
        srv_format = DXGI_FORMAT_R8G8_UNORM;
        return true;
    case DXGI_FORMAT_R8_TYPELESS:
        srv_format = DXGI_FORMAT_R8_UNORM;
        return true;

    case DXGI_FORMAT_UNKNOWN:
        return false;

    default:
        break;
    }

    switch (source_format) {
    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:

        return false;
    default:
        break;
    }
    srv_format = source_format;
    return true;
}

float depth_far_epsilon_for(const DXGI_FORMAT srv_format) noexcept
{
    switch (srv_format) {
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:

        return 0.5F / 16777215.0F;
    case DXGI_FORMAT_R16_UNORM:
        return 0.5F / 65535.0F;
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:

        return 1.0e-7F;
    default:
        return 1.0e-6F;
    }
}

void set_debug_view_log_sink(const DebugViewLogSink sink) noexcept
{
    g_log_sink = sink;
}

struct SavedPipelineState
{
    ComPtr<ID3D11InputLayout> input_layout;
    D3D11_PRIMITIVE_TOPOLOGY topology{D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED};

    ComPtr<ID3D11VertexShader> vertex_shader;
    ID3D11ClassInstance* vertex_instances[D3D11_SHADER_MAX_INTERFACES]{};
    UINT vertex_instance_count{};

    ComPtr<ID3D11PixelShader> pixel_shader;
    ID3D11ClassInstance* pixel_instances[D3D11_SHADER_MAX_INTERFACES]{};
    UINT pixel_instance_count{};

    ComPtr<ID3D11Buffer> pixel_constant_buffer;
    ComPtr<ID3D11ShaderResourceView> pixel_resource;
    ComPtr<ID3D11SamplerState> pixel_sampler;

    ComPtr<ID3D11RasterizerState> rasterizer;
    UINT viewport_count{};
    D3D11_VIEWPORT viewports
        [D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT scissor_count{};
    D3D11_RECT scissors
        [D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};

    ComPtr<ID3D11BlendState> blend;
    float blend_factor[4]{};
    UINT sample_mask{};

    ComPtr<ID3D11DepthStencilState> depth_stencil;
    UINT stencil_reference{};

    ComPtr<ID3D11RenderTargetView>
        render_targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ComPtr<ID3D11DepthStencilView> depth_view;

    void release_instances() noexcept
    {
        for (UINT i = 0; i < vertex_instance_count; ++i) {
            if (vertex_instances[i] != nullptr) {
                vertex_instances[i]->Release();
                vertex_instances[i] = nullptr;
            }
        }
        vertex_instance_count = 0;
        for (UINT i = 0; i < pixel_instance_count; ++i) {
            if (pixel_instances[i] != nullptr) {
                pixel_instances[i]->Release();
                pixel_instances[i] = nullptr;
            }
        }
        pixel_instance_count = 0;
    }
};

struct DebugViewRenderer::State
{

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11VertexShader> vertex_shader;
    ComPtr<ID3D11PixelShader> pixel_shader;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> depth;

    ComPtr<ID3D11RenderTargetView> target_view;
    ID3D11Texture2D* target_texture{};
    ComPtr<ID3D11ShaderResourceView> source_view;
    ID3D11Texture2D* source_texture{};
    DXGI_FORMAT source_view_format{DXGI_FORMAT_UNKNOWN};

    SavedPipelineState saved;
    bool shader_failure_logged{};
};

DebugViewRenderer::DebugViewRenderer() = default;

DebugViewRenderer::~DebugViewRenderer()
{
    shutdown();
}

bool DebugViewRenderer::holds_resources() const noexcept
{
    return state_ != nullptr;
}

DXGI_FORMAT DebugViewRenderer::resolved_source_view_format() const noexcept
{
    return state_ != nullptr ?
        state_->source_view_format : DXGI_FORMAT_UNKNOWN;
}

void DebugViewRenderer::shutdown() noexcept
{
    if (state_ == nullptr) {
        return;
    }

    state_->saved.release_instances();
    state_->source_view.Reset();
    state_->source_texture = nullptr;
    state_->target_view.Reset();
    state_->target_texture = nullptr;
    state_.reset();
}

DebugViewStatus DebugViewRenderer::render(
    ID3D11Device* const device,
    ID3D11DeviceContext* const context,
    ID3D11Texture2D* const target,
    ID3D11Texture2D* const source,
    const DebugViewParams& params)
{
    if (params.view == DebugView::off) {

        shutdown();
        return DebugViewStatus::off;
    }
    if (device == nullptr || context == nullptr) {
        return DebugViewStatus::no_device;
    }
    if (target == nullptr) {
        return DebugViewStatus::no_output_surface;
    }
    if (source == nullptr) {
        return DebugViewStatus::no_source;
    }

    if (state_ != nullptr && state_->device.Get() != device) {
        report(
            "Debug view: the D3D11 device changed identity; rebuilding every "
            "cached shader, state object and view");
        shutdown();
    }

    if (state_ == nullptr) {
        auto state = std::make_unique<State>();
        state->device = device;

        ComPtr<ID3DBlob> vertex_bytecode;
        ComPtr<ID3DBlob> pixel_bytecode;
        if (!compile_shader(kVertexSource, "main", "vs_5_0", vertex_bytecode) ||
            !compile_shader(kPixelSource, "main", "ps_5_0", pixel_bytecode)) {
            return DebugViewStatus::shader_compilation_failed;
        }

        auto result = device->CreateVertexShader(
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
        constant_description.ByteWidth = sizeof(DebugConstants);
        constant_description.Usage = D3D11_USAGE_DEFAULT;
        constant_description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (SUCCEEDED(result)) {
            result = device->CreateBuffer(
                &constant_description, nullptr, &state->constants);
        }

        D3D11_SAMPLER_DESC sampler_description{};

        sampler_description.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sampler_description.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_description.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_description.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_description.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sampler_description.MaxLOD = D3D11_FLOAT32_MAX;
        if (SUCCEEDED(result)) {
            result = device->CreateSamplerState(
                &sampler_description, &state->sampler);
        }

        D3D11_RASTERIZER_DESC rasterizer_description{};
        rasterizer_description.FillMode = D3D11_FILL_SOLID;
        rasterizer_description.CullMode = D3D11_CULL_NONE;
        rasterizer_description.DepthClipEnable = FALSE;
        if (SUCCEEDED(result)) {
            result = device->CreateRasterizerState(
                &rasterizer_description, &state->rasterizer);
        }

        D3D11_BLEND_DESC blend_description{};
        blend_description.RenderTarget[0].RenderTargetWriteMask =
            D3D11_COLOR_WRITE_ENABLE_ALL;
        if (SUCCEEDED(result)) {
            result = device->CreateBlendState(
                &blend_description, &state->blend);
        }

        D3D11_DEPTH_STENCIL_DESC depth_description{};
        depth_description.DepthEnable = FALSE;
        depth_description.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        if (SUCCEEDED(result)) {
            result = device->CreateDepthStencilState(
                &depth_description, &state->depth);
        }
        if (FAILED(result)) {
            reportf(
                "Creating debug view state failed: 0x%08X",
                static_cast<unsigned>(result));
            return DebugViewStatus::resource_creation_failed;
        }
        state_ = std::move(state);
    }

    if (state_->target_texture != target) {
        state_->target_view.Reset();
        state_->target_texture = nullptr;
        if (FAILED(device->CreateRenderTargetView(
                target, nullptr, &state_->target_view))) {
            return DebugViewStatus::output_not_renderable;
        }
        state_->target_texture = target;
    }

    D3D11_TEXTURE2D_DESC source_description{};
    source->GetDesc(&source_description);

    if (state_->source_texture != source) {
        state_->source_view.Reset();
        state_->source_texture = nullptr;
        state_->source_view_format = DXGI_FORMAT_UNKNOWN;

        if ((source_description.BindFlags &
             D3D11_BIND_SHADER_RESOURCE) == 0U) {
            reportf(
                "Debug view source cannot be sampled: format=%u BindFlags=0x%X "
                "lacks D3D11_BIND_SHADER_RESOURCE",
                static_cast<unsigned>(source_description.Format),
                static_cast<unsigned>(source_description.BindFlags));
            return DebugViewStatus::source_not_shader_readable;
        }

        DXGI_FORMAT view_format{};
        if (!srv_format_for(
                source_description.Format,
                source_description.BindFlags,
                view_format)) {
            reportf(
                "Debug view source format has no sampleable interpretation: "
                "DXGI_FORMAT %u, BindFlags=0x%X",
                static_cast<unsigned>(source_description.Format),
                static_cast<unsigned>(source_description.BindFlags));
            return DebugViewStatus::source_format_unsupported;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC view_description{};
        view_description.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        view_description.Texture2D.MostDetailedMip = 0;
        view_description.Texture2D.MipLevels = 1;
        view_description.Format = view_format;
        if (FAILED(device->CreateShaderResourceView(
                source, &view_description, &state_->source_view))) {
            reportf(
                "Debug view shader-resource view creation failed: source "
                "DXGI_FORMAT %u -> view DXGI_FORMAT %u, BindFlags=0x%X",
                static_cast<unsigned>(source_description.Format),
                static_cast<unsigned>(view_format),
                static_cast<unsigned>(source_description.BindFlags));
            return DebugViewStatus::source_format_unsupported;
        }
        state_->source_texture = source;
        state_->source_view_format = view_format;
        reportf(
            "Debug view source bound: DXGI_FORMAT %u -> sampled as "
            "DXGI_FORMAT %u (%ux%u, BindFlags=0x%X)",
            static_cast<unsigned>(source_description.Format),
            static_cast<unsigned>(view_format),
            source_description.Width,
            source_description.Height,
            static_cast<unsigned>(source_description.BindFlags));
    }

    D3D11_TEXTURE2D_DESC target_description{};
    target->GetDesc(&target_description);

    auto& saved = state_->saved;
    {
        ID3D11RenderTargetView*
            raw_targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        ID3D11DepthStencilView* raw_depth{};
        ID3D11UnorderedAccessView*
            raw_uavs[D3D11_PS_CS_UAV_REGISTER_COUNT]{};

        context->OMGetRenderTargets(
            D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
            raw_targets,
            &raw_depth);
        context->OMGetRenderTargetsAndUnorderedAccessViews(
            0,
            nullptr,
            nullptr,
            0,
            D3D11_PS_CS_UAV_REGISTER_COUNT,
            raw_uavs);

        bool uav_bound = false;
        for (auto*& uav : raw_uavs) {
            if (uav != nullptr) {
                uav_bound = true;
                uav->Release();
                uav = nullptr;
            }
        }
        if (uav_bound) {
            for (auto*& view : raw_targets) {
                if (view != nullptr) {
                    view->Release();
                    view = nullptr;
                }
            }
            if (raw_depth != nullptr) {
                raw_depth->Release();
            }
            return DebugViewStatus::unsupported_uav_binding;
        }

        for (std::size_t i = 0;
             i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
             ++i) {
            saved.render_targets[i].Attach(raw_targets[i]);
        }
        saved.depth_view.Attach(raw_depth);
    }

    saved.release_instances();

    {
        ID3D11InputLayout* raw{};
        context->IAGetInputLayout(&raw);
        saved.input_layout.Attach(raw);
    }
    context->IAGetPrimitiveTopology(&saved.topology);

    {
        ID3D11VertexShader* raw{};
        saved.vertex_instance_count = D3D11_SHADER_MAX_INTERFACES;
        context->VSGetShader(
            &raw, saved.vertex_instances, &saved.vertex_instance_count);
        saved.vertex_shader.Attach(raw);
    }
    {
        ID3D11PixelShader* raw{};
        saved.pixel_instance_count = D3D11_SHADER_MAX_INTERFACES;
        context->PSGetShader(
            &raw, saved.pixel_instances, &saved.pixel_instance_count);
        saved.pixel_shader.Attach(raw);
    }
    {
        ID3D11Buffer* raw{};
        context->PSGetConstantBuffers(0, 1, &raw);
        saved.pixel_constant_buffer.Attach(raw);
    }
    {
        ID3D11ShaderResourceView* raw{};
        context->PSGetShaderResources(0, 1, &raw);
        saved.pixel_resource.Attach(raw);
    }
    {
        ID3D11SamplerState* raw{};
        context->PSGetSamplers(0, 1, &raw);
        saved.pixel_sampler.Attach(raw);
    }
    {
        ID3D11RasterizerState* raw{};
        context->RSGetState(&raw);
        saved.rasterizer.Attach(raw);
    }
    saved.viewport_count =
        D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    context->RSGetViewports(&saved.viewport_count, saved.viewports);
    saved.scissor_count =
        D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    context->RSGetScissorRects(&saved.scissor_count, saved.scissors);
    {
        ID3D11BlendState* raw{};
        context->OMGetBlendState(
            &raw, saved.blend_factor, &saved.sample_mask);
        saved.blend.Attach(raw);
    }
    {
        ID3D11DepthStencilState* raw{};
        context->OMGetDepthStencilState(&raw, &saved.stencil_reference);
        saved.depth_stencil.Attach(raw);
    }

    DebugConstants constants{};
    constants.source_width = static_cast<float>(source_description.Width);
    constants.source_height = static_cast<float>(source_description.Height);
    constants.output_width = static_cast<float>(target_description.Width);
    constants.output_height = static_cast<float>(target_description.Height);
    constants.active_width = static_cast<float>(params.active_width);
    constants.active_height = static_cast<float>(params.active_height);

    constants.motion_scale_x = constants.active_width;
    constants.motion_scale_y = constants.active_height;

    constants.motion_range_pixels = constants.active_width * 0.06F;
    constants.reversed_depth = params.reversed_depth ? 1.0F : 0.0F;

    constants.mode =
        params.view == DebugView::motion_vectors ? 0.0F :
        params.view == DebugView::generator_motion ? 0.0F :
        params.view == DebugView::depth ? 1.0F :
        params.view == DebugView::generator_depth ? 1.0F :
        params.view == DebugView::scene_reference ? 2.0F :
        params.view == DebugView::ui_layer ? 5.0F : 4.0F;
    constants.taint = params.frame_tainted ? 1.0F : 0.0F;
    constants.depth_far_epsilon =
        depth_far_epsilon_for(state_->source_view_format);
    context->UpdateSubresource(
        state_->constants.Get(), 0, nullptr, &constants, 0, 0);

    ID3D11RenderTargetView* const views[] = {state_->target_view.Get()};
    context->OMSetRenderTargets(1, views, nullptr);
    const D3D11_VIEWPORT viewport{
        0.0F,
        0.0F,
        static_cast<float>(target_description.Width),
        static_cast<float>(target_description.Height),
        0.0F,
        1.0F};
    context->RSSetViewports(1, &viewport);
    const D3D11_RECT scissor{
        0,
        0,
        static_cast<LONG>(target_description.Width),
        static_cast<LONG>(target_description.Height)};
    context->RSSetScissorRects(1, &scissor);

    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(state_->vertex_shader.Get(), nullptr, 0);
    context->PSSetShader(state_->pixel_shader.Get(), nullptr, 0);
    ID3D11Buffer* const constant_buffers[] = {state_->constants.Get()};
    context->PSSetConstantBuffers(0, 1, constant_buffers);
    ID3D11ShaderResourceView* const resources[] = {state_->source_view.Get()};
    context->PSSetShaderResources(0, 1, resources);
    ID3D11SamplerState* const samplers[] = {state_->sampler.Get()};
    context->PSSetSamplers(0, 1, samplers);
    context->RSSetState(state_->rasterizer.Get());
    constexpr float kNoBlendFactor[4]{1.0F, 1.0F, 1.0F, 1.0F};
    context->OMSetBlendState(state_->blend.Get(), kNoBlendFactor, 0xFFFFFFFFU);
    context->OMSetDepthStencilState(state_->depth.Get(), 0);
    context->Draw(3, 0);

    {
        ID3D11ShaderResourceView* const none[] = {nullptr};
        context->PSSetShaderResources(0, 1, none);
    }
    context->OMSetRenderTargets(0, nullptr, nullptr);

    {
        ID3D11RenderTargetView*
            restore[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        for (std::size_t i = 0;
             i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
             ++i) {
            restore[i] = saved.render_targets[i].Get();
        }
        context->OMSetRenderTargets(
            D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
            restore,
            saved.depth_view.Get());
    }

    if (saved.viewport_count != 0) {
        context->RSSetViewports(saved.viewport_count, saved.viewports);
    } else {
        context->RSSetViewports(0, nullptr);
    }
    if (saved.scissor_count != 0) {
        context->RSSetScissorRects(saved.scissor_count, saved.scissors);
    } else {
        context->RSSetScissorRects(0, nullptr);
    }

    context->IASetInputLayout(saved.input_layout.Get());
    context->IASetPrimitiveTopology(saved.topology);
    context->VSSetShader(
        saved.vertex_shader.Get(),
        saved.vertex_instance_count != 0 ? saved.vertex_instances : nullptr,
        saved.vertex_instance_count);
    context->PSSetShader(
        saved.pixel_shader.Get(),
        saved.pixel_instance_count != 0 ? saved.pixel_instances : nullptr,
        saved.pixel_instance_count);
    {
        ID3D11Buffer* const restore[] = {saved.pixel_constant_buffer.Get()};
        context->PSSetConstantBuffers(0, 1, restore);
    }
    {
        ID3D11ShaderResourceView* const restore[] = {
            saved.pixel_resource.Get()};
        context->PSSetShaderResources(0, 1, restore);
    }
    {
        ID3D11SamplerState* const restore[] = {saved.pixel_sampler.Get()};
        context->PSSetSamplers(0, 1, restore);
    }
    context->RSSetState(saved.rasterizer.Get());
    context->OMSetBlendState(
        saved.blend.Get(), saved.blend_factor, saved.sample_mask);
    context->OMSetDepthStencilState(
        saved.depth_stencil.Get(), saved.stencil_reference);

    saved.release_instances();
    saved.input_layout.Reset();
    saved.vertex_shader.Reset();
    saved.pixel_shader.Reset();
    saved.pixel_constant_buffer.Reset();
    saved.pixel_resource.Reset();
    saved.pixel_sampler.Reset();
    saved.rasterizer.Reset();
    saved.blend.Reset();
    saved.depth_stencil.Reset();
    for (auto& view : saved.render_targets) {
        view.Reset();
    }
    saved.depth_view.Reset();

    return DebugViewStatus::drawn;
}
}
