#pragma once

#include "render/RenderDebug.hpp"

#include <memory>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace mfgdlss::render {
class UiCompositePass final {
public:
  UiCompositePass();
  ~UiCompositePass();

  UiCompositePass(const UiCompositePass &) = delete;
  UiCompositePass &operator=(const UiCompositePass &) = delete;

  [[nodiscard]] static UiCompositePass &instance() noexcept;
  [[nodiscard]] bool
  apply_delta(ID3D11Device *device, ID3D11DeviceContext *context,
              ID3D11Texture2D *scene_before_ui, ID3D11Texture2D *scene_with_ui,
              ID3D11Texture2D *target, ID3D11Texture2D *ui_color_alpha);
  [[nodiscard]] bool composite_layer(ID3D11Device *device,
                                     ID3D11DeviceContext *context,
                                     ID3D11Texture2D *ui_color_alpha,
                                     ID3D11Texture2D *target);
  [[nodiscard]] bool extract_ui_layer(ID3D11Device *device,
                                      ID3D11DeviceContext *context,
                                      ID3D11Texture2D *hudless_color,
                                      ID3D11Texture2D *final_color,
                                      ID3D11Texture2D *ui_color_alpha);

  [[nodiscard]] bool report_captured_ui_layer(ID3D11Device *device,
                                              ID3D11DeviceContext *context,
                                              ID3D11Texture2D *ui_color_alpha);
  void reset_capture_reports() noexcept;
  void shutdown() noexcept;

private:
  struct State;
  struct LayerState;
  struct ExtractionState;
  struct CaptureReportState;
  std::unique_ptr<State> state_;
  std::unique_ptr<LayerState> layer_state_;
  std::unique_ptr<ExtractionState> extraction_state_;
  std::unique_ptr<CaptureReportState> capture_report_state_;
  UiCompositeMode logged_mode_{UiCompositeMode::coverage};
  bool first_success_logged_{};
  bool first_layer_success_logged_{};
  bool first_extraction_logged_{};
  bool delta_failure_logged_{};
  bool composite_failure_logged_{};
  bool extraction_failure_logged_{};
  bool extraction_contract_logged_{};
  bool capture_readback_failure_logged_{};
  bool ui_layer_dumped_{};
};
}
