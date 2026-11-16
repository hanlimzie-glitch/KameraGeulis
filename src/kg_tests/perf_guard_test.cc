// M2k butir 3 — uji penjaga adaptif beban (murni logika, Linux penuh).
#include <catch2/catch_test_macros.hpp>

#include "kgcore/perf_guard.h"

using namespace kgcore;

TEST_CASE("perf_guard: FPS<20 selama 3s → turun laju seg dulu") {
  PerfGuard g;
  double t = 0;
  //10 detik FPS sehat → normal.
  for (int i = 0; i < 300; ++i) {
    g.Update(t, 60.0);
    t += 33.3;
  }
  REQUIRE(g.state() == PerfGuard::State::kNormal);
  REQUIRE(g.SegPeriodMs() == 250);
  // FPS rendah1,9 detik → masih normal (belum3 detik).
  for (int i = 0; i < 57; ++i) {
    g.Update(t, 12.0);
    t += 33.3;
  }
  REQUIRE(g.state() == PerfGuard::State::kNormal);
  // Lewat3 detik → SlowSeg + notifikasi.
  for (int i = 0; i < 40; ++i) {
    g.Update(t, 12.0);
    t += 33.3;
  }
  REQUIRE(g.state() == PerfGuard::State::kSlowSeg);
  REQUIRE(g.SegPeriodMs() == 1000);
  REQUIRE(!g.TakeNotice().empty());
  REQUIRE(g.TakeNotice().empty());  // diambil sekali
}

TEST_CASE("perf_guard: tetap rendah → Wajah saja; pulih saat beban turun") {
  PerfGuard g;
  double t = 0;
  for (int i = 0; i < 100; ++i) {
    g.Update(t, 10.0);
    t += 33.3;
  }
  REQUIRE(g.state() == PerfGuard::State::kSlowSeg);
  // Rendah terus melewati 2×3s → FaceOnly.
  for (int i = 0; i < 120; ++i) {
    g.Update(t, 10.0);
    t += 33.3;
  }
  REQUIRE(g.state() == PerfGuard::State::kFaceOnly);
  REQUIRE(g.ForceFaceOnly());
  REQUIRE(g.SegPeriodMs() == 2000);
  REQUIRE(!g.TakeNotice().empty());
  // Beban normal4 detik berturut → pulih.
  for (int i = 0; i < 130; ++i) {
    g.Update(t, 55.0);
    t += 33.3;
  }
  REQUIRE(g.state() == PerfGuard::State::kNormal);
  REQUIRE(!g.ForceFaceOnly());
  REQUIRE(g.SegPeriodMs() == 250);
}

TEST_CASE("perf_guard: pemulihan butuh4s berturut, goyang2 tidak memicu") {
  PerfGuard g;
  double t = 0;
  // Turun → kembali normal sebentar → turun lagi: tidak boleh reset timer
  // pemulihan setengah jalan (state FaceOnly dipertahankan).
  for (int i = 0; i < 200; ++i) {
    g.Update(t, 10.0);
    t += 33.3;
  }
  REQUIRE(g.state() == PerfGuard::State::kFaceOnly);
  for (int i = 0; i < 60; ++i) {  //2s normal (kurang dari4s)
    g.Update(t, 55.0);
    t += 33.3;
  }
  REQUIRE(g.state() == PerfGuard::State::kFaceOnly);
  for (int i = 0; i < 130; ++i) {  // lanjut normal → pulih
    g.Update(t, 55.0);
    t += 33.3;
  }
  REQUIRE(g.state() == PerfGuard::State::kNormal);
}
