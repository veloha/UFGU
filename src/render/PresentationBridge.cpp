#include "render/PresentationBridge.hpp"

#include "config/Settings.hpp"
#include "render/NFramePresentationPlan.hpp"
#include "enb/EnbApi.hpp"
#include "render/BaseFrameLimiter.hpp"
#include "render/BaseFrameLimiterPolicy.hpp"
#include "render/D3D12Backend.hpp"
#include "render/DebugViewPass.hpp"
#include "render/DynamicResolution.hpp"
#include "render/FsrFrameGeneration.hpp"
#include "render/MotionVectorCensus.hpp"
#include "render/NativeUiPolicy.hpp"
#include "render/PresentationModePolicy.hpp"
#include "render/ProfilePreflightRules.hpp"
#include "render/RenderDebug.hpp"
#include "render/RendererRuntime.hpp"
#include "render/ScaleformBoundary.hpp"
#include "render/SharedResources.hpp"
#include "render/StatusOverlay.hpp"
#include "render/SurfaceBlit.hpp"
#include "render/UiCompositePass.hpp"
#include "render/UpscalingPass.hpp"
#include "render/XessFrameGeneration.hpp"
#include "streamline/FeatureSupport.hpp"
#include "streamline/FrameGeneration.hpp"
#include "streamline/FrameSubmission.hpp"
#include "streamline/SuperResolution.hpp"

#include <Windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <RE/Skyrim.h>
#include <REL/Relocation.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <array>
#include <utility>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <vector>

namespace mfgdlss::render {
namespace {
namespace logger = SKSE::log;
using Microsoft::WRL::ComPtr;
using TimingClock = std::chrono::steady_clock;

template <class Function> class ScopeGuard final {
public:
  explicit ScopeGuard(Function callable) : function_(std::move(callable)) {}
  ScopeGuard(const ScopeGuard &) = delete;
  ScopeGuard &operator=(const ScopeGuard &) = delete;
  ~ScopeGuard() {
    if (armed_) {
      function_();
    }
  }
  void release() noexcept { armed_ = false; }

private:
  Function function_;
  bool armed_{true};
};

using D3D11CreateFunction = decltype(&D3D11CreateDeviceAndSwapChain);
using FactoryCreateFunction = HRESULT(STDMETHODCALLTYPE *)(
    IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);

D3D11CreateFunction original_d3d11_create{};
FactoryCreateFunction original_factory_create{};
std::atomic_bool factory_hook_installed{};

struct PresentStageTiming {
  std::uint32_t samples{};
  double scene_ui_total_ms{};
  double allocator_total_ms{};
  double interop_total_ms{};
  double present_total_ms{};
  double completion_total_ms{};
  double scene_ui_max_ms{};
  double allocator_max_ms{};
  double interop_max_ms{};
  double present_max_ms{};
  double completion_max_ms{};
  TimingClock::time_point last_slow_report{};

  void record(const double scene_ui_ms, const double allocator_ms,
              const double interop_ms, const double present_ms,
              const double completion_ms) {
    ++samples;
    scene_ui_total_ms += scene_ui_ms;
    allocator_total_ms += allocator_ms;
    interop_total_ms += interop_ms;
    present_total_ms += present_ms;
    completion_total_ms += completion_ms;
    scene_ui_max_ms = (std::max)(scene_ui_max_ms, scene_ui_ms);
    allocator_max_ms = (std::max)(allocator_max_ms, allocator_ms);
    interop_max_ms = (std::max)(interop_max_ms, interop_ms);
    present_max_ms = (std::max)(present_max_ms, present_ms);
    completion_max_ms = (std::max)(completion_max_ms, completion_ms);

    const auto total_ms =
        scene_ui_ms + allocator_ms + interop_ms + present_ms + completion_ms;
    const auto now = TimingClock::now();
    if (total_ms >= 20.0 &&
        (last_slow_report == TimingClock::time_point{} ||
         now - last_slow_report >= std::chrono::seconds(1))) {
      last_slow_report = now;
      logger::warn("Slow presentation frame: total={:.2f}ms "
                   "(scene/UI={:.2f}, allocator={:.2f}, interop={:.2f}, "
                   "Present={:.2f}, completion={:.2f})",
                   total_ms, scene_ui_ms, allocator_ms, interop_ms, present_ms,
                   completion_ms);
    }

    if (samples == 300) {
      logger::info("Presentation CPU timing over 300 frames (avg/max ms): "
                   "scene/UI={:.3f}/{:.3f}, allocator={:.3f}/{:.3f}, "
                   "interop={:.3f}/{:.3f}, Present={:.3f}/{:.3f}, "
                   "completion={:.3f}/{:.3f}",
                   scene_ui_total_ms / samples, scene_ui_max_ms,
                   allocator_total_ms / samples, allocator_max_ms,
                   interop_total_ms / samples, interop_max_ms,
                   present_total_ms / samples, present_max_ms,
                   completion_total_ms / samples, completion_max_ms);
      *this = PresentStageTiming{};
    }
  }
};

PresentStageTiming present_stage_timing;

[[nodiscard]] double
milliseconds_between(const TimingClock::time_point begin,
                     const TimingClock::time_point end) noexcept {
  return std::chrono::duration<double, std::milli>(end - begin).count();
}

struct FrameIntervalTiming {
  static constexpr std::size_t kWindow = 300U;

  TimingClock::time_point last_frame{};
  std::array<double, kWindow> intervals{};
  std::size_t samples{};

  double last_interval_ms{};

  void record(const TimingClock::time_point now) {
    if (last_frame != TimingClock::time_point{}) {
      last_interval_ms = milliseconds_between(last_frame, now);
      intervals[samples] = last_interval_ms;
      ++samples;
      if (samples == kWindow) {
        report();
        samples = 0U;
      }
    }
    last_frame = now;
  }

  void report() const {
    auto sorted = intervals;
    std::sort(sorted.begin(), sorted.end());
    const auto median = sorted[kWindow / 2U];
    const auto p95 = sorted[(kWindow * 95U) / 100U];
    const auto p99 = sorted[(kWindow * 99U) / 100U];
    const auto worst = sorted[kWindow - 1U];

    const auto doubled = static_cast<std::uint32_t>(std::count_if(
        sorted.begin(), sorted.end(),
        [median](const double value) { return value > median * 2.0; }));
    const auto quadrupled = static_cast<std::uint32_t>(std::count_if(
        sorted.begin(), sorted.end(),
        [median](const double value) { return value > median * 4.0; }));

    const auto base_fps = median > 0.0 ? 1000.0 / median : 0.0;
    const auto multiplier =
        config::Settings::instance().frame_generation_enabled()
            ? config::Settings::instance().frame_generation_multiplier()
            : 1U;
    logger::info(
        "Skyrim's own present interval over {} frames: median={:.2f}ms "
        "({:.0f} base FPS), p95={:.2f}, p99={:.2f}, worst={:.2f}. Frames over "
        "2x median: {}, over 4x: {}. This is the cadence BEFORE generated "
        "frames, which is what responsiveness follows; at the configured {}x "
        "the display receives about {:.0f} FPS. A high displayed rate that "
        "feels like the base rate is frame generation working as designed, "
        "not a pacing fault. The generated frames are spaced by the runtime "
        "inside its own swap chain and are not visible to this counter",
        kWindow, median, base_fps, p95, p99, worst,
        doubled, quadrupled, multiplier,
        base_fps * static_cast<double>(multiplier));
  }
};

struct VideoMemoryCensus final {
  static constexpr std::uint64_t kPresentsBetweenSamples = 30ULL;
  static constexpr std::uint64_t kPresentsBetweenReports = 1800ULL;

  std::uint64_t presents{};
  std::uint64_t peak_usage{};
  bool pressure_warned{};

  void sample() {
    ++presents;
    if (presents % kPresentsBetweenSamples != 0ULL) {
      return;
    }
    const auto status = D3D12Backend::instance().video_memory_status();
    if (!status.available) {
      return;
    }
    peak_usage = (std::max)(peak_usage, status.current_usage_bytes);

    const auto pressure =
        status.current_usage_bytes * 100ULL >= status.budget_bytes * 90ULL;
    if (pressure && !pressure_warned) {
      pressure_warned = true;
      report(status, true);
      peak_usage = 0ULL;
      return;
    }
    if (!pressure) {
      pressure_warned = false;
    }
    if (presents % kPresentsBetweenReports == 0ULL) {
      report(status, false);
      peak_usage = 0ULL;
    }
  }

  void report(const VideoMemoryStatus &status, const bool pressure) const {
    constexpr auto kMiB = 1024ULL * 1024ULL;
    const auto generation_bytes =
        XessFrameGeneration::selected()
            ? XessFrameGeneration::instance().estimated_vram_bytes()
            : (FsrFrameGeneration::selected()
                   ? FsrFrameGeneration::instance().estimated_vram_bytes()
                   : streamline::FeatureSupport::instance()
                         .estimated_vram_bytes());
    const auto &settings = config::Settings::instance();
    const auto multiplier = settings.frame_generation_enabled()
                                ? settings.frame_generation_multiplier()
                                : 1U;
    const auto headroom =
        status.budget_bytes > status.current_usage_bytes
            ? status.budget_bytes - status.current_usage_bytes
            : 0ULL;
    const auto percent =
        status.budget_bytes == 0ULL
            ? 0.0
            : 100.0 * static_cast<double>(status.current_usage_bytes) /
                  static_cast<double>(status.budget_bytes);

    if (pressure) {
      logger::warn(
          "Video memory is close to the driver budget: {} MiB used of {} MiB "
          "({:.1f}%), {} MiB free, peak {} MiB. Frame generation is holding "
          "about {} MiB at {}x. The multiplier is the lever this plugin "
          "controls: each extra generated frame costs another set of "
          "full-resolution buffers, so dropping one step frees memory "
          "immediately. Reducing texture resolution is the other lever and is "
          "not something this plugin does",
          status.current_usage_bytes / kMiB, status.budget_bytes / kMiB,
          percent, headroom / kMiB, peak_usage / kMiB,
          generation_bytes / kMiB, multiplier);
      return;
    }
    logger::info(
        "Video memory census over {} presents: {} MiB used of {} MiB budget "
        "({:.1f}%), {} MiB free, peak {} MiB this window. Frame generation is "
        "holding about {} MiB at {}x. Budget is what the driver is willing to "
        "give this process right now, not the card's total, so it moves when "
        "other applications take memory",
        kPresentsBetweenReports,
        status.current_usage_bytes / kMiB, status.budget_bytes / kMiB,
        percent, headroom / kMiB, peak_usage / kMiB, generation_bytes / kMiB,
        multiplier);
  }
};

FrameIntervalTiming frame_interval_timing;
VideoMemoryCensus video_memory_census;

bool vendor_generation_gate = true;

[[nodiscard]] bool same_com_identity(IUnknown *left, IUnknown *right) noexcept {
  if (left == nullptr || right == nullptr) {
    return left == right;
  }
  ComPtr<IUnknown> left_identity;
  ComPtr<IUnknown> right_identity;
  return SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(&left_identity))) &&
         SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(&right_identity))) &&
         left_identity.Get() == right_identity.Get();
}

[[nodiscard]] D3D12_RESOURCE_BARRIER
transition(ID3D12Resource *resource, const D3D12_RESOURCE_STATES before,
           const D3D12_RESOURCE_STATES after) noexcept {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  return barrier;
}
}

class SwapChainProxy final : public IDXGISwapChain {
public:
  explicit SwapChainProxy(PresentationBridge &bridge) noexcept
      : bridge_(bridge) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID interface_id,
                                           void **object) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID name, UINT size,
                                           const void *data) override;
  HRESULT STDMETHODCALLTYPE
  SetPrivateDataInterface(REFGUID name, const IUnknown *object) override;
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID name, UINT *size,
                                           void *data) override;
  HRESULT STDMETHODCALLTYPE GetParent(REFIID interface_id,
                                      void **parent) override;
  HRESULT STDMETHODCALLTYPE GetDevice(REFIID interface_id,
                                      void **device) override;
  HRESULT STDMETHODCALLTYPE Present(UINT interval, UINT flags) override;
  HRESULT STDMETHODCALLTYPE GetBuffer(UINT index, REFIID interface_id,
                                      void **surface) override;
  HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL fullscreen,
                                               IDXGIOutput *target) override;
  HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL *fullscreen,
                                               IDXGIOutput **target) override;
  HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC *description) override;
  HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count, UINT width, UINT height,
                                          DXGI_FORMAT format,
                                          UINT flags) override;
  HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC *target) override;
  HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput **output) override;
  HRESULT STDMETHODCALLTYPE
  GetFrameStatistics(DXGI_FRAME_STATISTICS *statistics) override;
  HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT *count) override;

private:
  PresentationBridge &bridge_;
  std::atomic_ulong references_{1};
};

struct PresentationBridge::State {
  struct UiDepthTarget {
    ComPtr<ID3D11DepthStencilView> source;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11DepthStencilView> view;
    ComPtr<ID3D11DepthStencilView> clear_view;
  };

  ComPtr<ID3D11Device5> d3d11_device;
  ComPtr<ID3D11DeviceContext4> d3d11_context;
  ComPtr<ID3D12Device> d3d12_device;
  ComPtr<ID3D12CommandQueue> command_queue;
  ComPtr<IDXGIFactory2> factory;

  ComPtr<IDXGISwapChain4> swap_chain;

  ComPtr<IDXGISwapChain4> generated_swap_chain;

  DXGI_SWAP_CHAIN_DESC1 presented_description{};

  HWND handoff_window{};
  ComPtr<ID3D12Resource> shared_back_buffer_d3d12;
  ComPtr<ID3D11Texture2D> shared_back_buffer_d3d11;
  ComPtr<ID3D11RenderTargetView> shared_back_buffer_view;
  ComPtr<ID3D11Texture2D> render_back_buffer_d3d11;
  ComPtr<ID3D11RenderTargetView> render_back_buffer_view;
  ComPtr<ID3D11ShaderResourceView> render_back_buffer_resource_view;
  ComPtr<ID3D12Fence> d3d12_fence;
  ComPtr<ID3D11Fence> d3d11_fence;
  std::vector<ComPtr<ID3D12Resource>> swap_chain_buffers;
  std::vector<ComPtr<ID3D12CommandAllocator>> allocators;
  std::vector<std::uint64_t> allocator_fence_values;
  std::vector<UiDepthTarget> ui_depth_targets;
  ComPtr<ID3D12GraphicsCommandList> command_list;
  HANDLE shared_resource_handle{};
  HANDLE shared_fence_handle{};
  HANDLE completion_event{};
  DXGI_SWAP_CHAIN_DESC requested_description{};
  SwapChainProxy *proxy{};
  std::uint64_t fence_value{1};
  std::uint32_t render_width{};
  std::uint32_t render_height{};
  std::uint32_t output_width{};
  std::uint32_t output_height{};
  bool virtual_render_surface{};

  bool sub_rect_surface{};
  bool first_present_logged{};
  bool proxy_description_logged{};
  bool proxy_buffer_logged{};
  std::atomic_bool proxy_buffer_published{};
  bool missing_pre_ui_resolve_logged{};
  bool native_presentation_logged{};
  bool late_scene_boundary_logged{};
  bool back_buffer_index_warning_logged{};
  bool vendor_frame_unhealthy{};
  bool command_list_recording{};
  bool command_list_recovery_logged{};
  bool ui_separation_unavailable_logged{};
  bool unsafe_ui_hint_logged{};
  bool native_ui_direct_logged{};
  bool ui_depth_shape_logged{};
  bool ui_depth_creation_logged{};
  bool upscaling_reconfiguration_pending{};

  ~State() {
    if (completion_event != nullptr) {
      CloseHandle(completion_event);
    }
    if (shared_fence_handle != nullptr) {
      CloseHandle(shared_fence_handle);
    }
    if (shared_resource_handle != nullptr) {
      CloseHandle(shared_resource_handle);
    }
  }

  [[nodiscard]] HRESULT create_shared_back_buffer(UINT width, UINT height,
                                                  DXGI_FORMAT format);
  [[nodiscard]] HRESULT create_render_back_buffer(UINT width, UINT height,
                                                  DXGI_FORMAT format);
  void release_frame_resources() noexcept;
  [[nodiscard]] HRESULT
  create_frame_resources(const DXGI_SWAP_CHAIN_DESC1 &description);
  [[nodiscard]] HRESULT recreate_own_swap_chain();
  [[nodiscard]] HRESULT wait_for_gpu_idle();
  [[nodiscard]] HRESULT wait_for_allocator(UINT index);
  [[nodiscard]] HRESULT resize_buffers(UINT count, UINT width, UINT height,
                                       DXGI_FORMAT format, UINT flags);
  [[nodiscard]] HRESULT present(UINT interval, UINT flags);
};

HRESULT PresentationBridge::State::create_shared_back_buffer(
    const UINT width, const UINT height, const DXGI_FORMAT format) {
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap.CreationNodeMask = 1;
  heap.VisibleNodeMask = 1;

  D3D12_RESOURCE_DESC description{};
  description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  description.Width = width;
  description.Height = height;
  description.DepthOrArraySize = 1;
  description.MipLevels = 1;
  description.Format = format;
  description.SampleDesc = {1, 0};
  description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                      D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

  auto result = d3d12_device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_SHARED, &description, D3D12_RESOURCE_STATE_COMMON,
      nullptr, IID_PPV_ARGS(&shared_back_buffer_d3d12));
  if (FAILED(result)) {
    return result;
  }

  result = d3d12_device->CreateSharedHandle(shared_back_buffer_d3d12.Get(),
                                            nullptr, GENERIC_ALL, nullptr,
                                            &shared_resource_handle);
  if (FAILED(result)) {
    return result;
  }

  ComPtr<ID3D11Device1> device;
  result = d3d11_device.As(&device);
  if (FAILED(result)) {
    return result;
  }
  result = device->OpenSharedResource1(shared_resource_handle,
                                       IID_PPV_ARGS(&shared_back_buffer_d3d11));
  if (SUCCEEDED(result)) {
    result = d3d11_device->CreateRenderTargetView(
        shared_back_buffer_d3d11.Get(), nullptr, &shared_back_buffer_view);
  }
  if (FAILED(result)) {
    shared_back_buffer_view.Reset();
    shared_back_buffer_d3d11.Reset();
  }
  return result;
}

HRESULT PresentationBridge::State::create_render_back_buffer(
    const UINT width, const UINT height, const DXGI_FORMAT format) {
  render_back_buffer_resource_view.Reset();
  render_back_buffer_view.Reset();
  render_back_buffer_d3d11.Reset();
  if (!virtual_render_surface) {
    return S_OK;
  }

  D3D11_TEXTURE2D_DESC description{};
  description.Width = width;
  description.Height = height;
  description.MipLevels = 1;
  description.ArraySize = 1;
  description.Format = format;
  description.SampleDesc = {1, 0};
  description.Usage = D3D11_USAGE_DEFAULT;
  description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  auto result = d3d11_device->CreateTexture2D(&description, nullptr,
                                              &render_back_buffer_d3d11);
  if (SUCCEEDED(result)) {
    result = d3d11_device->CreateRenderTargetView(
        render_back_buffer_d3d11.Get(), nullptr, &render_back_buffer_view);
  }
  if (SUCCEEDED(result)) {
    result = d3d11_device->CreateShaderResourceView(
        render_back_buffer_d3d11.Get(), nullptr,
        &render_back_buffer_resource_view);
  }
  if (FAILED(result)) {
    render_back_buffer_resource_view.Reset();
    render_back_buffer_view.Reset();
    render_back_buffer_d3d11.Reset();
  }
  return result;
}

void PresentationBridge::State::release_frame_resources() noexcept {

  DebugViewPass::instance().shutdown();
  d3d11_context->OMSetRenderTargets(0, nullptr, nullptr);
  d3d11_context->Flush();
  SurfaceBlit::instance().shutdown();
  command_list.Reset();
  allocators.clear();
  allocator_fence_values.clear();
  swap_chain_buffers.clear();
  ui_depth_targets.clear();
  ui_depth_shape_logged = false;
  ui_depth_creation_logged = false;
  render_back_buffer_resource_view.Reset();
  render_back_buffer_view.Reset();
  render_back_buffer_d3d11.Reset();
  shared_back_buffer_view.Reset();
  shared_back_buffer_d3d11.Reset();
  shared_back_buffer_d3d12.Reset();
  if (shared_resource_handle != nullptr) {
    CloseHandle(shared_resource_handle);
    shared_resource_handle = nullptr;
  }
}

HRESULT PresentationBridge::State::create_frame_resources(
    const DXGI_SWAP_CHAIN_DESC1 &description) {
  if (description.Width == 0 || description.Height == 0 ||
      description.BufferCount < 2) {
    return E_INVALIDARG;
  }

  swap_chain_buffers.resize(description.BufferCount);
  allocators.resize(description.BufferCount);
  allocator_fence_values.assign(description.BufferCount, 0);
  for (UINT index = 0; index < description.BufferCount; ++index) {
    auto result =
        swap_chain->GetBuffer(index, IID_PPV_ARGS(&swap_chain_buffers[index]));
    if (FAILED(result)) {
      release_frame_resources();
      return result;
    }
    result = d3d12_device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[index]));
    if (FAILED(result)) {
      release_frame_resources();
      return result;
    }
  }

  auto result = d3d12_device->CreateCommandList(
      0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0].Get(), nullptr,
      IID_PPV_ARGS(&command_list));
  if (FAILED(result) || FAILED(command_list->Close())) {
    release_frame_resources();
    return FAILED(result) ? result : E_FAIL;
  }

  result = create_shared_back_buffer(description.Width, description.Height,
                                     description.Format);
  if (SUCCEEDED(result)) {

    result = create_render_back_buffer(
        sub_rect_surface ? output_width : render_width,
        sub_rect_surface ? output_height : render_height, description.Format);
  }
  if (FAILED(result)) {
    release_frame_resources();
  }
  return result;
}

HRESULT PresentationBridge::State::wait_for_gpu_idle() {
  const auto d3d11_done = fence_value++;
  auto result = d3d11_context->Signal(d3d11_fence.Get(), d3d11_done);
  if (FAILED(result)) {
    return result;
  }
  d3d11_context->Flush();
  result = command_queue->Wait(d3d12_fence.Get(), d3d11_done);
  if (FAILED(result)) {
    return result;
  }

  const auto d3d12_done = fence_value++;
  result = command_queue->Signal(d3d12_fence.Get(), d3d12_done);
  if (FAILED(result)) {
    return result;
  }

  result = d3d11_context->Wait(d3d11_fence.Get(), d3d12_done);
  if (FAILED(result)) {
    return result;
  }

  if (d3d12_fence->GetCompletedValue() >= d3d12_done) {
    return S_OK;
  }
  result = d3d12_fence->SetEventOnCompletion(d3d12_done, completion_event);
  if (FAILED(result)) {
    return result;
  }
  return WaitForSingleObject(completion_event, 5000) == WAIT_OBJECT_0
             ? S_OK
             : DXGI_ERROR_DEVICE_HUNG;
}

HRESULT PresentationBridge::State::wait_for_allocator(const UINT index) {
  const auto value = allocator_fence_values[index];
  if (value == 0 || d3d12_fence->GetCompletedValue() >= value) {
    return S_OK;
  }
  auto result = d3d12_fence->SetEventOnCompletion(value, completion_event);
  if (FAILED(result)) {
    return result;
  }
  return WaitForSingleObject(completion_event, 5000) == WAIT_OBJECT_0
             ? S_OK
             : DXGI_ERROR_DEVICE_HUNG;
}

HRESULT PresentationBridge::State::resize_buffers(const UINT count,
                                                  const UINT width,
                                                  const UINT height,
                                                  const DXGI_FORMAT format,
                                                  UINT flags) {
  if (!streamline::FrameGeneration::instance().suspend_for_reset(
          "the swap chain buffers are being resized")) {
    return E_FAIL;
  }

  DynamicResolution::instance().end_full_resolution_ui();

  auto &renderer_runtime = RendererRuntime::instance();
  auto result = wait_for_gpu_idle();
  if (FAILED(result)) {
    logger::error("GPU idle wait before ResizeBuffers failed: 0x{:08X}",
                  static_cast<unsigned>(result));
    return result;
  }

  renderer_runtime.pre_reset();
  release_frame_resources();
  flags |= requested_description.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

  const auto mode_switch = upscaling_reconfiguration_pending;
  const auto physical_width =
      mode_switch && output_width != 0
          ? output_width
          : (virtual_render_surface ? output_width : width);
  const auto physical_height =
      mode_switch && output_height != 0
          ? output_height
          : (virtual_render_surface ? output_height : height);
  const auto resize_result = swap_chain->ResizeBuffers(
      count, physical_width, physical_height, format, flags);

  DXGI_SWAP_CHAIN_DESC1 description{};
  result = swap_chain->GetDesc1(&description);
  if (FAILED(result)) {
    logger::error("GetDesc1 after ResizeBuffers failed: 0x{:08X}",
                  static_cast<unsigned>(result));
    renderer_runtime.post_reset();
    return FAILED(resize_result) ? resize_result : result;
  }

  output_width = description.Width;
  output_height = description.Height;
  const auto desired_mode = config::Settings::instance().upscaling_mode();
  const auto desired_virtual_surface =
      uses_complete_frame_presentation(desired_mode);
  auto &super_resolution = streamline::SuperResolution::instance();
  if (mode_switch) {
    if (!super_resolution.reconfigure(output_width, output_height) ||
        !super_resolution.set_mode(desired_mode)) {
      logger::error("DLSS could not reconfigure the complete-frame surface "
                    "during ResizeBuffers");
      renderer_runtime.post_reset();
      return E_FAIL;
    }
    virtual_render_surface = desired_virtual_surface;
  } else if (virtual_render_surface &&
             (!super_resolution.reconfigure(output_width, output_height) ||
              !super_resolution.set_mode(desired_mode))) {
    logger::error("DLSS could not reconfigure the complete-frame surface "
                  "during ResizeBuffers");
    renderer_runtime.post_reset();
    return E_FAIL;
  }
  if (virtual_render_surface) {
    render_width = super_resolution.render_width();
    render_height = super_resolution.render_height();
  } else {
    render_width = output_width;
    render_height = output_height;
  }

  const auto reduced_extent =
      render_width < output_width || render_height < output_height;
  sub_rect_surface =
      virtual_render_surface && reduced_extent &&
      config::Settings::instance().surface_model() ==
          config::SurfaceModel::sub_rect;

  result = create_frame_resources(description);
  if (FAILED(result)) {
    logger::critical("Presentation resources could not be rebuilt after "
                     "ResizeBuffers: 0x{:08X}",
                     static_cast<unsigned>(result));
    renderer_runtime.post_reset();
    return result;
  }

  requested_description.BufferCount = description.BufferCount;
  requested_description.BufferDesc.Width = description.Width;
  requested_description.BufferDesc.Height = description.Height;
  requested_description.BufferDesc.Format = description.Format;
  requested_description.SampleDesc = description.SampleDesc;
  requested_description.BufferUsage = description.BufferUsage;
  requested_description.SwapEffect = description.SwapEffect;
  requested_description.Flags = description.Flags;

  if (virtual_render_surface && !sub_rect_surface) {
    requested_description.BufferDesc.Width = render_width;
    requested_description.BufferDesc.Height = render_height;
  }

  if (!renderer_runtime.resize(description.Width, description.Height,
                               description.BufferCount)) {
    logger::error(
        "Presentation resize succeeded, but DLSS feature reconfiguration "
        "failed; rendering will continue with frame generation suspended");
  }
  renderer_runtime.post_reset();

  if (FAILED(resize_result)) {
    logger::warn("ResizeBuffers failed with 0x{:08X}; previous presentation "
                 "resources were restored",
                 static_cast<unsigned>(resize_result));
    return resize_result;
  }

  if (swap_chain != nullptr) {
    DXGI_SWAP_CHAIN_DESC1 resized_description{};
    if (SUCCEEDED(swap_chain->GetDesc1(&resized_description))) {
      presented_description = resized_description;
    }
  }
  PresentationBridge::instance().publish_display_refresh();

  first_present_logged = false;
  upscaling_reconfiguration_pending = false;
  logger::info("Presentation bridge resized to {}x{}, render={}x{}, "
               "format={}, buffers={}, DLSS-mode={}, virtual-surface={}",
               description.Width, description.Height, render_width,
               render_height, static_cast<unsigned>(description.Format),
               description.BufferCount, static_cast<unsigned>(desired_mode),
               virtual_render_surface);
  return S_OK;
}

HRESULT PresentationBridge::State::present(const UINT interval, UINT flags) {
  vendor_frame_unhealthy = false;
  const auto fail_frame = [this](const char *const stage,
                                 const HRESULT reason) {
    static const char *last_stage = nullptr;
    static auto last_report = TimingClock::time_point{};
    const auto now = TimingClock::now();
    if (stage != last_stage || last_report == TimingClock::time_point{} ||
        now - last_report >= std::chrono::seconds(1)) {
      last_stage = stage;
      last_report = now;
      logger::error("Presentation abandoned this frame at {}: 0x{:08X}. Skyrim "
                    "received this HRESULT from SwapChainProxy::Present.",
                    stage, static_cast<unsigned>(reason));
      if (reason == DXGI_ERROR_DEVICE_REMOVED ||
          reason == DXGI_ERROR_DEVICE_RESET) {
        const auto removal = d3d12_device != nullptr
                                 ? d3d12_device->GetDeviceRemovedReason()
                                 : S_OK;
        logger::error(
            "The D3D12 device reports removal reason 0x{:08X}. A reason of "
            "0x887A0006 is a GPU hang, 0x887A0005 is a driver-internal "
            "error, and 0x887A0020 means the device was reset by another "
            "process. This attributes the failure to the device or its "
            "driver rather than to this plugin's presentation logic.",
            static_cast<unsigned>(removal));
      }
      if (config::Settings::instance().d3d12_debug_layer()) {
        D3D12Backend::instance().drain_debug_messages();
      }
    }
    return reason;
  };
  if (swap_chain == nullptr) {
    static auto last_rebuild_attempt = TimingClock::time_point{};
    const auto attempt_now = TimingClock::now();
    if (handoff_window != nullptr &&
        (last_rebuild_attempt == TimingClock::time_point{} ||
         attempt_now - last_rebuild_attempt >= std::chrono::seconds(1))) {
      last_rebuild_attempt = attempt_now;
      if (SUCCEEDED(recreate_own_swap_chain())) {
        logger::info("Presentation recovered: this plugin's own swap chain was "
                     "rebuilt on a later frame after the first attempt failed");
      }
    }
    if (swap_chain == nullptr) {
      return fail_frame("presentation has no swap chain, because rebuilding "
                        "this plugin's own chain failed after a frame "
                        "generator released it",
                        DXGI_ERROR_INVALID_CALL);
    }
  }
  const auto cadence = streamline::FrameGeneration::instance().cadence();
  const auto base_frame_limit =
      config::Settings::instance().base_frame_limit();
  const auto base_limiter_active =
      base_frame_limit_active(base_frame_limit, cadence);
  BaseFrameLimiter::instance().wait(
      base_frame_limit, !base_limiter_active);
  {
    static std::uint32_t logged_limit = UINT32_MAX;
    static auto logged_cadence = GenerationCadence::fixed_multiplier;
    if (logged_limit != base_frame_limit || logged_cadence != cadence) {
      logged_limit = base_frame_limit;
      logged_cadence = cadence;
      logger::info(
          "Base rendered FPS limiter: configured={}, effective={}, cadence={}",
          base_frame_limit,
          base_limiter_active ? base_frame_limit : 0U,
          describe(cadence));
    }
  }
  const auto frame_begin = TimingClock::now();

  frame_interval_timing.record(frame_begin);
  auto &renderer_runtime = RendererRuntime::instance();

  RenderDebug::instance().log_present_boundary_state(
      d3d11_context.Get(), requested_description.OutputWindow, render_width,
      render_height, output_width, output_height);

  auto &dynamic_resolution_trace = DynamicResolution::instance();
  dynamic_resolution_trace.note_production_ui_phase_end_boundary();

  if (virtual_render_surface) {
    auto &dynamic_resolution = DynamicResolution::instance();

    if (UpscalingPass::instance().scene_resolve_pending()) {
      static_cast<void>(UpscalingPass::instance().commit_pending_scene_resolve(
          "present-floor"));
      if (!late_scene_boundary_logged) {
        late_scene_boundary_logged = true;
        logger::warn("Skyrim's scene-domain episode did not end through an "
                     "output-merger transition this frame; the resolve was "
                     "committed at Present, so any UI drawn this frame went to "
                     "the reduced surface");
      }
    }
    const auto missing_pre_ui_presentation =
        !UpscalingPass::instance().presentation_prepared();
    const auto invalid_ui_redirection =
        dynamic_resolution.ui_target_redirection_failed();

    const auto ui_phase_was_open =
        dynamic_resolution.full_resolution_ui_active();

    auto &shared_resources = SharedResources::instance();
    const auto ui_captured_directly = shared_resources.ui_rendering();
    shared_resources.end_ui_rendering();
    dynamic_resolution.end_full_resolution_ui();

    const auto reduced_ui_reconstruction_failed = false;
    if (missing_pre_ui_presentation || invalid_ui_redirection ||
        reduced_ui_reconstruction_failed) {

      const auto fallback_presented =
          render_back_buffer_d3d11 != nullptr &&
          shared_back_buffer_d3d11 != nullptr &&
          SurfaceBlit::instance().apply(d3d11_device.Get(), d3d11_context.Get(),
                                        render_back_buffer_d3d11.Get(),
                                        shared_back_buffer_d3d11.Get());
      const auto before_first_gameplay_frame =
          missing_pre_ui_presentation && !invalid_ui_redirection &&
          !UpscalingPass::instance().scene_ever_finalized();
      if (!missing_pre_ui_resolve_logged) {
        missing_pre_ui_resolve_logged = true;
        if (fallback_presented && before_first_gameplay_frame) {
          logger::info(
              "No gameplay frame has been rendered yet, so there is no scene "
              "to reconstruct and the menu is presented through the spatial "
              "fallback with frame generation suspended. This is the expected "
              "state in the main menu and while a save is loading, and it "
              "ends on the first rendered gameplay frame. Suspension counts "
              "reported before that point describe the menu, not a fault");
        } else if (fallback_presented) {
          logger::warn(
              "Presenting the complete reduced frame through the "
              "spatial fallback because {}; MFG is suspended",
              invalid_ui_redirection
                  ? "the full-resolution UI target remap was unsafe"
                  : (missing_pre_ui_presentation
                         ? "no pre-UI full-resolution presentation was "
                           "prepared"
                         : "the reduced Skyrim HUD reconstruction "
                           "failed"));
        } else {
          logger::warn("The completed-frame spatial presentation fallback "
                       "failed; retaining the previous complete frame and "
                       "suspending MFG");
        }
      }
      static_cast<void>(streamline::FrameGeneration::instance().suspend(
          before_first_gameplay_frame ?
              "no gameplay frame has been rendered yet, which is the expected "
              "state in the main menu and while a save loads" :
              "no pre-UI full-resolution presentation was prepared, so the "
              "frame was completed through the spatial fallback"));
      vendor_frame_unhealthy = true;
    } else {
      if (missing_pre_ui_resolve_logged) {
        logger::info("Pre-UI presentation and UI target remap recovered; "
                     "completed-frame spatial fallback ended");
      }
      missing_pre_ui_resolve_logged = false;
      if (!native_presentation_logged) {
        native_presentation_logged = true;

        logger::info("Presenting the native {}x{} surface unmodified; frame "
                     "generation is eligible to resume. Whether temporal "
                     "reconstruction produced its contents is reported "
                     "separately by the upscaling pass.",
                     output_width, output_height);
      }

      const auto inputs_available =
          shared_resources.ui_recomposition_available() &&
          shared_resources.frame_generation_hudless_captured();
      auto ui_layer_ready = false;
      auto ui_submission_safe = false;
      if (ui_captured_directly) {
        ui_layer_ready =
            inputs_available && UiCompositePass::instance().composite_layer(
                                    d3d11_device.Get(), d3d11_context.Get(),
                                    shared_resources.ui_color_alpha_d3d11(),
                                    shared_back_buffer_d3d11.Get());
        if (ui_layer_ready) {
          ScaleformBoundary::instance().note_first_composite();
          ui_submission_safe =
              UiCompositePass::instance().report_captured_ui_layer(
                  d3d11_device.Get(), d3d11_context.Get(),
                  shared_resources.ui_color_alpha_d3d11());
        }
      } else {

        ui_layer_ready = ui_phase_was_open && inputs_available &&
                         UiCompositePass::instance().extract_ui_layer(
                             d3d11_device.Get(), d3d11_context.Get(),
                             shared_resources.frame_generation_hudless_d3d11(),
                             shared_back_buffer_d3d11.Get(),
                             shared_resources.ui_color_alpha_d3d11());
        ui_submission_safe = ui_layer_ready;
      }
      const auto ui_tags_ready = ui_layer_ready && ui_submission_safe;
      const auto nvidia_generator_selected =
          !FsrFrameGeneration::selected() && !XessFrameGeneration::selected();
      const auto withhold_unsafe_hint =
          ui_layer_ready && !ui_submission_safe && nvidia_generator_selected;
      if (ui_tags_ready) {

        shared_resources.mark_ui_captured();
      } else if (withhold_unsafe_hint) {

        if (!unsafe_ui_hint_logged) {
          unsafe_ui_hint_logged = true;
          logger::warn("Reduced-resolution UI capture resembled a full scene "
                       "at {}x{}; the unsafe optional HUD-less/UI tags were "
                       "withheld and DLSS-G REMAINS ACTIVE on automatic final "
                       "colour. Before 2026-08-21 this branch suspended "
                       "generation outright, which the native path never did, "
                       "and because the guarded recovery counter is reset by "
                       "the same frame that increments it, generation could "
                       "not come back without an alt-tab",
                       output_width, output_height);
        }
      } else if (ui_phase_was_open) {

        if (!ui_separation_unavailable_logged) {
          ui_separation_unavailable_logged = true;
          logger::warn("No valid HUD-less/UI separation for this frame; "
                       "reduced-resolution frame generation is suspended "
                       "rather than submitting an unsafe optional UI hint");
        }
        static_cast<void>(streamline::FrameGeneration::instance().suspend(
            "this frame had no valid HUD-less and UI separation"));
        vendor_frame_unhealthy = true;
      }
      if (ui_tags_ready && unsafe_ui_hint_logged) {
        unsafe_ui_hint_logged = false;
        logger::info("Reduced-resolution HUD-less/UI separation is safe again; "
                     "the optional tags are being submitted once more");
      }
      if (ui_tags_ready && ui_separation_unavailable_logged) {
        ui_separation_unavailable_logged = false;
        logger::info("HUD-less/UI separation restored; frame generation may "
                     "resume");
      }

      if (ui_tags_ready || withhold_unsafe_hint || !ui_phase_was_open) {
        streamline::FrameGeneration::instance().note_presentation_healthy();
      }
    }
  } else {

    auto &shared_resources = SharedResources::instance();
    const auto frame_generation_enabled =
        config::Settings::instance().frame_generation_enabled();
    const auto native_ui_policy = make_native_ui_policy(
        frame_generation_enabled, false, shared_resources.ui_rendering());
    if (native_ui_policy.service_at_present) {
      auto &dynamic_resolution = DynamicResolution::instance();
      const auto ui_phase_was_open =
          dynamic_resolution.full_resolution_ui_active();
      const auto ui_captured_directly = shared_resources.ui_rendering();

      shared_resources.end_ui_rendering();
      dynamic_resolution.end_full_resolution_ui();

      const auto inputs_available =
          shared_resources.ui_recomposition_available() &&
          shared_resources.frame_generation_hudless_captured();
      auto ui_layer_ready = false;
      auto ui_submission_safe = false;
      if (ui_captured_directly) {

        ui_layer_ready = inputs_available &&
                         shared_back_buffer_d3d11 != nullptr &&
                         UiCompositePass::instance().composite_layer(
                             d3d11_device.Get(), d3d11_context.Get(),
                             shared_resources.ui_color_alpha_d3d11(),
                             shared_back_buffer_d3d11.Get());
        if (ui_layer_ready) {
          ScaleformBoundary::instance().note_first_composite();
          ui_submission_safe =
              UiCompositePass::instance().report_captured_ui_layer(
                  d3d11_device.Get(), d3d11_context.Get(),
                  shared_resources.ui_color_alpha_d3d11());
        }
      } else if (frame_generation_enabled) {

        ui_layer_ready = inputs_available &&
                         shared_back_buffer_d3d11 != nullptr &&
                         UiCompositePass::instance().extract_ui_layer(
                             d3d11_device.Get(), d3d11_context.Get(),
                             shared_resources.frame_generation_hudless_d3d11(),
                             shared_back_buffer_d3d11.Get(),
                             shared_resources.ui_color_alpha_d3d11());
        ui_submission_safe = ui_layer_ready;
      }

      const auto ui_tags_ready = ui_layer_ready && ui_submission_safe;
      const auto nvidia_generator_selected =
          !FsrFrameGeneration::selected() && !XessFrameGeneration::selected();
      if (ui_tags_ready) {
        shared_resources.mark_ui_captured();
        unsafe_ui_hint_logged = false;
        if (ui_captured_directly && !native_ui_direct_logged) {
          native_ui_direct_logged = true;
          logger::info("Exact native Scaleform HUD-less/UI separation active "
                       "at {}x{}; frame generation receives both optional "
                       "layers and the visible frame is composited once",
                       output_width, output_height);
        }
        if (ui_separation_unavailable_logged) {
          ui_separation_unavailable_logged = false;
          logger::info("Native-resolution HUD-less/UI separation restored");
        }
      } else if (ui_layer_ready && !ui_submission_safe &&
                 nvidia_generator_selected) {

        if (!unsafe_ui_hint_logged) {
          unsafe_ui_hint_logged = true;
          logger::warn("Direct native UI capture resembled a full scene at "
                       "{}x{}; unsafe optional HUD-less/UI tags were withheld "
                       "and DLSS-G remains active on automatic final colour",
                       output_width, output_height);
        }
        if (ui_separation_unavailable_logged) {
          ui_separation_unavailable_logged = false;
          logger::info("Completed-frame NVIDIA presentation restored");
        }
      } else if (ui_captured_directly || ui_phase_was_open) {

        if (!ui_separation_unavailable_logged) {
          ui_separation_unavailable_logged = true;
          logger::warn("Direct native UI capture opened but its exact "
                       "composite could not be completed at {}x{}; frame "
                       "generation is suspended for this frame",
                       output_width, output_height);
        }
        static_cast<void>(streamline::FrameGeneration::instance().suspend(
            "the native UI capture opened but its exact composite could not "
            "be completed"));
        vendor_frame_unhealthy = true;
      } else if (frame_generation_enabled &&
                 !ui_separation_unavailable_logged) {
        ui_separation_unavailable_logged = true;
        if (nvidia_generator_selected) {
          logger::warn(
              "No native Scaleform boundary was captured and no "
              "HUD-less/UI pair could be extracted at {}x{}: inputs "
              "{}available, HUD-less capture {}. DLSS-G will use "
              "Streamline's automatic final-color path; the completed "
              "frame is not tagged as HUD-less.",
              output_width, output_height, inputs_available ? "" : "un",
              shared_resources.frame_generation_hudless_captured() ? "present"
                                                                   : "missing");
        } else {
          logger::warn(
              "No native Scaleform boundary was captured and no "
              "HUD-less/UI pair could be extracted at {}x{}: inputs "
              "{}available, HUD-less capture {}. The selected vendor "
              "frame generator cannot install without it.",
              output_width, output_height, inputs_available ? "" : "un",
              shared_resources.frame_generation_hudless_captured() ? "present"
                                                                   : "missing");
        }
      }
    }
  }

  const auto scene_ui_capture_end = TimingClock::now();
  renderer_runtime.end_frame();
  const auto scene_ui_runtime_end = TimingClock::now();
  StatusOverlay::instance().draw(d3d11_device.Get(), d3d11_context.Get(),
                                 shared_back_buffer_d3d11.Get());

  {
    auto &shared_resources = SharedResources::instance();
    if (shared_resources.streamline_ui_recomposition_available()) {
      StatusOverlay::instance().draw_into_ui_layer(
          d3d11_context.Get(), shared_resources.ui_color_alpha_d3d11());
    }
  }

  const auto scene_ui_end = TimingClock::now();

  {
    const auto scene_ui_ms = milliseconds_between(frame_begin, scene_ui_end);
    static auto last_scene_ui_report = TimingClock::time_point{};
    const auto now = TimingClock::now();
    if (scene_ui_ms >= 8.0 &&
        (last_scene_ui_report == TimingClock::time_point{} ||
         now - last_scene_ui_report >= std::chrono::seconds(1))) {
      last_scene_ui_report = now;
      logger::warn(
          "Slow scene/UI stage: {:.2f}ms (capture={:.2f}, "
          "end_frame={:.2f}, overlay={:.2f})",
          scene_ui_ms, milliseconds_between(frame_begin, scene_ui_capture_end),
          milliseconds_between(scene_ui_capture_end, scene_ui_runtime_end),
          milliseconds_between(scene_ui_runtime_end, scene_ui_end));
    }
  }
  const auto index = swap_chain->GetCurrentBackBufferIndex();
  if (index >= swap_chain_buffers.size() || index >= allocators.size() ||
      index >= allocator_fence_values.size()) {
    if (!back_buffer_index_warning_logged) {
      back_buffer_index_warning_logged = true;
      logger::error(
          "The presenting swap chain reported back buffer index {} but this "
          "bridge tracks {} buffer(s). The frame was dropped rather than "
          "written out of bounds; the adopted chain's buffer count does not "
          "match the description this bridge was built from.",
          index, static_cast<unsigned>(swap_chain_buffers.size()));
    }
    return DXGI_ERROR_INVALID_CALL;
  }
  auto result = wait_for_allocator(index);
  if (FAILED(result)) {
    return fail_frame("wait_for_allocator", result);
  }
  const auto allocator_end = TimingClock::now();

  const auto ready_value = fence_value++;
  result = d3d11_context->Signal(d3d11_fence.Get(), ready_value);
  if (FAILED(result)) {
    return fail_frame("the D3D11 ready-fence signal", result);
  }
  d3d11_context->Flush();
  const auto interop_flush_end = TimingClock::now();
  result = command_queue->Wait(d3d12_fence.Get(), ready_value);
  if (FAILED(result)) {
    return fail_frame("the D3D12 queue wait on the ready fence", result);
  }

  if (command_list_recording) {
    const auto close_result = command_list->Close();
    command_list_recording = false;
    if (!command_list_recovery_logged) {
      command_list_recovery_logged = true;
      if (SUCCEEDED(close_result)) {
        logger::warn("The D3D12 command list was still recording from a frame "
                     "that was abandoned partway through. It has been closed "
                     "so presentation can recover, rather than failing every "
                     "frame from here on.");
      } else {
        logger::warn("The D3D12 command list was still recording from a frame "
                     "that was abandoned partway through, and closing it "
                     "failed with 0x{:08X}. The command list reset below is "
                     "the next chance to recover and will report if it fails "
                     "as well.",
                     static_cast<unsigned>(close_result));
      }
    }
  }

  result = allocators[index]->Reset();
  if (FAILED(result)) {
    return fail_frame("the command allocator reset", result);
  }
  result = command_list->Reset(allocators[index].Get(), nullptr);
  if (FAILED(result)) {
    return fail_frame("the command list reset", result);
  }
  command_list_recording = true;

  const std::array before_copy{
      transition(shared_back_buffer_d3d12.Get(), D3D12_RESOURCE_STATE_COMMON,
                 D3D12_RESOURCE_STATE_COPY_SOURCE),
      transition(swap_chain_buffers[index].Get(), D3D12_RESOURCE_STATE_PRESENT,
                 D3D12_RESOURCE_STATE_COPY_DEST)};
  command_list->ResourceBarrier(static_cast<UINT>(before_copy.size()),
                                before_copy.data());
  command_list->CopyResource(swap_chain_buffers[index].Get(),
                             shared_back_buffer_d3d12.Get());
  const std::array after_copy{
      transition(shared_back_buffer_d3d12.Get(),
                 D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
      transition(swap_chain_buffers[index].Get(),
                 D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT)};
  command_list->ResourceBarrier(static_cast<UINT>(after_copy.size()),
                                after_copy.data());

  if (FsrFrameGeneration::selected()) {
    auto &generation = FsrFrameGeneration::instance();
    if (generation.owns_presentation()) {
      static_cast<void>(generation.submit_frame(
          command_list.Get(), render_width, render_height, output_width,
          output_height,
          static_cast<float>(frame_interval_timing.last_interval_ms), false));
    }
  }

  result = command_list->Close();
  if (FAILED(result)) {
    return fail_frame("the command list close", result);
  }
  command_list_recording = false;

  ID3D12CommandList *lists[]{command_list.Get()};
  const auto interop_record_end = TimingClock::now();
  streamline::FrameSubmission::instance().render_submit_start();
  command_queue->ExecuteCommandLists(1, lists);
  ScopeGuard completion_guard{[this, index]() {
    const auto orphan_value = fence_value++;
    if (SUCCEEDED(command_queue->Signal(d3d12_fence.Get(), orphan_value))) {
      allocator_fence_values[index] = orphan_value;
    }
  }};
  streamline::FrameSubmission::instance().render_submit_end();
  const auto interop_end = TimingClock::now();

  {
    const auto interop_ms = milliseconds_between(allocator_end, interop_end);
    static auto last_interop_report = TimingClock::time_point{};
    const auto now = TimingClock::now();
    if (interop_ms >= 8.0 &&
        (last_interop_report == TimingClock::time_point{} ||
         now - last_interop_report >= std::chrono::seconds(1))) {
      last_interop_report = now;
      logger::warn("Slow interop stage: {:.2f}ms (D3D11 flush={:.2f}, "
                   "record={:.2f}, ExecuteCommandLists={:.2f})",
                   interop_ms,
                   milliseconds_between(allocator_end, interop_flush_end),
                   milliseconds_between(interop_flush_end, interop_record_end),
                   milliseconds_between(interop_record_end, interop_end));
    }
  }

  DXGI_SWAP_CHAIN_DESC1 description{};
  swap_chain->GetDesc1(&description);
  const auto chain_permits_tearing =
      (description.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;
  const auto tearing_configured = config::Settings::instance().allow_tearing();

  const auto dynamic_cadence =
      streamline::FrameGeneration::instance().cadence() !=
      GenerationCadence::fixed_multiplier;
  const auto effective_interval = dynamic_cadence ? 0U : interval;
  const auto tearing_this_present = chain_permits_tearing &&
                                    effective_interval == 0 &&
                                    (tearing_configured || dynamic_cadence);
  if (tearing_this_present) {
    flags |= DXGI_PRESENT_ALLOW_TEARING;
  } else {
    flags &= ~DXGI_PRESENT_ALLOW_TEARING;
  }

  {
    static auto logged_tearing = false;
    static auto tearing_logged = false;
    if (!tearing_logged || logged_tearing != tearing_this_present) {
      tearing_logged = true;
      logged_tearing = tearing_this_present;
      logger::info(
          "Present tearing {}: [Display] AllowTearing={}, swap chain "
          "permits tearing={}, game sync interval={}, interval "
          "presented={}{}",
          tearing_this_present ? "ALLOWED"
                               : "NOT REQUESTED BY US, which is not the "
                                 "same as suppressed: the NVIDIA Control "
                                 "Panel can force V-Sync off and override "
                                 "this, in which case the flip does NOT "
                                 "wait for vertical blank and the frame "
                                 "CAN tear. Streamline reports the real "
                                 "state on its own line, search the log "
                                 "for shouldEnableVSync and RSYNC. A "
                                 "tear drifts slowly when the output rate "
                                 "is close to the display refresh, so at "
                                 "an output equal to refresh it can sit "
                                 "as a near stationary band",
          tearing_configured, chain_permits_tearing, interval,
          effective_interval,
          dynamic_cadence
              ? " (forced to 0: NVIDIA documents V-Sync as unsupported "
                "with Dynamic Multi Frame Generation, which paces itself. "
                "Enable G-SYNC/VRR for this to be tear-free, and leave "
                "V-Sync as \"Use the 3D application setting\" in the "
                "NVIDIA Control Panel or it overrides this)"
              : "");
    }
  }

  const auto configured_multiplier =
      config::Settings::instance().frame_generation_multiplier();
  const auto generated_frames =
      configured_multiplier > 1U ? configured_multiplier - 1U : 1U;

  const auto xess_selected = XessFrameGeneration::selected();
  const auto fsr_selected = FsrFrameGeneration::selected();
  const auto vendor_selected = xess_selected || fsr_selected;

  const auto plugin_menu_open = StatusOverlay::instance().menu_open();
  auto *const game_ui = vendor_selected ? RE::UI::GetSingleton() : nullptr;
  const auto game_paused = game_ui != nullptr && game_ui->GameIsPaused();
  const auto generation_setting_enabled =
      config::Settings::instance().frame_generation_enabled();
  const auto vendor_should_generate = generation_setting_enabled &&
                                      !plugin_menu_open && !game_paused &&
                                      !vendor_frame_unhealthy;
  const auto *const vendor_suspension_reason =
      !generation_setting_enabled
          ? "frame generation is turned off in the settings"
          : (plugin_menu_open
                 ? "the plugin menu is open"
                 : (game_paused
                        ? "Skyrim is paused"
                        : "this frame's HUD-less/UI separation was not "
                          "usable, so interpolating from it would smear"));
  const auto vendor_gate_changed =
      vendor_should_generate != vendor_generation_gate;
  if (vendor_gate_changed) {
    vendor_generation_gate = vendor_should_generate;
    if (vendor_selected && !vendor_should_generate) {
      logger::warn("Vendor frame generation suspended: {}",
                   vendor_suspension_reason);
    } else if (vendor_selected) {
      logger::info("Vendor frame generation resumed; interpolation history "
                   "was reset");
    }
  }
  {
    static std::uint64_t vendor_suspended_frames = 0ULL;
    if (vendor_selected && !vendor_should_generate &&
        generation_setting_enabled) {
      ++vendor_suspended_frames;
      const auto expected = plugin_menu_open || game_paused;
      if (expected) {
        if (vendor_suspended_frames == 6000ULL ||
            vendor_suspended_frames % 60000ULL == 0ULL) {
          logger::info("Vendor frame generation has been held for {} "
                       "consecutive frames because {}. This is normal and it "
                       "resumes on the first complete gameplay frame",
                       vendor_suspended_frames, vendor_suspension_reason);
        }
      } else if (vendor_suspended_frames == 60ULL ||
                 vendor_suspended_frames == 600ULL ||
                 vendor_suspended_frames == 6000ULL ||
                 vendor_suspended_frames % 60000ULL == 0ULL) {
        logger::warn("Vendor frame generation has been suspended for {} "
                     "consecutive frames: {}",
                     vendor_suspended_frames, vendor_suspension_reason);
      }
    } else {
      vendor_suspended_frames = 0ULL;
    }
  }
  {
    static std::uint64_t vendor_uninstalled_frames = 0ULL;
    const auto vendor_installed =
        xess_selected
            ? XessFrameGeneration::instance().state() ==
                  XessGenerationState::installed
            : (fsr_selected ? FsrFrameGeneration::instance().state() ==
                                  FsrGenerationState::installed
                            : true);
    if (vendor_selected && vendor_should_generate && !vendor_installed) {
      ++vendor_uninstalled_frames;
      if (vendor_uninstalled_frames == 600ULL ||
          vendor_uninstalled_frames % 60000ULL == 0ULL) {
        logger::warn("The selected vendor frame generator has not installed "
                     "after {} frames in which it should have been "
                     "generating; the reason it refused is on an earlier line",
                     vendor_uninstalled_frames);
      }
    } else {
      vendor_uninstalled_frames = 0ULL;
    }
  }
  if (xess_selected) {
    auto &generation = XessFrameGeneration::instance();
    static_cast<void>(generation.install_if_ready(generated_frames));
    generation.set_output_target_fps(
        config::Settings::instance().frame_limit());
    if (vendor_gate_changed) {
      generation.set_enabled(vendor_should_generate);
      if (vendor_should_generate) {
        generation.reset_history();
      }
    }
    if (generation.owns_presentation()) {
      static_cast<void>(generation.submit_frame(
          render_width, render_height, output_width, output_height,
          static_cast<float>(frame_interval_timing.last_interval_ms), false));
    }
  } else if (fsr_selected) {

    static_cast<void>(
        FsrFrameGeneration::instance().install_if_ready(generated_frames));

    if (vendor_gate_changed) {
      FsrFrameGeneration::instance().set_enabled(vendor_should_generate);
      if (vendor_should_generate) {
        FsrFrameGeneration::instance().reset_history();
      }
    }
  }

  XessFrameGeneration::instance().add_latency_marker(
      XessFrameGeneration::LatencyMarker::render_submit_end);
  XessFrameGeneration::instance().add_latency_marker(
      XessFrameGeneration::LatencyMarker::present_start);
  video_memory_census.sample();
  sample_motion_vector_census();
  streamline::FrameSubmission::instance().present_start();
  result = swap_chain->Present(effective_interval, flags);
  streamline::FrameSubmission::instance().present_end();
  XessFrameGeneration::instance().add_latency_marker(
      XessFrameGeneration::LatencyMarker::present_end);
  const auto present_end = TimingClock::now();
  if (FAILED(result)) {
    return fail_frame("the swap chain Present", result);
  }
  if (result != S_OK) {
    static auto present_status_logged = false;
    if (!present_status_logged) {
      present_status_logged = true;
      logger::info(
          "The swap chain returned success status 0x{:08X} from Present, "
          "which this proxy reports to Skyrim as S_OK. 0x087A0001 is "
          "DXGI_STATUS_OCCLUDED, meaning the window is hidden and DXGI "
          "expects the caller to throttle. Reported once, so this line "
          "means it happened at least one frame, not that it is happening "
          "now",
          static_cast<unsigned>(result));
    }
  }
  {
    constexpr DWORD kOccludedThrottleMilliseconds{16};
    static auto was_occluded = false;
    const auto occluded = result == DXGI_STATUS_OCCLUDED;
    if (occluded) {
      Sleep(kOccludedThrottleMilliseconds);
    } else if (was_occluded) {
      BaseFrameLimiter::instance().reset();
      logger::info(
          "The window is visible again after DXGI_STATUS_OCCLUDED. The base "
          "frame limiter deadline was re-primed, because frames presented "
          "while the window was hidden do not return from Present at the "
          "display cadence and their timing must not carry into the first "
          "visible frames");
    }
    was_occluded = occluded;
  }
  streamline::FrameGeneration::instance().after_present();

  const auto complete_value = fence_value++;
  result = command_queue->Signal(d3d12_fence.Get(), complete_value);
  if (FAILED(result)) {
    return fail_frame("the D3D12 completion signal", result);
  }
  result = d3d11_context->Wait(d3d11_fence.Get(), complete_value);
  if (FAILED(result)) {
    return fail_frame("the D3D11 wait on the completion fence", result);
  }
  allocator_fence_values[index] = complete_value;
  completion_guard.release();
  if (config::Settings::instance().d3d12_debug_layer()) {
    static auto last_drain = TimingClock::time_point{};
    const auto drain_now = TimingClock::now();
    if (last_drain == TimingClock::time_point{} ||
        drain_now - last_drain >= std::chrono::seconds(1)) {
      last_drain = drain_now;
      D3D12Backend::instance().drain_debug_messages();
    }
  }
  const auto completion_end = TimingClock::now();
  present_stage_timing.record(
      milliseconds_between(frame_begin, scene_ui_end),
      milliseconds_between(scene_ui_end, allocator_end),
      milliseconds_between(allocator_end, interop_end),
      milliseconds_between(interop_end, present_end),
      milliseconds_between(present_end, completion_end));
  if (renderer_runtime.uses(RendererAdapter::vanilla)) {

    renderer_runtime.begin_frame();
  }

  if (!first_present_logged) {
    first_present_logged = true;
    logger::info("First frame presented through the {} D3D12 path: "
                 "logical-render={}x{}, physical-output={}x{}",
                 D3D12Backend::instance().streamline_proxy_active()
                     ? "Streamline"
                     : "native",
                 render_width, render_height, output_width, output_height);
  }
  return S_OK;
}

PresentationBridge &PresentationBridge::instance() noexcept {
  static PresentationBridge bridge;
  return bridge;
}

bool PresentationBridge::create(ID3D11Device *d3d11_device,
                                const DXGI_SWAP_CHAIN_DESC &requested) {
  if (ready()) {
    return true;
  }
  if (d3d11_device == nullptr || requested.OutputWindow == nullptr ||
      !D3D12Backend::instance().initialize(d3d11_device)) {
    return false;
  }

  auto state = std::make_unique<State>();
  auto result =
      d3d11_device->QueryInterface(IID_PPV_ARGS(&state->d3d11_device));
  if (FAILED(result)) {
    return false;
  }

  ComPtr<ID3D11DeviceContext> context;
  d3d11_device->GetImmediateContext(&context);
  result = context.As(&state->d3d11_context);
  if (FAILED(result)) {
    return false;
  }

  state->d3d12_device =
      static_cast<ID3D12Device *>(D3D12Backend::instance().native_device());
  state->command_queue = static_cast<ID3D12CommandQueue *>(
      D3D12Backend::instance().command_queue());
  result =
      static_cast<IDXGIFactory7 *>(D3D12Backend::instance().proxy_factory())
          ->QueryInterface(IID_PPV_ARGS(&state->factory));
  if (FAILED(result)) {
    return false;
  }

  DXGI_SWAP_CHAIN_DESC1 description{};
  description.Width = requested.BufferDesc.Width;
  description.Height = requested.BufferDesc.Height;
  description.Format = requested.BufferDesc.Format;
  description.SampleDesc = {1, 0};
  description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  const auto ordinary_buffer_count = (std::max)(requested.BufferCount, 3U);
  const auto configured_multiplier =
      config::Settings::instance().frame_generation_multiplier();
  const auto override_buffer_count =
      config::Settings::instance().back_buffer_count();
  const auto experimental_buffer_count =
      config::Settings::instance().experimental_features() &&
              multiplier_is_supported(configured_multiplier) ?
          compute_back_buffer_count(configured_multiplier) :
          ordinary_buffer_count;
  description.BufferCount = override_buffer_count != 0U ?
      (std::max)(ordinary_buffer_count, override_buffer_count) :
      (std::max)(ordinary_buffer_count, experimental_buffer_count);
  if (override_buffer_count != 0U) {
    logger::info(
        "[Display] BackBufferCount={} set the swap chain to {} back buffers "
        "instead of the ordinary {}. This key exists because Streamline raises "
        "its own frame latency target to the multiplier while our buffer count "
        "is fixed at creation: the 6x measurement read SetMaximumFrameLatency "
        "changed from 1 to 6 against buffers=3, where 2x asked for only 2 and "
        "fitted. If a present blocks waiting for a free buffer that mismatch "
        "is the reason, and raising this to multiplier plus one is the test. "
        "It is deliberately SEPARATE from the experimental gate so it can be "
        "measured without also enabling sampler mip bias",
        override_buffer_count,
        description.BufferCount,
        ordinary_buffer_count);
  } else if (description.BufferCount != ordinary_buffer_count) {
    logger::info(
        "Experimental presentation: swap chain created with {} back buffers "
        "instead of the ordinary {}, because a fixed {}x needs {} frames "
        "queued plus the one being written. This is the step 7 requirement "
        "recorded from the 13:36 log, where UFGU reported 3 buffers to DLSS-G "
        "and Streamline's own cloneFakeBuffers immediately made 6. It is "
        "behind the experimental gate, so the Candidate build keeps the "
        "untouched count",
        description.BufferCount,
        ordinary_buffer_count,
        configured_multiplier,
        configured_multiplier);
  }
  description.Scaling = DXGI_SCALING_STRETCH;
  description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  description.Flags = requested.Flags;

  state->output_width = description.Width;
  state->output_height = description.Height;
  state->render_width = description.Width;
  state->render_height = description.Height;

  const auto &settings = config::Settings::instance();
  if (!settings.loaded()) {
    logger::error(
        "Presentation startup refused because the configuration was not "
        "loaded before swap-chain creation");
    return false;
  }
  const auto configured_mode = settings.upscaling_mode();
  const auto complete_frame_mode =
      uses_complete_frame_presentation(configured_mode);
  auto &super_resolution = streamline::SuperResolution::instance();
  super_resolution.set_color_input(
      complete_frame_mode ? streamline::DlssColorInput::display_ldr
                          : streamline::DlssColorInput::linear_hdr);
  if (complete_frame_mode) {
    if (!super_resolution.initialize(
            state->d3d11_device.Get(), state->d3d11_context.Get(),
            state->output_width, state->output_height) ||
        !super_resolution.set_mode(configured_mode)) {
      logger::error("Complete-frame presentation bridge could not initialize "
                    "the configured super-resolution mode");
      return false;
    }
    state->render_width = super_resolution.render_width();
    state->render_height = super_resolution.render_height();
    state->virtual_render_surface = true;
    const auto reduced_extent =
        state->render_width < state->output_width ||
        state->render_height < state->output_height;
    state->sub_rect_surface =
        reduced_extent &&
        settings.surface_model() == config::SurfaceModel::sub_rect;
    if (!valid_complete_frame_extent(
            configured_mode, state->render_width, state->render_height,
            state->output_width, state->output_height)) {
      logger::error("Complete-frame presentation startup contract is invalid: "
                    "logical "
                    "{}x{}, physical {}x{}",
                    state->render_width, state->render_height,
                    state->output_width, state->output_height);
      return false;
    }
  }

  logger::info(
      "Presentation startup contract verified before proxy publication: "
       "provider={}, mode={}, logical={}x{}, physical={}x{}, "
       "complete-frame={}, reduced={}",
       providers::vendor_display_name(settings.upscaling_provider()),
       static_cast<std::uint32_t>(configured_mode), state->render_width,
       state->render_height, state->output_width, state->output_height,
       state->virtual_render_surface,
       state->render_width < state->output_width ||
           state->render_height < state->output_height);

  BOOL allow_tearing{};
  ComPtr<IDXGIFactory5> factory5;
  if (SUCCEEDED(state->factory.As(&factory5)) &&
      SUCCEEDED(factory5->CheckFeatureSupport(
          DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing,
          sizeof(allow_tearing))) &&
      allow_tearing != FALSE) {
    description.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
  }

  ComPtr<IDXGISwapChain1> swap_chain;
  result = state->factory->CreateSwapChainForHwnd(
      state->command_queue.Get(), requested.OutputWindow, &description, nullptr,
      nullptr, &swap_chain);
  if (FAILED(result)) {
    logger::error("Presentation CreateSwapChainForHwnd failed: 0x{:08X}",
                  static_cast<unsigned>(result));
    return false;
  }
  result = swap_chain.As(&state->swap_chain);
  if (FAILED(result)) {
    return false;
  }

  state->presented_description = description;
  result = state->create_frame_resources(description);
  if (FAILED(result)) {
    logger::error("Presentation resource creation failed: 0x{:08X}",
                  static_cast<unsigned>(result));
    return false;
  }

  result = state->d3d12_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
                                            IID_PPV_ARGS(&state->d3d12_fence));
  if (FAILED(result)) {
    return false;
  }
  result = state->d3d12_device->CreateSharedHandle(
      state->d3d12_fence.Get(), nullptr, GENERIC_ALL, nullptr,
      &state->shared_fence_handle);
  if (FAILED(result)) {
    return false;
  }
  result = state->d3d11_device->OpenSharedFence(
      state->shared_fence_handle, IID_PPV_ARGS(&state->d3d11_fence));
  if (FAILED(result)) {
    return false;
  }
  state->completion_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (state->completion_event == nullptr) {
    return false;
  }

  state->requested_description = requested;
  state->requested_description.BufferCount = description.BufferCount;
  state->requested_description.BufferDesc.Width = description.Width;
  state->requested_description.BufferDesc.Height = description.Height;
  state->requested_description.BufferDesc.Format = description.Format;
  state->requested_description.SwapEffect = description.SwapEffect;
  state->requested_description.Flags = description.Flags;

  if (state->virtual_render_surface && !state->sub_rect_surface) {
    state->requested_description.BufferDesc.Width = state->render_width;
    state->requested_description.BufferDesc.Height = state->render_height;
  }
  state->proxy = new SwapChainProxy(*this);

  logger::info(
      "D3D11-compatible {} presentation bridge ready: {}x{}, format={}, "
      "buffers={}",
      D3D12Backend::instance().streamline_proxy_active() ? "Streamline"
                                                         : "native D3D12",
      description.Width, description.Height,
      static_cast<unsigned>(description.Format), description.BufferCount);
  if (state->sub_rect_surface) {
    logger::info(
        "SUB-RECT upscaling surface active: Skyrim is told {}x{} and "
        "allocates full-size targets; the world renders into the top-left "
        "{}x{} sub-rect through its own dynamic-resolution ratio, and the "
        "upscaler reads that rectangle",
        state->output_width, state->output_height, state->render_width,
        state->render_height);
  } else if (state->virtual_render_surface) {
    logger::info("Complete-frame upscaling surface active: game/ENB={}x{}, "
                 "physical-output={}x{}",
                 state->render_width, state->render_height, state->output_width,
                 state->output_height);
  }
  state_ = std::move(state);
  publish_display_refresh();
  if (!enb::Api::instance().connected()) {
    auto &runtime = RendererRuntime::instance();
    if (!runtime.initialize(
            RendererAdapter::vanilla, state_->d3d11_device.Get(),
            state_->d3d11_context.Get(), state_->swap_chain.Get(),
            description.Width, description.Height)) {
      logger::error("Vanilla renderer adapter initialization failed; "
                    "the native presentation bridge remains active");
    } else {

      runtime.begin_frame();
      logger::info("Vanilla renderer opened its initial Streamline frame");
    }
  }
  return true;
}

void PresentationBridge::publish_display_refresh() noexcept {

  std::uint32_t refresh_hz = 0;
  if (state_ != nullptr && state_->swap_chain != nullptr) {
    ComPtr<IDXGIOutput> output;
    if (SUCCEEDED(state_->swap_chain->GetContainingOutput(&output)) &&
        output != nullptr) {
      DXGI_SWAP_CHAIN_DESC1 description{};
      if (SUCCEEDED(state_->swap_chain->GetDesc1(&description))) {
        DXGI_MODE_DESC wanted{};
        wanted.Width = description.Width;
        wanted.Height = description.Height;
        wanted.Format = description.Format;
        DXGI_MODE_DESC closest{};
        if (SUCCEEDED(
                output->FindClosestMatchingMode(&wanted, &closest, nullptr)) &&
            closest.RefreshRate.Denominator != 0) {
          refresh_hz =
              static_cast<std::uint32_t>((closest.RefreshRate.Numerator +
                                          closest.RefreshRate.Denominator / 2) /
                                         closest.RefreshRate.Denominator);
        }
      }
    }
  }
  if (refresh_hz == 0) {
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) != 0) {
      refresh_hz = static_cast<std::uint32_t>(mode.dmDisplayFrequency);
    }
  }

  if (refresh_hz <= 1) {
    refresh_hz = 0;
    logger::warn(
        "The display refresh rate could not be determined; the output FPS "
        "target will be paced without a VRR headroom allowance");
  }
  streamline::FrameGeneration::instance().note_display_refresh(refresh_hz);
}

void PresentationBridge::shutdown() noexcept {

  XessFrameGeneration::instance().shutdown();
  FsrFrameGeneration::instance().shutdown();
  DebugViewPass::instance().shutdown();
  StatusOverlay::instance().shutdown();
  SurfaceBlit::instance().shutdown();
  state_.reset();
  logger::info(
      "Presentation teardown complete. This plugin's hooks stay installed for "
      "the life of the process, which is normal, and from here they fall "
      "through to Skyrim's own functions because the presentation state they "
      "test has been released. If this line is absent at the end of a log, "
      "teardown did not run: it is reached from the ENB exit callback and "
      "from nowhere else, so a session without ENB never produces it");
}

bool PresentationBridge::ready() const noexcept { return state_ != nullptr; }

IDXGISwapChain *PresentationBridge::proxy() const noexcept {
  return state_ != nullptr ? state_->proxy : nullptr;
}

void *PresentationBridge::presentation_swap_chain() const noexcept {
  return state_ != nullptr ? static_cast<void *>(state_->swap_chain.Get())
                           : nullptr;
}

void *PresentationBridge::begin_presentation_handoff() {
  if (state_ == nullptr || state_->swap_chain == nullptr) {
    return nullptr;
  }
  if (state_->generated_swap_chain != nullptr) {
    logger::error(
        "A frame generator already owns presentation; a second handoff "
        "was refused");
    return nullptr;
  }

  HWND window{};
  auto result = state_->swap_chain->GetHwnd(&window);
  if (FAILED(result) || window == nullptr) {
    logger::error(
        "Presentation handoff refused: the swap chain would not report its "
        "window: 0x{:08X}",
        static_cast<unsigned>(result));
    return nullptr;
  }

  result = state_->wait_for_gpu_idle();
  if (FAILED(result)) {
    logger::error("Presentation handoff refused: the GPU could not be idled: "
                  "0x{:08X}",
                  static_cast<unsigned>(result));
    return nullptr;
  }

  state_->handoff_window = window;
  state_->swap_chain_buffers.clear();

  logger::info(
      "Releasing the presentation swap chain for a vendor frame generator: "
      "{} back buffer(s) dropped and this bridge's reference transferred, so "
      "DXGI can destroy it and the generator can create its own on the same "
      "window",
      state_->presented_description.BufferCount);

  return state_->swap_chain.Detach();
}

HRESULT PresentationBridge::State::recreate_own_swap_chain() {
  if (handoff_window == nullptr || factory == nullptr) {
    return E_FAIL;
  }
  auto description = presented_description;
  ComPtr<IDXGISwapChain1> replacement;
  auto result = factory->CreateSwapChainForHwnd(command_queue.Get(),
                                                handoff_window, &description,
                                                nullptr, nullptr, &replacement);
  if (FAILED(result)) {
    return result;
  }
  result = replacement.As(&swap_chain);
  if (FAILED(result)) {
    swap_chain.Reset();
    return result;
  }
  result = create_frame_resources(presented_description);
  if (FAILED(result)) {
    release_frame_resources();
    swap_chain.Reset();
    return result;
  }
  handoff_window = nullptr;
  return S_OK;
}

bool PresentationBridge::abort_presentation_handoff() {
  if (state_ == nullptr || state_->handoff_window == nullptr) {
    return false;
  }
  if (state_->generated_swap_chain != nullptr) {
    release_presentation_swap_chain();
    return state_->swap_chain != nullptr;
  }
  if (state_->swap_chain != nullptr) {
    state_->handoff_window = nullptr;
    return true;
  }

  const auto result = state_->recreate_own_swap_chain();
  if (FAILED(result)) {
    logger::error(
        "The vendor frame generator refused the swap chain AND a "
        "replacement could not be created: 0x{:08X}. This session cannot "
        "present.",
        static_cast<unsigned>(result));
    return false;
  }

  logger::warn(
      "The vendor frame generator refused the swap chain, so presentation "
      "was rebuilt on the same window and continues without it. "
      "SwapChainProxy's identity is unchanged; Skyrim and ENB observe "
      "nothing.");
  return true;
}

bool PresentationBridge::adopt_presentation_swap_chain(void *const chain) {

  if (state_ == nullptr || chain == nullptr) {
    return false;
  }
  if (state_->generated_swap_chain != nullptr) {
    logger::error(
        "A frame generator already owns presentation; a second adoption "
        "was refused");
    return false;
  }
  ComPtr<IDXGISwapChain4> adopted;
  const auto result = static_cast<IDXGISwapChain *>(chain)->QueryInterface(
      IID_PPV_ARGS(&adopted));
  if (FAILED(result) || adopted == nullptr) {
    logger::error("The frame generator's swap chain does not expose "
                  "IDXGISwapChain4: 0x{:08X}",
                  static_cast<unsigned>(result));
    return false;
  }

  if (FAILED(state_->wait_for_gpu_idle())) {
    logger::error("Could not idle the GPU before adopting the generator's swap "
                  "chain; adoption refused");
    return false;
  }
  state_->swap_chain = adopted;
  state_->generated_swap_chain = adopted;

  auto adopted_description = state_->presented_description;
  DXGI_SWAP_CHAIN_DESC1 queried_description{};
  if (SUCCEEDED(adopted->GetDesc1(&queried_description))) {
    adopted_description = queried_description;
    logger::info(
        "The frame generator's swap chain reports {}x{}, format={}, {} "
        "buffer(s), flags=0x{:08X}; this bridge was built from {}x{}, "
        "format={}, {} buffer(s)",
        queried_description.Width, queried_description.Height,
        static_cast<unsigned>(queried_description.Format),
        queried_description.BufferCount,
        static_cast<unsigned>(queried_description.Flags),
        state_->presented_description.Width,
        state_->presented_description.Height,
        static_cast<unsigned>(state_->presented_description.Format),
        state_->presented_description.BufferCount);
  } else {
    logger::warn("The frame generator's swap chain would not report its "
                 "description; this bridge's own description was assumed");
  }

  const auto reacquired =
      state_->create_frame_resources(adopted_description);
  if (FAILED(reacquired)) {
    state_->release_frame_resources();
    state_->swap_chain.Reset();
    state_->generated_swap_chain.Reset();
    const auto rebuilt = state_->recreate_own_swap_chain();
    logger::error(
        "Could not acquire back buffers from the generator's swap chain: "
        "0x{:08X}; presentation rebuilt on the same window: {}",
        static_cast<unsigned>(reacquired),
        SUCCEEDED(rebuilt) ? "yes" : "no");
    return false;
  }
  logger::info("Presentation adopted a frame generator's swap chain; "
               "SwapChainProxy identity, HWND and D3D12 queue are unchanged");
  return true;
}

void PresentationBridge::release_presentation_swap_chain() noexcept {
  if (state_ == nullptr || state_->generated_swap_chain == nullptr) {
    return;
  }
  static_cast<void>(state_->wait_for_gpu_idle());
  state_->release_frame_resources();
  state_->swap_chain.Reset();
  state_->generated_swap_chain.Reset();
  const auto rebuilt = state_->recreate_own_swap_chain();
  if (FAILED(rebuilt)) {
    logger::error(
        "Presentation could not be rebuilt on this plugin's own swap chain "
        "after the frame generator released it: 0x{:08X}. This session "
        "cannot present.",
        static_cast<unsigned>(rebuilt));
    return;
  }
  logger::info("Presentation returned to this plugin's own swap chain");
}

ID3D11Device *PresentationBridge::d3d11_device() const noexcept {
  return state_ != nullptr ? state_->d3d11_device.Get() : nullptr;
}

ID3D11DeviceContext *PresentationBridge::d3d11_context() const noexcept {
  return state_ != nullptr ? state_->d3d11_context.Get() : nullptr;
}

ID3D11Texture2D *PresentationBridge::d3d11_back_buffer() const noexcept {
  return state_ != nullptr ? state_->shared_back_buffer_d3d11.Get() : nullptr;
}

ID3D11RenderTargetView *
PresentationBridge::d3d11_back_buffer_view() const noexcept {
  return state_ != nullptr ? state_->shared_back_buffer_view.Get() : nullptr;
}

ID3D11Texture2D *PresentationBridge::d3d11_render_buffer() const noexcept {
  if (state_ == nullptr) {
    return nullptr;
  }
  return state_->virtual_render_surface
             ? state_->render_back_buffer_d3d11.Get()
             : state_->shared_back_buffer_d3d11.Get();
}

ID3D11RenderTargetView *
PresentationBridge::d3d11_render_target_view() const noexcept {
  return state_ != nullptr ? state_->render_back_buffer_view.Get() : nullptr;
}

ID3D11ShaderResourceView *
PresentationBridge::d3d11_render_resource_view() const noexcept {
  return state_ != nullptr ? state_->render_back_buffer_resource_view.Get()
                           : nullptr;
}

ID3D11RenderTargetView *PresentationBridge::full_resolution_ui_target(
    ID3D11RenderTargetView *requested) const noexcept {
  if (state_ == nullptr || !state_->virtual_render_surface ||
      requested == nullptr || state_->render_back_buffer_d3d11 == nullptr ||
      state_->shared_back_buffer_view == nullptr) {
    return requested;
  }

  ComPtr<ID3D11Resource> resource;
  requested->GetResource(&resource);
  return same_com_identity(resource.Get(),
                           state_->render_back_buffer_d3d11.Get())
             ? state_->shared_back_buffer_view.Get()
             : requested;
}

bool PresentationBridge::full_resolution_ui_source(
    ID3D11RenderTargetView *requested) const noexcept {
  if (state_ == nullptr || requested == nullptr) {
    return false;
  }
  auto *const expected_source = state_->virtual_render_surface
                                    ? state_->render_back_buffer_d3d11.Get()
                                    : state_->shared_back_buffer_d3d11.Get();
  if (expected_source == nullptr) {
    return false;
  }

  ComPtr<ID3D11Resource> resource;
  requested->GetResource(&resource);
  return same_com_identity(resource.Get(), expected_source);
}

bool PresentationBridge::full_resolution_ui_target_compatible(
    ID3D11RenderTargetView *requested) const noexcept {
  if (requested == nullptr) {
    return true;
  }
  if (state_ == nullptr) {
    return false;
  }

  ComPtr<ID3D11Resource> resource;
  requested->GetResource(&resource);
  ComPtr<ID3D11Texture2D> texture;
  if (resource == nullptr || FAILED(resource.As(&texture))) {
    return false;
  }
  D3D11_TEXTURE2D_DESC description{};
  texture->GetDesc(&description);
  return description.Width == state_->output_width &&
         description.Height == state_->output_height &&
         description.ArraySize == 1 && description.SampleDesc.Count == 1;
}

ID3D11DepthStencilView *PresentationBridge::full_resolution_ui_depth(
    ID3D11DepthStencilView *requested) {
  if (requested == nullptr || state_ == nullptr ||
      !state_->virtual_render_surface) {
    return requested;
  }
  for (const auto &target : state_->ui_depth_targets) {
    if (target.source.Get() == requested) {
      return target.view.Get();
    }
  }

  ComPtr<ID3D11Resource> resource;
  requested->GetResource(&resource);
  ComPtr<ID3D11Texture2D> source;
  D3D11_TEXTURE2D_DESC source_description{};
  D3D11_DEPTH_STENCIL_VIEW_DESC view_description{};
  if (resource == nullptr || FAILED(resource.As(&source))) {
    return nullptr;
  }
  source->GetDesc(&source_description);
  requested->GetDesc(&view_description);

  if (source_description.Width == state_->output_width &&
      source_description.Height == state_->output_height &&
      source_description.ArraySize == 1 &&
      source_description.SampleDesc.Count == 1) {
    return requested;
  }
  if (source_description.Width != state_->render_width ||
      source_description.Height != state_->render_height ||
      source_description.ArraySize != 1 ||
      source_description.SampleDesc.Count != 1 ||
      view_description.ViewDimension != D3D11_DSV_DIMENSION_TEXTURE2D) {
    if (!state_->ui_depth_shape_logged) {
      state_->ui_depth_shape_logged = true;
      logger::warn("Cannot create a full-resolution UI depth/stencil companion "
                   "for {}x{}, array={}, samples={}, dimension={}",
                   source_description.Width, source_description.Height,
                   source_description.ArraySize,
                   source_description.SampleDesc.Count,
                   static_cast<unsigned>(view_description.ViewDimension));
    }
    return nullptr;
  }

  auto physical_description = source_description;
  physical_description.Width = state_->output_width;
  physical_description.Height = state_->output_height;
  physical_description.MipLevels = 1;
  physical_description.ArraySize = 1;
  physical_description.SampleDesc = {1, 0};
  physical_description.Usage = D3D11_USAGE_DEFAULT;
  physical_description.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  physical_description.CPUAccessFlags = 0;
  physical_description.MiscFlags = 0;
  view_description.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
  view_description.Texture2D.MipSlice = 0;

  State::UiDepthTarget target;
  target.source = requested;
  auto result = state_->d3d11_device->CreateTexture2D(&physical_description,
                                                      nullptr, &target.texture);
  if (SUCCEEDED(result)) {
    result = state_->d3d11_device->CreateDepthStencilView(
        target.texture.Get(), &view_description, &target.view);
  }
  if (SUCCEEDED(result)) {
    if (view_description.Flags == 0) {
      target.clear_view = target.view;
    } else {
      auto clear_description = view_description;
      clear_description.Flags = 0;
      result = state_->d3d11_device->CreateDepthStencilView(
          target.texture.Get(), &clear_description, &target.clear_view);
    }
  }
  if (FAILED(result)) {
    if (!state_->ui_depth_creation_logged) {
      state_->ui_depth_creation_logged = true;
      logger::warn(
          "Full-resolution UI depth/stencil companion creation failed: "
          "0x{:08X}",
          static_cast<unsigned>(result));
    }
    return nullptr;
  }

  auto clear_flags = static_cast<UINT>(D3D11_CLEAR_DEPTH);
  if (view_description.Format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
      view_description.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) {
    clear_flags |= D3D11_CLEAR_STENCIL;
  }
  state_->d3d11_context->ClearDepthStencilView(target.clear_view.Get(),
                                               clear_flags, 1.0F, 0);
  state_->ui_depth_targets.push_back(std::move(target));
  logger::info(
      "Full-resolution UI depth/stencil companion ready: {}x{} -> {}x{}, "
      "format={}",
      source_description.Width, source_description.Height, state_->output_width,
      state_->output_height, static_cast<unsigned>(view_description.Format));
  return state_->ui_depth_targets.back().view.Get();
}

ID3D11DepthStencilView *
PresentationBridge::mapped_full_resolution_ui_clear_depth(
    ID3D11DepthStencilView *requested) const noexcept {
  if (requested == nullptr || state_ == nullptr) {
    return nullptr;
  }
  for (const auto &target : state_->ui_depth_targets) {
    if (target.source.Get() == requested) {
      return target.clear_view.Get();
    }
  }
  return nullptr;
}

bool PresentationBridge::uses_sub_rect_render_surface() const noexcept {
  return state_ != nullptr && state_->virtual_render_surface &&
         state_->sub_rect_surface;
}

bool PresentationBridge::uses_virtual_render_surface() const noexcept {
  return state_ != nullptr && state_->virtual_render_surface;
}

std::uint32_t PresentationBridge::render_width() const noexcept {
  return state_ != nullptr ? state_->render_width : 0;
}

std::uint32_t PresentationBridge::render_height() const noexcept {
  return state_ != nullptr ? state_->render_height : 0;
}

std::uint32_t PresentationBridge::output_width() const noexcept {
  return state_ != nullptr ? state_->output_width : 0;
}

std::uint32_t PresentationBridge::output_height() const noexcept {
  return state_ != nullptr ? state_->output_height : 0;
}

DXGI_FORMAT PresentationBridge::back_buffer_format() const noexcept {
  return state_ != nullptr ? state_->presented_description.Format
                           : DXGI_FORMAT_UNKNOWN;
}

bool PresentationBridge::evaluate_profile_change_preflight(
    const config::UpscalingMode requested_mode, const char *&reason,
    bool &user_facing) {

  ProfilePreflightInputs inputs{};
  inputs.bridge_ready = state_ != nullptr;
  inputs.output_width = state_ != nullptr ? state_->output_width : 0;
  inputs.output_height = state_ != nullptr ? state_->output_height : 0;
  inputs.renderer_available =
      RE::BSGraphics::Renderer::GetSingleton() != nullptr;
  std::uint32_t requested_width{};
  std::uint32_t requested_height{};
  inputs.extent_resolved =
      streamline::SuperResolution::instance().optimal_render_resolution(
          requested_mode, requested_width, requested_height);
  inputs.requested_width = requested_width;
  inputs.requested_height = requested_height;
  inputs.current_width = state_ != nullptr ? state_->render_width : 0;
  inputs.current_height = state_ != nullptr ? state_->render_height : 0;
  inputs.stable_proxy_identity_published =
      state_ != nullptr &&
      state_->proxy_buffer_published.load(std::memory_order_acquire);

  inputs.live_extent_reconfiguration_supported = false;
  inputs.current_complete_frame_route =
      state_ != nullptr && state_->virtual_render_surface;
  inputs.requested_complete_frame_route =
      uses_complete_frame_presentation(requested_mode);

  const auto decision = evaluate_profile_preflight(inputs);
  reason = describe(decision);
  user_facing = is_user_facing(decision);
  if (is_rejection(decision)) {
    logger::warn(
        "A live profile change to mode {} was refused during preflight: "
        "{}. Nothing was changed.",
        static_cast<unsigned>(requested_mode), reason);
    return false;
  }
  return true;
}

bool PresentationBridge::request_upscaling_reconfiguration(
    const config::UpscalingMode requested_mode) {

  const char *reason = "";
  bool user_facing = false;
  if (!evaluate_profile_change_preflight(requested_mode, reason, user_facing)) {
    return false;
  }

  const auto desired_mode = requested_mode;
  std::uint32_t requested_width{};
  std::uint32_t requested_height{};
  if (!streamline::SuperResolution::instance().optimal_render_resolution(
          desired_mode, requested_width, requested_height)) {
    return false;
  }

  if (requested_width != state_->render_width ||
      requested_height != state_->render_height) {
    logger::error(
        "Profile reconfiguration reached the mutation path despite an "
        "extent change ({}x{} -> {}x{}); refusing without touching the "
        "renderer",
        state_->render_width, state_->render_height, requested_width,
        requested_height);
    return false;
  }

  auto &super_resolution = streamline::SuperResolution::instance();
  const auto previous_mode = super_resolution.mode();
  if (!super_resolution.set_mode(desired_mode)) {
    if (previous_mode != desired_mode) {
      static_cast<void>(super_resolution.set_mode(previous_mode));
    }
    return false;
  }
  state_->upscaling_reconfiguration_pending = false;
  logger::info(
      "Applied same-extent DLSS profile reconfiguration without a window "
      "resize, swap-chain reset, proxy replacement or ENB reset: "
      "logical={}x{}, physical={}x{}",
      requested_width, requested_height, state_->output_width,
      state_->output_height);
  return true;
}

HRESULT SwapChainProxy::QueryInterface(REFIID interface_id, void **object) {
  if (object == nullptr) {
    return E_POINTER;
  }
  *object = nullptr;
  if (interface_id == __uuidof(IUnknown) ||
      interface_id == __uuidof(IDXGIObject) ||
      interface_id == __uuidof(IDXGIDeviceSubObject) ||
      interface_id == __uuidof(IDXGISwapChain)) {
    *object = static_cast<IDXGISwapChain *>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

ULONG SwapChainProxy::AddRef() { return ++references_; }

ULONG SwapChainProxy::Release() {
  const auto references = --references_;
  if (references == 0) {
    delete this;
  }
  return references;
}

HRESULT SwapChainProxy::SetPrivateData(REFGUID name, UINT size,
                                       const void *data) {
  return bridge_.state_->swap_chain->SetPrivateData(name, size, data);
}

HRESULT SwapChainProxy::SetPrivateDataInterface(REFGUID name,
                                                const IUnknown *object) {
  return bridge_.state_->swap_chain->SetPrivateDataInterface(name, object);
}

HRESULT SwapChainProxy::GetPrivateData(REFGUID name, UINT *size, void *data) {
  return bridge_.state_->swap_chain->GetPrivateData(name, size, data);
}

HRESULT SwapChainProxy::GetParent(REFIID interface_id, void **parent) {
  return bridge_.state_->swap_chain->GetParent(interface_id, parent);
}

HRESULT SwapChainProxy::GetDevice(REFIID interface_id, void **device) {
  if (interface_id == __uuidof(ID3D11Device) ||
      interface_id == __uuidof(ID3D11Device1) ||
      interface_id == __uuidof(ID3D11Device2) ||
      interface_id == __uuidof(ID3D11Device3) ||
      interface_id == __uuidof(ID3D11Device4) ||
      interface_id == __uuidof(ID3D11Device5)) {
    return bridge_.state_->d3d11_device->QueryInterface(interface_id, device);
  }
  return bridge_.state_->swap_chain->GetDevice(interface_id, device);
}

HRESULT SwapChainProxy::Present(const UINT interval, const UINT flags) {
  return bridge_.state_->present(interval, flags);
}

HRESULT SwapChainProxy::GetBuffer(const UINT index, REFIID interface_id,
                                  void **surface) {
  if (surface == nullptr) {
    return E_POINTER;
  }
  *surface = nullptr;
  if (index != 0) {
    return DXGI_ERROR_INVALID_CALL;
  }
  auto *buffer = bridge_.state_->virtual_render_surface
                     ? bridge_.state_->render_back_buffer_d3d11.Get()
                     : bridge_.state_->shared_back_buffer_d3d11.Get();
  if (buffer == nullptr) {
    return DXGI_ERROR_DEVICE_REMOVED;
  }
  const auto result = buffer->QueryInterface(interface_id, surface);
  if (FAILED(result)) {
    return result;
  }
  bridge_.state_->proxy_buffer_published.store(true, std::memory_order_release);
  if (!bridge_.state_->proxy_buffer_logged) {
    bridge_.state_->proxy_buffer_logged = true;
    D3D11_TEXTURE2D_DESC description{};
    buffer->GetDesc(&description);
    logger::info("Proxy GetBuffer exposed stable game/ENB surface {}x{}, "
                 "format={}, physical-output={}x{}",
                 description.Width, description.Height,
                 static_cast<unsigned>(description.Format),
                 bridge_.state_->output_width, bridge_.state_->output_height);
  }
  return result;
}

namespace {
[[nodiscard]] const char *suspend_active_vendor_generator() {
  if (XessFrameGeneration::selected() &&
      XessFrameGeneration::instance().owns_presentation()) {
    XessFrameGeneration::instance().set_enabled(false);
    vendor_generation_gate = false;
    return "XeSS-FG";
  }
  if (FsrFrameGeneration::selected() &&
      FsrFrameGeneration::instance().owns_presentation()) {
    FsrFrameGeneration::instance().set_enabled(false);
    vendor_generation_gate = false;
    return "FidelityFX frame generation";
  }
  return nullptr;
}

void resume_active_vendor_generator(const char *const suspended) {
  if (suspended == nullptr) {
    return;
  }
  if (XessFrameGeneration::selected()) {
    XessFrameGeneration::instance().reset_history();
    XessFrameGeneration::instance().set_enabled(true);
  } else if (FsrFrameGeneration::selected()) {
    FsrFrameGeneration::instance().reset_history();
    FsrFrameGeneration::instance().set_enabled(true);
  }
  vendor_generation_gate = true;
}
}

HRESULT SwapChainProxy::SetFullscreenState(const BOOL fullscreen,
                                           IDXGIOutput *target) {
  if (!streamline::FrameGeneration::instance().suspend_for_reset(
          "the swap chain is changing fullscreen state")) {
    return E_FAIL;
  }
  const auto *const suspended = suspend_active_vendor_generator();
  const auto idle_result = bridge_.state_->wait_for_gpu_idle();
  if (FAILED(idle_result)) {
    logger::error("GPU idle wait before SetFullscreenState failed: 0x{:08X}",
                  static_cast<unsigned>(idle_result));
    resume_active_vendor_generator(suspended);
    return idle_result;
  }
  const auto result =
      bridge_.state_->swap_chain->SetFullscreenState(fullscreen, target);
  if (SUCCEEDED(result)) {
    bridge_.state_->requested_description.Windowed = fullscreen == FALSE;
    logger::info("Swap-chain fullscreen state changed to {} with {} suspended",
                 fullscreen != FALSE,
                 suspended != nullptr ? suspended : "DLSS-G");
  }
  resume_active_vendor_generator(suspended);
  return result;
}

HRESULT SwapChainProxy::GetFullscreenState(BOOL *fullscreen,
                                           IDXGIOutput **target) {
  return bridge_.state_->swap_chain->GetFullscreenState(fullscreen, target);
}

HRESULT SwapChainProxy::GetDesc(DXGI_SWAP_CHAIN_DESC *description) {
  if (description == nullptr) {
    return E_POINTER;
  }
  *description = bridge_.state_->requested_description;
  if (!bridge_.state_->proxy_description_logged) {
    bridge_.state_->proxy_description_logged = true;
    logger::info("Proxy GetDesc exposed logical {}x{} while retaining physical "
                 "{}x{}",
                 description->BufferDesc.Width, description->BufferDesc.Height,
                 bridge_.state_->output_width, bridge_.state_->output_height);
  }
  return S_OK;
}

HRESULT SwapChainProxy::ResizeBuffers(const UINT count, const UINT width,
                                      const UINT height,
                                      const DXGI_FORMAT format,
                                      const UINT flags) {
  return bridge_.state_->resize_buffers(count, width, height, format, flags);
}

HRESULT SwapChainProxy::ResizeTarget(const DXGI_MODE_DESC *target) {

  if (target == nullptr) {
    return E_INVALIDARG;
  }

  if (bridge_.state_->virtual_render_surface ||
      bridge_.state_->upscaling_reconfiguration_pending) {
    bridge_.state_->requested_description.BufferDesc = *target;
    logger::info("Logical swap-chain target accepted at {}x{} without changing "
                 "the physical {}x{} display target",
                 target->Width, target->Height, bridge_.state_->output_width,
                 bridge_.state_->output_height);
    return S_OK;
  }

  if (!streamline::FrameGeneration::instance().suspend_for_reset(
          "the swap chain display target is being resized")) {
    return E_FAIL;
  }
  const auto *const suspended = suspend_active_vendor_generator();
  const auto idle_result = bridge_.state_->wait_for_gpu_idle();
  if (FAILED(idle_result)) {
    logger::error("GPU idle wait before ResizeTarget failed: 0x{:08X}",
                  static_cast<unsigned>(idle_result));
    resume_active_vendor_generator(suspended);
    return idle_result;
  }
  auto physical_target = *target;
  if (bridge_.state_->virtual_render_surface) {
    physical_target.Width = bridge_.state_->output_width;
    physical_target.Height = bridge_.state_->output_height;
  }
  const auto result =
      bridge_.state_->swap_chain->ResizeTarget(&physical_target);
  if (SUCCEEDED(result)) {
    bridge_.state_->requested_description.BufferDesc = *target;
    logger::info("Swap-chain target changed to {}x{} with {} suspended",
                 target->Width, target->Height,
                 suspended != nullptr ? suspended : "DLSS-G");
  }
  resume_active_vendor_generator(suspended);
  return result;
}

HRESULT SwapChainProxy::GetContainingOutput(IDXGIOutput **output) {
  return bridge_.state_->swap_chain->GetContainingOutput(output);
}

HRESULT SwapChainProxy::GetFrameStatistics(DXGI_FRAME_STATISTICS *statistics) {
  return bridge_.state_->swap_chain->GetFrameStatistics(statistics);
}

HRESULT SwapChainProxy::GetLastPresentCount(UINT *count) {
  return bridge_.state_->swap_chain->GetLastPresentCount(count);
}

namespace {
HRESULT STDMETHODCALLTYPE factory_create_hook(IDXGIFactory *factory,
                                              IUnknown *device,
                                              DXGI_SWAP_CHAIN_DESC *description,
                                              IDXGISwapChain **swap_chain) {
  if (description == nullptr || swap_chain == nullptr) {
    return E_INVALIDARG;
  }

  ComPtr<ID3D11Device> d3d11_device;
  if (device != nullptr &&
      SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d3d11_device))) &&
      PresentationBridge::instance().create(d3d11_device.Get(), *description)) {
    *swap_chain = PresentationBridge::instance().proxy();
    (*swap_chain)->AddRef();
    return S_OK;
  }

  logger::warn("Using Skyrim's native D3D11 swap chain");
  return original_factory_create(factory, device, description, swap_chain);
}

HRESULT WINAPI d3d11_create_hook(
    IDXGIAdapter *adapter, D3D_DRIVER_TYPE driver_type, HMODULE software,
    UINT flags, const D3D_FEATURE_LEVEL *feature_levels,
    UINT feature_level_count, UINT sdk_version,
    const DXGI_SWAP_CHAIN_DESC *swap_chain_description,
    IDXGISwapChain **swap_chain, ID3D11Device **device,
    D3D_FEATURE_LEVEL *feature_level, ID3D11DeviceContext **immediate_context) {
  if (adapter != nullptr && !factory_hook_installed.exchange(true)) {
    ComPtr<IDXGIFactory> factory;
    if (SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
      const auto vtable_address =
          *reinterpret_cast<std::uintptr_t *>(factory.Get());
      REL::Relocation<std::uintptr_t> vtable{vtable_address};
      original_factory_create = reinterpret_cast<FactoryCreateFunction>(
          vtable.write_vfunc(10, factory_create_hook));
    }
  }

  return original_d3d11_create(adapter, driver_type, software, flags,
                               feature_levels, feature_level_count, sdk_version,
                               swap_chain_description, swap_chain, device,
                               feature_level, immediate_context);
}
}

bool install_presentation_bootstrap() {
  if (original_d3d11_create != nullptr) {
    return true;
  }
  original_d3d11_create = reinterpret_cast<D3D11CreateFunction>(SKSE::PatchIAT(
      d3d11_create_hook, "d3d11.dll", "D3D11CreateDeviceAndSwapChain"));
  if (original_d3d11_create == nullptr) {
    logger::error("Unable to install the D3D11 presentation bootstrap");
    return false;
  }
  logger::info("D3D11 presentation bootstrap installed");
  return true;
}
}
