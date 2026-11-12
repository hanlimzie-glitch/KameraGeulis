#pragma once
#include <array>
#include <cstdint>
#include <vector>
#include "kgcore/geometry.h"

namespace kgcore {
// MediaPipe FaceMesh 468-point model (see docs/LANDMARKS.md). Points are
// normalized to [0,1] relative to the source frame. Index reference:
// https://github.com/google-ai-edge/mediapipe (face_mesh_connections.py).
inline constexpr size_t kLandmarkCount = 468;

struct LandmarkSet {
  std::array<Vec2, kLandmarkCount> pts{};
  bool valid = false;        // false when no face / model unavailable
  float face_score = 0.0f;   // detector/face-flag confidence proxy [0,1]
  int64_t ts_ms = 0;         // capture timestamp (monotonic)
};

// Flatten to [x0,y0,...,x467,y467] in normalized [0,1] coordinates —
// the format expected by GPUPixel filter SetFaceLandmarks(vector<float>).
std::vector<float> FlattenLandmarks(const LandmarkSet& set);

// Smooth gain ramp for landmark-driven effects so a face leaving/entering the
// frame fades effects over `fade_ms` instead of popping.
class LandmarkFade {
 public:
  explicit LandmarkFade(float fade_ms = 300.0f) : fade_ms_(fade_ms) {}
  void Update(bool face_present, int64_t now_ms);
  float Gain() const { return gain_; }  // [0,1]
 private:
  float fade_ms_;
  float gain_ = 0.0f;
  int64_t prev_ts_ms_ = 0;
  bool has_prev_ = false;
};
}  // namespace kgcore
