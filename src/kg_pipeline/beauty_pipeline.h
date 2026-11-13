#pragma once
#include <memory>
#include <string>
#include <vector>
#include "kgcore/frame.h"
#include "kgcore/landmark.h"
#include "face_tracker.h"

namespace gpupixel {
class SourceRawData;
class BeautyFaceFilter;
class FaceReshapeFilter;
class SinkRawData;
}  // namespace gpupixel

namespace kgcore {
// GPUPixel chain: SourceRawData -> BeautyFaceFilter -> SinkRawData.
// M1 wires the beauty stage; FaceReshapeFilter/lipstick/blusher are created in
// M2+. All landmark-driven filters are gated by FaceTracker gain (0 when no
// provider or no face), so they can be attached now without changing behavior.
// NOT thread-safe; owned by the render thread (GL context current).
class BeautyPipeline {
 public:
  BeautyPipeline();
  ~BeautyPipeline();
  BeautyPipeline(const BeautyPipeline&) = delete;
  BeautyPipeline& operator=(const BeautyPipeline&) = delete;

  // Must be called with a current GL context.
  bool Init(const std::string& resource_path);
  void Shutdown();

  // Pushes one captured frame through the chain; result readable via
  // OutputRGBA() (valid until the next ProcessFrame call).
  // M2e: BGRA frames are converted to RGBA (alpha forced 255) BEFORE the
  // gpupixel upload - gpupixel's BGRA glTexImage2D path is a no-op on
  // Windows (guarded to iOS/macOS), which produced a black preview.
  void ProcessFrame(const Frame& frame);

  // Live-apply the two global beauty parameters ([0,1] each); idempotent.
  void SetBeautyStrength(float smoothing, float whitening);

  // Human-readable reason when Init() returns false ("resource res/…" /
  // "BeautyFaceFilter::Create gagal …"). Empty on success.
  const std::string& LastResourceError() const { return last_resource_error_; }

  // M2e: returns a TOP-DOWN RGBA copy of the sink readback with alpha
  // forced to 255 (owned by the pipeline; valid until the next call).
  // Was: direct pointer into gpupixel's glReadPixels buffer.
  const uint8_t* OutputRGBA();
  int OutputWidth() const;
  int OutputHeight() const;

  // Debug overlay (M2): per-stage timings (capture dipisah, diukur app).
  // Isi detect/mesh dari provider (0 bila detector tidak jalan),
  // track_ms dari FaceTracker. false hanya bila pipeline belum Init.
  bool GetStageTimings(StageTimings* out) const;

  gpupixel::BeautyFaceFilter* BeautyFilter() const { return beauty_filter_.get(); }
  FaceTracker& Tracker() { return *tracker_; }

 private:
  std::string last_resource_error_;
  std::unique_ptr<FaceTracker> tracker_;
  std::shared_ptr<gpupixel::SourceRawData> source_;
  std::shared_ptr<gpupixel::BeautyFaceFilter> beauty_filter_;
  std::shared_ptr<gpupixel::SinkRawData> sink_;
  bool initialized_ = false;
  // M2e conversion/output buffers (owned here so OutputRGBA stays valid
  // until the next ProcessFrame/OutputRGBA call).
  std::vector<uint8_t> convert_buf_;  // pre-upload BGRA->RGBA staging
  std::vector<uint8_t> out_buf_;      // post-readback RGBA (alpha forced)
};
}  // namespace kgcore
