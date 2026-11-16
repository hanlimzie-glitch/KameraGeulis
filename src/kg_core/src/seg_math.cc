#include "kgcore/seg_math.h"
#include "kgcore/face_mesh_topology.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace kgcore {

LetterboxInfo ComputeSegLetterbox(int src_w, int src_h, int dst) {
  // Sama persis dgn ComputeLetterbox BlazeFace (skala min + pad tengah).
  return ComputeLetterbox(src_w, src_h, dst);
}

bool UnletterboxPoint(const LetterboxInfo& lb, float mx, float my, float* sx,
                      float* sy) {
  if (lb.scale <= 0.0f || lb.src_w <= 0 || lb.src_h <= 0) return false;
  const float cx = mx - lb.pad_x;
  const float cy = my - lb.pad_y;
  const float sw = static_cast<float>(lb.src_w) * lb.scale;
  const float sh = static_cast<float>(lb.src_h) * lb.scale;
  if (cx < 0.0f || cy < 0.0f || cx > sw || cy > sh) {
    return false;  // di area pad
  }
  if (sx) *sx = cx / lb.scale;
  if (sy) *sy = cy / lb.scale;
  return true;
}

void LetterboxToInput(const uint8_t* src, int w, int h, int stride, bool bgra,
                      const LetterboxInfo& lb, float* out, int dst) {
  if (!src || !out || w <= 0 || h <= 0 || dst <= 0) return;
  // Latar pad = replikasi tepi konten; isi area konten dgn sampling nearest.
  const int ch = 3;
  for (int y = 0; y < dst; ++y) {
    // model y -> src y (clamp ke tepi konten bila di pad)
    int sy = static_cast<int>((static_cast<float>(y) - lb.pad_y + 0.5f) /
                              lb.scale);
    if (sy < 0) sy = 0;
    if (sy >= h) sy = h - 1;
    const uint8_t* row = src + static_cast<size_t>(sy) * stride;
    for (int x = 0; x < dst; ++x) {
      int sx = static_cast<int>((static_cast<float>(x) - lb.pad_x + 0.5f) /
                                lb.scale);
      if (sx < 0) sx = 0;
      if (sx >= w) sx = w - 1;
      const uint8_t* p = row + static_cast<size_t>(sx) * 4;
      const uint8_t r = bgra ? p[2] : p[0];
      const uint8_t g = p[1];
      const uint8_t b = bgra ? p[0] : p[2];
      float* o = out + (static_cast<size_t>(y) * dst + x) * ch;
      o[0] = static_cast<float>(r) / 127.5f - 1.0f;
      o[1] = static_cast<float>(g) / 127.5f - 1.0f;
      o[2] = static_cast<float>(b) / 127.5f - 1.0f;
    }
  }
}

bool SegNeedsSoftmax(const float* probs, int n_pix) {
  if (!probs || n_pix <= 0) return false;
  // Sampel piksel (lompat besar) - jumlah kelas ~1 bila sudah probabilitas.
  double dev = 0.0;
  int n = 0;
  const int stride = std::max(1, n_pix / 512);
  for (int i = 0; i < n_pix; i += stride) {
    const float* p = probs + static_cast<size_t>(i) * kSegClasses;
    float s = 0.0f;
    for (int c = 0; c < kSegClasses; ++c) s += p[c];
    dev += std::fabs(static_cast<double>(s) - 1.0);
    ++n;
  }
  return n > 0 && (dev / n) > 0.02;
}

void SegSoftmaxInPlace(float* logits, int n_pix, int n_class) {
  if (!logits || n_pix <= 0 || n_class <= 0) return;
  for (int i = 0; i < n_pix; ++i) {
    float* p = logits + static_cast<size_t>(i) * n_class;
    float mx = p[0];
    for (int c = 1; c < n_class; ++c) mx = std::max(mx, p[c]);
    float sum = 0.0f;
    for (int c = 0; c < n_class; ++c) {
      p[c] = std::exp(p[c] - mx);
      sum += p[c];
    }
    if (sum > 0.0f) {
      for (int c = 0; c < n_class; ++c) p[c] /= sum;
    }
  }
}

void SegActivate(float* data, int n_pix, int n_class) {
  (void)n_class;  // kelas selalu 6 pada model ini
  if (SegNeedsSoftmax(data, n_pix)) SegSoftmaxInPlace(data, n_pix, kSegClasses);
}

void SegSkinProbability(const float* probs, int n_pix, float* out) {
  if (!probs || !out || n_pix <= 0) return;
  for (int i = 0; i < n_pix; ++i) {
    const float* p = probs + static_cast<size_t>(i) * kSegClasses;
    float s = p[kSegClassBodySkin] + p[kSegClassFaceSkin];
    out[i] = s > 1.0f ? 1.0f : (s < 0.0f ? 0.0f : s);
  }
}

void SegArgmax(const float* probs, int n_pix, uint8_t* out, int n_class) {
  if (!probs || !out || n_pix <= 0) return;
  for (int i = 0; i < n_pix; ++i) {
    const float* p = probs + static_cast<size_t>(i) * n_class;
    int best = 0;
    for (int c = 1; c < n_class; ++c) {
      if (p[c] > p[best]) best = c;
    }
    out[i] = static_cast<uint8_t>(best);
  }
}

void SegComputeStats(const uint8_t* argmax_map, int n_pix, SegClassStats* out) {
  if (!argmax_map || !out || n_pix <= 0) return;
  *out = SegClassStats{};
  out->total = n_pix;
  for (int i = 0; i < n_pix; ++i) {
    const uint8_t c = argmax_map[i];
    if (c < kSegClasses) ++out->counts[c];
  }
  for (int c = 0; c < kSegClasses; ++c) {
    out->pct[c] = 100.0f * static_cast<float>(out->counts[c]) /
                  static_cast<float>(n_pix);
  }
}

void SegClassColor(uint8_t cls, uint8_t rgba[4]) {
  switch (cls) {
    case 0: rgba[0] = 0;   rgba[1] = 0;   rgba[2] = 0;   break;  // latar
    case 1: rgba[0] = 60;  rgba[1] = 90;  rgba[2] = 220; break;  // rambut
    case 2: rgba[0] = 40;  rgba[1] = 200; rgba[2] = 90;  break;  // kulit tubuh
    case 3: rgba[0] = 255; rgba[1] = 120; rgba[2] = 40;  break;  // kulit wajah
    case 4: rgba[0] = 170; rgba[1] = 70;  rgba[2] = 220; break;  // pakaian
    case 5: rgba[0] = 240; rgba[1] = 220; rgba[2] = 40;  break;  // aksesori
    default: rgba[0] = 255; rgba[1] = 0; rgba[2] = 255; break;   // ???
  }
  rgba[3] = 255;
}

namespace {
inline float BilinearGuide(const uint8_t* guide, int gw, int gh, float x,
                           float y) {
  // x,y dalam piksel guide (boleh di luar -> clamp)
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x > gw - 1) x = static_cast<float>(gw - 1);
  if (y > gh - 1) y = static_cast<float>(gh - 1);
  const int x0 = static_cast<int>(x);
  const int y0 = static_cast<int>(y);
  const int x1 = std::min(x0 + 1, gw - 1);
  const int y1 = std::min(y0 + 1, gh - 1);
  const float fx = x - static_cast<float>(x0);
  const float fy = y - static_cast<float>(y0);
  const float a = guide[static_cast<size_t>(y0) * gw + x0];
  const float b = guide[static_cast<size_t>(y0) * gw + x1];
  const float c = guide[static_cast<size_t>(y1) * gw + x0];
  const float d = guide[static_cast<size_t>(y1) * gw + x1];
  return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
}
}  // namespace

SegPoseAnchor ComputeSegPoseAnchor(const LandmarkSet& lm, int w, int h) {
  SegPoseAnchor out;
  if (!lm.valid || w <= 0 || h <= 0) return out;
  float xy[kLandmarkCount * 2];
  for (size_t i = 0; i < kLandmarkCount; ++i) {
    xy[i * 2] = lm.pts[i].x * static_cast<float>(w);
    xy[i * 2 + 1] = lm.pts[i].y * static_cast<float>(h);
  }
  const FaceEyeBasis b = ComputeFaceEyeBasis(xy);
  if (b.valid && b.iod > 1.0f) {
    out.cx_px = b.eye_mid_x;
    out.cy_px = b.eye_mid_y;
    out.iod_px = b.iod;
    out.angle_rad = std::atan2(b.uy, b.ux);
    out.valid = true;
    return out;
  }
  // Fallback ke centroid apabila mata belum terisi.
  double sx = 0.0, sy = 0.0;
  for (const auto& p : lm.pts) { sx += p.x; sy += p.y; }
  out.cx_px = static_cast<float>(sx / kLandmarkCount * w);
  out.cy_px = static_cast<float>(sy / kLandmarkCount * h);
  out.iod_px = 0.0f;
  out.angle_rad = 0.0f;
  out.valid = true;
  return out;
}

SegSimilarity ComputeSegSimilarity(const SegPoseAnchor& anchor,
                                   const SegPoseAnchor& curr) {
  SegSimilarity sim;
  if (!anchor.valid || !curr.valid) return sim;
  sim.curr_cx = curr.cx_px;
  sim.curr_cy = curr.cy_px;
  sim.anchor_cx = anchor.cx_px;
  sim.anchor_cy = anchor.cy_px;
  if (anchor.iod_px > 4.0f && curr.iod_px > 4.0f) {
    const float scale = std::clamp(anchor.iod_px / curr.iod_px, 0.5f, 2.0f);
    const float dtheta = anchor.angle_rad - curr.angle_rad;
    sim.c = scale * std::cos(dtheta);
    sim.s = scale * std::sin(dtheta);
  } else {
    sim.c = 1.0f;
    sim.s = 0.0f;
  }
  sim.valid = true;
  return sim;
}

float ComputeSegMotionAttenuation(float seg_age_frames,
                                  float head_speed_px_per_frame) {
  if (seg_age_frames <= 1.0f || head_speed_px_per_frame <= 2.0f) return 1.0f;
  const float age_factor = std::clamp((seg_age_frames - 1.0f) / 8.0f, 0.0f, 1.0f);
  const float speed_factor =
      std::clamp((head_speed_px_per_frame - 2.0f) / 18.0f, 0.0f, 1.0f);
  return std::clamp(1.0f - age_factor * speed_factor, 0.0f, 1.0f);
}

namespace {
inline void ApplySimilarityToFramePx(const SegSimilarity& sim, float x, float y,
                                     float* sx, float* sy) {
  if (!sim.valid) {
    *sx = x;
    *sy = y;
    return;
  }
  const float dx = x - sim.curr_cx;
  const float dy = y - sim.curr_cy;
  *sx = sim.anchor_cx + sim.c * dx - sim.s * dy;
  *sy = sim.anchor_cy + sim.s * dx + sim.c * dy;
}
}  // namespace

void SegProbsToMaskSkinSimilarity(const float* probs, const LetterboxInfo& lb,
                                  int mw, int mh, float* skin_out,
                                  const SegSimilarity& sim, int dst) {
  if (!probs || !skin_out || mw <= 0 || mh <= 0 || dst <= 0) return;
  for (int y = 0; y < mh; ++y) {
    const float fy = (static_cast<float>(y) + 0.5f) / mh * lb.src_h;
    for (int x = 0; x < mw; ++x) {
      const float fx = (static_cast<float>(x) + 0.5f) / mw * lb.src_w;
      float sx = fx, sy = fy;
      ApplySimilarityToFramePx(sim, fx, fy, &sx, &sy);
      const float mx = sx * lb.scale + lb.pad_x;
      const float my = sy * lb.scale + lb.pad_y;
      const float px = std::clamp(mx - 0.5f, 0.0f, static_cast<float>(dst) - 1.001f);
      const float py = std::clamp(my - 0.5f, 0.0f, static_cast<float>(dst) - 1.001f);
      const int x0 = static_cast<int>(px), y0 = static_cast<int>(py);
      const int x1 = std::min(x0 + 1, dst - 1), y1 = std::min(y0 + 1, dst - 1);
      const float ax = px - x0, ay = py - y0;
      auto skin_at = [&](int xx, int yy) {
        const float* p = probs + (static_cast<size_t>(yy) * dst + xx) * kSegClasses;
        const float v = p[kSegClassBodySkin] + p[kSegClassFaceSkin];
        return v > 1.0f ? 1.0f : v;
      };
      skin_out[static_cast<size_t>(y) * mw + x] =
          (skin_at(x0, y0) * (1 - ax) + skin_at(x1, y0) * ax) * (1 - ay) +
          (skin_at(x0, y1) * (1 - ax) + skin_at(x1, y1) * ax) * ay;
    }
  }
}

void SegProbsToMaskClassSimilarity(const float* probs, const LetterboxInfo& lb,
                                   int class_id, int mw, int mh, float* out,
                                   const SegSimilarity& sim, int dst) {
  if (!probs || !out || class_id < 0 || class_id >= kSegClasses || mw <= 0 ||
      mh <= 0 || dst <= 0) return;
  for (int y = 0; y < mh; ++y) {
    const float fy = (static_cast<float>(y) + 0.5f) / mh * lb.src_h;
    for (int x = 0; x < mw; ++x) {
      const float fx = (static_cast<float>(x) + 0.5f) / mw * lb.src_w;
      float sx = fx, sy = fy;
      ApplySimilarityToFramePx(sim, fx, fy, &sx, &sy);
      const float px = std::clamp(sx * lb.scale + lb.pad_x - 0.5f,
                                  0.0f, static_cast<float>(dst) - 1.001f);
      const float py = std::clamp(sy * lb.scale + lb.pad_y - 0.5f,
                                  0.0f, static_cast<float>(dst) - 1.001f);
      const int x0 = static_cast<int>(px), y0 = static_cast<int>(py);
      const int x1 = std::min(x0 + 1, dst - 1), y1 = std::min(y0 + 1, dst - 1);
      const float ax = px - x0, ay = py - y0;
      auto at = [&](int xx, int yy) {
        return probs[(static_cast<size_t>(yy) * dst + xx) * kSegClasses + class_id];
      };
      out[static_cast<size_t>(y) * mw + x] =
          (at(x0, y0) * (1 - ax) + at(x1, y0) * ax) * (1 - ay) +
          (at(x0, y1) * (1 - ax) + at(x1, y1) * ax) * ay;
    }
  }
}

void SegProbsToMaskSkinAndFaceSimilarity(const float* probs,
                                         const LetterboxInfo& lb, int mw,
                                         int mh, float* skin_out,
                                         float* face_out,
                                         const SegSimilarity& sim, int dst) {
  if (!probs || !skin_out || !face_out || mw <= 0 || mh <= 0 || dst <= 0) return;
  for (int y = 0; y < mh; ++y) {
    const float fy = (static_cast<float>(y) + 0.5f) / mh * lb.src_h;
    for (int x = 0; x < mw; ++x) {
      const float fx = (static_cast<float>(x) + 0.5f) / mw * lb.src_w;
      float sx = fx, sy = fy;
      ApplySimilarityToFramePx(sim, fx, fy, &sx, &sy);
      const float px = std::clamp(sx * lb.scale + lb.pad_x - 0.5f,
                                  0.0f, static_cast<float>(dst) - 1.001f);
      const float py = std::clamp(sy * lb.scale + lb.pad_y - 0.5f,
                                  0.0f, static_cast<float>(dst) - 1.001f);
      const int x0 = static_cast<int>(px), y0 = static_cast<int>(py);
      const int x1 = std::min(x0 + 1, dst - 1), y1 = std::min(y0 + 1, dst - 1);
      const float ax = px - x0, ay = py - y0;
      auto at = [&](int xx, int yy, int cls) {
        return probs[(static_cast<size_t>(yy) * dst + xx) * kSegClasses + cls];
      };
      const float f00 = at(x0, y0, kSegClassFaceSkin), f10 = at(x1, y0, kSegClassFaceSkin);
      const float f01 = at(x0, y1, kSegClassFaceSkin), f11 = at(x1, y1, kSegClassFaceSkin);
      const float b00 = at(x0, y0, kSegClassBodySkin) + f00;
      const float b10 = at(x1, y0, kSegClassBodySkin) + f10;
      const float b01 = at(x0, y1, kSegClassBodySkin) + f01;
      const float b11 = at(x1, y1, kSegClassBodySkin) + f11;
      const float face = (f00 * (1 - ax) + f10 * ax) * (1 - ay) +
                         (f01 * (1 - ax) + f11 * ax) * ay;
      const float skin = (b00 * (1 - ax) + b10 * ax) * (1 - ay) +
                         (b01 * (1 - ax) + b11 * ax) * ay;
      skin_out[static_cast<size_t>(y) * mw + x] = std::min(1.0f, skin);
      face_out[static_cast<size_t>(y) * mw + x] = face;
    }
  }
}

void SegProbabilitiesToFrameArgmaxSimilarity(const float* probs,
                                             const LetterboxInfo& lb, int w,
                                             int h, uint8_t* classes,
                                             const SegSimilarity& sim,
                                             int dst) {
  if (!probs || !classes || w <= 0 || h <= 0 || dst <= 0) return;
  for (int y = 0; y < h; ++y) {
    const float fy = static_cast<float>(y) + 0.5f;
    for (int x = 0; x < w; ++x) {
      const float fx = static_cast<float>(x) + 0.5f;
      float sx = fx, sy = fy;
      ApplySimilarityToFramePx(sim, fx, fy, &sx, &sy);
      const float mx = sx * lb.scale + lb.pad_x;
      const float my = sy * lb.scale + lb.pad_y;
      if (mx < 0.0f || my < 0.0f || mx >= dst || my >= dst) {
        classes[static_cast<size_t>(y) * w + x] = 0;
        continue;
      }
      const int ix = std::clamp(static_cast<int>(mx), 0, dst - 1);
      const int iy = std::clamp(static_cast<int>(my), 0, dst - 1);
      const float* p = probs + (static_cast<size_t>(iy) * dst + ix) * kSegClasses;
      int best = 0;
      for (int c = 1; c < kSegClasses; ++c)
        if (p[c] > p[best]) best = c;
      classes[static_cast<size_t>(y) * w + x] = static_cast<uint8_t>(best);
    }
  }
}

void SegProbsToMaskSkin(const float* probs, const LetterboxInfo& lb, int mw,
                        int mh, float* skin_out, int dst, float shift_x_px,
                        float shift_y_px) {
  if (!probs || !skin_out || mw <= 0 || mh <= 0 || dst <= 0) return;
  // Mask menutupi FRAME; frame -> model px lewat letterbox (MapToTensor),
  // lalu bilinear pada peta probabilitas model.
  for (int y = 0; y < mh; ++y) {
    const float vn = (static_cast<float>(y) + 0.5f) / static_cast<float>(mh) -
                     shift_y_px / static_cast<float>(lb.src_h);
    for (int x = 0; x < mw; ++x) {
      const float un = (static_cast<float>(x) + 0.5f) / static_cast<float>(mw) -
                       shift_x_px / static_cast<float>(lb.src_w);
      // frame_norm -> model px (termasuk pad; selalu di dalam konten).
      const float mx = un * static_cast<float>(lb.src_w) * lb.scale + lb.pad_x;
      const float my = vn * static_cast<float>(lb.src_h) * lb.scale + lb.pad_y;
      const float px =
          std::min(static_cast<float>(dst) - 1.001f, std::max(0.0f, mx - 0.5f));
      const float py =
          std::min(static_cast<float>(dst) - 1.001f, std::max(0.0f, my - 0.5f));
      const int x0 = static_cast<int>(px);
      const int y0 = static_cast<int>(py);
      const int x1 = std::min(x0 + 1, dst - 1);
      const int y1 = std::min(y0 + 1, dst - 1);
      const float ax = px - static_cast<float>(x0);
      const float ay = py - static_cast<float>(y0);
      auto skin_at = [&](int xx, int yy) {
        const float* p =
            probs + (static_cast<size_t>(yy) * dst + xx) * kSegClasses;
        const float v = p[kSegClassBodySkin] + p[kSegClassFaceSkin];
        return v > 1.0f ? 1.0f : v;
      };
      const float s =
          (skin_at(x0, y0) * (1 - ax) + skin_at(x1, y0) * ax) * (1 - ay) +
          (skin_at(x0, y1) * (1 - ax) + skin_at(x1, y1) * ax) * ay;
      skin_out[static_cast<size_t>(y) * mw + x] = s;
    }
  }
}

void SegProbsToMaskClass(const float* probs, const LetterboxInfo& lb,
                         int class_id, int mw, int mh, float* out, int dst,
                         float shift_x_px, float shift_y_px) {
  if (!probs || !out || class_id < 0 || class_id >= kSegClasses || mw <= 0 ||
      mh <= 0 || dst <= 0) return;
  for (int y = 0; y < mh; ++y) {
    const float vn = (static_cast<float>(y) + 0.5f) / mh -
                     shift_y_px / static_cast<float>(lb.src_h);
    for (int x = 0; x < mw; ++x) {
      const float un = (static_cast<float>(x) + 0.5f) / mw -
                       shift_x_px / static_cast<float>(lb.src_w);
      const float mx = un * lb.src_w * lb.scale + lb.pad_x;
      const float my = vn * lb.src_h * lb.scale + lb.pad_y;
      const float px = std::clamp(mx - 0.5f, 0.0f, static_cast<float>(dst) - 1.001f);
      const float py = std::clamp(my - 0.5f, 0.0f, static_cast<float>(dst) - 1.001f);
      const int x0 = static_cast<int>(px), y0 = static_cast<int>(py);
      const int x1 = std::min(x0 + 1, dst - 1), y1 = std::min(y0 + 1, dst - 1);
      const float ax = px - x0, ay = py - y0;
      auto at = [&](int xx, int yy) {
        return probs[(static_cast<size_t>(yy) * dst + xx) * kSegClasses + class_id];
      };
      out[static_cast<size_t>(y) * mw + x] =
          (at(x0,y0) * (1-ax) + at(x1,y0) * ax) * (1-ay) +
          (at(x0,y1) * (1-ax) + at(x1,y1) * ax) * ay;
    }
  }
}

void SegProbabilitiesToFrameArgmax(const float* probs, const LetterboxInfo& lb,
                                   int w, int h, uint8_t* classes, int dst,
                                   float shift_x_px, float shift_y_px) {
  if (!probs || !classes || w <= 0 || h <= 0 || dst <= 0) return;
  for (int y = 0; y < h; ++y) {
    const float vn = (static_cast<float>(y) + 0.5f) / h - shift_y_px / lb.src_h;
    for (int x = 0; x < w; ++x) {
      const float un = (static_cast<float>(x) + 0.5f) / w - shift_x_px / lb.src_w;
      const float mx = un * lb.src_w * lb.scale + lb.pad_x;
      const float my = vn * lb.src_h * lb.scale + lb.pad_y;
      // Di luar konten letterbox = background; tidak tarik piksel pad ke frame.
      if (mx < 0.0f || my < 0.0f || mx >= dst || my >= dst) {
        classes[static_cast<size_t>(y) * w + x] = 0;  // background
        continue;
      }
      const int sx = std::clamp(static_cast<int>(mx), 0, dst - 1);
      const int sy = std::clamp(static_cast<int>(my), 0, dst - 1);
      const float* p = probs + (static_cast<size_t>(sy) * dst + sx) * kSegClasses;
      int best = 0;
      for (int c = 1; c < kSegClasses; ++c)
        if (p[c] > p[best]) best = c;
      classes[static_cast<size_t>(y) * w + x] = static_cast<uint8_t>(best);
    }
  }
}

void SegProbsToMaskSkinAndFace(const float* probs, const LetterboxInfo& lb,
                               int mw, int mh, float* skin_out,
                               float* face_out, int dst, float shift_x_px,
                               float shift_y_px) {
  if (!probs || !skin_out || !face_out || mw <= 0 || mh <= 0 || dst <= 0) return;
  for (int y = 0; y < mh; ++y) {
    const float vn = (static_cast<float>(y) + 0.5f) / mh - shift_y_px / lb.src_h;
    for (int x = 0; x < mw; ++x) {
      const float un = (static_cast<float>(x) + 0.5f) / mw - shift_x_px / lb.src_w;
      const float px = std::clamp(un * lb.src_w * lb.scale + lb.pad_x - 0.5f,
                                  0.0f, static_cast<float>(dst) - 1.001f);
      const float py = std::clamp(vn * lb.src_h * lb.scale + lb.pad_y - 0.5f,
                                  0.0f, static_cast<float>(dst) - 1.001f);
      const int x0 = static_cast<int>(px), y0 = static_cast<int>(py);
      const int x1 = std::min(x0 + 1, dst - 1), y1 = std::min(y0 + 1, dst - 1);
      const float ax = px - x0, ay = py - y0;
      auto at = [&](int xx, int yy, int cls) {
        return probs[(static_cast<size_t>(yy) * dst + xx) * kSegClasses + cls];
      };
      const float f00 = at(x0,y0,kSegClassFaceSkin), f10 = at(x1,y0,kSegClassFaceSkin);
      const float f01 = at(x0,y1,kSegClassFaceSkin), f11 = at(x1,y1,kSegClassFaceSkin);
      const float b00 = at(x0,y0,kSegClassBodySkin) + f00;
      const float b10 = at(x1,y0,kSegClassBodySkin) + f10;
      const float b01 = at(x0,y1,kSegClassBodySkin) + f01;
      const float b11 = at(x1,y1,kSegClassBodySkin) + f11;
      float face = (f00*(1-ax)+f10*ax)*(1-ay)+(f01*(1-ax)+f11*ax)*ay;
      float skin = (b00*(1-ax)+b10*ax)*(1-ay)+(b01*(1-ax)+b11*ax)*ay;
      skin_out[static_cast<size_t>(y)*mw+x] = std::min(1.0f, skin);
      face_out[static_cast<size_t>(y)*mw+x] = face;
    }
  }
}

void JointBilateralUpsample(const float* coarse, int cw, int ch,
                            const uint8_t* guide_full, int gw, int gh,
                            int dst_w, int dst_h, float* out, float sigma_space,
                            float sigma_range) {
  if (!coarse || !out || cw <= 0 || ch <= 0 || dst_w <= 0 || dst_h <= 0) return;
  const float inv_spat2 = 1.0f / (2.0f * sigma_space * sigma_space);
  const float inv_range2 = 1.0f / (2.0f * sigma_range * sigma_range);
  // Radius jendela (piksel tujuan) ~ 2 sigma; tetangga kasar dihitung lewat
  // posisinya pada ruang tujuan.
  const float step_x = static_cast<float>(dst_w) / static_cast<float>(cw);
  const float step_y = static_cast<float>(dst_h) / static_cast<float>(ch);
  const int rad = std::max(1, static_cast<int>(std::ceil(2.0f * sigma_space /
                                                         std::min(step_x, step_y))));
  for (int y = 0; y < dst_h; ++y) {
    const float gy =
        (static_cast<float>(y) + 0.5f) * static_cast<float>(gh) / dst_h - 0.5f;
    for (int x = 0; x < dst_w; ++x) {
      const float gx =
          (static_cast<float>(x) + 0.5f) * static_cast<float>(gw) / dst_w - 0.5f;
      const float g_dst = guide_full
          ? BilinearGuide(guide_full, gw, gh, gx, gy) / 255.0f
          : 0.5f;
      // indeks kasar yang menaungi piksel ini
      const int cx0 = std::min(cw - 1, std::max(0, static_cast<int>(
              (static_cast<float>(x) + 0.5f) / step_x - 0.5f)));
      const int cy0 = std::min(ch - 1, std::max(0, static_cast<int>(
              (static_cast<float>(y) + 0.5f) / step_y - 0.5f)));
      float wsum = 0.0f, vsum = 0.0f;
      for (int dy = -rad; dy <= rad; ++dy) {
        const int cy = cy0 + dy;
        if (cy < 0 || cy >= ch) continue;
        for (int dx = -rad; dx <= rad; ++dx) {
          const int cx = cx0 + dx;
          if (cx < 0 || cx >= cw) continue;
          const float px = (static_cast<float>(cx) + 0.5f) * step_x;
          const float py = (static_cast<float>(cy) + 0.5f) * step_y;
          const float d2 = (px - static_cast<float>(x)) * (px - static_cast<float>(x)) +
                           (py - static_cast<float>(y)) * (py - static_cast<float>(y));
          float g_c = 0.5f;
          if (guide_full) {
            // panduan pada pusat tetangga kasar (di resolusi penuh)
            const float cxf =
                (static_cast<float>(cx) + 0.5f) * static_cast<float>(gw) / cw -
                0.5f;
            const float cyf =
                (static_cast<float>(cy) + 0.5f) * static_cast<float>(gh) / ch -
                0.5f;
            g_c = BilinearGuide(guide_full, gw, gh, cxf, cyf) / 255.0f;
          }
          const float dg = g_dst - g_c;
          const float w =
              std::exp(-d2 * inv_spat2 - (dg * dg) * inv_range2);
          wsum += w;
          vsum += w * coarse[static_cast<size_t>(cy) * cw + cx];
        }
      }
      out[static_cast<size_t>(y) * dst_w + x] =
          wsum > 0.0f ? vsum / wsum
                      : coarse[static_cast<size_t>(cy0) * cw + cx0];
    }
  }
}

void UnionAndExclude(const uint8_t* a, const uint8_t* b,
                     const uint8_t* exclude, int n, uint8_t* out) {
  if (!a || !b || !out || n <= 0) return;
  for (int i = 0; i < n; ++i) {
    uint8_t v = a[i] > b[i] ? a[i] : b[i];
    if (exclude && exclude[i] != 0) v = 0;
    out[i] = v;
  }
}

}  // namespace kgcore
