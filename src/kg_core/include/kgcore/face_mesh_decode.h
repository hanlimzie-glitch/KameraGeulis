#pragma once
#include <array>
#include "kgcore/landmark.h"

namespace kgcore {
// face_mesh.onnx (MediaPipe "facemesh_facecontours_faceflag_blaze", 468
// points, the legacy facecontours model; see docs/LANDMARKS.md §2.2).
//   input:  "input_1"  [1,192,192,3] f32 RGB, range [0,1]
//   output: "conv2d_21" [1,1,1,1404] f32 = 468 * (x,y,z) in 192x192 tensor
//           pixel units; we keep only x,y and rescale to [0,1] canvas coords
//   output: "conv2d_31" [1,1,1,1]   f32 face flag (presence of a face)
//
// z (depth) is intentionally discarded in v1 (no effect consumes it).
void DecodeMeshTensor(const float* conv2d_21, std::array<Vec2, kLandmarkCount>* out_pts);

// Raw face flag passthrough (presence score of a face in the ROI). The v1
// gate uses `> 0.0f` (valid face in ROI) combined with the detector score;
// see docs/LANDMARKS.md §6.
float ReadMeshFaceFlag(const float* conv2d_31);
}  // namespace kgcore
