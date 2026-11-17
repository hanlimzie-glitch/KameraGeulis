#pragma once
// M2k — matematika layer mask yang DIPELANGGARI GPU (mask_gen.h) dan
// fallback CPU. Satu sumber kebenaran untuk urutan: union dulu →
// pengecualian dikalikan (1−excl) → ambang keras. Uji Linux (kg_tests)
// memverifikasi urutan & sifat luar-mask di sini; paritas GPU hanya bisa
// diverifikasi di PC ber-GPU (jujur, tercatat di TESTING-WINDOWS.md).
#include <cmath>
#include <cstdint>
#include <vector>

namespace kgcore {

// Gerbang warna: jarak CbCr dari sampel kulit wajah. Tumpul-dalam (di area
// wajah hasil raster tidak dipakai), TAJAM di luar wajah: nol pada
// chroma_radius*(1+span) — jauh lebih ketat dari gerbang lama (2,4×radius
// linear) yang membuka bocoran pintu/kepala-merah.
struct ColorGate {
  float cb0 = 128, cr0 = 128;
  float radius = 25.0f;
  float span = 0.35f;  // lebar fade setelah radius → nol di radius*(1+span)
};

inline float GateValue(const ColorGate& g, float cb, float cr) {
  float dcb = cb - g.cb0, dcr = cr - g.cr0;
  float d = std::sqrt(dcb * dcb + dcr * dcr);
  if (d <= g.radius) return 1.0f;
  float t = (d - g.radius) / (g.radius * g.span);
  if (t >= 1.0f) return 0.0f;
  return 1.0f - t;
}

// Sampel warna kulit: ambang rata-rata CbCr dari jendela kulit (dipakai jalur
// GPU & CPU supaya gate identik).
bool SampleSkinCbCr(const float* xy, int n_pts, const uint8_t* rgba, int w,
                    int h, int stride, float* cb0, float* cr0);

// Pergeseran sampel tekstur seg akibat gerak wajah (kompensasi motion):
// delta_px = centroid_sekarang − centroid_saat_submit (frame px).
// Menghasilkan delta UV pada ruang model 256 (termasuk letterbox).
struct Letterbox {
  float scale = 1.0f, pad_x = 0.0f, pad_y = 0.0f;
  float src_w = 1, src_h = 1;
  // frame px → uv model (0..1 pada tex 256)
  void FrameToModel(float fx, float fy, float* u, float* v) const {
    float mx = fx * scale + pad_x;
    float my = fy * scale + pad_y;
    *u = mx / 256.0f;
    *v = my / 256.0f;
  }
};
// UV shift untuk sampling: konten tex seg = frame pada anchor; piksel frame
// posisi p harus mengambil konten p − delta → uv_model(p − delta).
inline void SegUvShift(const Letterbox& lb, float delta_x, float delta_y,
                       float* du, float* dv) {
  float u0, v0, u1, v1;
  lb.FrameToModel(0, 0, &u0, &v0);
  lb.FrameToModel(delta_x, delta_y, &u1, &v1);
  *du = u1 - u0;
  *dv = v1 - v0;
}

// Compose referensi CPU (paritas GPU + jalur fallback): semua di resolusi
// mask (mw×mh). xy = landmark frame-px; seg_p = P_kulit (0..1) di mask res
// (sudah di-letterbox); seg_g = P_wajah; gate dihitung dari warna frame.
// Urutan WAJIB: union(face, seg) → ×(1−excl) → ambang keras.
// thresh: nilai di bawah ini dipaksa 0 (supaya di luar mask == raw persis).
struct ComposeParams {
  float seg_body_strength = 0.75f;
  float face_trim = 0.60f;   // ambang P_wajah untuk memangkas tepi wajah
  float thresh = 0.02f;      // ~5/255
  float excl_feather = 1.0f; // radius blur pengecualian (px mask)
};

// excl: 0..1 per piksel (sudah di-blur/feather). gate: 0..1 per piksel
// (GateValue dihitung di warna piksel) atau null (tanpa gerbang warna).
// mask_out mw*mh byte.
void ComposeMaskCpu(const float* face_a, const float* seg_p, const float* seg_g,
                    const float* excl, const float* gate, int mw, int mh,
                    const ComposeParams& prm, std::vector<uint8_t>* mask_out);

// Statistik F5 per kelas seg + per rentang mask (tugas M2k butir 4).
struct F5RangeStat {
  double sum = 0;
  long n = 0;
  long ge3_n = 0;  // M2n: jumlah piksel dengan |raw-out| rata-rata kanal >= 3 level
};
struct F5DiffReport {
  F5RangeStat per_range[5];  // <0.01, 0.01–0.1, 0.1–0.5, 0.5–0.9, >0.9
  F5RangeStat per_class[6];  // seg argmax 0..5 (dari p argmax CPU)
  double outside_mean = 0;   // mask*gain < 8/255 (definisi spec)
  long outside_n = 0;
};
// diff = |raw−out| per kanal (rata-rata 3 kanal); mask dipakai bilinear
// (sumber yang sama dengan blend). argmax_seg: w×h byte (kelas 0..5) atau
// kosong → kelas dihitung 0.
void ComputeF5DiffReport(const uint8_t* raw, const uint8_t* out,
                         const uint8_t* mask, int mw, int mh, int w, int h,
                         float gain, const uint8_t* argmax_seg,
                         F5DiffReport* rep);

// Gambar amplifikasi |raw−out|×gain_ke (0..255, saturasi) untuk diff_*.png.
void WriteDiffAmplified(const uint8_t* raw, const uint8_t* out, int w, int h,
                        float amplify, std::vector<uint8_t>* rgba_out);

// M2l: versi grayscale + downsample (mis. 960x540) untuk perkecil dump F5.
void WriteDiffAmplifiedGray(const uint8_t* raw, const uint8_t* out, int src_w,
                            int src_h, float amplify, int dst_w, int dst_h,
                            std::vector<uint8_t>* gray_out);

}  // namespace kgcore
