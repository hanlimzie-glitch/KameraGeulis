#include "kgcore/black_frame_guard.h"

namespace kgcore {

void BlackFrameGuard::ResetWindow() {
  samples_.clear();
  window_start_ms_ = -1.0;
}

void BlackFrameGuard::ResetAll() {
  ResetWindow();
  was_active_ = false;
  sec_total_ = 0;
  sec_dark_ = 0;
}

void BlackFrameGuard::PruneOld_(double now_ms) {
  while (!samples_.empty() && (now_ms - samples_.front().ts_ms) > kWindowMs) {
    samples_.pop_front();
  }
}

void BlackFrameGuard::RecordSample(double now_ms, float raw_luma,
                                   float out_luma, bool blend_active) {
  if (!blend_active) {
    if (was_active_) {
      ResetWindow();
      was_active_ = false;
    }
    return;
  }
  was_active_ = true;
  if (raw_luma <= kRawBrightMinLuma) {
    // Kamera sendiri gelap (mis. penutup lensa) -> jangan hitung anomali.
    PruneOld_(now_ms);
    return;
  }
  if (window_start_ms_ < 0.0) window_start_ms_ = now_ms;
  const bool dark = out_luma < kOutDarkMaxLuma;
  samples_.push_back({now_ms, dark});
  PruneOld_(now_ms);
  ++sec_total_;
  if (dark) ++sec_dark_;
}

double BlackFrameGuard::WindowDarkRatio(double now_ms) const {
  if (samples_.empty()) return 0.0;
  size_t total = 0;
  size_t dark = 0;
  for (const auto& s : samples_) {
    if ((now_ms - s.ts_ms) <= kWindowMs) {
      ++total;
      if (s.dark) ++dark;
    }
  }
  return total > 0 ? static_cast<double>(dark) / static_cast<double>(total) : 0.0;
}

bool BlackFrameGuard::ShouldFallback(double now_ms) const {
  if (window_start_ms_ < 0.0) return false;
  if ((now_ms - window_start_ms_) < kWindowMs) return false;
  size_t total = 0;
  size_t dark = 0;
  for (const auto& s : samples_) {
    if ((now_ms - s.ts_ms) <= kWindowMs) {
      ++total;
      if (s.dark) ++dark;
    }
  }
  if (total < kMinSamples) return false;
  return (static_cast<double>(dark) / static_cast<double>(total)) >
         kMaxDarkRatio;
}

BlackFrameGuard::SecondSummary BlackFrameGuard::TakeSecondSummary(
    double now_ms) {
  PruneOld_(now_ms);
  SecondSummary out;
  out.total = sec_total_;
  out.dark = sec_dark_;
  out.window_dark_ratio = WindowDarkRatio(now_ms);
  out.window_samples = samples_.size();
  sec_total_ = 0;
  sec_dark_ = 0;
  return out;
}

}  // namespace kgcore
