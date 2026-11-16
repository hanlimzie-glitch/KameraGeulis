#pragma once
// M2l — kebijakan pelacakan ROI, deteksi periodik adaptif, uji kewajaran
// landmark (jarak mata, rotasi vs ROI, bentuk mesh), IoU kotak tracking vs
// deteksi, dan renderer overlay landmark ber-indeks untuk dump F5.
// Murni logika tanpa GL/OS (unit-tested penuh di Linux).
#include <cstdint>
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

// Ambang minimum conv2d_31 (face flag logit) sebelum dianggap hilang/reset.
inline constexpr float kFaceFlagMinThreshold = 0.5f;
// Ambang minimum IoU kotak tracking vs deteksi sebelum ROI direset.
inline constexpr float kTrackingResetIouThreshold = 0.60f;
// Rentang interval detektor periodik adaptif (10-15 frame).
inline constexpr int kDetectIntervalMin = 10;
inline constexpr int kDetectIntervalMax = 15;
inline constexpr int kDetectIntervalDefault = 12;

inline bool IsFaceFlagValid(float face_flag) {
  return face_flag >= kFaceFlagMinThreshold;
}

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
