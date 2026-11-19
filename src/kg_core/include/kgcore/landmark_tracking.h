#pragma once
// M2l — kebijakan pelacakan ROI, deteksi periodik adaptif, uji kewajaran
// landmark (jarak mata, rotasi vs ROI, bentuk mesh), IoU kotak tracking vs
// deteksi, dan renderer overlay landmark ber-indeks untuk dump F5.
// Murni logika tanpa GL/OS (unit-tested penuh di Linux).
#include <cstdint>
#include <deque>
#include <string>
#include <vector>
#include "kgcore/landmark.h"
#include "kgcore/roi_align.h"

namespace kgcore {

struct LandmarkSanityResult {
  bool ok = false;
  std::string reason;  // alasan singkat saat gagal (untuk log reset)
  float eye_dist_px = 0.0f;
  float eye_ratio = 0.0f;      // eye_dist_px / roi.side_px
  float angle_diff_rad = 0.0f; // |angle_landmark - roi.angle_rad|
  float mesh_w_canvas = 0.0f;
  float mesh_h_canvas = 0.0f;
  float mesh_aspect = 0.0f;
  float center_drift = 0.0f;   // jarak pusat mesh ke (0.5, 0.5) di canvas ROI
};

// Ambang probabilitas pasca-sigmoid conv2d_31 (face flag) sebelum dianggap
// hilang/reset. Model mengembalikan logit mentah; 0.50 probabilitas == logit 0.
inline constexpr float kFaceFlagMinThreshold = 0.50f;
// Ambang minimum IoU kotak tracking vs deteksi sebelum ROI direset.
inline constexpr float kTrackingResetIouThreshold = 0.60f;
// Rentang interval detektor periodik adaptif (10-15 frame).
inline constexpr int kDetectIntervalMin = 10;
inline constexpr int kDetectIntervalMax = 15;
inline constexpr int kDetectIntervalDefault = 12;

// Konversi logit conv2d_31 -> probabilitas [0, 1] via sigmoid.
float FaceFlagToProb(float face_flag_logit);
inline bool IsFaceFlagValid(float face_flag_logit) {
  return FaceFlagToProb(face_flag_logit) >= kFaceFlagMinThreshold;
}

// ---------------------------------------------------------------------------
// M2p — Anti-thrash ResetGuard (histeresis N-frame + cooldown >= 10 frame +
// rate limiter jendela bergulir 60 detik). Murni C++17 (diuji Linux).
// ---------------------------------------------------------------------------
struct ResetGuardConfig {
  int fail_frames_before_reset = 4;  // N frame berturut-turut di bawah ambang (3-5)
  int low_iou_before_reset = 2;      // deteksi IoU rendah berturut-turut sebelum reset
  int cooldown_frames = 12;          // jeda pasca-reset (>= 10 frame)
  int max_resets_per_min = 15;       // batas laju reset dalam jendela 60 detik
  int64_t rate_window_ms = 60000;    // jendela bergulir 60 detik
};

class ResetGuard {
 public:
  explicit ResetGuard(ResetGuardConfig cfg = {});

  void BeginFrame(int64_t now_ms);
  void OnMeshResult(bool mesh_valid);
  bool ShouldTriggerMeshReset(int64_t now_ms, bool* rate_limited = nullptr);
  bool ShouldTriggerIouReset(bool low_iou, bool mesh_healthy, int64_t now_ms,
                             bool* rate_limited = nullptr);
  void RecordReset(int64_t now_ms);
  void ClearStreaks();
  void ResetAll();

  int ResetsInLastMinute(int64_t now_ms) const;
  int TotalResets() const { return total_resets_; }
  int CooldownRemaining() const { return cooldown_remaining_; }
  int BadMeshStreak() const { return bad_mesh_streak_; }
  int LowIouStreak() const { return low_iou_streak_; }

 private:
  void PruneWindow_(int64_t now_ms) const;
  bool IsRateLimited_(int64_t now_ms) const;

  ResetGuardConfig cfg_;
  int bad_mesh_streak_ = 0;
  int low_iou_streak_ = 0;
  int cooldown_remaining_ = 0;
  int total_resets_ = 0;
  mutable std::deque<int64_t> reset_timestamps_ms_;
};

// ---------------------------------------------------------------------------
// M2p — State machine pelacakan: hold-last-good (~300 ms), pembekuan mask
// fitur saat tidak stabil / fade-out, dan fade halus keluar/masuk.
// ---------------------------------------------------------------------------
enum class TrackingPhase { kLost, kRecovering, kTracking, kHolding, kFadingOut };

const char* TrackingPhaseName(TrackingPhase phase);

struct TrackingStateConfig {
  float hold_ms = 300.0f;  // tahan landmark baik terakhir selama ~300 ms
  float fade_ms = 300.0f;  // durasi fade efek kecantikan keluar/masuk
};

struct TrackingStepResult {
  TrackingPhase prev_phase = TrackingPhase::kLost;
  TrackingPhase phase = TrackingPhase::kLost;
  bool phase_changed = false;
  bool use_live_landmarks = false;
  bool holding = false;
  bool mask_frozen = false;  // true di kHolding dan kFadingOut selama gain > 0
  float gain = 0.0f;
  int hold_frames = 0;
  float fade_duration_ms = 0.0f;
  std::string transition_log;
};

class TrackingStateMachine {
 public:
  explicit TrackingStateMachine(TrackingStateConfig cfg = {});

  TrackingStepResult Update(bool raw_valid, const LandmarkSet* raw_lm,
                            const LandmarkSet* smoothed_lm, int64_t now_ms);
  void Reset();

  TrackingPhase Phase() const { return phase_; }
  float Gain() const { return fade_.Gain(); }
  int HoldFrames() const { return hold_frames_; }
  int TotalHoldFrames() const { return total_hold_frames_; }
  float LastFadeOutMs() const { return last_fade_out_ms_; }
  float LastFadeInMs() const { return last_fade_in_ms_; }
  bool HasLastGood() const { return has_last_good_; }
  const LandmarkSet& LastGoodRaw() const { return last_good_raw_; }
  const LandmarkSet& LastGoodSmoothed() const { return last_good_smoothed_; }

 private:
  TrackingStateConfig cfg_;
  TrackingPhase phase_ = TrackingPhase::kLost;
  LandmarkFade fade_;
  bool has_last_good_ = false;
  LandmarkSet last_good_raw_{};
  LandmarkSet last_good_smoothed_{};
  int64_t last_good_ts_ms_ = 0;
  int hold_frames_ = 0;
  int total_hold_frames_ = 0;
  int64_t hold_start_ts_ms_ = 0;
  int64_t fade_start_ts_ms_ = 0;
  bool fade_out_active_ = false;
  bool fade_in_active_ = false;
  float last_fade_out_ms_ = 0.0f;
  float last_fade_in_ms_ = 0.0f;
};

// Bungkus selisih sudut ke [-pi, pi].
float WrapAnglePi(float rad);

// IoU dua kotak ROI persegi (cx, cy, side) dalam ruang piksel frame.
float RoiBoxIoU(const AlignedRoi& a, const AlignedRoi& b);

// IoU kotak sumbu-sejajar landmark (468 titik) vs kotak deteksi (keduanya
// ternormalisasi [0,1]).
float LandmarkBoxVsDetIoU(const LandmarkSet& lm, float det_cx, float det_cy,
                          float det_w, float det_h);

// Hitung ROI frame berikutnya dari 468 landmark frame sebelumnya (tanpa
// offset statis yang memicu drift saat menunduk/miring). `ref_side_px` (>0)
// membatasi perubahan skala liar di antara deteksi periodik.
AlignedRoi TrackRoiFromLandmarks(const LandmarkSet& prev_lm, int frame_w,
                                 int frame_h, float ref_side_px = 0.0f);

// Interval detektor periodik adaptif [10..15] frame berdasar kecepatan gerak
// ROI dan keyakinan face_flag.
int AdaptiveDetectInterval(float roi_shift_frac, float angle_delta_rad,
                           float face_flag);

// Uji kewajaran landmark hasil FaceMesh terhadap ROI yang dipakai:
//  1) jarak mata relatif terhadap ROI + urutan kiri/kanan,
//  2) rotasi sumbu mata vs sudut ROI,
//  3) bentuk mesh (ukuran, rasio aspek, pusat di ROI, urutan vertikal dahi/mata/mulut/dagu).
LandmarkSanityResult CheckLandmarkSanity(const LandmarkSet& lm,
                                         const AlignedRoi& roi, int frame_w,
                                         int frame_h);

// Gambar overlay debug pada salinan frame RGBA (untuk dump F5 landmarks_*.png):
// kotak ROI hijau, bbox landmark cyan, 468 titik kuning, dan indeks kunci
// ber-angka (3x5 bitmap font) agar mudah diperiksa offline.
bool RenderLandmarkDebugOverlayRgba(const uint8_t* raw_rgba, int w, int h,
                                    const LandmarkSet& lm,
                                    const AlignedRoi* roi,
                                    std::vector<uint8_t>* out_rgba);

}  // namespace kgcore
