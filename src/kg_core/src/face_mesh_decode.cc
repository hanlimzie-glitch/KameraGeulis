#include "kgcore/face_mesh_decode.h"

namespace kgcore {

void DecodeMeshTensor(const float* conv2d_21,
                      std::array<Vec2, kLandmarkCount>* out_pts) {
  if (conv2d_21 == nullptr || out_pts == nullptr) return;
  constexpr float kInv = 1.0f / 192.0f;  // tensor pixel units -> [0,1]
  for (size_t i = 0; i < kLandmarkCount; ++i) {
    const float x = conv2d_21[i * 3 + 0] * kInv;
    const float y = conv2d_21[i * 3 + 1] * kInv;
    // conv2d_21[i*3 + 2] is z (depth); unused in v1.
    (*out_pts)[i] = {x, y};
  }
}

float ReadMeshFaceFlag(const float* conv2d_31) {
  return conv2d_31 != nullptr ? conv2d_31[0] : 0.0f;
}
}  // namespace kgcore
