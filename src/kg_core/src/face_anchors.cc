#include "kgcore/face_anchors.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace kgcore {
namespace {
constexpr float kMinScale = 0.1484375f;
constexpr float kMaxScale = 0.75f;
constexpr int kStrides[] = {8, 16, 16, 16};
constexpr int kInputSize = 128;
constexpr float kAnchorOffset = 0.5f;
constexpr float kInterpolatedScaleAspectRatio = 1.0f;  // face_detection_short_range.pbtxt
constexpr size_t kNumLayers = 4;

float CalculateScale(int stride_index) {
  if (kNumLayers == 1) return (kMinScale + kMaxScale) * 0.5f;
  return kMinScale + (kMaxScale - kMinScale) * static_cast<float>(stride_index) /
                         static_cast<float>(kNumLayers - 1);
}
}  // namespace

std::array<FaceAnchor, kNumFaceAnchors> GenerateFaceAnchors() {
  std::array<FaceAnchor, kNumFaceAnchors> anchors{};
  size_t out = 0;

  for (size_t layer_id = 0; layer_id < kNumLayers;) {
    // Merge layers sharing the same stride, in order of appearance.
    size_t last_same_stride = layer_id;
    std::vector<float> aspect_ratios;
    std::vector<float> scales;
    while (last_same_stride < kNumLayers && kStrides[last_same_stride] == kStrides[layer_id]) {
      const float scale = CalculateScale(static_cast<int>(last_same_stride));
      aspect_ratios.push_back(1.0f);  // options.aspect_ratios() == {1.0}
      scales.push_back(scale);
      const float scale_next =
          (last_same_stride == kNumLayers - 1)
              ? 1.0f
              : CalculateScale(static_cast<int>(last_same_stride + 1));
      scales.push_back(std::sqrt(scale * scale_next));
      aspect_ratios.push_back(kInterpolatedScaleAspectRatio);
      ++last_same_stride;
    }

    const int stride = kStrides[layer_id];
    const int fmap = (kInputSize + stride - 1) / stride;  // ceil, matches upstream
    for (int y = 0; y < fmap; ++y) {
      for (int x = 0; x < fmap; ++x) {
        for (size_t anchor_id = 0; anchor_id < aspect_ratios.size(); ++anchor_id) {
          FaceAnchor anchor;
          anchor.cx = (static_cast<float>(x) + kAnchorOffset) / static_cast<float>(fmap);
          anchor.cy = (static_cast<float>(y) + kAnchorOffset) / static_cast<float>(fmap);
          anchor.w = 1.0f;  // fixed_anchor_size
          anchor.h = 1.0f;
          if (out >= kNumFaceAnchors) return anchors;  // defensive; unreachable by construction
          anchors[out++] = anchor;
        }
      }
    }
    layer_id = last_same_stride;
  }
  return anchors;
}
}  // namespace kgcore
