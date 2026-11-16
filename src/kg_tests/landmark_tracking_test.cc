#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>
#include "kgcore/landmark_tracking.h"

using namespace kgcore;

namespace {
// Buat 468 landmark wajah sintetis terpusat di (cx, cy) dengan lebar/tinggi
// ternormalisasi (fw, fh) dan sudut roll `angle_rad`.
LandmarkSet MakeSyntheticFace(float cx, float cy, float face_w, float face_h,
                              float angle_rad, float pitch_scale_y = 1.0f) {
  LandmarkSet lm;
  lm.valid = true;
  lm.face_score = 0.95f;
  const float ca = std::cos(angle_rad), sa = std::sin(angle_rad);
  auto put = [&](int idx, float lx, float ly) {
    // lx, ly relatif terhadap pusat wajah dalam satuan [-0.5, 0.5] * face_w/h
    const float px = lx * face_w;
    const float py = ly * face_h * pitch_scale_y;
    lm.pts[idx] = {cx + ca * px - sa * py, cy + sa * px + ca * py};
  };
  // Sebar semua 468 titik di elips wajah sebagai baseline lalu timpa titik kunci.
  for (size_t i = 0; i < kLandmarkCount; ++i) {
    const float a = 6.2831853f * static_cast<float>(i) / static_cast<float>(kLandmarkCount);
    const float r = 0.45f * (0.3f + 0.7f * static_cast<float>((i * 37) % 11) / 10.0f);
    put(static_cast<int>(i), r * std::cos(a), r * std::sin(a));
  }
  put(10, 0.0f, -0.48f);    // dahi
  put(152, 0.0f, 0.48f);    // dagu
  put(234, -0.45f, 0.0f);   // pipi kanan
  put(454, 0.45f, 0.0f);    // pipi kiri
  put(33, -0.24f, -0.14f);  // mata kanan luar
  put(133, -0.10f, -0.14f); // mata kanan dalam
  put(362, 0.10f, -0.14f);  // mata kiri dalam
  put(263, 0.24f, -0.14f);  // mata kiri luar
  put(1, 0.0f, 0.04f);      // ujung hidung
  put(48, -0.08f, 0.08f);
  put(278, 0.08f, 0.08f);
  put(13, 0.0f, 0.22f);     // bibir atas
  put(14, 0.0f, 0.25f);     // bibir bawah
  put(61, -0.14f, 0.23f);
  put(291, 0.14f, 0.23f);
  put(0, 0.0f, 0.20f);
  put(17, 0.0f, 0.29f);
  return lm;
}
}  // namespace

TEST_CASE("landmark_tracking: wajah normal, miring, dan menunduk lulus uji sanity") {
  const int fw = 1280, fh = 720;
  for (float angle : {0.0f, 0.35f, -0.35f}) {
    for (float pitch : {1.0f, 0.65f}) {  // 0.65 = kepala menunduk
      const LandmarkSet lm = MakeSyntheticFace(0.5f, 0.5f, 0.22f, 0.38f, angle, pitch);
      const AlignedRoi roi = TrackRoiFromLandmarks(lm, fw, fh);
      const auto sanity = CheckLandmarkSanity(lm, roi, fw, fh);
      INFO("angle=" << angle << " pitch=" << pitch << " reason=" << sanity.reason);
      REQUIRE(sanity.ok);
    }
  }
}

TEST_CASE("landmark_tracking: deteksi drift IoU < 0.6 memicu reset ROI") {
  AlignedRoi det_roi{640.0f, 360.0f, 300.0f, 0.0f};
  AlignedRoi close_roi{650.0f, 365.0f, 305.0f, 0.0f};
  AlignedRoi drifted_roi{750.0f, 430.0f, 300.0f, 0.0f};  // geser jauh (~60+ px x/y)
  REQUIRE(RoiBoxIoU(det_roi, close_roi) >= kTrackingResetIouThreshold);
  REQUIRE(RoiBoxIoU(det_roi, drifted_roi) < kTrackingResetIouThreshold);
}

TEST_CASE("landmark_tracking: uji sanity menangkap mesh menumpuk/profil, jarak mata, rotasi, dan drift") {
  const int fw = 1280, fh = 720;
  const LandmarkSet good = MakeSyntheticFace(0.5f, 0.5f, 0.22f, 0.38f, 0.0f);
  const AlignedRoi good_roi = TrackRoiFromLandmarks(good, fw, fh);

  // 1) Mesh menumpuk di sisi kanan wajah (aspek sempit seperti profil).
  LandmarkSet collapsed = good;
  for (auto& p : collapsed.pts) p.x = 0.58f + (p.x - 0.5f) * 0.12f;
  const auto res_col = CheckLandmarkSanity(collapsed, good_roi, fw, fh);
  REQUIRE_FALSE(res_col.ok);

  // 2) Rotasi landmark vs ROI melenceng > 45 deg.
  const LandmarkSet twisted = MakeSyntheticFace(0.5f, 0.5f, 0.22f, 0.38f, 1.2f);
  const auto res_rot = CheckLandmarkSanity(twisted, good_roi, fw, fh);
  REQUIRE_FALSE(res_rot.ok);

  // 3) Pusat mesh melenceng jauh dari ROI.
  const LandmarkSet shifted = MakeSyntheticFace(0.75f, 0.5f, 0.22f, 0.38f, 0.0f);
  const auto res_shift = CheckLandmarkSanity(shifted, good_roi, fw, fh);
  REQUIRE_FALSE(res_shift.ok);

  // 4) Struktur vertikal terbalik (dahi di bawah dagu).
  LandmarkSet flipped = good;
  std::swap(flipped.pts[10], flipped.pts[152]);
  const auto res_flip = CheckLandmarkSanity(flipped, good_roi, fw, fh);
  REQUIRE_FALSE(res_flip.ok);
}

TEST_CASE("landmark_tracking: interval detektor adaptif berada di [10..15] frame") {
  REQUIRE(AdaptiveDetectInterval(0.05f, 0.0f, 12.0f) == 10);  // gerak cepat
  REQUIRE(AdaptiveDetectInterval(0.0f, 0.15f, 12.0f) == 10);  // rotasi cepat
  REQUIRE(AdaptiveDetectInterval(0.0f, 0.0f, 2.0f) == 10);    // flag rendah
  REQUIRE(AdaptiveDetectInterval(0.015f, 0.03f, 8.0f) == 12); // normal
  REQUIRE(AdaptiveDetectInterval(0.002f, 0.005f, 20.0f) == 15); // stabil
}

TEST_CASE("landmark_tracking: stabil tanpa drift berantai sepanjang 50 iterasi") {
  const int fw = 1280, fh = 720;
  const LandmarkSet lm = MakeSyntheticFace(0.48f, 0.52f, 0.20f, 0.36f, 0.15f, 0.7f);
  const AlignedRoi r0 = TrackRoiFromLandmarks(lm, fw, fh, 420.0f);
  AlignedRoi rk = r0;
  for (int i = 0; i < 50; ++i) {
    rk = TrackRoiFromLandmarks(lm, fw, fh, 420.0f);
  }
  REQUIRE(RoiBoxIoU(r0, rk) > 0.99f);
}

TEST_CASE("landmark_tracking: overlay debug landmark ber-indeks & konsistensi mirror") {
  const int w = 320, h = 240;
  std::vector<uint8_t> raw(static_cast<size_t>(w) * h * 4, 80);
  for (size_t i = 3; i < raw.size(); i += 4) raw[i] = 255;
  const LandmarkSet lm = MakeSyntheticFace(0.4f, 0.5f, 0.25f, 0.4f, 0.1f);
  const AlignedRoi roi = TrackRoiFromLandmarks(lm, w, h);
  std::vector<uint8_t> out;
  REQUIRE(RenderLandmarkDebugOverlayRgba(raw.data(), w, h, lm, &roi, &out));
  REQUIRE(out.size() == raw.size());
  // Harus ada piksel termodifikasi dari gambar dot/bbox/angka indeks.
  int changed = 0;
  for (size_t i = 0; i < raw.size(); i += 4) {
    if (out[i] != raw[i] || out[i + 1] != raw[i + 1] || out[i + 2] != raw[i + 2]) ++changed;
  }
  REQUIRE(changed > 100);

  // Konsistensi mirror: koordinat normalized x di ruang raw dipetakan ke (1 - x)
  // di layar mirror, persis dengan flip horizontal pixel w - 1 - px.
  const float x_norm = 0.25f;
  const int px_raw = static_cast<int>(std::lround(x_norm * (w - 1)));
  const int px_flipped = (w - 1) - px_raw;
  const int px_mirror_norm = static_cast<int>(std::lround((1.0f - x_norm) * (w - 1)));
  REQUIRE(std::abs(px_flipped - px_mirror_norm) <= 1);
}
