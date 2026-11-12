#pragma once
#include <array>
#include <cstddef>

namespace kgcore {
// BlazeFace short-range anchor generation (128x128 input).
//
// Source-faithful port of MediaPipe `SsdAnchorsCalculator` semantics with
// `fixed_anchor_size: true` (anchor w/h are always 1.0; centers are
// normalized to [0,1] of the input tensor). Configuration from
// `mediapipe/modules/face_detection/face_detection.pbtxt` (the
// FaceDetection subgraph) as overridden by `face_detection_short_range.pbtxt`
// (num_layers=4, strides=[8,16,16,16], interpolated_scale_aspect_ratio=1.0):
//   min_scale 0.1484375, max_scale 0.75, aspect_ratios [1.0],
//   anchor_offset 0.5 (both axes), fixed_anchor_size true.
//
// With fixed_anchor_size the scale values only decide HOW MANY anchors are
// emitted per cell (2: the per-layer scale and the interpolated
// sqrt(scale * next_scale)); the anchor w/h stay 1.0. Output order matches
// the model regression tensor order: for each stride group, y-then-x grid,
// then anchor id (layer scale, interpolated scale).
//
// Verified against upstream (counts: stride 8 -> 16*16*2 = 512,
// stride 16 -> 8*8*6 = 384; total 896 = num_boxes in the short-range graph).
inline constexpr size_t kNumFaceAnchors = 896;

struct FaceAnchor {
  float cx = 0.0f;   // normalized x center, in [0,1]
  float cy = 0.0f;   // normalized y center, in [0,1]
  float w = 1.0f;    // fixed_anchor_size -> always 1.0
  float h = 1.0f;    // fixed_anchor_size -> always 1.0
};

// Deterministic pure function. The returned array is identical on every call.
std::array<FaceAnchor, kNumFaceAnchors> GenerateFaceAnchors();
}  // namespace kgcore
