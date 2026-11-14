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
                             const uint8_t* rgba, int fw, int fh, int stride) {
  if (mask_w_ <= 0 || !rgba || fw <= 0 || fh <= 0) return false;
  if (!face_present || !lm.valid) {
    face_valid_ = false;
    return false;  // mask tidak diubah - gain di luar yang memudarkan
  }
  const int n = mask_w_ * mask_h_;
  const int w = mask_w_, h = mask_h_;

  // 1) Oval wajah -> 255 (domain mask u8 0..255).
  FillMask(mask_.data(), w, h, 0);
  RasterizeRing(topology::kFaceOvalRing, 36, lm, w, h, mask_.data(), 255);
  // Region oval polos (untuk gating warna: piksel di luar oval selalu 0).
  std::vector<uint8_t> region(mask_.begin(), mask_.end());

  // 2) Pengurangan: mata + bibir (poligon), alis (goresan), lubang hidung.
  RasterizeRing(topology::kRightEyeRing, 16, lm, w, h, mask_.data(), 0);
  RasterizeRing(topology::kLeftEyeRing, 16, lm, w, h, mask_.data(), 0);
  RasterizeRing(topology::kLipsOuterRing, 20, lm, w, h, mask_.data(), 0);
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
    StrokeChain(topology::kBrowChains[c], 5, lm, w, h, brow_r, mask_.data(), 0);
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
  FillDiscN(lm.pts[topology::kNostrilPts[0]], nostril_r, w, h, mask_.data(), 0);
  FillDiscN(lm.pts[topology::kNostrilPts[1]], nostril_r, w, h, mask_.data(), 0);

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

  // 4) Feather tepi (mask & lubang fitur sama-sama melembut).
  FeatherMask(mask_.data(), w, h, cfg_.feather_radius, cfg_.feather_passes);

  // 5) Perataan temporal.
  if (has_prev_) {
    TemporalBlend(prev_.data(), mask_.data(), n, cfg_.temporal_alpha);
  }
  prev_ = mask_;
  has_prev_ = true;
  face_valid_ = true;
  return true;
}

}  // namespace kgcore
