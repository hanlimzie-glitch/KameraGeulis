#pragma once
// M2k butir 3 — penjaga adaptif beban: FPS<20 selama 3 detik → (1) turunkan
// laju seg dulu, (2) bila tetap rendah → otomatis "Wajah saja" + notifikasi
// jelas; pulih otomatis saat beban turun. Murni logika (uji Linux penuh).
#include <cstdint>
#include <string>

namespace kgcore {

class PerfGuard {
 public:
  enum class State { kNormal, kSlowSeg, kFaceOnly };

  void Reset();
  // fps = FPS render saat ini; dipanggil tiap frame.
  void Update(double now_ms, double fps);

  State state() const { return state_; }
  // Periode seg yang diminta (ms) — 200..3000; normal = base_period_ms.
  int SegPeriodMs() const;
  // true bila coverage wajah saja dipaksa (di atas pilihan UI).
  bool ForceFaceOnly() const { return state_ == State::kFaceOnly; }
  // Notifikasi perubahan status (diambil sekali; dikosongkan setelah dibaca).
  std::string TakeNotice();
  // Pemicu transisi (untuk log & uji).
  bool JustChanged() const { return just_changed_; }

  static constexpr double kFpsThreshold = 20.0;
  static constexpr double kLowMs = 3000.0;
  static constexpr double kRecoverMs = 4000.0;

 private:
  State state_ = State::kNormal;
  double low_since_ms_ = -1;
  double ok_since_ms_ = -1;
  int base_period_ms_ = 250;
  bool just_changed_ = false;
  std::string pending_notice_;
};

}  // namespace kgcore
