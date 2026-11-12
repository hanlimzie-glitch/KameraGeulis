#pragma once
#include <array>
#include <vector>
#include "kgcore/face_anchors.h"
#include "kgcore/geometry.h"

namespace kgcore {
// One decoded face detection on the detector input canvas (normalized [0,1]
// coordinates relative to the 128x128 letterboxed image). Apply
// LetterboxInfo::MapToFrame() to get coordinates in the source frame.
struct Detection {
  float cx = 0.0f, cy = 0.0f, w = 0.0f, h = 0.0f;  // center + size, normalized
  float score = 0.0f;
  std::array<Vec2, 6> keypoints{};  // BlazeFace: 6 keypoints, normalized
};

// Keep-aspect letterbox parameters for mapping between the source frame
// (src_w x src_h) and the square detector/mesh tensor (dst x dst, zero
// border). Mirrors ImageToTensorCalculator + NORM_RECT (full frame ROI).
struct LetterboxInfo {
  float scale = 1.0f;           // src px -> dst px
  float pad_x = 0.0f;           // dst px padding on each side
  float pad_y = 0.0f;
  int dst = 128;
  int src_w = 0, src_h = 0;

  // Point in dst-tensor pixel units -> normalized [0,1] coordinates in the
  // source frame.
  Vec2 MapToFrame(Vec2 dst_px) const;
  // Point in normalized source-frame [0,1] -> dst-tensor pixel units.
  Vec2 MapToTensor(Vec2 frame_norm) const;
};

LetterboxInfo ComputeLetterbox(int src_w, int src_h, int dst = 128);

float Sigmoid(float x);
float BoxIoU(float ax0, float ay0, float ax1, float ay1, float bx0, float by0, float bx1, float by1);

// Decode BlazeFace short-range detector outputs into source-canvas
// detections (normalized [0,1] of the 128x128 letterboxed tensor).
//   regressors:     896*16 floats (x,y,w,h + 6 keypoints x2), raw model output
//                   "regressors". reverse_output_order=true, x/y/w/h scales
//                   = 128, no exponential on box size. With
//                   fixed_anchor_size anchors (w=h=1.0) the decode is
//                   cx = raw/128 + anchor_center.
//   classificators: 896 floats (logit), sigmoid applied here.
// Output is sorted by score (desc), weighted-NMS merged (IoU > nms_iou_threshold
// suppresses), matching NonMaxSuppressionCalculator(WEIGHTED, IoU, 0.3).
std::vector<Detection> DecodeDetections(
    const float* regressors, const float* classificators,
    const std::array<FaceAnchor, kNumFaceAnchors>& anchors,
    float score_threshold = 0.5f, float nms_iou_threshold = 0.3f);
}  // namespace kgcore
