#include "kgcore/perf_guard.h"

namespace kgcore {

void PerfGuard::Reset() {
  state_ = State::kNormal;
  low_since_ms_ = -1;
  ok_since_ms_ = -1;
  just_changed_ = false;
  pending_notice_.clear();
}

int PerfGuard::SegPeriodMs() const {
  switch (state_) {
    case State::kNormal:
      return base_period_ms_;
    case State::kSlowSeg:
      return 1000;  // ~1 Hz — beban seg diturunkan dulu
    case State::kFaceOnly:
      return 2000;  // nyaris mati; wajah saja yang dipakai
  }
  return base_period_ms_;
}

std::string PerfGuard::TakeNotice() {
  std::string s;
  s.swap(pending_notice_);
  return s;
}

void PerfGuard::Update(double now_ms, double fps) {
  just_changed_ = false;
  const bool low = fps > 0.0 && fps < kFpsThreshold;
  if (low) {
    ok_since_ms_ = -1;
    if (low_since_ms_ < 0) low_since_ms_ = now_ms;
    if (state_ == State::kNormal && now_ms - low_since_ms_ >= kLowMs) {
      state_ = State::kSlowSeg;
      just_changed_ = true;
      pending_notice_ =
          "Beban tinggi: laju seg dikurangi otomatis (penjaga performa)";
    } else if (state_ == State::kSlowSeg && now_ms - low_since_ms_ >= 2 * kLowMs) {
      state_ = State::kFaceOnly;
      just_changed_ = true;
      pending_notice_ =
          "Beban tinggi: mode Wajah saja diaktifkan otomatis (penjaga "
          "performa) — kembali normal saat beban turun";
    }
  } else {
    low_since_ms_ = -1;
    if (ok_since_ms_ < 0) ok_since_ms_ = now_ms;
    if (state_ != State::kNormal && now_ms - ok_since_ms_ >= kRecoverMs) {
      state_ = State::kNormal;
      just_changed_ = true;
      pending_notice_ = "Beban normal kembali — penjaga performa memulihkan "
                        "pengaturan";
    }
  }
}

}  // namespace kgcore
