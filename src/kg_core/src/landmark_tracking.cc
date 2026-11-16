#include "kgcore/landmark_tracking.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>
#include "kgcore/face_detection_decode.h"

namespace kgcore {
namespace {
constexpr float kPi = 3.14159265358979323846f;

Vec2 RightEyeMid(const LandmarkSet& lm) {
  return {(lm.pts[33].x + lm.pts[133].x) * 0.5f,
          (lm.pts[33].y + lm.pts[133].y) * 0.5f};
}

Vec2 LeftEyeMid(const LandmarkSet& lm) {
  return {(lm.pts[362].x + lm.pts[263].x) * 0.5f,
          (lm.pts[362].y + lm.pts[263].y) * 0.5f};
}

// 3x5 bitmap glyphs untuk '0'-'9' (5 baris x 3 bit MSB->LSB).
const uint8_t kDigits3x5[10][5] = {
    {0b111, 0b101, 0b101, 0b101, 0b111},  // 0
    {0b010, 0b110, 0b010, 0b010, 0b111},  // 1
    {0b111, 0b001, 0b111, 0b100, 0b111},  // 2
    {0b111, 0b001, 0b111, 0b001, 0b111},  // 3
    {0b101, 0b101, 0b111, 0b001, 0b001},  // 4
    {0b111, 0b100, 0b111, 0b001, 0b111},  // 5
    {0b111, 0b100, 0b111, 0b101, 0b111},  // 6
    {0b111, 0b001, 0b001, 0b010, 0b010},  // 7
    {0b111, 0b101, 0b111, 0b101, 0b111},  // 8
    {0b111, 0b101, 0b111, 0b001, 0b111},  // 9
};

void PutPixel(std::vector<uint8_t>& buf, int w, int h, int x, int y,
              uint8_t r, uint8_t g, uint8_t b) {
  if (x < 0 || y < 0 || x >= w || y >= h) return;
  uint8_t* p = buf.data() + (static_cast<size_t>(y) * w + x) * 4;
  p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
}

void DrawDisc(std::vector<uint8_t>& buf, int w, int h, int cx, int cy, int rad,
              uint8_t r, uint8_t g, uint8_t b) {
  for (int dy = -rad; dy <= rad; ++dy) {
    for (int dx = -rad; dx <= rad; ++dx) {
      if (dx * dx + dy * dy <= rad * rad) PutPixel(buf, w, h, cx + dx, cy + dy, r, g, b);
    }
  }
}

void DrawLine(std::vector<uint8_t>& buf, int w, int h, float x0, float y0,
              float x1, float y1, uint8_t r, uint8_t g, uint8_t b) {
  const float dx = x1 - x0, dy = y1 - y0;
  const int steps = std::max(1, static_cast<int>(std::ceil(std::max(std::fabs(dx), std::fabs(dy)))));
  for (int i = 0; i <= steps; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(steps);
    const int px = static_cast<int>(std::lround(x0 + dx * t));
    const int py = static_cast<int>(std::lround(y0 + dy * t));
    PutPixel(buf, w, h, px, py, r, g, b);
    PutPixel(buf, w, h, px + 1, py, r, g, b);
  }
}

void DrawNumber(std::vector<uint8_t>& buf, int w, int h, int x0, int y0, int val) {
  char txt[16];
  std::snprintf(txt, sizeof(txt), "%d", val);
  int cx = x0;
  for (int k = 0; txt[k] != '\0'; ++k) {
    if (txt[k] < '0' || txt[k] > '9') continue;
    const int d = txt[k] - '0';
    // Kotak gelap kecil di belakang angka agar terbaca di atas kulit terang.
    for (int ry = -1; ry <= 5; ++ry) {
      for (int rx = -1; rx <= 3; ++rx) {
        PutPixel(buf, w, h, cx + rx, y0 + ry, 0, 0, 0);
      }
    }
    for (int ry = 0; ry < 5; ++ry) {
      for (int rx = 0; rx < 3; ++rx) {
        if ((kDigits3x5[d][ry] >> (2 - rx)) & 1) {
          PutPixel(buf, w, h, cx + rx, y0 + ry, 255, 255, 255);
        }
      }
    }
    cx += 4;
  }
}
}  // namespace

float WrapAnglePi(float rad) {
  while (rad > kPi) rad -= 2.0f * kPi;
  while (rad < -kPi) rad += 2.0f * kPi;
  return rad;
}

float RoiBoxIoU(const AlignedRoi& a, const AlignedRoi& b) {
  if (a.side_px <= 0.0f || b.side_px <= 0.0f) return 0.0f;
  const float ha = a.side_px * 0.5f;
  const float hb = b.side_px * 0.5f;
  return BoxIoU(a.cx_px - ha, a.cy_px - ha, a.cx_px + ha, a.cy_px + ha,
                b.cx_px - hb, b.cy_px - hb, b.cx_px + hb, b.cy_px + hb);
}

float LandmarkBoxVsDetIoU(const LandmarkSet& lm, float det_cx, float det_cy,
                          float det_w, float det_h) {
  if (!lm.valid || det_w <= 0.0f || det_h <= 0.0f) return 0.0f;
  float min_x = 1e9f, min_y = 1e9f, max_x = -1e9f, max_y = -1e9f;
  for (const auto& p : lm.pts) {
    min_x = std::min(min_x, p.x);
    min_y = std::min(min_y, p.y);
    max_x = std::max(max_x, p.x);
    max_y = std::max(max_y, p.y);
  }
  return BoxIoU(min_x, min_y, max_x, max_y,
                det_cx - det_w * 0.5f, det_cy - det_h * 0.5f,
                det_cx + det_w * 0.5f, det_cy + det_h * 0.5f);
}

AlignedRoi TrackRoiFromLandmarks(const LandmarkSet& prev_lm, int frame_w,
                                 int frame_h, float ref_side_px) {
  AlignedRoi roi;
  if (frame_w <= 0 || frame_h <= 0) return roi;
  const Vec2 re = RightEyeMid(prev_lm);
  const Vec2 le = LeftEyeMid(prev_lm);
  const float edx = (le.x - re.x) * static_cast<float>(frame_w);
  const float edy = (le.y - re.y) * static_cast<float>(frame_h);
  roi.angle_rad = std::atan2(edy, edx);

  // Hitung bounding box di ruang ter-rotasi sejajar sumbu mata (seperti
  // FaceLandmarksToRect MediaPipe) supaya tidak ada offset statis yang drift.
  Vec2 sum{};
  for (const auto& p : prev_lm.pts) sum = sum + p;
  const float inv = 1.0f / static_cast<float>(kLandmarkCount);
  const float pivot_x = sum.x * inv * static_cast<float>(frame_w);
  const float pivot_y = sum.y * inv * static_cast<float>(frame_h);

  const float ca = std::cos(-roi.angle_rad);
  const float sa = std::sin(-roi.angle_rad);
  float min_u = 1e9f, max_u = -1e9f, min_v = 1e9f, max_v = -1e9f;
  for (const auto& p : prev_lm.pts) {
    const float dx = p.x * static_cast<float>(frame_w) - pivot_x;
    const float dy = p.y * static_cast<float>(frame_h) - pivot_y;
    const float u = ca * dx - sa * dy;
    const float v = sa * dx + ca * dy;
    min_u = std::min(min_u, u);
    max_u = std::max(max_u, u);
    min_v = std::min(min_v, v);
    max_v = std::max(max_v, v);
  }
  const float cu = (min_u + max_u) * 0.5f;
  const float cv = (min_v + max_v) * 0.5f;
  const float rca = std::cos(roi.angle_rad);
  const float rsa = std::sin(roi.angle_rad);
  roi.cx_px = pivot_x + rca * cu - rsa * cv;
  roi.cy_px = pivot_y + rsa * cu + rca * cv;

  float side = std::max(max_u - min_u, max_v - min_v) * 1.5f;
  if (ref_side_px > 0.0f) {
    side = std::clamp(side, ref_side_px * 0.75f, ref_side_px * 1.35f);
  }
  roi.side_px = std::max(16.0f, side);
  return roi;
}

int AdaptiveDetectInterval(float roi_shift_frac, float angle_delta_rad,
                           float face_flag) {
  if (roi_shift_frac > 0.025f || std::fabs(angle_delta_rad) > 0.08f ||
      face_flag < 5.0f) {
    return kDetectIntervalMin;  // 10 frame saat gerak cepat / keyakinan turun
  }
  if (roi_shift_frac < 0.008f && std::fabs(angle_delta_rad) < 0.02f &&
      face_flag > 10.0f) {
    return kDetectIntervalMax;  // 15 frame saat kepala relatif stabil
  }
  return kDetectIntervalDefault;  // 12 frame normal
}

LandmarkSanityResult CheckLandmarkSanity(const LandmarkSet& lm,
                                         const AlignedRoi& roi, int frame_w,
                                         int frame_h) {
  LandmarkSanityResult res;
  if (!lm.valid || frame_w <= 0 || frame_h <= 0 || roi.side_px <= 1.0f) {
    res.reason = "input invalid";
    return res;
  }

  int out_of_bounds = 0;
  for (const auto& p : lm.pts) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
      res.reason = "koordinat NaN/Inf";
      return res;
    }
    if (p.x < -0.30f || p.x > 1.30f || p.y < -0.30f || p.y > 1.30f) {
      ++out_of_bounds;
    }
  }
  if (out_of_bounds > static_cast<int>(kLandmarkCount / 4)) {
    res.reason = "terlalu banyak titik keluar frame";
    return res;
  }

  // 1) Jarak mata + urutan kiri/kanan di ruang ROI
  const Vec2 re = RightEyeMid(lm);
  const Vec2 le = LeftEyeMid(lm);
  const Vec2 re_px{re.x * frame_w, re.y * frame_h};
  const Vec2 le_px{le.x * frame_w, le.y * frame_h};
  res.eye_dist_px = std::hypot(le_px.x - re_px.x, le_px.y - re_px.y);
  res.eye_ratio = res.eye_dist_px / roi.side_px;
  if (res.eye_dist_px < 8.0f || res.eye_ratio < 0.09f ||
      res.eye_ratio > 0.65f) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "jarak mata tidak wajar (%.1f px, ratio %.3f)",
                  res.eye_dist_px, res.eye_ratio);
    res.reason = buf;
    return res;
  }

  const Vec2 re_c = FrameToMeshCanvas(re_px, roi, frame_w, frame_h);
  const Vec2 le_c = FrameToMeshCanvas(le_px, roi, frame_w, frame_h);
  if (le_c.x - re_c.x <= 0.03f) {
    res.reason = "mata kolaps/terbalik di ruang ROI";
    return res;
  }

  // 2) Rotasi vs ROI
  const float lm_angle = std::atan2(le_px.y - re_px.y, le_px.x - re_px.x);
  res.angle_diff_rad = std::fabs(WrapAnglePi(lm_angle - roi.angle_rad));
  if (res.angle_diff_rad > 0.7854f) {  // > 45 deg
    char buf[96];
    std::snprintf(buf, sizeof(buf), "rotasi vs ROI=%.1f deg (>45)",
                  res.angle_diff_rad * 180.0f / kPi);
    res.reason = buf;
    return res;
  }

  // 3) Bentuk mesh di ruang canvas ROI [0,1]
  float min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f;
  double sum_x = 0.0, sum_y = 0.0;
  for (const auto& p : lm.pts) {
    const Vec2 c = FrameToMeshCanvas({p.x * frame_w, p.y * frame_h}, roi,
                                     frame_w, frame_h);
    min_x = std::min(min_x, c.x);
    max_x = std::max(max_x, c.x);
    min_y = std::min(min_y, c.y);
    max_y = std::max(max_y, c.y);
    sum_x += c.x;
    sum_y += c.y;
  }
  res.mesh_w_canvas = max_x - min_x;
  res.mesh_h_canvas = max_y - min_y;
  res.mesh_aspect = res.mesh_w_canvas / std::max(1e-4f, res.mesh_h_canvas);
  const float cx_c = static_cast<float>(sum_x / kLandmarkCount);
  const float cy_c = static_cast<float>(sum_y / kLandmarkCount);
  res.center_drift = std::hypot(cx_c - 0.5f, cy_c - 0.5f);

  if (res.mesh_w_canvas < 0.16f || res.mesh_h_canvas < 0.18f ||
      res.mesh_w_canvas > 1.35f || res.mesh_h_canvas > 1.35f) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "ukuran mesh ROI tidak wajar (%.2fx%.2f)",
                  res.mesh_w_canvas, res.mesh_h_canvas);
    res.reason = buf;
    return res;
  }
  if (res.mesh_aspect < 0.25f || res.mesh_aspect > 2.0f) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "bentuk mesh kolaps/profil (aspek %.2f)",
                  res.mesh_aspect);
    res.reason = buf;
    return res;
  }
  if (res.center_drift > 0.30f) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "pusat mesh melenceng dari ROI (drift %.2f)",
                  res.center_drift);
    res.reason = buf;
    return res;
  }

  // Urutan vertikal anatomi di ROI yang sudah disejajarkan: dahi < mata < mulut < dagu
  const Vec2 forehead_c = FrameToMeshCanvas(
      {lm.pts[10].x * frame_w, lm.pts[10].y * frame_h}, roi, frame_w, frame_h);
  const Vec2 mouth_c = FrameToMeshCanvas(
      {(lm.pts[13].x + lm.pts[14].x) * 0.5f * frame_w,
       (lm.pts[13].y + lm.pts[14].y) * 0.5f * frame_h},
      roi, frame_w, frame_h);
  const Vec2 chin_c = FrameToMeshCanvas(
      {lm.pts[152].x * frame_w, lm.pts[152].y * frame_h}, roi, frame_w, frame_h);
  const float eye_mid_y = (re_c.y + le_c.y) * 0.5f;
  if ((mouth_c.y - forehead_c.y) < 0.12f || (chin_c.y - eye_mid_y) < 0.12f) {
    res.reason = "struktur vertikal mesh (dahi/mata/mulut/dagu) tidak wajar";
    return res;
  }

  res.ok = true;
  res.reason = "ok";
  return res;
}

bool RenderLandmarkDebugOverlayRgba(const uint8_t* raw_rgba, int w, int h,
                                    const LandmarkSet& lm,
                                    const AlignedRoi* roi,
                                    std::vector<uint8_t>* out_rgba) {
  if (!raw_rgba || !out_rgba || w <= 0 || h <= 0) return false;
  const size_t nbytes = static_cast<size_t>(w) * h * 4;
  out_rgba->assign(raw_rgba, raw_rgba + nbytes);
  // Pastikan alpha 255
  for (size_t i = 3; i < nbytes; i += 4) (*out_rgba)[i] = 255;

  // Kotak ROI rotasi (hijau)
  if (roi && roi->side_px > 1.0f) {
    const float hs = roi->side_px * 0.5f;
    const float ca = std::cos(roi->angle_rad), sa = std::sin(roi->angle_rad);
    auto corner = [&](float ux, float uy) {
      return std::pair<float, float>{roi->cx_px + ca * ux - sa * uy,
                                     roi->cy_px + sa * ux + ca * uy};
    };
    const auto c0 = corner(-hs, -hs), c1 = corner(hs, -hs);
    const auto c2 = corner(hs, hs), c3 = corner(-hs, hs);
    DrawLine(*out_rgba, w, h, c0.first, c0.second, c1.first, c1.second, 60, 255, 90);
    DrawLine(*out_rgba, w, h, c1.first, c1.second, c2.first, c2.second, 60, 255, 90);
    DrawLine(*out_rgba, w, h, c2.first, c2.second, c3.first, c3.second, 60, 255, 90);
    DrawLine(*out_rgba, w, h, c3.first, c3.second, c0.first, c0.second, 60, 255, 90);
  }

  if (!lm.valid) return true;

  // Bbox cyan + 468 titik kuning
  float min_x = 1.0f, min_y = 1.0f, max_x = 0.0f, max_y = 0.0f;
  for (const auto& p : lm.pts) {
    min_x = std::min(min_x, p.x);
    max_x = std::max(max_x, p.x);
    min_y = std::min(min_y, p.y);
    max_y = std::max(max_y, p.y);
    const int px = static_cast<int>(std::lround(p.x * w));
    const int py = static_cast<int>(std::lround(p.y * h));
    DrawDisc(*out_rgba, w, h, px, py, 2, 255, 226, 92);
  }
  const float bx0 = min_x * w, by0 = min_y * h, bx1 = max_x * w, by1 = max_y * h;
  DrawLine(*out_rgba, w, h, bx0, by0, bx1, by0, 0, 229, 255);
  DrawLine(*out_rgba, w, h, bx1, by0, bx1, by1, 0, 229, 255);
  DrawLine(*out_rgba, w, h, bx1, by1, bx0, by1, 0, 229, 255);
  DrawLine(*out_rgba, w, h, bx0, by1, bx0, by0, 0, 229, 255);

  // Indeks anatomi kunci
  static const int kKeyIndices[] = {
      10, 152, 234, 454,          // oval dahi, dagu, pipi R/L
      33, 133, 362, 263,          // sudut mata
      70, 107, 336, 300,          // alis
      1, 48, 278,                 // hidung & lubang hidung
      61, 0, 291, 17              // bibir
  };
  for (int idx : kKeyIndices) {
    const int px = static_cast<int>(std::lround(lm.pts[idx].x * w));
    const int py = static_cast<int>(std::lround(lm.pts[idx].y * h));
    DrawDisc(*out_rgba, w, h, px, py, 3, 255, 70, 70);
    DrawNumber(*out_rgba, w, h, px + 4, py - 2, idx);
  }
  return true;
}

}  // namespace kgcore
