#include "kgcore/skin_mask.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace kgcore {

// ---------------------------------------------------------------------------
// Topologi resmi MediaPipe - lihat skin_mask.h (sumber face_mesh_connections.py)
// ---------------------------------------------------------------------------
namespace topology {
const int kFaceOvalRing[36] = {10,  338, 297, 332, 284, 251, 389, 356, 454, 323,
                               361, 288, 397, 365, 379, 378, 400, 377, 152, 148,
                               176, 149, 150, 136, 172, 58,  132, 93,  234, 127,
                               162, 21,  54,  103, 67,  109};
const int kLeftEyeRing[16] = {263, 249, 390, 373, 374, 380, 381, 382,
                              362, 398, 384, 385, 386, 387, 388, 466};
const int kRightEyeRing[16] = {33, 7,   163, 144, 145, 153, 154, 155,
                               133, 173, 157, 158, 159, 160, 161, 246};
const int kLipsOuterRing[20] = {61, 146, 91,  181, 84,  17,  314, 405, 321, 375,
                                291, 409, 270, 269, 267, 0,   37,  39,  40,  185};
const int kBrowChains[4][5] = {{276, 283, 282, 295, 285},
                               {300, 293, 334, 296, 336},
                               {46, 53, 52, 65, 55},
                               {70, 63, 105, 66, 107}};
// Titik bawah sayap hidung dari FACEMESH_NOSE (pasangan resmi) - approx.
const int kNostrilPts[2] = {48, 278};
}  // namespace topology

namespace {

// Rasterisasi poligon tertutup, even-odd scanline. P piksel = koordinat
// normalisasi * (w,h). Menulis ke m HANYA selama isinya (tidak menimpa
// piksel di luar poligon - penting untuk pengurangan fitur).
void RasterizePolyXY(const float* xs, const float* ys, int n, int w, int h,
                     uint8_t* m, uint8_t v) {
  if (n < 3 || w <= 0 || h <= 0) return;
  float min_y = ys[0], max_y = ys[0];
  for (int i = 1; i < n; ++i) {
    min_y = std::min(min_y, ys[i]);
    max_y = std::max(max_y, ys[i]);
  }
  const int y0 = std::max(0, static_cast<int>(std::floor(min_y)));
  const int y1 = std::min(h - 1, static_cast<int>(std::ceil(max_y)));
  float cross[128];
  for (int y = y0; y <= y1; ++y) {
    const float sy = static_cast<float>(y) + 0.5f;
    int cn = 0;
    for (int i = 0; i < n && cn < 128; ++i) {
      const int j = (i + 1) % n;
      const float y_a = ys[i], y_b = ys[j];
      if ((y_a <= sy && y_b > sy) || (y_b <= sy && y_a > sy)) {
        const float t = (sy - y_a) / (y_b - y_a);
        cross[cn++] = xs[i] + t * (xs[j] - xs[i]);
      }
    }
    if (cn < 2) continue;
    std::sort(cross, cross + cn);
    for (int k = 0; k + 1 < cn; k += 2) {
      int xa = static_cast<int>(std::ceil(cross[k] - 0.5f));
      int xb = static_cast<int>(std::floor(cross[k + 1] - 0.5f));
      xa = std::max(0, xa);
      xb = std::min(w - 1, xb);
      if (xb < xa) continue;
      std::memset(m + static_cast<size_t>(y) * w + xa, v,
                  static_cast<size_t>(xb - xa + 1));
    }
  }
}

void RingToXY(const int* ring, size_t n, const LandmarkSet& lm, int w, int h,
              float* xs, float* ys) {
  for (size_t i = 0; i < n; ++i) {
    const Vec2& p = lm.pts[ring[i]];
    xs[i] = p.x * static_cast<float>(w);
    ys[i] = p.y * static_cast<float>(h);
  }
}

// Jarak kuadrat titik ke segmen (2D).
float Dist2PointSeg(const Vec2& p, const Vec2& a, const Vec2& b) {
  const float dx = b.x - a.x, dy = b.y - a.y;
  const float len2 = dx * dx + dy * dy;
  float t = 0.0f;
  if (len2 > 1e-9f) t = std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / len2, 0.0f, 1.0f);
  const float px = p.x - (a.x + t * dx), py = p.y - (a.y + t * dy);
  return px * px + py * py;
}

// YCbCr (BT.601) dari RGB.
inline void RgbToCbCr(const uint8_t* px, float* cb, float* cr) {
  const float r = static_cast<float>(px[0]);
  const float g = static_cast<float>(px[1]);
  const float b = static_cast<float>(px[2]);
  const float y = 0.299f * r + 0.587f * g + 0.114f * b;
  *cb = 128.0f + 0.564f * (b - y);
  *cr = 128.0f + 0.713f * (r - y);
}

void BoxBlurH(const uint8_t* src, uint8_t* dst, int w, int h, int r) {
  const int win = 2 * r + 1;
  for (int y = 0; y < h; ++y) {
    const uint8_t* s = src + static_cast<size_t>(y) * w;
    uint8_t* d = dst + static_cast<size_t>(y) * w;
    int sum = 0;
    for (int i = -r; i <= r; ++i) sum += s[std::clamp(i, 0, w - 1)];
    for (int x = 0; x < w; ++x) {
      d[x] = static_cast<uint8_t>((sum + win / 2) / win);
      const int add = s[std::clamp(x + r + 1, 0, w - 1)];
      const int sub = s[std::clamp(x - r, 0, w - 1)];
      sum += add - sub;
    }
  }
}

void BoxBlurV(const uint8_t* src, uint8_t* dst, int w, int h, int r) {
  const int win = 2 * r + 1;
  for (int x = 0; x < w; ++x) {
    int sum = 0;
    for (int i = -r; i <= r; ++i) sum += src[std::clamp(i, 0, h - 1) * w + x];
    for (int y = 0; y < h; ++y) {
      dst[static_cast<size_t>(y) * w + x] =
          static_cast<uint8_t>((sum + win / 2) / win);
      const int add = src[std::clamp(y + r + 1, 0, h - 1) * w + x];
      const int sub = src[std::clamp(y - r, 0, h - 1) * w + x];
      sum += add - sub;
    }
  }
}

}  // namespace

void FillMask(uint8_t* m, int w, int h, uint8_t v) {
  std::memset(m, v, static_cast<size_t>(w) * h);
}

void RasterizeRing(const int* ring_idx, size_t ring_n, const LandmarkSet& lm,
                   int w, int h, uint8_t* m, uint8_t v) {
  if (ring_n < 3 || ring_n > 64) return;
  float xs[64], ys[64];
  RingToXY(ring_idx, ring_n, lm, w, h, xs, ys);
  RasterizePolyXY(xs, ys, static_cast<int>(ring_n), w, h, m, v);
}

void StrokeChain(const int* chain_idx, size_t chain_n, const LandmarkSet& lm,
                 int w, int h, float radius_px, uint8_t* m, uint8_t v) {
  if (chain_n < 2 || radius_px <= 0.0f) return;
  // Bounding box rantai + radius, agar hanya wilayah kecil yang dicek.
  float min_x = 1e9f, min_y = 1e9f, max_x = -1e9f, max_y = -1e9f;
  Vec2 pts[64];
  const size_t n = std::min<size_t>(chain_n, 64);
  for (size_t i = 0; i < n; ++i) {
    pts[i] = Vec2(lm.pts[chain_idx[i]].x * static_cast<float>(w),
                  lm.pts[chain_idx[i]].y * static_cast<float>(h));
    min_x = std::min(min_x, pts[i].x);
    min_y = std::min(min_y, pts[i].y);
    max_x = std::max(max_x, pts[i].x);
    max_y = std::max(max_y, pts[i].y);
  }
  const int x0 = std::max(0, static_cast<int>(std::floor(min_x - radius_px)));
  const int x1 = std::min(w - 1, static_cast<int>(std::ceil(max_x + radius_px)));
  const int y0 = std::max(0, static_cast<int>(std::floor(min_y - radius_px)));
  const int y1 = std::min(h - 1, static_cast<int>(std::ceil(max_y + radius_px)));
  const float r2 = radius_px * radius_px;
  for (int y = y0; y <= y1; ++y) {
    for (int x = x0; x <= x1; ++x) {
      const Vec2 p(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
      for (size_t i = 0; i + 1 < n; ++i) {
        if (Dist2PointSeg(p, pts[i], pts[i + 1]) <= r2) {
          m[static_cast<size_t>(y) * w + x] = v;
          break;
        }
      }
    }
  }
}

void FeatherMask(uint8_t* m, int w, int h, int radius, int passes) {
  if (radius <= 0 || passes <= 0) return;
  std::vector<uint8_t> tmp(static_cast<size_t>(w) * h);
  for (int p = 0; p < passes; ++p) {
    BoxBlurH(m, tmp.data(), w, h, radius);
    BoxBlurV(tmp.data(), m, w, h, radius);
  }
}

void TemporalBlend(const uint8_t* prev, uint8_t* cur, int n, float alpha) {
  const float a = std::clamp(alpha, 0.0f, 1.0f);
  for (int i = 0; i < n; ++i) {
    const float v = a * static_cast<float>(cur[i]) +
                    (1.0f - a) * static_cast<float>(prev[i]);
    cur[i] = static_cast<uint8_t>(v + 0.5f);
  }
}

void FillDiscN(const Vec2& c, float radius_norm, int w, int h, uint8_t* m,
               uint8_t v) {
  // Radius normalisasi dipakai di ruang piksel via rata-rata w/h aspek -
  // pakai ruang piksel langsung (x*w, y*h) dengan jari-jari berbasis lebar.
  const float cx = c.x * static_cast<float>(w);
  const float cy = c.y * static_cast<float>(h);
  const float r = radius_norm * static_cast<float>(w);
  const int x0 = std::max(0, static_cast<int>(std::floor(cx - r)));
  const int x1 = std::min(w - 1, static_cast<int>(std::ceil(cx + r)));
  const int y0 = std::max(0, static_cast<int>(std::floor(cy - r)));
  const int y1 = std::min(h - 1, static_cast<int>(std::ceil(cy + r)));
  const float r2 = r * r;
  for (int y = y0; y <= y1; ++y) {
    for (int x = x0; x <= x1; ++x) {
      const float dx = (static_cast<float>(x) + 0.5f) - cx;
      const float dy = (static_cast<float>(y) + 0.5f) - cy;
      if (dx * dx + dy * dy <= r2) m[static_cast<size_t>(y) * w + x] = v;
    }
  }
}

bool SkinSamplePoints(const LandmarkSet& lm, Vec2 out_pts[3]) {
  if (!lm.valid) return false;
  // Bbox oval dari ring resmi.
  float min_x = 1e9f, min_y = 1e9f, max_x = -1e9f, max_y = -1e9f;
  for (int i = 0; i < 36; ++i) {
    const Vec2& p = lm.pts[topology::kFaceOvalRing[i]];
    min_x = std::min(min_x, p.x);
    min_y = std::min(min_y, p.y);
    max_x = std::max(max_x, p.x);
    max_y = std::max(max_y, p.y);
  }
  const float fh = max_y - min_y;
  const float fw = max_x - min_x;
  if (fh <= 0.02f || fw <= 0.02f) return false;
  // 1) Dahi: tengah horizontal, sedikit di bawah garis rambut (oval atas).
  out_pts[0] = Vec2((min_x + max_x) * 0.5f, min_y + 0.18f * fh);
  // 2/3) Pipi: di bawah pusat mata, menuju tepi oval lateral.
  auto eye_center = [&](const int* ring, size_t n) {
    Vec2 s(0, 0);
    for (size_t i = 0; i < n; ++i) s += lm.pts[ring[i]];
    return Vec2(s.x / static_cast<float>(n), s.y / static_cast<float>(n));
  };
  const Vec2 eye_l = eye_center(topology::kRightEyeRing, 16);  // sisi kiri layar
  const Vec2 eye_r = eye_center(topology::kLeftEyeRing, 16);   // sisi kanan layar
  // Tepi oval pada y = eye.y + 0.15*fh (segmen terdekat pada y tsb).
  auto oval_edge_x = [&](float y_q, bool left_side) {
    float best = left_side ? 1e9f : -1e9f;
    for (int i = 0; i < 36; ++i) {
      const Vec2& a = lm.pts[topology::kFaceOvalRing[i]];
      const Vec2& b =
          lm.pts[topology::kFaceOvalRing[(i + 1) % 36]];
      const float ya = a.y, yb = b.y;
      if ((ya <= y_q && yb >= y_q) || (yb <= y_q && ya >= y_q)) {
        const float t = (y_q - ya) / (yb - ya + 1e-9f);
        const float x = a.x + t * (b.x - a.x);
        best = left_side ? std::min(best, x) : std::max(best, x);
      }
    }
    if (left_side && best > 1e8f) best = min_x;
    if (!left_side && best < -1e8f) best = max_x;
    return best;
  };
  const float y_l = eye_l.y + 0.15f * fh;
  const float y_r = eye_r.y + 0.15f * fh;
  const float edge_l = oval_edge_x(y_l, /*left_side=*/true);
  const float edge_r = oval_edge_x(y_r, /*left_side=*/false);
  out_pts[1] = Vec2((eye_l.x + edge_l) * 0.5f, y_l);
  out_pts[2] = Vec2((eye_r.x + edge_r) * 0.5f, y_r);
  return true;
}

int SampleChromaAt(const uint8_t* rgba, int fw, int fh, int stride,
                   const Vec2& center, float radius_px, float* cb, float* cr) {
  if (!rgba || radius_px < 1.0f) return 0;
  const int cx = static_cast<int>(center.x * static_cast<float>(fw));
  const int cy = static_cast<int>(center.y * static_cast<float>(fh));
  const int r = static_cast<int>(radius_px);
  double cb_sum = 0.0, cr_sum = 0.0;
  int n = 0;
  for (int y = std::max(0, cy - r); y <= std::min(fh - 1, cy + r); ++y) {
    const uint8_t* row = rgba + static_cast<size_t>(y) * stride;
    for (int x = std::max(0, cx - r); x <= std::min(fw - 1, cx + r); ++x) {
      const int dx = x - cx, dy = y - cy;
      if (dx * dx + dy * dy > r * r) continue;
      float b_cb, b_cr;
      RgbToCbCr(row + static_cast<size_t>(x) * 4, &b_cb, &b_cr);
      cb_sum += b_cb;
      cr_sum += b_cr;
      ++n;
    }
  }
  if (n == 0) return 0;
  *cb = static_cast<float>(cb_sum / n);
  *cr = static_cast<float>(cr_sum / n);
  return n;
}

void ApplySkinColorProb(const uint8_t* rgba, int fw, int fh, int stride,
                        const uint8_t* region, int mw, int mh,
                        const SkinMaskConfig& cfg, float cb0, float cr0,
                        bool use_adaptif, uint8_t* m) {
  const float sx = static_cast<float>(fw) / static_cast<float>(mw);
  const float sy = static_cast<float>(fh) / static_cast<float>(mh);
  const float r_in = cfg.chroma_radius * cfg.chroma_feather;
  for (int y = 0; y < mh; ++y) {
    for (int x = 0; x < mw; ++x) {
      const size_t idx = static_cast<size_t>(y) * mw + x;
      if (region[idx] == 0) {
        m[idx] = 0;
        continue;
      }
      if (m[idx] == 0) continue;  // sudah dibuang fitur anatomi
      int px = std::min(fw - 1, static_cast<int>((static_cast<float>(x) + 0.5f) * sx));
      int py = std::min(fh - 1, static_cast<int>((static_cast<float>(y) + 0.5f) * sy));
      float cb, cr;
      RgbToCbCr(rgba + static_cast<size_t>(py) * stride +
                    static_cast<size_t>(px) * 4, &cb, &cr);
      // Gate lebar klasik (sanity) - bukan syarat tunggal.
      const bool band = cb >= cfg.cb_min && cb <= cfg.cb_max &&
                        cr >= cfg.cr_min && cr <= cfg.cr_max;
      float prob = 1.0f;
      if (use_adaptif) {
        const float dcb = cb - cb0, dcr = cr - cr0;
        const float d = std::sqrt(dcb * dcb + dcr * dcr);
        // 1 di dalam r*0.6, memudar ke 0 di r*1.4 (dan memotong di r).
        const float r1 = r_in, r2 = cfg.chroma_radius * 1.4f;
        prob = d <= r1 ? 1.0f : std::clamp(1.0f - (d - r1) / (r2 - r1), 0.0f, 1.0f);
      }
      if (!band) prob = 0.0f;
      m[idx] = static_cast<uint8_t>(prob * static_cast<float>(m[idx]) + 0.5f);
    }
  }
}

float SampleMaskBilinear(const uint8_t* m, int mw, int mh, float u, float v) {
  if (!m || mw <= 0 || mh <= 0) return 0.0f;
  const float x = std::clamp(u, 0.0f, 1.0f) * static_cast<float>(mw) - 0.5f;
  const float y = std::clamp(v, 0.0f, 1.0f) * static_cast<float>(mh) - 0.5f;
  const int x0 = std::max(0, static_cast<int>(std::floor(x)));
  const int y0 = std::max(0, static_cast<int>(std::floor(y)));
  const int x1 = std::min(mw - 1, x0 + 1);
  const int y1 = std::min(mh - 1, y0 + 1);
  const float fx = std::clamp(x - static_cast<float>(x0), 0.0f, 1.0f);
  const float fy = std::clamp(y - static_cast<float>(y0), 0.0f, 1.0f);
  const auto at = [&](int xx, int yy) {
    return static_cast<float>(m[static_cast<size_t>(yy) * mw + xx]);
  };
  const float a = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * fx;
  const float b = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * fx;
  return (a + (b - a) * fy) / 255.0f;
}

void MixMasked(const uint8_t* raw, const uint8_t* beauty, const uint8_t* mask,
               int mw, int mh, int w, int h, float gain, uint8_t* out) {
  const float g = std::clamp(gain, 0.0f, 1.0f);
  for (int y = 0; y < h; ++y) {
    const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(h);
    const uint8_t* r = raw + static_cast<size_t>(y) * w * 4;
    const uint8_t* b = beauty + static_cast<size_t>(y) * w * 4;
    uint8_t* o = out + static_cast<size_t>(y) * w * 4;
    for (int x = 0; x < w; ++x) {
      const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(w);
      const float t = SampleMaskBilinear(mask, mw, mh, u, v) * g;
      for (int c = 0; c < 4; ++c) {
        const float rf = static_cast<float>(r[x * 4 + c]);
        const float bf = static_cast<float>(b[x * 4 + c]);
        o[x * 4 + c] = static_cast<uint8_t>(rf + (bf - rf) * t + 0.5f);
      }
    }
  }
}

int MaskDiffStats(const uint8_t* raw, const uint8_t* out, const uint8_t* mask,
                  int mw, int mh, int w, int h, float gain,
                  float* outside_mean_abs, float* inside_mean_abs) {
  double out_sum = 0.0, in_sum = 0.0;
  long out_n = 0, in_n = 0;
  const float g = std::clamp(gain, 0.0f, 1.0f);
  for (int y = 0; y < h; ++y) {
    const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(h);
    const uint8_t* r = raw + static_cast<size_t>(y) * w * 4;
    const uint8_t* o = out + static_cast<size_t>(y) * w * 4;
    for (int x = 0; x < w; ++x) {
      const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(w);
      const float t = SampleMaskBilinear(mask, mw, mh, u, v) * g;
      // Piksel RGB saja (alpha selalu 255 di kedua sisi).
      int dsum = 0;
      for (int c = 0; c < 3; ++c) {
        dsum += std::abs(static_cast<int>(r[x * 4 + c]) -
                         static_cast<int>(o[x * 4 + c]));
      }
      if (t < 8.0f / 255.0f) {  // efek ~nol di kelas ini
        out_sum += dsum / 3.0;
        ++out_n;
      } else if (t >= 200.0f / 255.0f) {
        in_sum += dsum / 3.0;
        ++in_n;
      }
    }
  }
  if (outside_mean_abs)
    *outside_mean_abs = out_n ? static_cast<float>(out_sum / out_n) : 0.0f;
  if (inside_mean_abs)
    *inside_mean_abs = in_n ? static_cast<float>(in_sum / in_n) : 0.0f;
  return static_cast<int>(out_n + in_n);
}

void UpsampleMaskToGrayRgba(const uint8_t* mask, int mw, int mh, int w, int h,
                            uint8_t* rgba_out) {
  for (int y = 0; y < h; ++y) {
    const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(h);
    uint8_t* row = rgba_out + static_cast<size_t>(y) * w * 4;
    for (int x = 0; x < w; ++x) {
      const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(w);
      const uint8_t g =
          static_cast<uint8_t>(SampleMaskBilinear(mask, mw, mh, u, v) * 255.0f +
                               0.5f);
      row[x * 4 + 0] = g;
      row[x * 4 + 1] = g;
      row[x * 4 + 2] = g;
      row[x * 4 + 3] = 255;
    }
  }
}

// ---------------------------------------------------------------------------
// M2i - guided smoothing luma, komposit chroma-preserving, metrik F5
// ---------------------------------------------------------------------------

namespace {
// Box blur terpisah pada buffer float (untuk mean/var luma guided).
void BoxBlurFloatH(const float* src, float* dst, int w, int h, int r) {
  const int win = 2 * r + 1;
  for (int y = 0; y < h; ++y) {
    const float* s = src + static_cast<size_t>(y) * w;
    float* d = dst + static_cast<size_t>(y) * w;
    float sum = 0.0f;
    for (int i = -r; i <= r; ++i) sum += s[std::clamp(i, 0, w - 1)];
    for (int x = 0; x < w; ++x) {
      d[x] = sum / static_cast<float>(win);
      sum += s[std::clamp(x + r + 1, 0, w - 1)] - s[std::clamp(x - r, 0, w - 1)];
    }
  }
}
void BoxBlurFloatV(const float* src, float* dst, int w, int h, int r) {
  const int win = 2 * r + 1;
  for (int x = 0; x < w; ++x) {
    float sum = 0.0f;
    for (int i = -r; i <= r; ++i) sum += src[std::clamp(i, 0, h - 1) * w + x];
    for (int y = 0; y < h; ++y) {
      dst[static_cast<size_t>(y) * w + x] = sum / static_cast<float>(win);
      sum += src[std::clamp(y + r + 1, 0, h - 1) * w + x] -
             src[std::clamp(y - r, 0, h - 1) * w + x];
    }
  }
}
}  // namespace

void SmoothLumaGuidedCpu(const uint8_t* rgba_in, int w, int h, int radius,
                         float eps, float amount, uint8_t* rgba_out) {
  if (!rgba_in || !rgba_out || w <= 0 || h <= 0) return;
  const int n = w * h;
  const int r = std::clamp(radius, 1, 64);
  const float a_amt = std::clamp(amount, 0.0f, 1.0f);
  std::vector<float> y0(n), y1(n), y2(n), ysq(n);
  for (int i = 0; i < n; ++i) {
    const uint8_t* p = rgba_in + static_cast<size_t>(i) * 4;
    y0[i] = (0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]) / 255.0f;
    ysq[i] = y0[i] * y0[i];
  }
  BoxBlurFloatH(y0.data(), y1.data(), w, h, r);
  BoxBlurFloatV(y1.data(), y2.data(), w, h, r);   // y2 = meanY
  std::vector<float> m1(n), m2(n);
  BoxBlurFloatH(ysq.data(), m1.data(), w, h, r);
  BoxBlurFloatV(m1.data(), m2.data(), w, h, r);   // m2 = meanY2
  for (int i = 0; i < n; ++i) {
    const float var = std::max(0.0f, m2[i] - y2[i] * y2[i]);
    const float a = var / (var + std::max(eps, 1e-6f));
    const float yg = a * y0[i] + (1.0f - a) * y2[i];
    const float ys = y0[i] + (yg - y0[i]) * a_amt;  // mix(Y, yg, amount)
    const float dy = (ys - y0[i]) * 255.0f;         // geser luma, chroma tetap
    const uint8_t* p = rgba_in + static_cast<size_t>(i) * 4;
    uint8_t* o = rgba_out + static_cast<size_t>(i) * 4;
    for (int c = 0; c < 3; ++c) {
      const float v = static_cast<float>(p[c]) + dy + 0.5f;
      o[c] = static_cast<uint8_t>(v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v));
    }
    o[3] = 255;
  }
}

void ComposeBeautyRaw(const uint8_t* raw, const uint8_t* beauty,
                      const uint8_t* mask, int mw, int mh, int w, int h,
                      float gain, const BeautyComposeConfig& cfg, uint8_t* out) {
  if (!raw || !beauty || !mask || !out || w <= 0 || h <= 0 || mw <= 0 ||
      mh <= 0) {
    return;
  }
  const float g = std::clamp(gain, 0.0f, 1.0f);
  const float retain = std::clamp(cfg.chroma_retain, 0.0f, 1.0f);
  // 1) kandidat: beauty dengan luma diguided (butuh buffer sementara)
  std::vector<uint8_t> cand(static_cast<size_t>(w) * h * 4);
  SmoothLumaGuidedCpu(beauty, w, h, cfg.smooth_radius, cfg.smooth_eps,
                      cfg.smooth_amount, cand.data());
  for (int y = 0; y < h; ++y) {
    const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(h);
    const uint8_t* r = raw + static_cast<size_t>(y) * w * 4;
    const uint8_t* c = cand.data() + static_cast<size_t>(y) * w * 4;
    uint8_t* o = out + static_cast<size_t>(y) * w * 4;
    for (int x = 0; x < w; ++x) {
      const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(w);
      const float t = SampleMaskBilinear(mask, mw, mh, u, v) * g;
      const int i4 = x * 4;
      if (t <= 0.0f) {  // byte-identik dengan raw di luar mask
        o[i4 + 0] = r[i4 + 0]; o[i4 + 1] = r[i4 + 1];
        o[i4 + 2] = r[i4 + 2]; o[i4 + 3] = 255;
        continue;
      }
      // 2) mix kandidat dgn raw
      float mixc[3];
      for (int ch = 0; ch < 3; ++ch) {
        const float rv = static_cast<float>(r[i4 + ch]);
        const float cv = static_cast<float>(c[i4 + ch]);
        mixc[ch] = rv + (cv - rv) * t;
      }
      // 2b) M2j: lift Cerahan proporsional (hanya di dalam mask; chroma
      //     mentah dipertahankan di langkah 3 -> tanpa cast warna).
      if (cfg.lift > 0.0f) {
        const float k = cfg.lift;
        for (int ch = 0; ch < 3; ++ch) {
          mixc[ch] += k * (255.0f - mixc[ch]);
        }
      }
      // 3) retensi chroma MENTAH: Y dari hasil mix, CbCr diarahkan ke raw
      //    sebanyak `retain` (float; tanpa kuantisasi ganda).
      const float y_m = 0.299f * mixc[0] + 0.587f * mixc[1] + 0.114f * mixc[2];
      const float cb_m = 128.0f + 0.564f * (mixc[2] - y_m);
      const float cr_m = 128.0f + 0.713f * (mixc[0] - y_m);
      const YCbCrF rc = RgbToYCbCrF(r[i4 + 0], r[i4 + 1], r[i4 + 2]);
      const float cb = cb_m + (rc.cb - cb_m) * retain;
      const float cr = cr_m + (rc.cr - cr_m) * retain;
      YCbCrToRgb8(y_m, cb, cr, o + i4);
      o[i4 + 3] = 255;
    }
  }
}

void ComputeInMaskMetrics(const uint8_t* raw, const uint8_t* out,
                          const uint8_t* mask, int mw, int mh, int w, int h,
                          float gain, InMaskMetrics* met) {
  if (!raw || !out || !mask || !met || w <= 0 || h <= 0) return;
  *met = InMaskMetrics{};
  const float g = std::clamp(gain, 0.0f, 1.0f);
  // Mask inti: ambang 200 lalu erosi 2 iterasi (4-tetangga) di ruang mask.
  std::vector<uint8_t> core(static_cast<size_t>(mw) * mh);
  for (size_t i = 0; i < core.size(); ++i)
    core[i] = (static_cast<float>(mask[i]) * g >= 200.0f) ? 1 : 0;
  for (int pass = 0; pass < 2; ++pass) {
    std::vector<uint8_t> nxt(core.size(), 1);
    for (int y = 0; y < mh; ++y) {
      for (int x = 0; x < mw; ++x) {
        const size_t i = static_cast<size_t>(y) * mw + x;
        if (!core[i]) { nxt[i] = 0; continue; }
        if (x == 0 || y == 0 || x == mw - 1 || y == mh - 1) { nxt[i] = 0; continue; }
        if (core[i - 1] && core[i + 1] && core[i - mw] && core[i + mw])
          nxt[i] = 1;
        else
          nxt[i] = 0;
      }
    }
    core.swap(nxt);
  }
  // High-pass luma 3x3 (untuk energi detail halus).
  auto luma_at = [](const uint8_t* p) {
    return 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
  };
  std::vector<float> hp(static_cast<size_t>(w) * h, 0.0f);
  for (int y = 1; y + 1 < h; ++y) {
    for (int x = 1; x + 1 < w; ++x) {
      const uint8_t* p = raw + (static_cast<size_t>(y) * w + x) * 4;
      const float blur9 =
          (luma_at(p) + luma_at(p - 4) + luma_at(p + 4) + luma_at(p - w * 4) +
           luma_at(p + w * 4) + luma_at(p - 4 - w * 4) + luma_at(p + 4 - w * 4) +
           luma_at(p - 4 + w * 4) + luma_at(p + 4 + w * 4)) / 9.0f;
      hp[static_cast<size_t>(y) * w + x] = luma_at(p) - blur9;
    }
  }
  std::vector<float> hp_out(hp.size(), 0.0f);
  for (int y = 1; y + 1 < h; ++y) {
    for (int x = 1; x + 1 < w; ++x) {
      const uint8_t* p = out + (static_cast<size_t>(y) * w + x) * 4;
      const float blur9 =
          (luma_at(p) + luma_at(p - 4) + luma_at(p + 4) + luma_at(p - w * 4) +
           luma_at(p + w * 4) + luma_at(p - 4 - w * 4) + luma_at(p + 4 - w * 4) +
           luma_at(p - 4 + w * 4) + luma_at(p + 4 + w * 4)) / 9.0f;
      hp_out[static_cast<size_t>(y) * w + x] = luma_at(p) - blur9;
    }
  }
  double dy_sum = 0.0, dc_sum = 0.0, det_r = 0.0, det_o = 0.0;
  double edge_sum = 0.0, out_sum = 0.0;
  long core_n = 0, edge_n = 0, out_n = 0;
  for (int y = 0; y < h; ++y) {
    const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(h);
    const uint8_t* r = raw + static_cast<size_t>(y) * w * 4;
    const uint8_t* o = out + static_cast<size_t>(y) * w * 4;
    for (int x = 0; x < w; ++x) {
      const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(w);
      const float t = SampleMaskBilinear(mask, mw, mh, u, v) * g;
      const int cx = std::min(mw - 1, static_cast<int>(u * mw));
      const int cy = std::min(mh - 1, static_cast<int>(v * mh));
      const bool is_core = core[static_cast<size_t>(cy) * mw + cx] != 0;
      const int i4 = x * 4;
      int dsum = 0;
      for (int c = 0; c < 3; ++c)
        dsum += std::abs(static_cast<int>(r[i4 + c]) - static_cast<int>(o[i4 + c]));
      if (is_core) {
        const YCbCrF rc = RgbToYCbCrF(r[i4], r[i4 + 1], r[i4 + 2]);
        const YCbCrF oc = RgbToYCbCrF(o[i4], o[i4 + 1], o[i4 + 2]);
        dy_sum += oc.y - rc.y;
        const float dcb = oc.cb - rc.cb, dcr = oc.cr - rc.cr;
        dc_sum += std::sqrt(dcb * dcb + dcr * dcr);
        const float hi = hp_out[static_cast<size_t>(y) * w + x];
        const float hr = hp[static_cast<size_t>(y) * w + x];
        det_o += static_cast<double>(hi) * hi;
        det_r += static_cast<double>(hr) * hr;
        ++core_n;
      } else if (t > 16.0f / 255.0f && t < 200.0f / 255.0f) {
        edge_sum += dsum / 3.0;
        ++edge_n;
      } else if (t < 8.0f / 255.0f) {
        out_sum += dsum / 3.0;
        ++out_n;
      }
    }
  }
  met->core_n = static_cast<int>(core_n);
  met->edge_n = static_cast<int>(edge_n);
  met->outside_n = static_cast<int>(out_n);
  if (core_n) {
    met->delta_luma = static_cast<float>(dy_sum / core_n);
    met->delta_chroma = static_cast<float>(dc_sum / core_n);
    met->detail_ratio =
        det_r > 1e-12 ? static_cast<float>(det_o / det_r) : 1.0f;
  }
  if (edge_n) met->edge_band_diff = static_cast<float>(edge_sum / edge_n);
  if (out_n) met->outside_diff = static_cast<float>(out_sum / out_n);
}

// ---------------------------------------------------------------------------
// SkinMaskBuilder
// ---------------------------------------------------------------------------

void SkinMaskBuilder::Configure(int mask_w, int mask_h,
                                const SkinMaskConfig& cfg) {
  mask_w_ = std::max(8, mask_w);
  mask_h_ = std::max(8, mask_h);
  cfg_ = cfg;
  mask_.assign(static_cast<size_t>(mask_w_) * mask_h_, 0);
  prev_.assign(static_cast<size_t>(mask_w_) * mask_h_, 0);
  has_prev_ = false;
  face_valid_ = false;
}

bool SkinMaskBuilder::Update(const LandmarkSet& lm, bool face_present,
                             const uint8_t* rgba, int fw, int fh, int stride,
                             const SegMaskInput* seg) {
  if (mask_w_ <= 0 || !rgba || fw <= 0 || fh <= 0) return false;
  if (!face_present || !lm.valid) {
    face_valid_ = false;
    return false;  // mask tidak diubah - gain di luar yang memudarkan
  }
  const int n = mask_w_ * mask_h_;
  const int w = mask_w_, h = mask_h_;

  // M2i (3c): oval dan fitur (mata/alis/bibir/hidung) dibangun TERPISAH
  // agar feather punya radius berbeda: tepi luar wajah = feather_bias,
  // lubang fitur = feather LEBAR (mencegah cincin), inti lubang restore 0.
  // 1) Oval wajah -> 255 (domain mask u8 0..255).
  FillMask(mask_.data(), w, h, 0);
  RasterizeRing(topology::kFaceOvalRing, 36, lm, w, h, mask_.data(), 255);
  // Region oval polos (untuk gating warna: piksel di luar oval selalu 0).
  std::vector<uint8_t> region(mask_.begin(), mask_.end());

  // 2) Fitur yang dikeluarkan dari mask -> buffer `feat` (255 = lubang).
  std::vector<uint8_t> feat(static_cast<size_t>(w) * h, 0);
  RasterizeRing(topology::kRightEyeRing, 16, lm, w, h, feat.data(), 255);
  RasterizeRing(topology::kLeftEyeRing, 16, lm, w, h, feat.data(), 255);
  RasterizeRing(topology::kLipsOuterRing, 20, lm, w, h, feat.data(), 255);
  // Radius alis proporsional tinggi mata (mask space).
  float eye_min_y = 1e9f, eye_max_y = -1e9f;
  for (int i = 0; i < 16; ++i) {
    const float y1 = lm.pts[topology::kRightEyeRing[i]].y;
    const float y2 = lm.pts[topology::kLeftEyeRing[i]].y;
    eye_min_y = std::min(eye_min_y, std::min(y1, y2));
    eye_max_y = std::max(eye_max_y, std::max(y1, y2));
  }
  const float eye_h_px = (eye_max_y - eye_min_y) * static_cast<float>(h);
  const float brow_r = std::max(1.0f, eye_h_px * 0.35f);
  for (int c = 0; c < 4; ++c) {
    StrokeChain(topology::kBrowChains[c], 5, lm, w, h, brow_r, feat.data(), 255);
  }
  const float face_w = [&] {
    float mn = 1e9f, mx = -1e9f;
    for (int i = 0; i < 36; ++i) {
      mn = std::min(mn, lm.pts[topology::kFaceOvalRing[i]].x);
      mx = std::max(mx, lm.pts[topology::kFaceOvalRing[i]].x);
    }
    return mx - mn;
  }();
  // Jari-jari lubang hidung ~1.5% lebar oval; minimal 2.5 px di ruang mask
  // agar tetap terukir setelah feather (mask resolusi rendah).
  const float nostril_r =
      std::max(2.5f / static_cast<float>(w), face_w * 0.015f);
  FillDiscN(lm.pts[topology::kNostrilPts[0]], nostril_r, w, h, feat.data(), 255);
  FillDiscN(lm.pts[topology::kNostrilPts[1]], nostril_r, w, h, feat.data(), 255);

  // 3) Kalibrasi warna kulit adaptif (3 sampel: dahi + 2 pipi).
  Vec2 samples[3];
  float cb0 = 0.0f, cr0 = 0.0f;
  bool use_adaptif = false;
  if (SkinSamplePoints(lm, samples)) {
    // Radius sampel (px full-res) = 5% lebar oval.
    float min_x = 1e9f, max_x = -1e9f;
    for (int i = 0; i < 36; ++i) {
      const float x = lm.pts[topology::kFaceOvalRing[i]].x;
      min_x = std::min(min_x, x);
      max_x = std::max(max_x, x);
    }
    const float radius_px =
        std::max(2.0f, (max_x - min_x) * 0.05f * static_cast<float>(fw));
    double cb_sum = 0.0, cr_sum = 0.0;
    int got = 0;
    for (const Vec2& s : samples) {
      float cb, cr;
      if (SampleChromaAt(rgba, fw, fh, stride, s, radius_px, &cb, &cr) > 0) {
        cb_sum += cb;
        cr_sum += cr;
        ++got;
      }
    }
    if (got > 0) {
      cb0 = static_cast<float>(cb_sum / got);
      cr0 = static_cast<float>(cr_sum / got);
      use_adaptif = true;
    }
  }
  ApplySkinColorProb(rgba, fw, fh, stride, region.data(), w, h, cfg_, cb0, cr0,
                     use_adaptif, mask_.data());

  // 4) Feather PER-REGION (M2i 3c): tepi luar = feather_bias; lubang fitur
  //    = feather LEBAR (ramp melebar KE LUAR batas fitur), inti lubang
  //    di-restore 0 supaya mata/alis/bibir tetap sepenuhnya bebas efek.
  FeatherMask(mask_.data(), w, h, cfg_.feather_radius, cfg_.feather_passes);
  std::vector<uint8_t> fsoft = feat;
  FeatherMask(fsoft.data(), w, h, cfg_.feature_feather_radius,
              cfg_.feather_passes);
  for (size_t i = 0; i < mask_.size(); ++i) {
    if (feat[i]) {
      mask_[i] = 0;  // inti lubang: keras 0
    } else if (fsoft[i]) {
      // ramp di LUAR fitur: 255 -> 0 seluas feather fitur
      const float mul = 1.0f - static_cast<float>(fsoft[i]) / 255.0f;
      mask_[i] = static_cast<uint8_t>(mul * static_cast<float>(mask_[i]) + 0.5f);
    }
  }

  // 5) M2j: fusion segmenasi (cakupan SEMUA kulit, bila hasil seg ada).
  //    P_kulit = min(1, P(kulit tubuh)+P(kulit wajah)) -> guided refine
  //    (luma penuh) -> gerbang warna HANYA di luar oval -> body_strength
  //    -> union dgn mask wajah -> ramp fitur (fsoft) -> inti fitur nol.
  const bool use_seg = cfg_.coverage == SkinCoverage::kAllSkin && seg &&
                       seg->valid && seg->probs && seg->letterbox;
  if (use_seg) {
    // 5a) unletterbox bilinear -> kasar di resolusi mask.
    std::vector<float> skin0(static_cast<size_t>(n));
    SegProbsToMaskSkin(seg->probs, *seg->letterbox, w, h, skin0.data());
    // 5b) guided refine: panduan = luma frame RESOLUSI PENUH (rasio aspek
    //     mask = rasio frame -> sampel titik-ke-titik).
    luma_full_.resize(static_cast<size_t>(fw) * fh);
    for (int y = 0; y < fh; ++y) {
      const uint8_t* row = rgba + static_cast<size_t>(y) * stride;
      uint8_t* lum = luma_full_.data() + static_cast<size_t>(y) * fw;
      for (int x = 0; x < fw; ++x) {
        const uint8_t* p = row + static_cast<size_t>(x) * 4;
        lum[x] = static_cast<uint8_t>(
            0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2] + 0.5f);
      }
    }
    std::vector<float> skin1(static_cast<size_t>(n));
    JointBilateralUpsample(skin0.data(), w, h, luma_full_.data(), fw, fh, w, h,
                           skin1.data(), /*sigma_space=*/2.0f,
                           /*sigma_range=*/0.08f);
    // 5c) gerbang warna adaptif + body_strength HANYA di luar oval;
    //     di dalam oval mask landmark tetap raja (presisi fitur).
    const float sx = static_cast<float>(fw) / static_cast<float>(w);
    const float sy = static_cast<float>(fh) / static_cast<float>(h);
    const float gate_r1 = cfg_.chroma_radius * cfg_.chroma_feather;
    const float gate_r2 = cfg_.chroma_radius * cfg_.seg_gate_fade;
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        const size_t idx = static_cast<size_t>(y) * w + x;
        if (region[idx] != 0) continue;  // dalam oval: bagian wajah presisi
        float v = skin1[idx];
        if (v <= 0.0f) continue;
        if (use_adaptif) {
          int px = std::min(fw - 1,
                            static_cast<int>((static_cast<float>(x) + 0.5f) * sx));
          int py = std::min(fh - 1,
                            static_cast<int>((static_cast<float>(y) + 0.5f) * sy));
          float cb, cr;
          RgbToCbCr(rgba + static_cast<size_t>(py) * stride +
                        static_cast<size_t>(px) * 4, &cb, &cr);
          const bool band = cb >= cfg_.cb_min && cb <= cfg_.cb_max &&
                            cr >= cfg_.cr_min && cr <= cfg_.cr_max;
          if (!band) {
            v = 0.0f;
          } else {
            const float dcb = cb - cb0, dcr = cr - cr0;
            const float d = std::sqrt(dcb * dcb + dcr * dcr);
            if (d > gate_r1) {
              const float fade = std::clamp(1.0f - (d - gate_r1) /
                                                        (gate_r2 - gate_r1),
                                            0.0f, 1.0f);
              v *= fade;
            }
          }
        }
        v *= cfg_.body_strength;
        skin1[idx] = v;
      }
    }
    // 5d) ramp fitur berlaku utk seg juga (celah antar-fitur tetap halus).
    for (size_t i = 0; i < mask_.size(); ++i) {
      if (fsoft[i]) {
        const float mul = 1.0f - static_cast<float>(fsoft[i]) / 255.0f;
        skin1[i] *= mul;
      }
    }
    // 5e) union dengan mask wajah landmark.
    for (size_t i = 0; i < mask_.size(); ++i) {
      const uint8_t sv =
          static_cast<uint8_t>(std::min(255.0f, skin1[i] * 255.0f + 0.5f));
      if (sv > mask_[i]) mask_[i] = sv;
    }
  }

  // 6) M2j: perataan temporal KOMPENSASI GERAK + adaptif (alpha naik saat
  //    gerak cepat -> lag 20-40 px dari M2i hilang; diam -> tetap halus).
  double cen_x = 0.0, cen_y = 0.0;
  int cen_n = 0;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      if (mask_[static_cast<size_t>(y) * w + x] > 8) {
        cen_x += x;
        cen_y += y;
        ++cen_n;
      }
    }
  }
  const float cx = cen_n ? static_cast<float>(cen_x / cen_n)
                         : static_cast<float>(w) * 0.5f;
  const float cy = cen_n ? static_cast<float>(cen_y / cen_n)
                         : static_cast<float>(h) * 0.5f;
  if (has_prev_ && cen_n > 0) {
    const float dx = cx - prev_cx_;
    const float dy = cy - prev_cy_;
    const float speed = std::sqrt(dx * dx + dy * dy);  // px mask / frame
    // Geser mask lama sebesar delta centroid (kompensasi translasi).
    const int ix = static_cast<int>(std::lround(dx));
    const int iy = static_cast<int>(std::lround(dy));
    prev_shift_.assign(static_cast<size_t>(n), 0);
    for (int y = 0; y < h; ++y) {
      const int sy2 = std::clamp(y - iy, 0, h - 1);
      for (int x = 0; x < w; ++x) {
        const int sx2 = std::clamp(x - ix, 0, w - 1);
        prev_shift_[static_cast<size_t>(y) * w + x] =
            prev_[static_cast<size_t>(sy2) * w + sx2];
      }
    }
    const float alpha =
        std::clamp(cfg_.temporal_alpha + cfg_.temporal_speed_gain * speed,
                   cfg_.temporal_alpha, 1.0f);
    for (size_t i = 0; i < mask_.size(); ++i) {
      const float a = static_cast<float>(mask_[i]);
      const float b = static_cast<float>(prev_shift_[i]);
      mask_[i] = static_cast<uint8_t>(alpha * a + (1.0f - alpha) * b + 0.5f);
    }
    last_alpha_ = alpha;
  } else if (has_prev_) {
    TemporalBlend(prev_.data(), mask_.data(), n, cfg_.temporal_alpha);
    last_alpha_ = cfg_.temporal_alpha;
  } else {
    last_alpha_ = 1.0f;
  }
  prev_ = mask_;
  prev_cx_ = cx;
  prev_cy_ = cy;
  has_prev_ = true;

  // 7) M2j: inti fitur frame INI selalu keras 0 SETELAH temporal (lubang
  //    mengikuti landmark frame N - bukan tertinggal).
  for (size_t i = 0; i < mask_.size(); ++i) {
    if (feat[i]) mask_[i] = 0;
  }
  face_valid_ = true;
  return true;
}

}  // namespace kgcore
