#include <catch2/catch_test_macros.hpp>
#include "kgcore/black_frame_guard.h"

using kgcore::BlackFrameGuard;

TEST_CASE("black_frame_guard: deteksi anomali gelap saat input mentah terang") {
  REQUIRE(BlackFrameGuard::IsDarkAnomaly(80.0f, 0.0f));
  REQUIRE(BlackFrameGuard::IsDarkAnomaly(25.0f, 7.9f));
  REQUIRE_FALSE(BlackFrameGuard::IsDarkAnomaly(15.0f, 0.0f));   // kamera sendiri gelap
  REQUIRE_FALSE(BlackFrameGuard::IsDarkAnomaly(80.0f, 40.0f));  // normal
}

TEST_CASE("black_frame_guard: >5% frame hitam selama 1 detik memicu fallback") {
  BlackFrameGuard g;
  // 20 sampel dalam 1.1 detik, 2 gelap (10% > 5%) -> harus memicu.
  for (int i = 0; i < 20; ++i) {
    const double t = 1000.0 + i * 60.0;
    const float out_y = (i == 5 || i == 15) ? 1.0f : 90.0f;
    g.RecordSample(t, 95.0f, out_y, true);
  }
  REQUIRE(g.ShouldFallback(2140.0));
  REQUIRE(g.WindowDarkRatio(2140.0) > 0.05);
  const auto sum = g.TakeSecondSummary(2140.0);
  REQUIRE(sum.total == 20);
  REQUIRE(sum.dark == 2);
}

TEST_CASE("black_frame_guard: <=5% frame hitam atau jendela <1 detik tidak memicu") {
  BlackFrameGuard g;
  // 25 sampel, 1 gelap (4% <= 5%) selama 1.2 detik -> aman.
  for (int i = 0; i < 25; ++i) {
    const double t = 100.0 + i * 50.0;
    const float out_y = (i == 10) ? 0.5f : 110.0f;
    g.RecordSample(t, 100.0f, out_y, true);
  }
  REQUIRE_FALSE(g.ShouldFallback(1300.0));

  // Semua gelap tapi baru 500 ms -> belum memicu (< 1 detik).
  BlackFrameGuard g2;
  for (int i = 0; i < 15; ++i) {
    g2.RecordSample(1000.0 + i * 35.0, 100.0f, 0.0f, true);
  }
  REQUIRE_FALSE(g2.ShouldFallback(1490.0));
  g2.RecordSample(2050.0, 100.0f, 0.0f, true);
  REQUIRE(g2.ShouldFallback(2050.0));
}

TEST_CASE("black_frame_guard: nonaktif blend me-reset jendela geser") {
  BlackFrameGuard g;
  for (int i = 0; i < 12; ++i) {
    g.RecordSample(1000.0 + i * 100.0, 90.0f, 0.0f, true);
  }
  REQUIRE(g.ShouldFallback(2100.0));
  // Mode dimatikan lalu dinyalakan lagi -> jendela bersih.
  g.RecordSample(2200.0, 90.0f, 0.0f, false);
  REQUIRE(g.WindowSampleCount() == 0);
  g.RecordSample(2300.0, 90.0f, 90.0f, true);
  REQUIRE_FALSE(g.ShouldFallback(2300.0));
}
