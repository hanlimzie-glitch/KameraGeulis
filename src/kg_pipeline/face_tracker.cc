#include "face_tracker.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

namespace kgcore {

FaceTracker::FaceTracker(std::unique_ptr<ILandmarkProvider> provider)
    : provider_(std::move(provider)) {
  for (auto& s : smoothers_) {
    s = OneEuroVec2(kDefaultMinCutoff, kDefaultBeta, kDefaultDCutoff);
  }
}

void FaceTracker::Track(const Frame& frame) {
  const auto t0 = std::chrono::steady_clock::now();
  LandmarkSet raw = provider_->Track(frame);
  const auto t1 = std::chrono::steady_clock::now();
  last_track_ms_ = std::chrono::duration<float, std::milli>(t1 - t0).count();
  const int64_t ts = frame.timestamp_ms;
  current_raw_ = raw;
  last_filter_lag_mean_px_ = 0.0f;
  last_filter_lag_p95_px_ = 0.0f;
  if (raw.valid) {
    if (!was_valid_ || raw.roi_reset) {
      for (auto& s : smoothers_) s.Reset();
    }
    std::array<float, kLandmarkCount> dists{};
    double sum = 0.0;
    const float fw = static_cast<float>(std::max(1, frame.width));
    const float fh = static_cast<float>(std::max(1, frame.height));
    for (size_t i = 0; i < kLandmarkCount; ++i) {
      const Vec2 unfiltered = raw.pts[i];
      raw.pts[i] = smoothers_[i].Filter(unfiltered, ts);
      const float d = std::hypot((raw.pts[i].x - unfiltered.x) * fw,
                                 (raw.pts[i].y - unfiltered.y) * fh);
      dists[i] = d;
      sum += d;
    }
    last_filter_lag_mean_px_ = static_cast<float>(sum / kLandmarkCount);
    const size_t idx95 = static_cast<size_t>(0.95f * (kLandmarkCount - 1));
    std::nth_element(dists.begin(), dists.begin() + static_cast<std::ptrdiff_t>(idx95),
                     dists.end());
    last_filter_lag_p95_px_ = dists[idx95];
  }
  was_valid_ = raw.valid;
  current_ = raw;
  fade_.Update(raw.valid, ts);
}
}  // namespace kgcore
