#pragma once
#include <array>
#include <memory>
#include <string>
#include <vector>
#include "kgcore/camera_orientation.h"
#include "kgcore/frame.h"
#include "kgcore/landmark.h"
#include "kgcore/landmark_tracking.h"
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
  int reset_count = 0;         // M2l: jumlah ROI reset sejak mulai
  int resets_in_last_min = 0;  // M2p: reset dalam jendela bergulir 60 detik
  float resets_per_min = 0.0f; // M2m/M2p: laju reset per menit
  int hold_frames = 0;         // M2p: jumlah frame hold-last-good aktif
  int total_hold_frames = 0;   // M2p: total frame hold-last-good sesi ini
  float last_fade_out_ms = 0.0f;
  float last_fade_in_ms = 0.0f;
  TrackingPhase tracking_phase = TrackingPhase::kLost;
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
  // M2p: uji detektor cepat pada 4 rotasi (0/90/180/270 CW) dari frame mentah.
  virtual bool ProbeOrientations(const Frame& raw_frame, OrientationScores* out) {
    (void)raw_frame;
    (void)out;
    return false;
  }
  // M2p: bersihkan state pelacakan saat rotasi atau kamera berubah.
  virtual void ResetTracking() {}
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

// Wraps a provider with OneEuro smoothing + hold-last-good + fade gain.
// Thread-confined to the render thread (no internal locking).
class FaceTracker {
 public:
  // Parameter OneEuro untuk koordinat ternormalisasi [0,1]: beta lebih besar
  // (~25.0) agar responsif saat kepala bergerak cepat tanpa mengorbankan
  // peredaman getar saat diam.
  static constexpr float kDefaultMinCutoff = 2.0f;
  static constexpr float kDefaultBeta = 25.0f;
  static constexpr float kDefaultDCutoff = 1.0f;

  explicit FaceTracker(std::unique_ptr<ILandmarkProvider> provider);

  void Track(const Frame& frame);
  void Reset();
  bool ProbeOrientations(const Frame& raw_frame, OrientationScores* out) {
    return provider_ && provider_->ProbeOrientations(raw_frame, out);
  }

  const LandmarkSet& Current() const { return current_; }
  const LandmarkSet& CurrentRaw() const { return current_raw_; }
  // M2p: landmark yang dipakai untuk meraster mask fitur — tetap membekukan
  // wajah terakhir selama hold-last-good maupun fade-out agar eksklusi
  // mata/alis/bibir/hidung tidak mendadak hilang.
  const LandmarkSet& MaskLandmarks() const;
  float Gain() const { return state_machine_.Gain(); }  // smooth [0,1] for effect fade
  bool IsHolding() const { return last_step_.holding; }
  bool IsMaskFrozen() const { return last_step_.mask_frozen; }
  TrackingPhase Phase() const { return state_machine_.Phase(); }
  int HoldFrames() const { return state_machine_.HoldFrames(); }
  int TotalHoldFrames() const { return state_machine_.TotalHoldFrames(); }
  float LastFadeOutMs() const { return state_machine_.LastFadeOutMs(); }
  float LastFadeInMs() const { return state_machine_.LastFadeInMs(); }
  bool ProviderAvailable() const { return provider_->Available(); }
  const char* ProviderName() const { return provider_->Name(); }
  const ILandmarkProvider* Provider() const { return provider_.get(); }
  float LastTrackMs() const { return last_track_ms_; }
  float LastFilterLagMeanPx() const { return last_filter_lag_mean_px_; }
  float LastFilterLagP95Px() const { return last_filter_lag_p95_px_; }

 private:
  std::unique_ptr<ILandmarkProvider> provider_;
  std::array<OneEuroVec2, kLandmarkCount> smoothers_;
  TrackingStateMachine state_machine_;
  TrackingStepResult last_step_{};
  LandmarkSet current_;
  LandmarkSet current_raw_;
  float last_track_ms_ = 0.0f;
  float last_filter_lag_mean_px_ = 0.0f;
  float last_filter_lag_p95_px_ = 0.0f;
};
}  // namespace kgcore
