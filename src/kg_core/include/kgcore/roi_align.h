#pragma once
#include "kgcore/geometry.h"

namespace kgcore {
// Square, rotation-aligned region of interest used to crop the face for the
// 468-pt mesh model (MediaPipe DetectionsToRects + RectTransformation:
// start/end keypoint indices 0/1, target angle 0, square_long side,
// scale 1.5).
//
// Convention: all pixel coordinates are in the SOURCE frame (pixels, origin
// top-left, y down). `angle_rad` rotates the crop so the kp0->kp1 axis lies
// horizontally; the crop is sampled un-rotated into the mesh tensor.
struct AlignedRoi {
  float cx_px = 0.0f;
  float cy_px = 0.0f;
  float side_px = 1.0f;
  float angle_rad = 0.0f;
};

// box_*: detection box in source-frame pixels. kp0/kp1: eye keypoints
// (BlazeFace indices 0 and 1) in source-frame pixels. `scale` is the
// rect transformation enlargement (MediaPipe uses 1.5).
AlignedRoi ComputeAlignedRoi(float box_cx_px, float box_cy_px, float box_w_px,
                             float box_h_px, Vec2 kp0_px, Vec2 kp1_px,
                             float scale = 1.5f);

// Map a point from the mesh canvas (normalized [0,1] over the 192x192 model
// input) back to normalized [0,1] coordinates in the source frame.
Vec2 MeshCanvasToFrame(Vec2 pt_norm, const AlignedRoi& roi, int frame_w, int frame_h);

// Inverse of MeshCanvasToFrame (used by tests and diagnostics).
Vec2 FrameToMeshCanvas(Vec2 pt_px, const AlignedRoi& roi, int frame_w, int frame_h);
}  // namespace kgcore
