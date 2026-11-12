#include "kgcore/face_detection_decode.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace kgcore {

float Sigmoid(float x) {
  if (x >= 0.0f) {
    const float e = std::exp(-x);
    return 1.0f / (1.0f + e);
  }
  const float e = std::exp(x);
  return e / (1.0f + e);
}

float BoxIoU(float ax0, float ay0, float ax1, float ay1, float bx0, float by0, float bx1, float by1) {
  const float ix0 = std::max(ax0, bx0);
  const float iy0 = std::max(ay0, by0);
  const float ix1 = std::min(ax1, bx1);
  const float iy1 = std::min(ay1, by1);
  const float iw = std::max(0.0f, ix1 - ix0);
  const float ih = std::max(0.0f, iy1 - iy0);
  const float inter = iw * ih;
  const float area_a = std::max(0.0f, ax1 - ax0) * std::max(0.0f, ay1 - ay0);
  const float area_b = std::max(0.0f, bx1 - bx0) * std::max(0.0f, by1 - by0);
  const float union_ = area_a + area_b - inter;
  return union_ > 0.0f ? inter / union_ : 0.0f;
}

LetterboxInfo ComputeLetterbox(int src_w, int src_h, int dst) {
  LetterboxInfo info;
  info.dst = dst;
  info.src_w = src_w;
  info.src_h = src_h;
  if (src_w <= 0 || src_h <= 0) {
    info.scale = 1.0f;
    info.pad_x = info.pad_y = 0.0f;
    return info;
  }
  info.scale = std::min(static_cast<float>(dst) / src_w, static_cast<float>(dst) / src_h);
  const float new_w = src_w * info.scale;
  const float new_h = src_h * info.scale;
  info.pad_x = (dst - new_w) * 0.5f;
  info.pad_y = (dst - new_h) * 0.5f;
  return info;
}

Vec2 LetterboxInfo::MapToFrame(Vec2 dst_px) const {
  const float x = (dst_px.x - pad_x) / scale;
  const float y = (dst_px.y - pad_y) / scale;
  return {src_w > 0 ? x / src_w : 0.0f, src_h > 0 ? y / src_h : 0.0f};
}

Vec2 LetterboxInfo::MapToTensor(Vec2 frame_norm) const {
  const float x = frame_norm.x * src_w * scale + pad_x;
  const float y = frame_norm.y * src_h * scale + pad_y;
  return {x, y};
}

std::vector<Detection> DecodeDetections(
    const float* regressors, const float* classificators,
    const std::array<FaceAnchor, kNumFaceAnchors>& anchors,
    float score_threshold, float nms_iou_threshold) {
  std::vector<Detection> candidates;
  candidates.reserve(kNumFaceAnchors);

  for (size_t i = 0; i < kNumFaceAnchors; ++i) {
    const float logit = classificators[i];
    const float score = Sigmoid(logit);
    if (score < score_threshold) continue;

    const float* raw = regressors + i * 16;
    // reverse_output_order = true: (x_center, y_center, w, h), then 6 keypoints (x,y).
    // x/y/w/h scales are 128; apply_exponential_on_box_size = false.
    const float cx = raw[0] / 128.0f * anchors[i].w + anchors[i].cx;
    const float cy = raw[1] / 128.0f * anchors[i].h + anchors[i].cy;
    const float w = raw[2] / 128.0f * anchors[i].w;
    const float h = raw[3] / 128.0f * anchors[i].h;
    if (w < 0.0f || h < 0.0f) continue;  // upstream drops negative-size boxes

    Detection det;
    det.cx = cx;
    det.cy = cy;
    det.w = w;
    det.h = h;
    det.score = score;
    for (int k = 0; k < 6; ++k) {
      det.keypoints[k].x = raw[4 + 2 * k + 0] / 128.0f * anchors[i].w + anchors[i].cx;
      det.keypoints[k].y = raw[4 + 2 * k + 1] / 128.0f * anchors[i].h + anchors[i].cy;
    }
    candidates.push_back(det);
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const Detection& a, const Detection& b) { return a.score > b.score; });

  // Weighted NMS (NonMaxSuppressionCalculator WEIGHTED, INTERSECTION_OVER_UNION,
  // min_suppression_threshold=0.3): iterate by descending score; cluster every
  // remaining box overlapping the current best by > threshold and emit the
  // score-weighted average of the cluster (box + keypoints).
  std::vector<Detection> kept;
  std::vector<char> consumed(candidates.size(), 0);
  for (size_t i = 0; i < candidates.size(); ++i) {
    if (consumed[i]) continue;
    const Detection& best = candidates[i];
    const float bx0 = best.cx - best.w * 0.5f, by0 = best.cy - best.h * 0.5f;
    const float bx1 = best.cx + best.w * 0.5f, by1 = best.cy + best.h * 0.5f;

    float total = 0.0f;
    float wx0 = 0.0f, wy0 = 0.0f, wx1 = 0.0f, wy1 = 0.0f;
    std::array<float, 12> wkp{};
    std::vector<size_t> cluster;
    for (size_t j = i; j < candidates.size(); ++j) {
      if (consumed[j]) continue;
      const Detection& other = candidates[j];
      const float ox0 = other.cx - other.w * 0.5f, oy0 = other.cy - other.h * 0.5f;
      const float ox1 = other.cx + other.w * 0.5f, oy1 = other.cy + other.h * 0.5f;
      if (BoxIoU(bx0, by0, bx1, by1, ox0, oy0, ox1, oy1) > nms_iou_threshold) {
        cluster.push_back(j);
        total += other.score;
        wx0 += ox0 * other.score;
        wy0 += oy0 * other.score;
        wx1 += ox1 * other.score;
        wy1 += oy1 * other.score;
        for (int k = 0; k < 6; ++k) {
          wkp[2 * k + 0] += other.keypoints[k].x * other.score;
          wkp[2 * k + 1] += other.keypoints[k].y * other.score;
        }
      }
    }
    for (size_t j : cluster) consumed[j] = 1;
    if (total <= 0.0f) continue;
    Detection out;
    const float x0 = wx0 / total, y0 = wy0 / total, x1 = wx1 / total, y1 = wy1 / total;
    out.cx = (x0 + x1) * 0.5f;
    out.cy = (y0 + y1) * 0.5f;
    out.w = x1 - x0;
    out.h = y1 - y0;
    out.score = best.score;  // upstream: weighted detection keeps the top score
    for (int k = 0; k < 6; ++k) {
      out.keypoints[k].x = wkp[2 * k + 0] / total;
      out.keypoints[k].y = wkp[2 * k + 1] / total;
    }
    kept.push_back(out);
  }
  return kept;
}
}  // namespace kgcore
