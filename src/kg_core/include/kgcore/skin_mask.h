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

namespace kgcore {

struct SkinMaskConfig {
  int feather_radius = 1;   // px di ruang mask (box blur per pass)
  int feather_passes = 2;   // pass feather (2 pass ~ gaussian lembut)
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

// ---- Builder stateful (dipanggil per frame oleh BeautyPipeline) ----
class SkinMaskBuilder {
 public:
  // wajib dipanggil sekali; bila frame berubah ukuran, Configure ulang.
  void Configure(int mask_w, int mask_h, const SkinMaskConfig& cfg = {});
  bool configured() const { return mask_w_ > 0; }
  int w() const { return mask_w_; }
  int h() const { return mask_h_; }

  // rgba = frame RGBA penuh (untuk sampling warna). Bila face_present false
  // atau lm invalid, mask TIDAK diubah (gain di luar yang memudarkan).
  // Mengembalikan true bila mask diperbarui frame ini.
  bool Update(const LandmarkSet& lm, bool face_present, const uint8_t* rgba,
              int fw, int fh, int stride);

  const std::vector<uint8_t>& mask() const { return mask_; }
  bool has_face() const { return face_valid_; }

 private:
  SkinMaskConfig cfg_;
  int mask_w_ = 0, mask_h_ = 0;
  std::vector<uint8_t> mask_, prev_;
  bool has_prev_ = false;
  bool face_valid_ = false;
};

}  // namespace kgcore
