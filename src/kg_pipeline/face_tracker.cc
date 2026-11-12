#include "face_tracker.h"
#include <vector>

namespace kgcore {

FaceTracker::FaceTracker(std::unique_ptr<ILandmarkProvider> provider)
    : provider_(std::move(provider)) {}

void FaceTracker::Track(const Frame& frame) {
  LandmarkSet raw = provider_->Track(frame);
  const int64_t ts = frame.timestamp_ms;
  if (raw.valid) {
    for (size_t i = 0; i < kLandmarkCount; ++i) {
      raw.pts[i] = smoothers_[i].Filter(raw.pts[i], ts);
    }
  }
  current_ = raw;
  fade_.Update(raw.valid, ts);
}
}  // namespace kgcore
