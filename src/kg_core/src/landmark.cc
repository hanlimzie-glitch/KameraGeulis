#include "kgcore/landmark.h"
#include <algorithm>

namespace kgcore {

std::vector<float> FlattenLandmarks(const LandmarkSet& set) {
  std::vector<float> out;
  out.reserve(kLandmarkCount * 2);
  for (const auto& p : set.pts) {
    out.push_back(p.x);
    out.push_back(p.y);
  }
  return out;
}

void LandmarkFade::Update(bool face_present, int64_t now_ms) {
  if (!has_prev_) {
    has_prev_ = true;
    prev_ts_ms_ = now_ms;
    gain_ = face_present ? 1.0f : 0.0f;
    return;
  }
  float dt_ms = static_cast<float>(now_ms - prev_ts_ms_);
  prev_ts_ms_ = now_ms;
  if (dt_ms < 0.0f) dt_ms = 0.0f;
  const float step = fade_ms_ > 0.0f ? dt_ms / fade_ms_ : 1.0f;
  if (face_present) {
    gain_ = std::min(1.0f, gain_ + step);
  } else {
    gain_ = std::max(0.0f, gain_ - step);
  }
}
}  // namespace kgcore
