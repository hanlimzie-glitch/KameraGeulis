#include "kgcore/roi_align.h"

#include <algorithm>
#include <cmath>

namespace kgcore {

AlignedRoi ComputeAlignedRoi(float box_cx_px, float box_cy_px, float box_w_px,
                             float box_h_px, Vec2 kp0_px, Vec2 kp1_px,
                             float scale) {
  AlignedRoi roi;
  roi.cx_px = box_cx_px;
  roi.cy_px = box_cy_px;
  roi.side_px = std::max(box_w_px, box_h_px) * scale;
  roi.angle_rad = std::atan2(kp1_px.y - kp0_px.y, kp1_px.x - kp0_px.x);
  return roi;
}

Vec2 FrameToMeshCanvas(Vec2 pt_px, const AlignedRoi& roi, int frame_w, int frame_h) {
  // Work in pixel space with a square (aspect-corrected) coordinate system:
  // translate to ROI center, un-rotate by -angle (so the crop axis becomes
  // horizontal), normalize to [0,1] over the square side.
  const float dx = pt_px.x - roi.cx_px;
  const float dy = pt_px.y - roi.cy_px;
  const float ca = std::cos(-roi.angle_rad);
  const float sa = std::sin(-roi.angle_rad);
  const float ux = ca * dx - sa * dy;
  const float uy = sa * dx + ca * dy;
  const float inv = roi.side_px > 0.0f ? 1.0f / roi.side_px : 0.0f;
  return {0.5f + ux * inv, 0.5f + uy * inv};
}

Vec2 MeshCanvasToFrame(Vec2 pt_norm, const AlignedRoi& roi, int frame_w, int frame_h) {
  const float ux = (pt_norm.x - 0.5f) * roi.side_px;
  const float uy = (pt_norm.y - 0.5f) * roi.side_px;
  const float ca = std::cos(roi.angle_rad);
  const float sa = std::sin(roi.angle_rad);
  const float dx = ca * ux - sa * uy;
  const float dy = sa * ux + ca * uy;
  const float fx = roi.cx_px + dx;
  const float fy = roi.cy_px + dy;
  const float nx = frame_w > 0 ? fx / static_cast<float>(frame_w) : 0.0f;
  const float ny = frame_h > 0 ? fy / static_cast<float>(frame_h) : 0.0f;
  return {nx, ny};
}
}  // namespace kgcore
