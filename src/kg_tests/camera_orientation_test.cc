#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>
#include "kgcore/camera_orientation.h"
#include "kgcore/frame.h"
#include "kgcore/frame_convert.h"

using namespace kgcore;

namespace {
// Buat frame 4x2 BGRA asimetris (tiap piksel unik berdasarkan koordinat x,y)
// dengan padding stride untuk menguji rotasi 0/90/180/270 secara eksak.
Frame MakeAsymmetricTestFrame() {
  Frame f;
  f.width = 4;
  f.height = 2;
  f.stride_bytes = 24;  // 16 byte data + 8 byte padding per baris
  f.format = kFormatBGRA;
  f.timestamp_ms = 12345;
  f.pixels.assign(static_cast<size_t>(f.stride_bytes) * f.height, 0xEE);
  for (int y = 0; y < f.height; ++y) {
    for (int x = 0; x < f.width; ++x) {
      uint8_t* p = f.pixels.data() + static_cast<size_t>(y) * f.stride_bytes + x * 4;
      p[0] = static_cast<uint8_t>(x * 10 + y);
      p[1] = static_cast<uint8_t>(100 + x);
      p[2] = static_cast<uint8_t>(200 + y);
      p[3] = 255;
    }
  }
  return f;
}

uint8_t PixelId(const Frame& f, int x, int y) {
  return f.pixels[static_cast<size_t>(y) * f.stride_bytes + static_cast<size_t>(x) * 4];
}
}  // namespace

TEST_CASE("M2p frame_rotate: rotasi 0/90/180/270 CW mempertahankan dimensi & piksel") {
  const Frame src = MakeAsymmetricTestFrame();

  Frame r0;
  REQUIRE(RotateFrameCW(src, 0, &r0));
  REQUIRE(r0.width == 4);
  REQUIRE(r0.height == 2);
  REQUIRE(r0.stride_bytes == 16);
  REQUIRE(r0.timestamp_ms == 12345);
  REQUIRE(PixelId(r0, 0, 0) == 0);
  REQUIRE(PixelId(r0, 3, 1) == 31);

  Frame r90;
  REQUIRE(RotateFrameCW(src, 90, &r90));
  REQUIRE(r90.width == 2);
  REQUIRE(r90.height == 4);
  // 90° CW: (0,1)->(0,0), (0,0)->(1,0), (3,1)->(0,3), (3,0)->(1,3)
  REQUIRE(PixelId(r90, 0, 0) == PixelId(src, 0, 1));
  REQUIRE(PixelId(r90, 1, 0) == PixelId(src, 0, 0));
  REQUIRE(PixelId(r90, 0, 3) == PixelId(src, 3, 1));
  REQUIRE(PixelId(r90, 1, 3) == PixelId(src, 3, 0));

  Frame r180;
  REQUIRE(RotateFrameCW(src, 180, &r180));
  REQUIRE(r180.width == 4);
  REQUIRE(r180.height == 2);
  REQUIRE(PixelId(r180, 0, 0) == PixelId(src, 3, 1));
  REQUIRE(PixelId(r180, 3, 0) == PixelId(src, 0, 1));
  REQUIRE(PixelId(r180, 0, 1) == PixelId(src, 3, 0));
  REQUIRE(PixelId(r180, 3, 1) == PixelId(src, 0, 0));

  Frame r270;
  REQUIRE(RotateFrameCW(src, 270, &r270));
  REQUIRE(r270.width == 2);
  REQUIRE(r270.height == 4);
  // 270° CW: (3,0)->(0,0), (3,1)->(1,0), (0,0)->(0,3), (0,1)->(1,3)
  REQUIRE(PixelId(r270, 0, 0) == PixelId(src, 3, 0));
  REQUIRE(PixelId(r270, 1, 0) == PixelId(src, 3, 1));
  REQUIRE(PixelId(r270, 0, 3) == PixelId(src, 0, 0));
  REQUIRE(PixelId(r270, 1, 3) == PixelId(src, 0, 1));

  // Komposisi 90 + 270 = identitas
  Frame id = r90;
  REQUIRE(RotateFrameCW(id, 270, &id));  // uji aliasing dst == &src juga
  REQUIRE(id.width == 4);
  REQUIRE(id.height == 2);
  for (int y = 0; y < 2; ++y) {
    for (int x = 0; x < 4; ++x) {
      REQUIRE(PixelId(id, x, y) == PixelId(src, x, y));
    }
  }
}

TEST_CASE("M2p orientation: memilih skor tertinggi dari 4 rotasi, menyimpan prior, mode manual menimpa auto") {
  OrientationConfig cfg;
  cfg.confirm_searches = 2;
  cfg.switch_cooldown_frames = 15;
  cfg.fail_trigger_frames = 4;
  AutoOrientationController ctrl(cfg);

  REQUIRE(ctrl.Prior() == 0);
  REQUIRE(ctrl.EffectiveRotation(-1) == 0);
  REQUIRE(ctrl.EffectiveRotation(90) == 90);
  REQUIRE(ctrl.EffectiveRotation(270) == 270);

  // Frame 1 gagal di 0° -> memicu pencarian perdana
  ctrl.NoteFrameResult(false, false);
  REQUIRE(ctrl.ShouldSearch(-1));
  REQUIRE_FALSE(ctrl.ShouldSearch(0));  // mode manual tidak mencari

  // Skor tertinggi pada 90° (0.88), butuh konfirmasi 2 pencarian
  OrientationScores s90{{0.0f, 0.88f, 0.0f, 0.0f}, true};
  auto d1 = ctrl.ApplySearchScores(s90);
  REQUIRE_FALSE(d1.switched);
  REQUIRE(ctrl.EffectiveRotation(-1) == 0);
  REQUIRE(ctrl.CandidateRotation() == 90);
  REQUIRE(ctrl.CandidateStreak() == 1);

  // Tunggu fast_retry_frames (2 frame gagal) -> pencarian konfirmasi
  ctrl.NoteFrameResult(false, false);
  REQUIRE_FALSE(ctrl.ShouldSearch(-1));
  ctrl.NoteFrameResult(false, false);
  ctrl.NoteFrameResult(false, false);
  ctrl.NoteFrameResult(false, false);
  REQUIRE(ctrl.ShouldSearch(-1));

  auto d2 = ctrl.ApplySearchScores(s90);
  REQUIRE(d2.switched);
  REQUIRE(d2.new_rot == 90);
  REQUIRE(ctrl.EffectiveRotation(-1) == 90);
  REQUIRE(ctrl.Prior() == 90);  // diingat sebagai prior terbaik
}

TEST_CASE("M2p orientation: hysteresis margin, lonjakan 1 frame, dan cooldown cegah ping-pong") {
  OrientationConfig cfg;
  cfg.min_detect_score = 0.55f;
  cfg.switch_margin = 0.10f;
  cfg.confirm_searches = 2;
  cfg.switch_cooldown_frames = 15;
  cfg.search_cooldown_frames = 5;
  cfg.fail_trigger_frames = 3;
  AutoOrientationController ctrl(cfg);
  ctrl.SetPrior(90);  // mulai dengan prior 90°

  // 1) Lonjakan noise 1 frame di 180° lalu kembali ke 90° -> tidak berpindah
  OrientationScores spike180{{0.0f, 0.0f, 0.85f, 0.0f}, true};
  auto d_spike = ctrl.ApplySearchScores(spike180);
  REQUIRE_FALSE(d_spike.switched);
  REQUIRE(ctrl.EffectiveRotation(-1) == 90);

  OrientationScores back90{{0.0f, 0.82f, 0.0f, 0.0f}, true};
  auto d_back = ctrl.ApplySearchScores(back90);
  REQUIRE_FALSE(d_back.switched);
  REQUIRE(ctrl.EffectiveRotation(-1) == 90);
  REQUIRE(ctrl.CandidateStreak() == 0);

  // 2) Keunggulan tipis (< +0.10 margin) saat 90° masih mendeteksi wajah (0.72 vs 0.78)
  OrientationScores marginal{{0.78f, 0.72f, 0.0f, 0.0f}, true};
  for (int i = 0; i < 4; ++i) {
    auto d = ctrl.ApplySearchScores(marginal);
    REQUIRE_FALSE(d.switched);
    REQUIRE(ctrl.EffectiveRotation(-1) == 90);
  }

  // 3) Pindah sah ke 270° (skor 0.90 vs 0.00), lalu langsung coba balik ke 0° dalam cooldown
  OrientationScores s270{{0.0f, 0.0f, 0.0f, 0.90f}, true};
  REQUIRE_FALSE(ctrl.ApplySearchScores(s270).switched);
  REQUIRE(ctrl.ApplySearchScores(s270).switched);
  REQUIRE(ctrl.EffectiveRotation(-1) == 270);
  REQUIRE(ctrl.Prior() == 270);

  // Dalam masa cooldown (<15 frame), skor 0° tinggi tidak memicu ping-pong
  OrientationScores s0{{0.92f, 0.0f, 0.0f, 0.0f}, true};
  for (int f = 0; f < 10; ++f) ctrl.NoteFrameResult(false, false);
  REQUIRE_FALSE(ctrl.ApplySearchScores(s0).switched);
  REQUIRE(ctrl.EffectiveRotation(-1) == 270);

  // Setelah cooldown >= 15 frame terlewati dan konfirmasi 2x tercapai -> pindah ke 0°
  for (int f = 0; f < 6; ++f) ctrl.NoteFrameResult(false, false);
  REQUIRE_FALSE(ctrl.ApplySearchScores(s0).switched);
  REQUIRE(ctrl.ApplySearchScores(s0).switched);
  REQUIRE(ctrl.EffectiveRotation(-1) == 0);
  REQUIRE(ctrl.Prior() == 0);
}
