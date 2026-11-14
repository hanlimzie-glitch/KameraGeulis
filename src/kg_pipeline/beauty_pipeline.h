#pragma once
#include <memory>
#include <string>
#include <vector>
#include "kgcore/frame.h"
#include "kgcore/landmark.h"
#include "kgcore/skin_mask.h"
#include "face_tracker.h"
#include "seg_provider.h"

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

  // M2h: mask kulit wajah (hanya dihitung bila aktif - hemat CPU saat mati).
  // M2j: menyalakan/mematikan mode kulit MENGIKUT-SETELAHU parameter beauty
  // (jalur kulit = sharpen/white gpupixel netral; lihat SetBeautyStrength).
  void SetSkinMaskEnabled(bool on) {
    skin_mask_enabled_ = on;
    if (beauty_filter_) SetBeautyStrength(smoothing_, whitening_);
  }
  bool SkinMaskEnabled() const { return skin_mask_enabled_; }
  const SkinMaskBuilder& MaskBuilder() const { return mask_builder_; }
  // M2j: segmentasi semua kulit (thread terpisah; lihat seg_provider.h).
  SegProvider& Seg() { return seg_; }
  // M2j: cakupan mask (true = Wajah saja, false = Semua kulit/default).
  void SetSkinCoverage(bool face_only) {
    mask_builder_.SetCoverage(face_only ? SkinCoverage::kFaceOnly
                                        : SkinCoverage::kAllSkin);
  }
  float SegMs() const { return seg_ms_; }
  // Lag frame: serial_frame_sekarang - serial_frame_hasil_seg (-1 bila belum).
  float SegLagFrames() const { return seg_lag_frames_; }
  bool SegActive() const { return seg_result_ok_; }
  uint64_t FrameSerial() const { return frame_serial_; }  // M2j: lag F5
  // RGBA penuh frame input terakhir (setara OutputRGBA - untuk komposit M2h).
  const uint8_t* InputRGBA() const { return convert_buf_.empty() ? nullptr : convert_buf_.data(); }

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
  // M2f: guard readback - hanya setelah pipeline benar-benar menghasilkan
  // output (sink pernah Render); sebelum itu GetRgbaBuffer crash (0xC0000005).
  uint64_t frames_produced_ = 0;
  bool readback_blocked_logged_ = false;
  // M2h: mask kulit + timingnya.
  SkinMaskBuilder mask_builder_;
  bool skin_mask_enabled_ = false;
  float mask_ms_ = 0.0f;
  // M2j: seg + serial frame (sinkronitas mask/seg dgn frame).
  SegProvider seg_;
  float seg_ms_ = 0.0f;
  float seg_lag_frames_ = -1.0f;
  bool seg_result_ok_ = false;
  bool seg_start_logged_ = false;
  bool seg_fail_logged_ = false;
  uint64_t frame_serial_ = 0;
  float smoothing_ = 0.0f;   // dipakai jalur kulit (guided radius)
  float whitening_ = 0.0f;
};
}  // namespace kgcore
