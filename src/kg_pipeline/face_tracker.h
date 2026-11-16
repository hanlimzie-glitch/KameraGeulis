#pragma once
#include <array>
#include <memory>
#include <string>
#include <vector>
#include "kgcore/frame.h"
#include "kgcore/landmark.h"
#include "kgcore/one_euro.h"
#include "kgcore/roi_align.h"

namespace kgcore {
// Per-stage timings for the debug overlay (docs/ARCHITECTURE.md §5.1).
// detect_ms: NaN/0 when the detector did not run this frame (track-only).
struct StageTimings {
  float detect_ms = 0.0f;
  float mesh_ms = 0.0f;
  float track_ms = 0.0f;  // whole provider Track() call (FaceTracker fills it)
  float mask_ms = 0.0f;   // M2h: pembuatan mask kulit (0 bila checkbox mati)
  int frames_since_detect = 0;
  int reset_count = 0;    // M2l: jumlah ROI reset sejak mulai
  float resets_per_min = 0.0f;  // M2m: laju reset per menit
  std::string last_reset_reason;
};

// Abstraction over face-landmark providers (see docs/ARCHITECTURE.md §5).
// Implementations: NullLandmarkProvider (model missing / unavailable),
// OrtLandmarkProvider (M1b — ONNX Runtime CPU, 468 pts).
class ILandmarkProvider {
 public:
  virtual ~ILandmarkProvider() = default;
  virtual bool Available() const = 0;
  virtual const char* Name() const = 0;
  // Returns a LandmarkSet; valid==false when no face or model unavailable.
  virtual LandmarkSet Track(const Frame& frame) = 0;
  // Fills `out` with per-stage timings; returns false if unsupported.
  virtual bool GetStageTimings(StageTimings* out) const {
    (void)out;
    return false;
  }
  // M2l: ambil potongan ROI 192x192 RGBA terakhir yang dikirim ke FaceMesh
  // (untuk dump F5 roi_*.png).
  virtual bool GetDebugRoi(std::vector<uint8_t>* rgba_out, int* w, int* h,
                           AlignedRoi* roi_out) const {
    (void)rgba_out;
    (void)w;
    (void)h;
    (void)roi_out;
    return false;
  }
};

class NullLandmarkProvider : public ILandmarkProvider {
 public:
  bool Available() const override { return false; }
  const char* Name() const override { return "none"; }
  LandmarkSet Track(const Frame&) override { return {}; }
};

// Wraps a provider with OneEuro smoothing + fade gain. Thread-confined to the
// render thread (no internal locking).
class FaceTracker {
 public:
  // Parameter OneEuro untuk koordinat ternormalisasi [0,1]: beta lebih besar
  // (~12.0) agar responsif saat kepala bergerak cepat tanpa mengorbankan
  // peredaman getar saat diam.
  static constexpr float kDefaultMinCutoff = 2.0f;
  static constexpr float kDefaultBeta = 25.0f;
  static constexpr float kDefaultDCutoff = 1.0f;

  explicit FaceTracker(std::unique_ptr<ILandmarkProvider> provider);

  void Track(const Frame& frame);
  const LandmarkSet& Current() const { return current_; }
  const LandmarkSet& CurrentRaw() const { return current_raw_; }
  float Gain() const { return fade_.Gain(); }  // smooth [0,1] for effect fade
  bool ProviderAvailable() const { return provider_->Available(); }
  const char* ProviderName() const { return provider_->Name(); }
  const ILandmarkProvider* Provider() const { return provider_.get(); }
  float LastTrackMs() const { return last_track_ms_; }
  float LastFilterLagMeanPx() const { return last_filter_lag_mean_px_; }
  float LastFilterLagP95Px() const { return last_filter_lag_p95_px_; }

 private:
  std::unique_ptr<ILandmarkProvider> provider_;
  std::array<OneEuroVec2, kLandmarkCount> smoothers_;
  LandmarkFade fade_{300.0f};
  LandmarkSet current_;
  LandmarkSet current_raw_;
  bool was_valid_ = false;
  float last_track_ms_ = 0.0f;
  float last_filter_lag_mean_px_ = 0.0f;
  float last_filter_lag_p95_px_ = 0.0f;
};
}  // namespace kgcore
