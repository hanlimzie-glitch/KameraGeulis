#include "kgcore/perf_guard.h"

#include <algorithm>
#include <cmath>

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
    } else if (state_ == State::kSlowSeg &&
               now_ms - low_since_ms_ >= 2 * kLowMs) {
      state_ = State::kFaceOnly;
      just_changed_ = true;
      pending_notice_ =
          "Beban tinggi: mode Wajah saja diaktifkan otomatis (penjaga "
          "performa) — pulih setelah stabil >= 20 detik";
    }
  } else {
    low_since_ms_ = -1;
    if (ok_since_ms_ < 0) ok_since_ms_ = now_ms;
    if (state_ != State::kNormal && now_ms - ok_since_ms_ >= kRecoverMs) {
      state_ = State::kNormal;
      just_changed_ = true;
      pending_notice_ =
          "Beban stabil >= 20 detik — penjaga performa memulihkan pengaturan";
    }
  }
}

int ComputeAdaptiveSegPeriodMs(float head_speed_px, double infer_ms,
                               int perf_guard_min_period_ms,
                               bool has_valid_seg) {
  const int guard_min = std::clamp(perf_guard_min_period_ms, 200, 3000);
  if (!has_valid_seg) {
    return guard_min;
  }
  constexpr float kStillSpeedPx = 0.8f;
  constexpr float kFastSpeedPx = 4.0f;
  constexpr int kStillPeriodMs = 1000;
  constexpr int kFastPeriodMs = 250;
  const float t = std::clamp(
      (head_speed_px - kStillSpeedPx) / (kFastSpeedPx - kStillSpeedPx), 0.0f,
      1.0f);
  int motion_period = static_cast<int>(
      std::lround(static_cast<float>(kStillPeriodMs) +
                  t * static_cast<float>(kFastPeriodMs - kStillPeriodMs)));
  if (infer_ms > 0.0) {
    const int safe_infer_floor =
        std::clamp(static_cast<int>(std::ceil(infer_ms * 2.0)), 100, 1000);
    motion_period = std::max(motion_period, safe_infer_floor);
  }
  return std::clamp(std::max(motion_period, guard_min), 200, 3000);
}

std::vector<SegBenchConfig> BuildSegBenchMatrix() {
  std::vector<SegBenchConfig> out;
  constexpr int kIntraOps[2] = {2, 3};
  constexpr bool kSpins[2] = {false, true};
  constexpr int kPeriods[3] = {250, 500, 1000};
  out.reserve(12);
  for (int intra : kIntraOps) {
    for (bool spin : kSpins) {
      for (int period : kPeriods) {
        out.push_back(SegBenchConfig{intra, spin, period});
      }
    }
  }
  return out;
}

bool EvaluateSegBenchCriteria(const SegBenchResult& r) {
  if (r.recv_fps <= 1.0) return false;
  const bool fps_ok = r.proc_fps >= 0.90 * r.recv_fps;
  const bool lat_ok = r.cam2screen_med_ms > 0.0 && r.cam2screen_med_ms <= 60.0;
  return fps_ok && lat_ok;
}

int SelectBestSegBenchConfig(const std::vector<SegBenchResult>& results) {
  if (results.empty()) return -1;
  int best_pass = -1;
  double best_pass_score = 1e18;
  int best_fallback = -1;
  double best_fallback_score = -1e18;

  for (int i = 0; i < static_cast<int>(results.size()); ++i) {
    const auto& r = results[i];
    if (r.cfg.intra_op < 1 || r.cfg.intra_op > 3) continue;
    const bool pass = EvaluateSegBenchCriteria(r);
    if (pass) {
      // Semakin kecil cam2screen, semakin hemat CPU (allow_spinning=0, intra=2),
      // dan periode lebih responsif saat kriteria sama-sama terpenuhi.
      const double score =
          r.cam2screen_med_ms + 0.012 * static_cast<double>(r.cfg.period_ms) +
          (r.cfg.allow_spinning ? 6.0 : 0.0) +
          (r.cfg.intra_op > 2 ? 2.5 : 0.0);
      if (best_pass < 0 || score < best_pass_score) {
        best_pass = i;
        best_pass_score = score;
      }
    } else {
      const double ratio =
          r.recv_fps > 0.1 ? std::min(1.0, r.proc_fps / r.recv_fps) : 0.0;
      const double fb_score =
          ratio * 100.0 - 0.5 * r.cam2screen_med_ms -
          (r.cfg.allow_spinning ? 2.0 : 0.0) -
          (r.cfg.intra_op > 2 ? 1.0 : 0.0);
      if (best_fallback < 0 || fb_score > best_fallback_score) {
        best_fallback = i;
        best_fallback_score = fb_score;
      }
    }
  }
  return best_pass >= 0 ? best_pass : best_fallback;
}

}  // namespace kgcore
