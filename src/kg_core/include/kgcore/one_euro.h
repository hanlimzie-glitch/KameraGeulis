#pragma once
#include <cstdint>
#include "kgcore/geometry.h"

namespace kgcore {
// One Euro Filter (Casiez, Roussel, Vogel 2012) — adaptive low-pass filter:
// cutoff rises with signal speed (less lag) and falls at rest (less jitter).
// Used per landmark coordinate; see Casiez et al. CHI 2012.
class OneEuroFilter {
 public:
  OneEuroFilter() = default;
  OneEuroFilter(float min_cutoff, float beta, float d_cutoff);

  float Filter(float value, int64_t timestamp_ms);
  void Reset();

  float MinCutoff() const { return min_cutoff_; }
  float Beta() const { return beta_; }

 private:
  static float Alpha(float cutoff, float dt_seconds);

  float min_cutoff_ = 1.0f;
  float beta_ = 0.007f;
  float d_cutoff_ = 1.0f;
  bool has_prev_ = false;
  float prev_x_ = 0.0f;
  float prev_dx_ = 0.0f;
  int64_t prev_ts_ms_ = 0;
};

class OneEuroVec2 {
 public:
  explicit OneEuroVec2(float min_cutoff = 1.0f, float beta = 0.007f,
                       float d_cutoff = 1.0f)
      : fx_(min_cutoff, beta, d_cutoff), fy_(min_cutoff, beta, d_cutoff) {}
  Vec2 Filter(const Vec2& v, int64_t timestamp_ms);
  void Reset();

 private:
  OneEuroFilter fx_, fy_;
};
}  // namespace kgcore
