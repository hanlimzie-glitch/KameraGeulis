#pragma once
// M2h — mask kulit wajah per frame (murni, tanpa GL/OS; diuji di Linux).
//
// Pipeline mask: poligon FACEMESH_FACE_OVAL (topologi resmi MediaPipe)
//   dikurangi poligon mata + bibir, goresan alis, dan disc lubang hidung
//   -> probabilitas warna kulit adaptif (YCbCr, kalibrasi dari sampel
//   pipi+dahi yang diambil lewat landmark) -> feather (box blur) ->
//   perataan temporal (EMA). Hasil = buffer u8 [0..255] ber-resolusi
//   lebih rendah (mis. 1/4 frame); komposit kekuatan = mask * gain
//   (gain = fade saat wajah hilang, dari LandmarkFade).
//
// Catatan jujur: rasterisasi/feather/komposit = matematika murni (teruji
// Linux); tampilan GPU (shader sampling mask) dan kualitas visual pada
// webcam nyata BELUM terverifikasi - lihat STATUS.md M2h.
#include <cstdint>
#include <vector>
#include "kgcore/landmark.h"
#include "kgcore/seg_math.h"  // M2j: SegMaskInput + letterbox segmenasi

namespace kgcore {

// M2j: cakupan mask - SEMUA kulit (default; digabung dgn segmenasi tubuh
// MediaPipe selfie_multiclass) atau hanya wajah (perilaku M2h).
enum class SkinCoverage { kAllSkin = 0, kFaceOnly = 1 };

// M2j: hasil segmenasi utk satu frame (peta probabilitas model 256x256
// sudah diaktivasi; letterbox = transformasi frame -> model utk unletterbox).
struct SegMaskInput {
  const float* probs = nullptr;  // [256*256*6] (lihat seg_math.h)
  const LetterboxInfo* letterbox = nullptr;
  int64_t frame_serial = 0;      // serial frame saat inferensi (utk lag)
  bool valid = false;
};

struct SkinMaskConfig {
  // M2j: cakupan + kekuatan kulit tubuh (di luar oval wajah) - konstanta
  // teratur; tubuh sedikit lebih lemah agar tidak terlihat plastik.
  SkinCoverage coverage = SkinCoverage::kAllSkin;
  float body_strength = 0.75f;
  float seg_gate_fade = 1.4f;        // gerbang warna luar-oval: fade di chroma_radius*fade
  float temporal_speed_gain = 0.04f; // M2j: kenaikan alpha per px gerak/frame
  int feather_radius = 1;   // px di ruang mask - tepi LUAR wajah (box blur/pass)
  int feather_passes = 2;   // pass feather (2 pass ~ gaussian lembut)
  // M2i (3c): feather LEBAR terpisah utk lubang fitur (mata/alis/bibir/
  // lubang hidung) agar transisi di sekitar fitur tidak menghasilkan
  // cincin; inti lubang tetap 0 (di-restore setelah blur).
  int feature_feather_radius = 3;
  float temporal_alpha = 0.5f;  // EMA: bobot frame BARU (sisa ke frame lama)
  // Pagar adaptif YCbCr: elips di sekitar sampel pipi/dahi + band lebar
  // klasik sebagai sanity gate (Kovac-ish, dilebarkan agar tidak
  // over-reject kulit gelap/bercahaya).
  float chroma_radius = 22.0f;      // jari-jari elips (unit chroma)
  float chroma_feather = 0.55f;     // fraksi radius untuk mulai memudar
  float cb_min = 65.0f, cb_max = 145.0f;
  float cr_min = 120.0f, cr_max = 185.0f;
};

// Topologi resmi MediaPipe (mediapipe/python/solutions/
// face_mesh_connections.py, Apache-2.0) - ring urut hasil traversal pasangan.
// Akses eksplisit untuk unit test.
namespace topology {
// 36 titik, tertutup.
extern const int kFaceOvalRing[36];
// Mata (16 titik tertutup masing-masing) - indeks model 468 legacy.
extern const int kLeftEyeRing[16];   // nama MP (subjek) - geometri bebas
extern const int kRightEyeRing[16];
// Bibir LUAR (20 titik tertutup; cincin dalam otomatis ikut terbuang).
extern const int kLipsOuterRing[20];
// Alis: 4 rantai terbuka (2 garis per alis) - digores tebal.
extern const int kBrowChains[4][5];
// Titik bawah sayap hidung (dari FACEMESH_NOSE) - disc lubang hidung.
extern const int kNostrilPts[2];
}  // namespace topology

// ---- Fungsi murni (semua bisa diuji Linux) ----

// Isi buffer (w*h) dengan `v`.
void FillMask(uint8_t* m, int w, int h, uint8_t v);

// Rasterisasi poligon tertutup (even-odd scanline) - koordinat normalisasi
// [0,1] dikali (w,h). Nilai ditulis TANPA menimpa piksel di luar poligon.
void RasterizeRing(const int* ring_idx, size_t ring_n, const LandmarkSet& lm,
                   int w, int h, uint8_t* m, uint8_t v);

// Goresan tebal rantai (jarak titik-ke-segmen <= radius_px) - untuk alis.
// radius_px <= 0 diabaikan.
void StrokeChain(const int* chain_idx, size_t chain_n, const LandmarkSet& lm,
                 int w, int h, float radius_px, uint8_t* m, uint8_t v);

// Feather: box blur terpisah (horizontal+vertikal), radius & pass dari cfg.
void FeatherMask(uint8_t* m, int w, int h, int radius, int passes);

// Perataan temporal: cur = alpha*baru + (1-alpha)*lama (dibulatkan).
void TemporalBlend(const uint8_t* prev, uint8_t* cur, int n, float alpha);

// Disc (lingkaran) di koordinat normalisasi - untuk lubang hidung.
void FillDiscN(const Vec2& c, float radius_norm, int w, int h, uint8_t* m,
               uint8_t v);

// Pusat 3 sampel warna kulit (dahi, pipi kiri, pipi kanan) dari geometri
// oval + mata. Mengembalikan false bila landmark tidak berguna.
bool SkinSamplePoints(const LandmarkSet& lm, Vec2 out_pts[3]);

// Rata-rata YCbCr dalam disc (radius px) pada frame RGBA (stride bytes).
// Mengembalikan jumlah sampel piksel (0 = gagal).
int SampleChromaAt(const uint8_t* rgba, int fw, int fh, int stride,
                   const Vec2& center, float radius_px, float* cb, float* cr);

// Probabilitas warna kulit [0..255] per piksel MASK di mana region != 0.
// cb0/cr0 = pusat elips adaptif; bila use_adaptif=false pakai band lebar saja.
void ApplySkinColorProb(const uint8_t* rgba, int fw, int fh, int stride,
                        const uint8_t* region, int mw, int mh,
                        const SkinMaskConfig& cfg, float cb0, float cr0,
                        bool use_adaptif, uint8_t* m);

// Sampling bilinear mask [0..1] di koordinat normalisasi [0,1].
float SampleMaskBilinear(const uint8_t* m, int mw, int mh, float u, float v);

// Komposit CPU (rumus identik dengan shader GPU):
//   out = raw + (beauty - raw) * (mask(u,v)/255) * gain
// ukuran penuh (w*h*4 RGBA); mask di-upsample bilinear.
void MixMasked(const uint8_t* raw, const uint8_t* beauty, const uint8_t* mask,
               int mw, int mh, int w, int h, float gain, uint8_t* out);

// Selisih rata-rata |raw-out| LUAR mask (mask*gain < 8) dan DALAM mask
// (mask*gain >= 200). Mengembalikan jumlah piksel tiap kelas.
int MaskDiffStats(const uint8_t* raw, const uint8_t* out, const uint8_t* mask,
                  int mw, int mh, int w, int h, float gain,
                  float* outside_mean_abs, float* inside_mean_abs);

// Mask u8 -> PNG grayscale-RGBA berukuran penuh (untuk dump F5).
void UpsampleMaskToGrayRgba(const uint8_t* mask, int mw, int mh, int w, int h,
                            uint8_t* rgba_out);

// ---- M2i: komposit kecantikan (CPU - rumus IDENTIK dgn shader GPU) ----

// YCbCr ruang-nyata (Cb/Cr berbasis 128, skala level 0..255).
struct YCbCrF { float y = 0.0f, cb = 128.0f, cr = 128.0f; };
inline YCbCrF RgbToYCbCrF(uint8_t r, uint8_t g, uint8_t b) {
  const float y = 0.299f * r + 0.587f * g + 0.114f * b;
  return {y, 128.0f + 0.564f * (static_cast<float>(b) - y),
          128.0f + 0.713f * (static_cast<float>(r) - y)};
}
// Balik ke RGB; hasil di-clamp 0..255 (round).
inline void YCbCrToRgb8(float y, float cb, float cr, uint8_t* rgb) {
  const float r = y + 1.402f * (cr - 128.0f);
  const float b = y + 1.772f * (cb - 128.0f);
  const float g = (y - 0.299f * r - 0.114f * b) / 0.587f;
  auto q = [](float v) {
    v += 0.5f;
    return static_cast<uint8_t>(v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v));
  };
  rgb[0] = q(r); rgb[1] = q(g); rgb[2] = q(b);
}

struct BeautyComposeConfig {
  float smooth_amount = 0.18f;  // [0,1] bobot guided smoothing luma (preset M2i)
  float chroma_retain = 0.80f;  // [0,1] retensi chroma MENTAH (1 = tanpa cast)
  int smooth_radius = 8;        // px radius box utk statistik guided
  float smooth_eps = 0.0015f;   // ambang var luma (0..1); besar = lebih halus
  // M2j kualitas: lift Cerahan proporsional DALAM mask
  // (dv = lift*(255-v); 22% -> +6..8 luma inti, 100% -> ~+30). 0 = mati.
  float lift = 0.0f;
};

// Guided smoothing luma: Y' = mix(Y, a*Y + (1-a)*meanY, amount) dengan
// a = var/(var+eps) (mean/var dari box blur radius); RGB baru = Y baru +
// chroma asli. rgba_out harus berukuran sama.
void SmoothLumaGuidedCpu(const uint8_t* rgba_in, int w, int h, int radius,
                         float eps, float amount, uint8_t* rgba_out);

// Komposit M2i (rumus identik shader GPU mask_blender):
//  1) cand = beauty dgn Y diguided (radius/eps/amount dari cfg)
//  2) mixed = mix(raw, cand, mask*gain)
//  3) CbCr_out = lerp(CbCr(mixed), CbCr(raw), chroma_retain)  <- retensi
//     chroma MENTAH (mencegah cast dingin/pink dari LUT whitening)
//  4) out = YCbCr2Rgb(Y(mixed), CbCr_out); mask*gain == 0 -> out == raw
//     BYTE-IDENTIK (syarat "luar mask tak berubah" dipertahankan).
void ComposeBeautyRaw(const uint8_t* raw, const uint8_t* beauty,
                      const uint8_t* mask, int mw, int mh, int w, int h,
                      float gain, const BeautyComposeConfig& cfg, uint8_t* out);

// Metrik M2i utk log F5 (butuh 4):
//  - delta_luma / delta_chroma: rata-rata perubahan di MASK INTI
//    (mask ter-erosi 2x dari ambang 200);
//  - detail_ratio: rasio energi detail halus (high-pass luma, mean kuadrat)
//    out/raw di mask inti (smoothing terukur);
//  - edge_band_diff: mean |out-raw| RGB di pita tepi mask (transisi);
//  - outside_diff: mean |out-raw| di luar mask (harus ~0).
struct InMaskMetrics {
  float delta_luma = 0.0f;
  float delta_chroma = 0.0f;
  float detail_ratio = 0.0f;
  float edge_band_diff = 0.0f;
  float outside_diff = 0.0f;
  int core_n = 0, edge_n = 0, outside_n = 0;
};
void ComputeInMaskMetrics(const uint8_t* raw, const uint8_t* out,
                          const uint8_t* mask, int mw, int mh, int w, int h,
                          float gain, InMaskMetrics* m);

// ---- Builder stateful (dipanggil per frame oleh BeautyPipeline) ----
class SkinMaskBuilder {
 public:
  // wajib dipanggil sekali; bila frame berubah ukuran, Configure ulang.
  void Configure(int mask_w, int mask_h, const SkinMaskConfig& cfg = {});
  // M2j: ubah cakupan (Semua kulit / Wajah saja) tanpa re-configure ukuran.
  void SetCoverage(SkinCoverage c) { cfg_.coverage = c; }
  SkinCoverage coverage() const { return cfg_.coverage; }
  bool configured() const { return mask_w_ > 0; }
  int w() const { return mask_w_; }
  int h() const { return mask_h_; }

  // rgba = frame RGBA penuh (untuk sampling warna). Bila face_present false
  // atau lm invalid, mask TIDAK diubah (gain di luar yang memudarkan).
  // Mengembalikan true bila mask diperbarui frame ini.
  // `seg` (opsional, M2j): bila coverage==kAllSkin dan seg valid, mask
  // digabung (union) dgn P_kulit hasil segmentasi + gerbang warna di luar
  // oval. Tanpa seg -> otomatis perilaku wajah-saja (fallback jelas di log).
  bool Update(const LandmarkSet& lm, bool face_present, const uint8_t* rgba,
              int fw, int fh, int stride, const SegMaskInput* seg = nullptr);

  const std::vector<uint8_t>& mask() const { return mask_; }
  bool has_face() const { return face_valid_; }
  // M2j: alpha temporal terakhir (utk log latensi/gerak).
  float LastTemporalAlpha() const { return last_alpha_; }

 private:
  SkinMaskConfig cfg_;
  int mask_w_ = 0, mask_h_ = 0;
  std::vector<uint8_t> mask_, prev_, prev_shift_;
  std::vector<uint8_t> luma_full_;
  bool has_prev_ = false;
  bool face_valid_ = false;
  float prev_cx_ = 0.0f, prev_cy_ = 0.0f;  // centroid mask (ruang mask)
  float last_alpha_ = 0.0f;
};

}  // namespace kgcore
