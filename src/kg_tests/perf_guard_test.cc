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
  // Beban normal 20 detik berturut → pulih (M2o kRecoverMs = 20000 ms).
  for (int i = 0; i < 620; ++i) {
    g.Update(t, 55.0);
    t += 33.3;
  }
  REQUIRE(g.state() == PerfGuard::State::kNormal);
  REQUIRE(!g.ForceFaceOnly());
  REQUIRE(g.SegPeriodMs() == 250);
}

TEST_CASE("perf_guard: pemulihan butuh 20s berturut, goyang2 atau 10s belum pulih") {
  PerfGuard g;
  double t = 0;
  // Turun → kembali normal sebentar → turun lagi: tidak boleh reset timer
  // pemulihan setengah jalan (state FaceOnly dipertahankan).
  for (int i = 0; i < 200; ++i) {
    g.Update(t, 10.0);
    t += 33.3;
  }
  REQUIRE(g.state() == PerfGuard::State::kFaceOnly);
  for (int i = 0; i < 300; ++i) {  // 10s normal (kurang dari 20s)
    g.Update(t, 55.0);
    t += 33.3;
  }
  REQUIRE(g.state() == PerfGuard::State::kFaceOnly);
  for (int i = 0; i < 620; ++i) {  // lanjut normal 20s → pulih
    g.Update(t, 55.0);
    t += 33.3;
  }
  REQUIRE(g.state() == PerfGuard::State::kNormal);
}

TEST_CASE("M2o perf_guard: periode seg adaptif berdasar gerak kepala & matriks benchmark 12 konfig") {
  // Diam (< 4 px) -> 1000 ms; bergerak (>= 4 px) -> 250 ms; frame pertama -> 250 ms.
  REQUIRE(ComputeAdaptiveSegPeriodMs(0.0f, 50.0, 250, false) == 250);
  REQUIRE(ComputeAdaptiveSegPeriodMs(0.5f, 50.0, 250, true) == 1000);
  REQUIRE(ComputeAdaptiveSegPeriodMs(6.0f, 50.0, 250, true) == 250);
  // Inferensi lambat (200 ms) -> >= 2x infer (400 ms).
  REQUIRE(ComputeAdaptiveSegPeriodMs(6.0f, 200.0, 250, true) == 400);
  // PerfGuard SlowSeg floor (1000 ms) selalu dihormati.
  REQUIRE(ComputeAdaptiveSegPeriodMs(8.0f, 40.0, 1000, true) == 1000);

  const auto matrix = BuildSegBenchMatrix();
  REQUIRE(matrix.size() == 12);
  std::vector<SegBenchResult> results;
  for (size_t i = 0; i < matrix.size(); ++i) {
    SegBenchResult r;
    r.cfg = matrix[i];
    // Simulasikan hanya config intra_op=2, spin=0, period=1000 dan period=500 yang lolos.
    if (r.cfg.intra_op == 2 && !r.cfg.allow_spinning &&
        r.cfg.period_ms >= 500) {
      r.recv_fps = 21.0;
      r.proc_fps = 20.5;
      r.cam2screen_med_ms = (r.cfg.period_ms == 1000) ? 42.0 : 50.0;
      r.seg_infer_avg_ms = 60.0;
    } else {
      r.recv_fps = 21.0;
      r.proc_fps = 13.0;
      r.cam2screen_med_ms = 106.0;
      r.seg_infer_avg_ms = 90.0;
    }
    r.meets_criteria = EvaluateSegBenchCriteria(r);
    results.push_back(r);
  }
  const int best = SelectBestSegBenchConfig(results);
  REQUIRE(best >= 0);
  REQUIRE(results[static_cast<size_t>(best)].meets_criteria);
  REQUIRE(matrix[static_cast<size_t>(best)].intra_op == 2);
  REQUIRE_FALSE(matrix[static_cast<size_t>(best)].allow_spinning);
  REQUIRE(matrix[static_cast<size_t>(best)].period_ms == 1000);
}
