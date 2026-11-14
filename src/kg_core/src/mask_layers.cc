#include "kgcore/mask_layers.h"

#include <algorithm>
#include <cmath>

#include "kgcore/skin_mask.h"  // SampleMaskBilinear (sudah eksis)

namespace kgcore {

bool SampleSkinCbCr(const float* xy, int n_pts, const uint8_t* rgba, int w,
                    int h, int stride, float* cb0, float* cr0) {
  if (!xy || n_pts < 16 || !rgba || w <= 0 || h <= 0 || !cb0 || !cr0)
    return false;
  // Jendela pipi (cheek): sekitar landmark pipi & dahi — diambil titik yang
  // sama seperti SkinMaskBuilder lama (pipi 2 sisi + dahi).
  static const int kWin[][2] = {
      {205, 234}, {425, 454}, {10, 338},  // pipi kanan, pipi kiri, dahi
  };
  double cb_sum = 0, cr_sum = 0;
  int n = 0;
  for (const auto& win : kWin) {
    int idx = win[0];
    if (idx >= n_pts) continue;
    int cx = static_cast<int>(xy[idx * 2]);
    int cy = static_cast<int>(xy[idx * 2 + 1]);
    for (int dy = -6; dy <= 6; dy += 2) {
      int y = cy + dy;
      if (y < 0 || y >= h) continue;
      for (int dx = -6; dx <= 6; dx += 2) {
        int x = cx + dx;
        if (x < 0 || x >= w) continue;
        const uint8_t* p = rgba + static_cast<size_t>(y) * stride + x * 4;
        float r = p[0], g = p[1], b = p[2];
        cb_sum += -0.168736f * r - 0.331264f * g + 0.5f * b + 128.0f;
        cr_sum += 0.5f * r - 0.418688f * g - 0.081312f * b + 128.0f;
        ++n;
      }
    }
  }
  if (n < 8) return false;
  *cb0 = static_cast<float>(cb_sum / n);
  *cr0 = static_cast<float>(cr_sum / n);
  return true;
}

void ComposeMaskCpu(const float* face_a, const float* seg_p, const float* seg_g,
                    const float* excl, const float* gate, int mw, int mh,
                    const ComposeParams& prm, std::vector<uint8_t>* mask_out) {
  if (!face_a || !mask_out) return;
  mask_out->assign(static_cast<size_t>(mw) * mh, 0);
  for (int y = 0; y < mh; ++y) {
    for (int x = 0; x < mw; ++x) {
      const size_t i = static_cast<size_t>(y) * mw + x;
      float face = face_a[i];
      if (seg_p && seg_g) {
        float p3 = seg_g[i];
        // Tepi wajah dipangkas pakai probabilitas kulit-wajah seg (rambut/
        // janggut ikut terbuang di lingkar wajah).
        float trim = std::clamp((p3 - (prm.face_trim - 0.15f)) / 0.30f, 0.0f, 1.0f);
        face *= trim;
        float seg = seg_p[i] * prm.seg_body_strength;
        if (gate) seg *= gate[i];  // gerbang warna per piksel
        // Di luar raster wajah, seg harus lolos gerbang prob juga (ketat).
        if (face_a[i] < 0.5f) {
          float ps = std::clamp((seg_p[i] - 0.45f) / 0.20f, 0.0f, 1.0f);
          seg *= ps;
        }
        face = std::max(face, seg);
      }
      if (excl) face *= (1.0f - std::clamp(excl[i], 0.0f, 1.0f));  // setelah union
      if (face < prm.thresh) face = 0.0f;                          // ambang keras
      (*mask_out)[i] =
          static_cast<uint8_t>(std::clamp(face * 255.0f + 0.5f, 0.0f, 255.0f));
    }
  }
}

void ComputeF5DiffReport(const uint8_t* raw, const uint8_t* out,
                         const uint8_t* mask, int mw, int mh, int w, int h,
                         float gain, const uint8_t* argmax_seg,
                         F5DiffReport* rep) {
  if (!raw || !out || !mask || !rep || w <= 0 || h <= 0) return;
  *rep = F5DiffReport{};
  const float g = std::clamp(gain, 0.0f, 1.0f);
  for (int y = 0; y < h; ++y) {
    const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(h);
    const uint8_t* r = raw + static_cast<size_t>(y) * w * 4;
    const uint8_t* o = out + static_cast<size_t>(y) * w * 4;
    for (int x = 0; x < w; ++x) {
      const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(w);
      float m = SampleMaskBilinear(mask, mw, mh, u, v) * g;
      int dsum = 0;
      for (int c = 0; c < 3; ++c)
        dsum += std::abs(static_cast<int>(r[x * 4 + c]) -
                         static_cast<int>(o[x * 4 + c]));
      double d = dsum / 3.0;
      int band = m < 0.01f ? 0 : (m < 0.1f ? 1 : (m < 0.5f ? 2 : (m < 0.9f ? 3 : 4)));
      rep->per_range[band].sum += d;
      ++rep->per_range[band].n;
      int cls = argmax_seg ? argmax_seg[static_cast<size_t>(y) * w + x] : 0;
      if (cls < 0 || cls > 5) cls = 0;
      rep->per_class[cls].sum += d;
      ++rep->per_class[cls].n;
      if (m < 8.0f / 255.0f) {
        rep->outside_mean += d;
        ++rep->outside_n;
      }
    }
  }
  if (rep->outside_n) rep->outside_mean /= rep->outside_n;
}

void WriteDiffAmplified(const uint8_t* raw, const uint8_t* out, int w, int h,
                        float amplify, std::vector<uint8_t>* rgba_out) {
  if (!raw || !out || !rgba_out || w <= 0 || h <= 0) return;
  rgba_out->assign(static_cast<size_t>(w) * h * 4, 255);
  for (int i = 0; i < w * h; ++i) {
    int d = 0;
    for (int c = 0; c < 3; ++c)
      d += std::abs(static_cast<int>(raw[i * 4 + c]) -
                    static_cast<int>(out[i * 4 + c]));
    float g = (d / 3.0f) * amplify;
    uint8_t v = static_cast<uint8_t>(std::min(g, 255.0f));
    (*rgba_out)[i * 4 + 0] = v;
    (*rgba_out)[i * 4 + 1] = v;
    (*rgba_out)[i * 4 + 2] = v;
  }
}

}  // namespace kgcore
