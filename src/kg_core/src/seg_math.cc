#include "kgcore/seg_math.h"

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

void SegProbsToMaskSkin(const float* probs, const LetterboxInfo& lb, int mw,
                        int mh, float* skin_out, int dst) {
  if (!probs || !skin_out || mw <= 0 || mh <= 0 || dst <= 0) return;
  // Mask menutupi FRAME; frame -> model px lewat letterbox (MapToTensor),
  // lalu bilinear pada peta probabilitas model.
  for (int y = 0; y < mh; ++y) {
    const float vn = (static_cast<float>(y) + 0.5f) / static_cast<float>(mh);
    for (int x = 0; x < mw; ++x) {
      const float un = (static_cast<float>(x) + 0.5f) / static_cast<float>(mw);
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
