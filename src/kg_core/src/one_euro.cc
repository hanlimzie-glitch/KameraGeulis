#include "kgcore/one_euro.h"
#include <algorithm>
#include <cmath>

namespace kgcore {

OneEuroFilter::OneEuroFilter(float min_cutoff, float beta, float d_cutoff)
    : min_cutoff_(std::max(0.01f, min_cutoff)),
      beta_(std::max(0.0f, beta)),
      d_cutoff_(std::max(0.01f, d_cutoff)) {}

float OneEuroFilter::Alpha(float cutoff, float dt_seconds) {
  const float tau = 1.0f / (2.0f * 3.14159265358979323846f * cutoff);
  return 1.0f / (1.0f + tau / dt_seconds);
}

float OneEuroFilter::Filter(float value, int64_t timestamp_ms) {
  if (!has_prev_) {
    has_prev_ = true;
    prev_x_ = value;
    prev_dx_ = 0.0f;
    prev_ts_ms_ = timestamp_ms;
    return value;
  }
  float dt = (timestamp_ms - prev_ts_ms_) / 1000.0f;
  if (dt <= 0.0f) dt = 1e-3f;  // clamp non-monotonic timestamps

  const float dx = (value - prev_x_) / dt;
  const float edx = Alpha(d_cutoff_, dt) * dx + (1.0f - Alpha(d_cutoff_, dt)) * prev_dx_;
  const float cutoff = min_cutoff_ + beta_ * std::fabs(edx);
  const float alpha = Alpha(cutoff, dt);
  const float x = alpha * value + (1.0f - alpha) * prev_x_;

  prev_x_ = x;
  prev_dx_ = edx;
  prev_ts_ms_ = timestamp_ms;
  return x;
}

void OneEuroFilter::Reset() {
  has_prev_ = false;
  prev_x_ = 0.0f;
  prev_dx_ = 0.0f;
  prev_ts_ms_ = 0;
}

Vec2 OneEuroVec2::Filter(const Vec2& v, int64_t timestamp_ms) {
  return {fx_.Filter(v.x, timestamp_ms), fy_.Filter(v.y, timestamp_ms)};
}

void OneEuroVec2::Reset() {
  fx_.Reset();
  fy_.Reset();
}
}  // namespace kgcore
