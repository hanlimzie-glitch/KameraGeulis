#include "face_tracker.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>
#include "kgcore/log.h"

namespace kgcore {

FaceTracker::FaceTracker(std::unique_ptr<ILandmarkProvider> provider)
    : provider_(std::move(provider)) {
  for (auto& s : smoothers_) {
    s = OneEuroVec2(kDefaultMinCutoff, kDefaultBeta, kDefaultDCutoff);
  }
}

void FaceTracker::Reset() {
  for (auto& s : smoothers_) s.Reset();
  state_machine_.Reset();
  last_step_ = TrackingStepResult{};
  current_ = LandmarkSet{};
  current_raw_ = LandmarkSet{};
  last_filter_lag_mean_px_ = 0.0f;
  last_filter_lag_p95_px_ = 0.0f;
  if (provider_) provider_->ResetTracking();
}

const LandmarkSet& FaceTracker::MaskLandmarks() const {
  if (current_raw_.valid) return current_raw_;
  if (current_.valid) return current_;
  if (last_step_.mask_frozen && state_machine_.HasLastGood()) {
    return state_machine_.LastGoodRaw();
  }
  return current_;
}

void FaceTracker::Track(const Frame& frame) {
  const auto t0 = std::chrono::steady_clock::now();
  LandmarkSet raw = provider_->Track(frame);
  const auto t1 = std::chrono::steady_clock::now();
  last_track_ms_ = std::chrono::duration<float, std::milli>(t1 - t0).count();
  const int64_t ts = frame.timestamp_ms;
  last_filter_lag_mean_px_ = 0.0f;
  last_filter_lag_p95_px_ = 0.0f;

  LandmarkSet smoothed = raw;
  if (raw.valid) {
    if (state_machine_.Phase() == TrackingPhase::kLost || raw.roi_reset) {
      for (auto& s : smoothers_) s.Reset();
    }
    std::array<float, kLandmarkCount> dists{};
    double sum = 0.0;
    const float fw = static_cast<float>(std::max(1, frame.width));
    const float fh = static_cast<float>(std::max(1, frame.height));
    for (size_t i = 0; i < kLandmarkCount; ++i) {
      const Vec2 unfiltered = raw.pts[i];
      smoothed.pts[i] = smoothers_[i].Filter(unfiltered, ts);
      const float d = std::hypot((smoothed.pts[i].x - unfiltered.x) * fw,
                                 (smoothed.pts[i].y - unfiltered.y) * fh);
      dists[i] = d;
      sum += d;
    }
    last_filter_lag_mean_px_ = static_cast<float>(sum / kLandmarkCount);
    const size_t idx95 = static_cast<size_t>(0.95f * (kLandmarkCount - 1));
    std::nth_element(dists.begin(), dists.begin() + static_cast<std::ptrdiff_t>(idx95),
                     dists.end());
    last_filter_lag_p95_px_ = dists[idx95];
  }

  last_step_ = state_machine_.Update(raw.valid, raw.valid ? &raw : nullptr,
                                     raw.valid ? &smoothed : nullptr, ts);
  if (last_step_.phase_changed && !last_step_.transition_log.empty()) {
    Log("track", "state: %s", last_step_.transition_log.c_str());
  }

  if (raw.valid) {
    current_raw_ = raw;
    current_ = smoothed;
  } else if (last_step_.holding && state_machine_.HasLastGood()) {
    // Hold-last-good (~300 ms): pertahankan landmark baik terakhir agar
    // efek dan eksklusi fitur tidak berkedip akibat gangguan sesaat.
    current_raw_ = state_machine_.LastGoodRaw();
    current_ = state_machine_.LastGoodSmoothed();
  } else {
    current_raw_ = raw;
    current_ = raw;
  }
}
}  // namespace kgcore
