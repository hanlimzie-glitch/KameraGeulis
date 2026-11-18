#pragma once
// M2k/M2o — penjaga adaptif beban + periode seg adaptif + matriks benchmark.
// FPS < 20 selama 3 detik -> (1) turunkan laju seg dulu, (2) bila tetap rendah
// -> otomatis "Wajah saja" + notifikasi jelas; histeresis pemulihan >= 20 detik
// stabil untuk mencegah osilasi. Murni logika (uji Linux penuh).
#include <cstdint>
#include <string>
#include <vector>

namespace kgcore {

class PerfGuard {
 public:
  enum class State { kNormal, kSlowSeg, kFaceOnly };

  void Reset();
  // fps = FPS render saat ini; dipanggil tiap frame.
  void Update(double now_ms, double fps);

  State state() const { return state_; }
  // Periode minimum seg yang diminta (ms) — 250..2000; normal = base_period_ms.
  int SegPeriodMs() const;
  // true bila coverage wajah saja dipaksa (di atas pilihan UI).
  bool ForceFaceOnly() const { return state_ == State::kFaceOnly; }
  // Notifikasi perubahan status (diambil sekali; dikosongkan setelah dibaca).
  std::string TakeNotice();
  // Pemicu transisi (untuk log & uji).
  bool JustChanged() const { return just_changed_; }

  static constexpr double kFpsThreshold = 20.0;
  static constexpr double kLowMs = 3000.0;
  static constexpr double kRecoverMs = 20000.0;  // M2o: histeresis >= 20 detik

 private:
  State state_ = State::kNormal;
  double low_since_ms_ = -1;
  double ok_since_ms_ = -1;
  int base_period_ms_ = 250;
  bool just_changed_ = false;
  std::string pending_notice_;
};

// M2o: periode segmentasi adaptif berbasis gerak kepala (dalam px frame/frame):
//  - Kepala diam / gerak kecil (<= 0.8 px/frame): lambat (~1000 ms) karena
//    kompensasi kemiripan sudah menjaga akurasi dengan beban CPU sangat rendah;
//  - Gerak sedang (0.8..4.0 px/frame): interpolasi halus 1000 -> 250 ms;
//  - Gerak cepat (>= 4.0 px/frame): cepat (~250 ms) atau >= 2x infer_ms bila
//    inferensi lebih berat;
//  - Selalu menghormati batas bawah dari PerfGuard (`perf_guard_min_period_ms`).
int ComputeAdaptiveSegPeriodMs(float head_speed_px, double infer_ms,
                               int perf_guard_min_period_ms,
                               bool has_valid_seg);

// M2o: matriks benchmark in-session:
//   intra_op {2, 3} x allow_spinning {0, 1} x period {250, 500, 1000 ms}.
struct SegBenchConfig {
  int intra_op = 2;
  bool allow_spinning = false;
  int period_ms = 500;
};

struct SegBenchResult {
  SegBenchConfig cfg{};
  double recv_fps = 0.0;
  double proc_fps = 0.0;
  double cam2screen_med_ms = 0.0;
  double cam2screen_p95_ms = 0.0;
  double seg_infer_avg_ms = 0.0;
  int seg_samples = 0;
  bool meets_criteria = false;
};

std::vector<SegBenchConfig> BuildSegBenchMatrix();

// Menilai apakah hasil benchmark memenuhi target:
//   processed FPS ~ received FPS (>= 90% recv_fps) dan cam2screen median <= 60 ms.
bool EvaluateSegBenchCriteria(const SegBenchResult& r);

// Memilih konfigurasi terbaik dari data benchmark; mengembalikan indeks terbaik
// (atau -1 bila vector kosong). Tidak pernah menaikkan intra_op di atas 3.
int SelectBestSegBenchConfig(const std::vector<SegBenchResult>& results);

}  // namespace kgcore
