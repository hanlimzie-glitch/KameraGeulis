#pragma once
#include <memory>
#include <string>
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
  void ProcessFrame(const Frame& frame);

  // Live-apply the two global beauty parameters ([0,1] each); idempotent.
  void SetBeautyStrength(float smoothing, float whitening);

  const uint8_t* OutputRGBA() const;
  int OutputWidth() const;
  int OutputHeight() const;

  // Debug overlay (M2): per-stage timings (capture dipisah, diukur app).
  // Isi detect/mesh dari provider (0 bila detector tidak jalan),
  // track_ms dari FaceTracker. false hanya bila pipeline belum Init.
  bool GetStageTimings(StageTimings* out) const;

  gpupixel::BeautyFaceFilter* BeautyFilter() const { return beauty_filter_.get(); }
  FaceTracker& Tracker() { return *tracker_; }

 private:
  std::unique_ptr<FaceTracker> tracker_;
  std::shared_ptr<gpupixel::SourceRawData> source_;
  std::shared_ptr<gpupixel::BeautyFaceFilter> beauty_filter_;
  std::shared_ptr<gpupixel::SinkRawData> sink_;
  bool initialized_ = false;
};
}  // namespace kgcore
