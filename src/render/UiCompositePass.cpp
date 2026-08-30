#include "render/UiCompositePass.hpp"

#include <vector>

#include <cstdio>

#include "render/RenderDebug.hpp"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <SKSE/SKSE.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mfgdlss::render {
namespace {
namespace logger = SKSE::log;
using Microsoft::WRL::ComPtr;

class ScopedD3D11State final {
public:
  explicit ScopedD3D11State(ID3D11DeviceContext *context) noexcept
      : context_(context) {
    std::array<ID3D11RenderTargetView *, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT>
        render_targets{};
    ID3D11DepthStencilView *depth_stencil_view{};
    context_->OMGetRenderTargets(static_cast<UINT>(render_targets.size()),
                                 render_targets.data(), &depth_stencil_view);
    for (std::size_t i = 0; i < render_targets.size(); ++i) {
      render_targets_[i].Attach(render_targets[i]);
    }
    depth_stencil_view_.Attach(depth_stencil_view);

    ID3D11BlendState *blend_state{};
    context_->OMGetBlendState(&blend_state, blend_factor_.data(),
                              &sample_mask_);
    blend_state_.Attach(blend_state);

    ID3D11DepthStencilState *depth_stencil_state{};
    context_->OMGetDepthStencilState(&depth_stencil_state, &stencil_reference_);
    depth_stencil_state_.Attach(depth_stencil_state);

    ID3D11RasterizerState *rasterizer_state{};
    context_->RSGetState(&rasterizer_state);
    rasterizer_state_.Attach(rasterizer_state);
    viewport_count_ = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    context_->RSGetViewports(&viewport_count_, viewports_.data());
    scissor_count_ = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    context_->RSGetScissorRects(&scissor_count_, scissors_.data());

    ID3D11InputLayout *input_layout{};
    context_->IAGetInputLayout(&input_layout);
    input_layout_.Attach(input_layout);
    ID3D11Buffer *vertex_buffer{};
    context_->IAGetVertexBuffers(0, 1, &vertex_buffer, &vertex_stride_,
                                 &vertex_offset_);
    vertex_buffer_.Attach(vertex_buffer);
    ID3D11Buffer *index_buffer{};
    context_->IAGetIndexBuffer(&index_buffer, &index_format_, &index_offset_);
    index_buffer_.Attach(index_buffer);
    context_->IAGetPrimitiveTopology(&primitive_topology_);

    ID3D11VertexShader *vertex_shader{};
    context_->VSGetShader(&vertex_shader, nullptr, nullptr);
    vertex_shader_.Attach(vertex_shader);
    ID3D11PixelShader *pixel_shader{};
    context_->PSGetShader(&pixel_shader, nullptr, nullptr);
    pixel_shader_.Attach(pixel_shader);

    std::array<ID3D11ShaderResourceView *, 3> shader_resources{};
    context_->PSGetShaderResources(
        0, static_cast<UINT>(shader_resources.size()), shader_resources.data());
    for (std::size_t i = 0; i < shader_resources.size(); ++i) {
      shader_resources_[i].Attach(shader_resources[i]);
    }
    ID3D11SamplerState *sampler{};
    context_->PSGetSamplers(0, 1, &sampler);
    sampler_.Attach(sampler);
  }

  ~ScopedD3D11State() {
    std::array<ID3D11RenderTargetView *, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT>
        render_targets{};
    for (std::size_t i = 0; i < render_targets.size(); ++i) {
      render_targets[i] = render_targets_[i].Get();
    }
    auto *depth_stencil_view = depth_stencil_view_.Get();
    context_->OMSetRenderTargets(static_cast<UINT>(render_targets.size()),
                                 render_targets.data(), depth_stencil_view);
    context_->OMSetBlendState(blend_state_.Get(), blend_factor_.data(),
                              sample_mask_);
    context_->OMSetDepthStencilState(depth_stencil_state_.Get(),
                                     stencil_reference_);

    context_->RSSetState(rasterizer_state_.Get());
    context_->RSSetViewports(viewport_count_, viewports_.data());
    context_->RSSetScissorRects(scissor_count_, scissors_.data());

    context_->IASetInputLayout(input_layout_.Get());
    auto *vertex_buffer = vertex_buffer_.Get();
    context_->IASetVertexBuffers(0, 1, &vertex_buffer, &vertex_stride_,
                                 &vertex_offset_);
    context_->IASetIndexBuffer(index_buffer_.Get(), index_format_,
                               index_offset_);
    context_->IASetPrimitiveTopology(primitive_topology_);

    context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
    context_->PSSetShader(pixel_shader_.Get(), nullptr, 0);
    std::array<ID3D11ShaderResourceView *, 3> shader_resources{};
    for (std::size_t i = 0; i < shader_resources.size(); ++i) {
      shader_resources[i] = shader_resources_[i].Get();
    }
    context_->PSSetShaderResources(
        0, static_cast<UINT>(shader_resources.size()), shader_resources.data());
    auto *sampler = sampler_.Get();
    context_->PSSetSamplers(0, 1, &sampler);
  }

  ScopedD3D11State(const ScopedD3D11State &) = delete;
  ScopedD3D11State &operator=(const ScopedD3D11State &) = delete;

private:
  ID3D11DeviceContext *context_;
  std::array<ComPtr<ID3D11RenderTargetView>,
             D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT>
      render_targets_;
  ComPtr<ID3D11DepthStencilView> depth_stencil_view_;
  ComPtr<ID3D11BlendState> blend_state_;
  std::array<float, 4> blend_factor_{};
  UINT sample_mask_{};
  ComPtr<ID3D11DepthStencilState> depth_stencil_state_;
  UINT stencil_reference_{};
  ComPtr<ID3D11RasterizerState> rasterizer_state_;
  std::array<D3D11_VIEWPORT,
             D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>
      viewports_{};
  UINT viewport_count_{};
  std::array<D3D11_RECT,
             D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>
      scissors_{};
  UINT scissor_count_{};
  ComPtr<ID3D11InputLayout> input_layout_;
  ComPtr<ID3D11Buffer> vertex_buffer_;
  UINT vertex_stride_{};
  UINT vertex_offset_{};
  ComPtr<ID3D11Buffer> index_buffer_;
  DXGI_FORMAT index_format_{DXGI_FORMAT_UNKNOWN};
  UINT index_offset_{};
  D3D11_PRIMITIVE_TOPOLOGY primitive_topology_{
      D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED};
  ComPtr<ID3D11VertexShader> vertex_shader_;
  ComPtr<ID3D11PixelShader> pixel_shader_;
  std::array<ComPtr<ID3D11ShaderResourceView>, 3> shader_resources_;
  ComPtr<ID3D11SamplerState> sampler_;
};

[[nodiscard]] bool compile_shader(const std::string_view source,
                                  const char *profile,
                                  ComPtr<ID3DBlob> &bytecode) {
  ComPtr<ID3DBlob> errors;
  const auto result = D3DCompile(
      source.data(), source.size(), nullptr, nullptr, nullptr, "main", profile,
      D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS, 0,
      &bytecode, &errors);
  if (SUCCEEDED(result)) {
    return true;
  }
  const auto message = errors != nullptr
                           ? std::string_view(static_cast<const char *>(
                                                  errors->GetBufferPointer()),
                                              errors->GetBufferSize())
                           : std::string_view{"no compiler diagnostics"};
  logger::error("UI compositor shader compilation failed: 0x{:08X}: {}",
                static_cast<unsigned>(result), message);
  return false;
}
}

struct UiCompositePass::State {
  ComPtr<ID3D11ShaderResourceView> scene_before_ui_view;
  ComPtr<ID3D11ShaderResourceView> scene_with_ui_view;
  ComPtr<ID3D11Texture2D> upscaled_scene;
  ComPtr<ID3D11ShaderResourceView> upscaled_scene_view;
  ComPtr<ID3D11RenderTargetView> target_view;
  ComPtr<ID3D11RenderTargetView> ui_color_alpha_view;
  ComPtr<ID3D11VertexShader> vertex_shader;
  ComPtr<ID3D11PixelShader> pixel_shader;
  ComPtr<ID3D11PixelShader> coverage_pixel_shader;
  ComPtr<ID3D11PixelShader> exact_pixel_shader;
  ComPtr<ID3D11SamplerState> sampler;
  ComPtr<ID3D11RasterizerState> rasterizer;
  ComPtr<ID3D11BlendState> blend;
  ComPtr<ID3D11DepthStencilState> depth;
  ID3D11Texture2D *scene_before_ui{};
  ID3D11Texture2D *scene_with_ui{};
  ID3D11Texture2D *target{};
  ID3D11Texture2D *ui_color_alpha{};
  std::uint32_t target_width{};
  std::uint32_t target_height{};
};

struct UiCompositePass::ExtractionState {
  ComPtr<ID3D11ShaderResourceView> hudless_color_view;
  ComPtr<ID3D11ShaderResourceView> final_color_view;
  ComPtr<ID3D11RenderTargetView> ui_color_alpha_view;
  ComPtr<ID3D11Texture2D> validation_readback;
  ComPtr<ID3D11VertexShader> vertex_shader;
  ComPtr<ID3D11PixelShader> pixel_shader;
  ComPtr<ID3D11SamplerState> sampler;
  ComPtr<ID3D11RasterizerState> rasterizer;
  ComPtr<ID3D11BlendState> blend;
  ComPtr<ID3D11DepthStencilState> depth;
  ID3D11Texture2D *hudless_color{};
  ID3D11Texture2D *final_color{};
  ID3D11Texture2D *ui_color_alpha{};
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t validation_frames{};
  std::uint32_t validation_checks{};
  bool content_verified{};
  bool validation_copy_pending{};
  bool validation_exhausted_logged{};
};

struct UiCompositePass::LayerState {
  ComPtr<ID3D11ShaderResourceView> ui_color_alpha_view;
  ComPtr<ID3D11Texture2D> scene_snapshot;
  ComPtr<ID3D11ShaderResourceView> scene_snapshot_view;
  ComPtr<ID3D11RenderTargetView> target_view;
  ComPtr<ID3D11VertexShader> vertex_shader;
  ComPtr<ID3D11PixelShader> pixel_shader;
  ComPtr<ID3D11SamplerState> sampler;
  ComPtr<ID3D11RasterizerState> rasterizer;
  ComPtr<ID3D11BlendState> blend;
  ComPtr<ID3D11DepthStencilState> depth;
  ID3D11Texture2D *ui_color_alpha{};
  ID3D11Texture2D *target{};
  std::uint32_t width{};
  std::uint32_t height{};
};

struct UiCompositePass::CaptureReportState {
  ComPtr<ID3D11Texture2D> readback;
  ID3D11Texture2D *ui_color_alpha{};
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t frames_until_next_report{};
  std::uint32_t reports{};
  bool classification_available{};
  bool submission_safe{};
};

UiCompositePass::UiCompositePass() = default;
UiCompositePass::~UiCompositePass() = default;

UiCompositePass &UiCompositePass::instance() noexcept {
  static UiCompositePass pass;
  return pass;
}

bool UiCompositePass::apply_delta(ID3D11Device *device,
                                  ID3D11DeviceContext *context,
                                  ID3D11Texture2D *scene_before_ui,
                                  ID3D11Texture2D *scene_with_ui,
                                  ID3D11Texture2D *target,
                                  ID3D11Texture2D *ui_color_alpha) {

  auto &render_debug = RenderDebug::instance();
  render_debug.poll_hotkeys();

  if (device == nullptr || context == nullptr || scene_before_ui == nullptr ||
      scene_with_ui == nullptr || target == nullptr ||
      ui_color_alpha == nullptr) {
    return false;
  }

  D3D11_TEXTURE2D_DESC before_description{};
  D3D11_TEXTURE2D_DESC with_ui_description{};
  D3D11_TEXTURE2D_DESC target_description{};
  D3D11_TEXTURE2D_DESC ui_description{};
  scene_before_ui->GetDesc(&before_description);
  scene_with_ui->GetDesc(&with_ui_description);
  target->GetDesc(&target_description);
  ui_color_alpha->GetDesc(&ui_description);
  if (before_description.Width == 0 || before_description.Height == 0 ||
      before_description.Width != with_ui_description.Width ||
      before_description.Height != with_ui_description.Height ||
      before_description.Format != with_ui_description.Format ||
      before_description.SampleDesc.Count != 1 ||
      with_ui_description.SampleDesc.Count != 1 ||
      target_description.Width == 0 || target_description.Height == 0 ||
      before_description.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
      target_description.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
      ui_description.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
      ui_description.Width != target_description.Width ||
      ui_description.Height != target_description.Height ||
      target_description.SampleDesc.Count != 1) {
    return false;
  }

  const auto compatible =
      state_ != nullptr && state_->scene_before_ui == scene_before_ui &&
      state_->scene_with_ui == scene_with_ui && state_->target == target &&
      state_->ui_color_alpha == ui_color_alpha &&
      state_->target_width == target_description.Width &&
      state_->target_height == target_description.Height;
  if (!compatible) {
    state_.reset();
    auto state = std::make_unique<State>();
    state->scene_before_ui = scene_before_ui;
    state->scene_with_ui = scene_with_ui;
    state->target = target;
    state->ui_color_alpha = ui_color_alpha;
    state->target_width = target_description.Width;
    state->target_height = target_description.Height;

    auto result = device->CreateShaderResourceView(
        scene_before_ui, nullptr, &state->scene_before_ui_view);
    if (SUCCEEDED(result)) {
      result = device->CreateShaderResourceView(scene_with_ui, nullptr,
                                                &state->scene_with_ui_view);
    }
    if (SUCCEEDED(result)) {
      result =
          device->CreateRenderTargetView(target, nullptr, &state->target_view);
    }
    if (SUCCEEDED(result)) {
      result = device->CreateRenderTargetView(ui_color_alpha, nullptr,
                                              &state->ui_color_alpha_view);
    }
    auto upscaled_scene_description = target_description;
    upscaled_scene_description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    upscaled_scene_description.MiscFlags = 0;
    upscaled_scene_description.CPUAccessFlags = 0;
    upscaled_scene_description.Usage = D3D11_USAGE_DEFAULT;
    if (SUCCEEDED(result)) {
      result = device->CreateTexture2D(&upscaled_scene_description, nullptr,
                                       &state->upscaled_scene);
    }
    if (SUCCEEDED(result)) {
      result = device->CreateShaderResourceView(
          state->upscaled_scene.Get(), nullptr, &state->upscaled_scene_view);
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
Texture2D<float4> scene_before_ui : register(t0);
Texture2D<float4> scene_with_ui : register(t1);
Texture2D<float4> upscaled_scene : register(t2);
SamplerState linear_sampler : register(s0);

struct pixel_output
{
    float4 final_color : SV_Target0;
    float4 ui_color_alpha : SV_Target1;
};

 pixel_output main(
     float4 position : SV_Position,
     float2 texcoord : TEXCOORD0)
{
    const float3 before_ui =
        scene_before_ui.SampleLevel(linear_sampler, texcoord, 0.0).rgb;
    const float3 with_ui =
        scene_with_ui.SampleLevel(linear_sampler, texcoord, 0.0).rgb;
    const float4 upscaled =
        upscaled_scene.SampleLevel(linear_sampler, texcoord, 0.0);
    const float3 signed_delta = with_ui - before_ui;
    const float3 absolute_delta = abs(signed_delta);
    const float changed = step(
        0.5 / 255.0,
        max(absolute_delta.r, max(absolute_delta.g, absolute_delta.b)));
    const float3 reconstructed =
        saturate(upscaled.rgb + signed_delta);

    pixel_output output;
    output.final_color = float4(
        lerp(upscaled.rgb, reconstructed, changed),
        upscaled.a);

    output.ui_color_alpha = float4(0.0, 0.0, 0.0, 0.0);
    return output;
 }
)";

    constexpr std::string_view coverage_pixel_source = R"(
Texture2D<float4> scene_before_ui : register(t0);
Texture2D<float4> scene_with_ui : register(t1);
Texture2D<float4> upscaled_scene : register(t2);
SamplerState linear_sampler : register(s0);

struct pixel_output
{
    float4 final_color : SV_Target0;
    float4 ui_color_alpha : SV_Target1;
};

pixel_output main(
    float4 position : SV_Position,
    float2 texcoord : TEXCOORD0)
{
    const float3 before_ui =
        scene_before_ui.SampleLevel(linear_sampler, texcoord, 0.0).rgb;
    const float3 with_ui =
        scene_with_ui.SampleLevel(linear_sampler, texcoord, 0.0).rgb;
    const float4 upscaled =
        upscaled_scene.SampleLevel(linear_sampler, texcoord, 0.0);
    const float3 signed_delta = with_ui - before_ui;
    const float3 absolute_delta = abs(signed_delta);
    const float magnitude =
        max(absolute_delta.r, max(absolute_delta.g, absolute_delta.b));

    const float coverage =
        saturate((magnitude - (1.0 / 255.0)) * 8.0);
    const float3 base_difference = upscaled.rgb - before_ui;
    const float3 reconstructed =
        saturate(with_ui + ((1.0 - coverage) * base_difference));

    pixel_output output;
    output.final_color = float4(reconstructed, upscaled.a);
    output.ui_color_alpha = float4(0.0, 0.0, 0.0, 0.0);
    return output;
}
)";

    constexpr std::string_view exact_pixel_source = R"(
Texture2D<float4> scene_before_ui : register(t0);
Texture2D<float4> scene_with_ui : register(t1);
Texture2D<float4> upscaled_scene : register(t2);
SamplerState linear_sampler : register(s0);

struct pixel_output
{
    float4 final_color : SV_Target0;
    float4 ui_color_alpha : SV_Target1;
};

pixel_output main(
    float4 position : SV_Position,
    float2 texcoord : TEXCOORD0)
{
    const float3 before_ui =
        scene_before_ui.SampleLevel(linear_sampler, texcoord, 0.0).rgb;
    const float3 with_ui =
        scene_with_ui.SampleLevel(linear_sampler, texcoord, 0.0).rgb;
    const float4 upscaled =
        upscaled_scene.SampleLevel(linear_sampler, texcoord, 0.0);
    const float3 absolute_delta = abs(with_ui - before_ui);
    const float changed = step(
        0.5 / 255.0,
        max(absolute_delta.r, max(absolute_delta.g, absolute_delta.b)));

    pixel_output output;
    output.final_color = float4(
        lerp(upscaled.rgb, with_ui, changed),
        upscaled.a);
    output.ui_color_alpha = float4(0.0, 0.0, 0.0, 0.0);
    return output;
}
)";
    ComPtr<ID3DBlob> vertex_bytecode;
    ComPtr<ID3DBlob> pixel_bytecode;
    ComPtr<ID3DBlob> coverage_bytecode;
    ComPtr<ID3DBlob> exact_bytecode;
    if (FAILED(result) ||
        !compile_shader(vertex_source, "vs_5_0", vertex_bytecode) ||
        !compile_shader(pixel_source, "ps_5_0", pixel_bytecode) ||
        !compile_shader(coverage_pixel_source, "ps_5_0", coverage_bytecode) ||
        !compile_shader(exact_pixel_source, "ps_5_0", exact_bytecode)) {
      delta_failure_logged_ = true;
      return false;
    }
    result = device->CreateVertexShader(vertex_bytecode->GetBufferPointer(),
                                        vertex_bytecode->GetBufferSize(),
                                        nullptr, &state->vertex_shader);
    if (SUCCEEDED(result)) {
      result = device->CreatePixelShader(pixel_bytecode->GetBufferPointer(),
                                         pixel_bytecode->GetBufferSize(),
                                         nullptr, &state->pixel_shader);
    }
    if (SUCCEEDED(result)) {
      result =
          device->CreatePixelShader(coverage_bytecode->GetBufferPointer(),
                                    coverage_bytecode->GetBufferSize(), nullptr,
                                    &state->coverage_pixel_shader);
    }
    if (SUCCEEDED(result)) {
      result = device->CreatePixelShader(exact_bytecode->GetBufferPointer(),
                                         exact_bytecode->GetBufferSize(),
                                         nullptr, &state->exact_pixel_shader);
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
      result = device->CreateRasterizerState(&rasterizer, &state->rasterizer);
    }

    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = FALSE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    blend.RenderTarget[1] = blend.RenderTarget[0];
    if (SUCCEEDED(result)) {
      result = device->CreateBlendState(&blend, &state->blend);
    }

    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = FALSE;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    if (SUCCEEDED(result)) {
      result = device->CreateDepthStencilState(&depth, &state->depth);
    }
    if (FAILED(result)) {
      if (!delta_failure_logged_) {
        delta_failure_logged_ = true;
        logger::error("UI compositor state creation failed: 0x{:08X}",
                      static_cast<unsigned>(result));
      }
      return false;
    }
    state_ = std::move(state);
  }

  const auto composite_mode = render_debug.composite_mode();
  auto *selected_pixel_shader = state_->pixel_shader.Get();
  if (composite_mode == UiCompositeMode::coverage) {
    selected_pixel_shader = state_->coverage_pixel_shader.Get();
  } else if (composite_mode == UiCompositeMode::exact) {
    selected_pixel_shader = state_->exact_pixel_shader.Get();
  }
  if (composite_mode != logged_mode_) {
    logged_mode_ = composite_mode;
    first_success_logged_ = false;
  }

  const ScopedD3D11State restore_state{context};
  context->OMSetRenderTargets(0, nullptr, nullptr);
  context->CopyResource(state_->upscaled_scene.Get(), target);
  const D3D11_VIEWPORT viewport{0.0F,
                                0.0F,
                                static_cast<float>(state_->target_width),
                                static_cast<float>(state_->target_height),
                                0.0F,
                                1.0F};
  const D3D11_RECT scissor{0, 0, static_cast<LONG>(state_->target_width),
                           static_cast<LONG>(state_->target_height)};
  context->RSSetViewports(1, &viewport);
  context->RSSetScissorRects(1, &scissor);
  context->RSSetState(state_->rasterizer.Get());
  context->IASetInputLayout(nullptr);
  context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
  context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->VSSetShader(state_->vertex_shader.Get(), nullptr, 0);
  context->PSSetShader(selected_pixel_shader, nullptr, 0);
  const std::array views{state_->scene_before_ui_view.Get(),
                         state_->scene_with_ui_view.Get(),
                         state_->upscaled_scene_view.Get()};
  auto *sampler = state_->sampler.Get();
  context->PSSetShaderResources(0, static_cast<UINT>(views.size()),
                                views.data());
  context->PSSetSamplers(0, 1, &sampler);
  context->OMSetBlendState(state_->blend.Get(), nullptr, 0xFFFFFFFF);
  context->OMSetDepthStencilState(state_->depth.Get(), 0);
  const std::array target_views{state_->target_view.Get(),
                                state_->ui_color_alpha_view.Get()};
  context->OMSetRenderTargets(static_cast<UINT>(target_views.size()),
                              target_views.data(), nullptr);
  context->Draw(3, 0);

  const std::array<ID3D11ShaderResourceView *, 3> no_resources{};
  context->PSSetShaderResources(0, static_cast<UINT>(no_resources.size()),
                                no_resources.data());
  context->OMSetRenderTargets(0, nullptr, nullptr);

  if (render_debug.capture_pending()) {

    render_debug.dump(device, context, scene_before_ui, "1-reduced-before-ui");
    render_debug.dump(device, context, scene_with_ui, "2-reduced-with-ui");
    render_debug.dump(device, context, state_->upscaled_scene.Get(),
                      "3-upscaled-before-reconstruction");
    render_debug.dump(device, context, target, "4-final-composited");
    render_debug.dump(device, context, ui_color_alpha, "5-ui-color-alpha");
    render_debug.end_capture();
  }

  delta_failure_logged_ = false;
  if (!first_success_logged_) {
    first_success_logged_ = true;
    logger::info("Post-DLSS UI display composition verified in mode {}: "
                 "{}x{} -> {}x{}",
                 static_cast<unsigned>(composite_mode),
                 before_description.Width, before_description.Height,
                 target_description.Width, target_description.Height);
  }
  return true;
}

bool UiCompositePass::composite_layer(ID3D11Device *device,
                                      ID3D11DeviceContext *context,
                                      ID3D11Texture2D *ui_color_alpha,
                                      ID3D11Texture2D *target) {
  if (device == nullptr || context == nullptr || ui_color_alpha == nullptr ||
      target == nullptr) {
    return false;
  }

  D3D11_TEXTURE2D_DESC ui_description{};
  D3D11_TEXTURE2D_DESC target_description{};
  ui_color_alpha->GetDesc(&ui_description);
  target->GetDesc(&target_description);
  if (ui_description.Width == 0 || ui_description.Height == 0 ||
      ui_description.Width != target_description.Width ||
      ui_description.Height != target_description.Height ||
      ui_description.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
      target_description.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
      ui_description.SampleDesc.Count != 1 ||
      target_description.SampleDesc.Count != 1) {
    return false;
  }

  const auto compatible = layer_state_ != nullptr &&
                          layer_state_->ui_color_alpha == ui_color_alpha &&
                          layer_state_->target == target &&
                          layer_state_->width == target_description.Width &&
                          layer_state_->height == target_description.Height;
  if (!compatible) {
    layer_state_.reset();
    auto state = std::make_unique<LayerState>();
    state->ui_color_alpha = ui_color_alpha;
    state->target = target;
    state->width = target_description.Width;
    state->height = target_description.Height;

    auto result = device->CreateShaderResourceView(ui_color_alpha, nullptr,
                                                   &state->ui_color_alpha_view);
    if (SUCCEEDED(result)) {
      result =
          device->CreateRenderTargetView(target, nullptr, &state->target_view);
    }
    auto snapshot_description = target_description;
    snapshot_description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    snapshot_description.MiscFlags = 0;
    snapshot_description.CPUAccessFlags = 0;
    snapshot_description.Usage = D3D11_USAGE_DEFAULT;
    if (SUCCEEDED(result)) {
      result = device->CreateTexture2D(&snapshot_description, nullptr,
                                       &state->scene_snapshot);
    }
    if (SUCCEEDED(result)) {
      result = device->CreateShaderResourceView(
          state->scene_snapshot.Get(), nullptr, &state->scene_snapshot_view);
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
Texture2D<float4> ui_color_alpha : register(t0);
Texture2D<float4> scene_color : register(t1);
SamplerState linear_sampler : register(s0);

float4 main(float4 position : SV_Position, float2 texcoord : TEXCOORD0)
    : SV_Target0
{
    const float4 ui =
        ui_color_alpha.SampleLevel(linear_sampler, texcoord, 0.0);
    const float4 scene =
        scene_color.SampleLevel(linear_sampler, texcoord, 0.0);
    return float4(
        saturate(ui.rgb + ((1.0 - ui.a) * scene.rgb)),
        scene.a);
}
)";
    ComPtr<ID3DBlob> vertex_bytecode;
    ComPtr<ID3DBlob> pixel_bytecode;
    if (FAILED(result) ||
        !compile_shader(vertex_source, "vs_5_0", vertex_bytecode) ||
        !compile_shader(pixel_source, "ps_5_0", pixel_bytecode)) {
      composite_failure_logged_ = true;
      return false;
    }
    result = device->CreateVertexShader(vertex_bytecode->GetBufferPointer(),
                                        vertex_bytecode->GetBufferSize(),
                                        nullptr, &state->vertex_shader);
    if (SUCCEEDED(result)) {
      result = device->CreatePixelShader(pixel_bytecode->GetBufferPointer(),
                                         pixel_bytecode->GetBufferSize(),
                                         nullptr, &state->pixel_shader);
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
      result = device->CreateRasterizerState(&rasterizer, &state->rasterizer);
    }

    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = FALSE;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (SUCCEEDED(result)) {
      result = device->CreateBlendState(&blend, &state->blend);
    }

    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = FALSE;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    if (SUCCEEDED(result)) {
      result = device->CreateDepthStencilState(&depth, &state->depth);
    }
    if (FAILED(result)) {
      if (!composite_failure_logged_) {
        composite_failure_logged_ = true;
        logger::error("Exact UI compositor state creation failed: 0x{:08X}",
                      static_cast<unsigned>(result));
      }
      return false;
    }
    layer_state_ = std::move(state);
  }

  const ScopedD3D11State restore_state{context};
  context->OMSetRenderTargets(0, nullptr, nullptr);
  context->CopyResource(layer_state_->scene_snapshot.Get(), target);

  const D3D11_VIEWPORT viewport{0.0F,
                                0.0F,
                                static_cast<float>(layer_state_->width),
                                static_cast<float>(layer_state_->height),
                                0.0F,
                                1.0F};
  const D3D11_RECT scissor{0, 0, static_cast<LONG>(layer_state_->width),
                           static_cast<LONG>(layer_state_->height)};
  context->RSSetViewports(1, &viewport);
  context->RSSetScissorRects(1, &scissor);
  context->RSSetState(layer_state_->rasterizer.Get());
  context->IASetInputLayout(nullptr);
  context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
  context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->VSSetShader(layer_state_->vertex_shader.Get(), nullptr, 0);
  context->PSSetShader(layer_state_->pixel_shader.Get(), nullptr, 0);
  const std::array views{layer_state_->ui_color_alpha_view.Get(),
                         layer_state_->scene_snapshot_view.Get()};
  auto *sampler = layer_state_->sampler.Get();
  context->PSSetShaderResources(0, static_cast<UINT>(views.size()),
                                views.data());
  context->PSSetSamplers(0, 1, &sampler);
  context->OMSetBlendState(layer_state_->blend.Get(), nullptr, 0xFFFFFFFF);
  context->OMSetDepthStencilState(layer_state_->depth.Get(), 0);
  auto *target_view = layer_state_->target_view.Get();
  context->OMSetRenderTargets(1, &target_view, nullptr);
  context->Draw(3, 0);

  const std::array<ID3D11ShaderResourceView *, 2> no_resources{};
  context->PSSetShaderResources(0, static_cast<UINT>(no_resources.size()),
                                no_resources.data());
  context->OMSetRenderTargets(0, nullptr, nullptr);
  composite_failure_logged_ = false;
  if (!first_layer_success_logged_) {
    first_layer_success_logged_ = true;
    logger::info("Exact premultiplied native-resolution UI composition "
                 "verified at {}x{}",
                 target_description.Width, target_description.Height);
  }
  return true;
}

bool UiCompositePass::extract_ui_layer(ID3D11Device *device,
                                       ID3D11DeviceContext *context,
                                       ID3D11Texture2D *hudless_color,
                                       ID3D11Texture2D *final_color,
                                       ID3D11Texture2D *ui_color_alpha) {
  if (device == nullptr || context == nullptr || hudless_color == nullptr ||
      final_color == nullptr || ui_color_alpha == nullptr) {
    return false;
  }

  D3D11_TEXTURE2D_DESC hudless_description{};
  D3D11_TEXTURE2D_DESC final_description{};
  D3D11_TEXTURE2D_DESC ui_description{};
  hudless_color->GetDesc(&hudless_description);
  final_color->GetDesc(&final_description);
  ui_color_alpha->GetDesc(&ui_description);
  if (hudless_description.Width == 0 || hudless_description.Height == 0 ||
      hudless_description.Width != final_description.Width ||
      hudless_description.Height != final_description.Height ||
      hudless_description.Width != ui_description.Width ||
      hudless_description.Height != ui_description.Height ||
      hudless_description.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
      final_description.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
      ui_description.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
      hudless_description.SampleDesc.Count != 1 ||
      final_description.SampleDesc.Count != 1 ||
      ui_description.SampleDesc.Count != 1) {
    const auto surfaces_sized =
        hudless_description.Width != 0 && hudless_description.Height != 0 &&
        final_description.Width != 0 && final_description.Height != 0 &&
        ui_description.Width != 0 && ui_description.Height != 0;
    if (surfaces_sized && !extraction_contract_logged_) {
      extraction_contract_logged_ = true;
      logger::warn(
          "UI extraction refused this frame, so frame generation will be "
          "suspended for as long as it keeps refusing. It requires three "
          "single-sampled R8G8B8A8_UNORM (28) surfaces of one size. HUD-less "
          "is {}x{} format {} samples {}, final colour is {}x{} format {} "
          "samples {}, UI layer is {}x{} format {} samples {}. Whichever of "
          "those three does not match is the reason.",
          hudless_description.Width, hudless_description.Height,
          static_cast<unsigned>(hudless_description.Format),
          hudless_description.SampleDesc.Count, final_description.Width,
          final_description.Height,
          static_cast<unsigned>(final_description.Format),
          final_description.SampleDesc.Count, ui_description.Width,
          ui_description.Height,
          static_cast<unsigned>(ui_description.Format),
          ui_description.SampleDesc.Count);
    }
    return false;
  }
  extraction_contract_logged_ = false;

  const auto compatible = extraction_state_ != nullptr &&
                          extraction_state_->hudless_color == hudless_color &&
                          extraction_state_->final_color == final_color &&
                          extraction_state_->ui_color_alpha == ui_color_alpha &&
                          extraction_state_->width == ui_description.Width &&
                          extraction_state_->height == ui_description.Height;
  if (!compatible) {
    extraction_state_.reset();
    auto state = std::make_unique<ExtractionState>();
    state->hudless_color = hudless_color;
    state->final_color = final_color;
    state->ui_color_alpha = ui_color_alpha;
    state->width = ui_description.Width;
    state->height = ui_description.Height;

    auto result = device->CreateShaderResourceView(hudless_color, nullptr,
                                                   &state->hudless_color_view);
    if (SUCCEEDED(result)) {
      result = device->CreateShaderResourceView(final_color, nullptr,
                                                &state->final_color_view);
    }
    if (SUCCEEDED(result)) {
      result = device->CreateRenderTargetView(ui_color_alpha, nullptr,
                                              &state->ui_color_alpha_view);
    }
    auto readback_description = ui_description;
    readback_description.Usage = D3D11_USAGE_STAGING;
    readback_description.BindFlags = 0;
    readback_description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    readback_description.MiscFlags = 0;
    if (SUCCEEDED(result)) {
      result = device->CreateTexture2D(&readback_description, nullptr,
                                       &state->validation_readback);
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
Texture2D<float4> hudless_color : register(t0);
Texture2D<float4> final_color : register(t1);
SamplerState linear_sampler : register(s0);

float ui_alpha(float3 hudless, float3 final)
{
    const float3 darker = max(hudless - final, 0.0) /
        max(hudless, 1.0 / 255.0);
    const float3 brighter = max(final - hudless, 0.0) /
        max(1.0 - hudless, 1.0 / 255.0);
    const float3 candidates = max(darker, brighter);
    return saturate(max(candidates.r, max(candidates.g, candidates.b)));
}

float4 main(float4 position : SV_Position, float2 texcoord : TEXCOORD0)
    : SV_Target
{
    const float3 hudless =
        hudless_color.SampleLevel(linear_sampler, texcoord, 0.0).rgb;
    const float3 final =
        final_color.SampleLevel(linear_sampler, texcoord, 0.0).rgb;
    const float alpha = ui_alpha(hudless, final);
    const float3 premultiplied_ui = min(
        saturate(final - ((1.0 - alpha) * hudless)),
        alpha.xxx);
    return float4(premultiplied_ui, alpha);
}
)";
    ComPtr<ID3DBlob> vertex_bytecode;
    ComPtr<ID3DBlob> pixel_bytecode;
    if (FAILED(result) ||
        !compile_shader(vertex_source, "vs_5_0", vertex_bytecode) ||
        !compile_shader(pixel_source, "ps_5_0", pixel_bytecode)) {
      extraction_failure_logged_ = true;
      return false;
    }
    result = device->CreateVertexShader(vertex_bytecode->GetBufferPointer(),
                                        vertex_bytecode->GetBufferSize(),
                                        nullptr, &state->vertex_shader);
    if (SUCCEEDED(result)) {
      result = device->CreatePixelShader(pixel_bytecode->GetBufferPointer(),
                                         pixel_bytecode->GetBufferSize(),
                                         nullptr, &state->pixel_shader);
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
      result = device->CreateRasterizerState(&rasterizer, &state->rasterizer);
    }

    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = FALSE;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (SUCCEEDED(result)) {
      result = device->CreateBlendState(&blend, &state->blend);
    }

    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = FALSE;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    if (SUCCEEDED(result)) {
      result = device->CreateDepthStencilState(&depth, &state->depth);
    }
    if (FAILED(result)) {
      if (!extraction_failure_logged_) {
        extraction_failure_logged_ = true;
        logger::error("UI extraction state creation failed: 0x{:08X}",
                      static_cast<unsigned>(result));
      }
      return false;
    }
    extraction_state_ = std::move(state);
  }

  const ScopedD3D11State restore_state{context};
  context->OMSetRenderTargets(0, nullptr, nullptr);
  const D3D11_VIEWPORT viewport{0.0F,
                                0.0F,
                                static_cast<float>(extraction_state_->width),
                                static_cast<float>(extraction_state_->height),
                                0.0F,
                                1.0F};
  const D3D11_RECT scissor{0, 0, static_cast<LONG>(extraction_state_->width),
                           static_cast<LONG>(extraction_state_->height)};
  context->RSSetViewports(1, &viewport);
  context->RSSetScissorRects(1, &scissor);
  context->RSSetState(extraction_state_->rasterizer.Get());
  context->IASetInputLayout(nullptr);
  context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
  context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->VSSetShader(extraction_state_->vertex_shader.Get(), nullptr, 0);
  context->PSSetShader(extraction_state_->pixel_shader.Get(), nullptr, 0);
  const std::array views{extraction_state_->hudless_color_view.Get(),
                         extraction_state_->final_color_view.Get()};
  auto *sampler = extraction_state_->sampler.Get();
  context->PSSetShaderResources(0, static_cast<UINT>(views.size()),
                                views.data());
  context->PSSetSamplers(0, 1, &sampler);
  context->OMSetBlendState(extraction_state_->blend.Get(), nullptr, 0xFFFFFFFF);
  context->OMSetDepthStencilState(extraction_state_->depth.Get(), 0);
  auto *target_view = extraction_state_->ui_color_alpha_view.Get();
  context->OMSetRenderTargets(1, &target_view, nullptr);
  context->Draw(3, 0);

  const std::array<ID3D11ShaderResourceView *, 2> no_resources{};
  context->PSSetShaderResources(0, static_cast<UINT>(no_resources.size()),
                                no_resources.data());
  context->OMSetRenderTargets(0, nullptr, nullptr);

  if (!extraction_state_->content_verified) {
    constexpr std::uint32_t validation_interval = 30;
    constexpr std::uint32_t maximum_validation_checks = 8;
    auto polled_copy = false;
    if (extraction_state_->validation_copy_pending) {
      polled_copy = true;
      D3D11_MAPPED_SUBRESOURCE mapped{};
      const auto map_result =
          context->Map(extraction_state_->validation_readback.Get(), 0,
                       D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
      if (map_result == DXGI_ERROR_WAS_STILL_DRAWING) {

      } else {
        extraction_state_->validation_copy_pending = false;
        ++extraction_state_->validation_checks;
        if (SUCCEEDED(map_result)) {
          std::uint64_t nonzero_alpha{};
          std::uint8_t maximum_alpha{};
          for (std::uint32_t y = 0; y < ui_description.Height; ++y) {
            const auto *row = static_cast<const std::uint8_t *>(mapped.pData) +
                              (static_cast<std::size_t>(y) * mapped.RowPitch);
            for (std::uint32_t x = 0; x < ui_description.Width; ++x) {
              const auto alpha = row[x * 4 + 3];
              nonzero_alpha += alpha != 0;
              maximum_alpha = (std::max)(maximum_alpha, alpha);
            }
          }
          context->Unmap(extraction_state_->validation_readback.Get(), 0);
          const auto pixel_count = static_cast<double>(ui_description.Width) *
                                   static_cast<double>(ui_description.Height);
          const auto coverage =
              static_cast<double>(nonzero_alpha) / pixel_count;

          constexpr auto maximum_valid_coverage = 0.20;
          extraction_state_->content_verified =
              nonzero_alpha != 0 && maximum_alpha != 0 &&
              coverage <= maximum_valid_coverage;
          if (extraction_state_->content_verified) {
            logger::info("Premultiplied UI layer content verified: {} "
                         "nonzero-alpha pixels ({:.3f}%), max-alpha={}",
                         nonzero_alpha,
                         (static_cast<double>(nonzero_alpha) * 100.0) /
                             pixel_count,
                         maximum_alpha);
          } else if (coverage > maximum_valid_coverage) {
            logger::warn("Premultiplied UI layer rejected: {:.3f}% coverage "
                         "exceeds the {:.1f}% same-frame HUD safety limit",
                         coverage * 100.0, maximum_valid_coverage * 100.0);
          } else if (extraction_state_->validation_checks == 1) {
            logger::warn("Premultiplied UI layer is blank; DLSS-G UI "
                         "recomposition remains disabled until UI pixels "
                         "appear");
          }
        } else if (extraction_state_->validation_checks == 1) {
          logger::warn("Premultiplied UI layer asynchronous validation "
                       "readback failed: 0x{:08X}",
                       static_cast<unsigned>(map_result));
        }
      }
    }

    if (!extraction_state_->content_verified &&
        !extraction_state_->validation_copy_pending && !polled_copy &&
        extraction_state_->validation_checks < maximum_validation_checks) {
      if (extraction_state_->validation_frames == 0) {
        context->CopyResource(extraction_state_->validation_readback.Get(),
                              ui_color_alpha);
        extraction_state_->validation_copy_pending = true;
        extraction_state_->validation_frames = validation_interval;
      } else {
        --extraction_state_->validation_frames;
      }
    } else if (!extraction_state_->content_verified &&
               !extraction_state_->validation_copy_pending &&
               extraction_state_->validation_checks >=
                   maximum_validation_checks &&
               !extraction_state_->validation_exhausted_logged) {
      extraction_state_->validation_exhausted_logged = true;
      logger::warn("Premultiplied UI layer validation stopped after {} bounded "
                   "asynchronous checks; fallback recomposition remains "
                   "disabled",
                   maximum_validation_checks);
    }
  }

  extraction_failure_logged_ = false;
  if (extraction_state_->content_verified && !first_extraction_logged_) {
    first_extraction_logged_ = true;

    logger::info(
        "Heuristic premultiplied UI color/alpha estimate verified at "
        "{}x{}. Recomposition is exact, because the estimate is the smallest "
        "alpha that explains the observed change and the colour is solved "
        "against it. The alpha itself is a lower bound rather than the real "
        "one: opaque UI close in colour to what is behind it estimates low, "
        "so a frame generator is told that region is mostly scene. If UI "
        "ghosting is ever seen on AMD or Intel, this is where it comes from",
        ui_description.Width, ui_description.Height);
  }
  return extraction_state_->content_verified;
}

void UiCompositePass::reset_capture_reports() noexcept {
  if (capture_report_state_ == nullptr ||
      capture_report_state_->reports == 0U) {
    return;
  }
  logger::info(
      "The direct UI capture report budget is renewed for gameplay. The "
      "previous {} report(s) were taken before this load, and a menu frame is "
      "legitimately close to full-screen UI, so spending the budget there "
      "measures the menu rather than the gameplay HUD",
      capture_report_state_->reports);
  capture_report_state_->reports = 0U;
  capture_report_state_->frames_until_next_report = 0U;
  capture_report_state_->classification_available = false;
  capture_report_state_->submission_safe = false;
}

bool UiCompositePass::report_captured_ui_layer(
    ID3D11Device *device, ID3D11DeviceContext *context,
    ID3D11Texture2D *ui_color_alpha) {
  if (device == nullptr || context == nullptr || ui_color_alpha == nullptr) {
    return false;
  }
  constexpr std::uint32_t kMaximumReports{64};
  constexpr std::uint32_t kFramesBetweenReports{180};

  D3D11_TEXTURE2D_DESC description{};
  ui_color_alpha->GetDesc(&description);
  if (description.Width == 0 || description.Height == 0 ||
      description.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
    return false;
  }

  if (capture_report_state_ == nullptr ||
      capture_report_state_->ui_color_alpha != ui_color_alpha ||
      capture_report_state_->width != description.Width ||
      capture_report_state_->height != description.Height) {
    capture_report_state_.reset();
    auto state = std::make_unique<CaptureReportState>();
    state->ui_color_alpha = ui_color_alpha;
    state->width = description.Width;
    state->height = description.Height;
    auto readback_description = description;
    readback_description.Usage = D3D11_USAGE_STAGING;
    readback_description.BindFlags = 0;
    readback_description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    readback_description.MiscFlags = 0;
    const auto result = device->CreateTexture2D(&readback_description, nullptr,
                                                &state->readback);
    if (FAILED(result)) {
      if (!capture_readback_failure_logged_) {
        capture_readback_failure_logged_ = true;
        logger::warn("Direct UI layer validation readback creation failed: "
                     "0x{:08X}; optional UI tags remain withheld",
                     static_cast<unsigned>(result));
      }
      return false;
    }
    capture_readback_failure_logged_ = false;
    capture_report_state_ = std::move(state);
  }
  if (capture_report_state_->frames_until_next_report != 0) {
    --capture_report_state_->frames_until_next_report;
    return capture_report_state_->classification_available &&
           capture_report_state_->submission_safe;
  }
  capture_report_state_->frames_until_next_report = kFramesBetweenReports;

  const ScopedD3D11State restore_state{context};
  context->OMSetRenderTargets(0, nullptr, nullptr);
  context->CopyResource(capture_report_state_->readback.Get(), ui_color_alpha);
  D3D11_MAPPED_SUBRESOURCE mapped{};
  const auto map_result = context->Map(capture_report_state_->readback.Get(), 0,
                                       D3D11_MAP_READ, 0, &mapped);
  if (FAILED(map_result)) {
    if (!capture_readback_failure_logged_) {
      capture_readback_failure_logged_ = true;
      logger::warn("Direct UI layer validation readback mapping failed: "
                   "0x{:08X}; retaining the previous classification",
                   static_cast<unsigned>(map_result));
    }
    return capture_report_state_->classification_available &&
           capture_report_state_->submission_safe;
  }
  capture_readback_failure_logged_ = false;

  std::uint64_t covered{};
  std::uint64_t alpha_sum{};
  std::uint64_t premultiplied_violations{};
  std::uint8_t maximum_alpha{};
  auto minimum_x = description.Width;
  auto minimum_y = description.Height;
  std::uint32_t maximum_x{};
  std::uint32_t maximum_y{};
  std::uint32_t previous_row_covered{};
  std::uint32_t largest_row_coverage_jump{};
  std::uint32_t largest_row_coverage_jump_y{};
  for (std::uint32_t y = 0; y < description.Height; ++y) {
    const auto *const row = static_cast<const std::uint8_t *>(mapped.pData) +
                            (static_cast<std::size_t>(y) * mapped.RowPitch);
    std::uint32_t row_covered{};
    for (std::uint32_t x = 0; x < description.Width; ++x) {
      const auto offset = static_cast<std::size_t>(x) * 4;
      const auto alpha = row[offset + 3];
      if (alpha == 0) {
        continue;
      }
      ++covered;
      ++row_covered;
      alpha_sum += alpha;
      constexpr std::uint8_t kRoundingTolerance{1};
      const auto permitted_colour =
          static_cast<unsigned>(alpha) + kRoundingTolerance;
      premultiplied_violations +=
          static_cast<unsigned>(row[offset]) > permitted_colour ||
          static_cast<unsigned>(row[offset + 1]) > permitted_colour ||
          static_cast<unsigned>(row[offset + 2]) > permitted_colour;
      maximum_alpha = (std::max)(maximum_alpha, alpha);
      minimum_x = (std::min)(minimum_x, x);
      minimum_y = (std::min)(minimum_y, y);
      maximum_x = (std::max)(maximum_x, x);
      maximum_y = (std::max)(maximum_y, y);
    }
    if (y != 0) {
      const auto jump = row_covered > previous_row_covered
                            ? row_covered - previous_row_covered
                            : previous_row_covered - row_covered;
      if (jump > largest_row_coverage_jump) {
        largest_row_coverage_jump = jump;
        largest_row_coverage_jump_y = y;
      }
    }
    previous_row_covered = row_covered;
  }

  {
    const auto pixels = static_cast<double>(description.Width) *
                        static_cast<double>(description.Height);
    const auto coverage_now =
        pixels > 0.0 ? static_cast<double>(covered) / pixels : 0.0;
    if (coverage_now > 0.20 && !ui_layer_dumped_ &&
        capture_report_state_->reports >= 12U) {
      ui_layer_dumped_ = true;
      const auto* const colour_path =
          "Data/SKSE/Plugins/UFGU-ui-layer-colour.ppm";
      const auto* const alpha_path =
          "Data/SKSE/Plugins/UFGU-ui-layer-alpha.ppm";
      auto* colour_file = std::fopen(colour_path, "wb");
      auto* alpha_file = std::fopen(alpha_path, "wb");
      if (colour_file != nullptr && alpha_file != nullptr) {
        std::fprintf(colour_file, "P6%c%u %u%c255%c", 10,
                     description.Width, description.Height, 10, 10);
        std::fprintf(alpha_file, "P6%c%u %u%c255%c", 10,
                     description.Width, description.Height, 10, 10);
        std::vector<std::uint8_t> colour_row(
            static_cast<std::size_t>(description.Width) * 3U);
        std::vector<std::uint8_t> alpha_row(
            static_cast<std::size_t>(description.Width) * 3U);
        for (std::uint32_t y = 0; y < description.Height; ++y) {
          const auto* const src =
              static_cast<const std::uint8_t*>(mapped.pData) +
              (static_cast<std::size_t>(y) * mapped.RowPitch);
          for (std::uint32_t x = 0; x < description.Width; ++x) {
            const auto* const texel = src + (static_cast<std::size_t>(x) * 4U);
            colour_row[x * 3U + 0U] = texel[0];
            colour_row[x * 3U + 1U] = texel[1];
            colour_row[x * 3U + 2U] = texel[2];
            alpha_row[x * 3U + 0U] = texel[3];
            alpha_row[x * 3U + 1U] = texel[3];
            alpha_row[x * 3U + 2U] = texel[3];
          }
          std::fwrite(colour_row.data(), 1U, colour_row.size(), colour_file);
          std::fwrite(alpha_row.data(), 1U, alpha_row.size(), alpha_file);
        }
        logger::warn(
            "UI LAYER DUMPED because coverage {:.4f}% exceeded the safety "
            "limit AFTER report 12, so this is established gameplay rather "
            "than the startup transient. The first attempt at this dump fired "
            "60 seconds in and caught a normal working frame, which proved "
            "nothing. Colour written to {} and alpha written to {}",
            coverage_now * 100.0, colour_path, alpha_path);
      }
      if (colour_file != nullptr) {
        std::fclose(colour_file);
      }
      if (alpha_file != nullptr) {
        std::fclose(alpha_file);
      }
    }
  }

  context->Unmap(capture_report_state_->readback.Get(), 0);

  ++capture_report_state_->reports;
  const auto log_routine_report =
      capture_report_state_->reports <= kMaximumReports;
  const auto pixel_count = static_cast<double>(description.Width) *
                           static_cast<double>(description.Height);
  const auto coverage = static_cast<double>(covered) / pixel_count;
  constexpr auto kMaximumSafeOverlayCoverage = 0.20;
  capture_report_state_->classification_available = true;
  capture_report_state_->submission_safe =
      coverage <= kMaximumSafeOverlayCoverage;

  if (covered == 0) {
    if (log_routine_report) {
      logger::info(
        "Directly captured UI layer report {} of {}: {}x{} layer is empty, "
        "so no Skyrim UI was on screen for this frame; optional UI tags "
        "are safe",
        capture_report_state_->reports, kMaximumReports, description.Width,
        description.Height);
    }
    return true;
  }
  const auto alpha_weighted_coverage =
      (static_cast<double>(alpha_sum) * 100.0) / (pixel_count * 255.0);
  const auto violation_percentage =
      (static_cast<double>(premultiplied_violations) * 100.0) /
      static_cast<double>(covered);
  if (capture_report_state_->submission_safe) {
    if (log_routine_report) {
    logger::info(
        "Directly captured UI layer report {} of {} accepted: {} of {} "
        "pixels covered ({:.4f}%), alpha-weighted={:.4f}%, max-alpha={}, "
        "premultiplied-violations={} ({:.4f}%), bounds={},{}..{},{} in a "
        "{}x{} layer, largest adjacent-row coverage jump={} at y={}",
        capture_report_state_->reports, kMaximumReports, covered,
        static_cast<std::uint64_t>(pixel_count), coverage * 100.0,
        alpha_weighted_coverage, maximum_alpha, premultiplied_violations,
        violation_percentage, minimum_x, minimum_y, maximum_x, maximum_y,
        description.Width, description.Height, largest_row_coverage_jump,
        largest_row_coverage_jump_y);
    }
  } else {
    logger::warn(
        "Directly captured UI layer report {} of {} rejected for optional "
        "frame-generation tags: {:.4f}% coverage exceeds the {:.1f}% "
        "overlay safety limit (alpha-weighted={:.4f}%, max-alpha={}, "
        "premultiplied-violations={} ({:.4f}%), bounds={},{}..{},{} in "
        "{}x{}, largest adjacent-row coverage jump={} at y={}). The "
        "visible composite is retained; NVIDIA uses automatic final "
        "colour instead of the unsafe hint. A report number ABOVE the "
        "denominator means the logging budget is spent but classification "
        "is still running every {} frames, which is deliberate: before "
        "2026-08-21 the verdict FROZE at report 64 and a stale unsafe "
        "reading could gate generation for the rest of the session.",
        capture_report_state_->reports, kMaximumReports, coverage * 100.0,
        kMaximumSafeOverlayCoverage * 100.0, alpha_weighted_coverage,
        maximum_alpha, premultiplied_violations, violation_percentage,
        minimum_x, minimum_y, maximum_x, maximum_y, description.Width,
        description.Height, largest_row_coverage_jump,
        largest_row_coverage_jump_y, kFramesBetweenReports);
  }
  return capture_report_state_->submission_safe;
}

void UiCompositePass::shutdown() noexcept {
  state_.reset();
  layer_state_.reset();
  extraction_state_.reset();
  capture_report_state_.reset();
  logged_mode_ = RenderDebug::instance().composite_mode();
  first_success_logged_ = false;
  first_layer_success_logged_ = false;
  first_extraction_logged_ = false;
  delta_failure_logged_ = false;
  composite_failure_logged_ = false;
  extraction_failure_logged_ = false;
  capture_readback_failure_logged_ = false;
}
}
