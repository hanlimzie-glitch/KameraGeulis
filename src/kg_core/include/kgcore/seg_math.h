#pragma once
// M2j - matematika segmentasi kulit (murni, teruji Linux; tanpa ORT/GL).
// Model: MediaPipe selfie_multiclass_256x256 (Apache-2.0).
//   Input : float32 [1,256,256,3] RGB, ternormalisasi x/127.5 - 1 -> [-1,1]
//           (referensi LiteRT demo Google; lihat THIRD_PARTY.md).
//   Output: float32 [1,256,256,6] - logit/probabilitas kelas
//           0 latar, 1 rambut, 2 kulit tubuh, 3 kulit wajah, 4 pakaian,
//           5 aksesori (dokumentasi resmi Image Segmenter).
// Softmax (bila keluaran belum berjumlah 1) dilakukan DI LUAR model.
#include <cstdint>
#include "kgcore/face_detection_decode.h"  // LetterboxInfo bersama (BlazeFace)

namespace kgcore {

constexpr int kSegInputSize = 256;
constexpr int kSegClasses = 6;
// P_p kulit = min(1, P(kulit tubuh) + P(kulit wajah)).
constexpr int kSegClassBodySkin = 2;
constexpr int kSegClassFaceSkin = 3;

// ---- Letterbox 16:9 -> 256x256 (skala jaga rasio + pad) ----
// Memakai LetterboxInfo yang sama dgn detektor wajah (face_detection_decode).
LetterboxInfo ComputeSegLetterbox(int src_w, int src_h,
                                  int dst = kSegInputSize);
// Model px -> src px (balik letterbox). false bila di luar area konten.
bool UnletterboxPoint(const LetterboxInfo& lb, float mx, float my, float* sx,
                      float* sy);
// Isi buffer input model [dst*dst*3] (RGB, -1..1) dari piksel RGBA/BGRA
// top-down; area pad direplikasi dari tepi konten (tanpa bar hitam).
void LetterboxToInput(const uint8_t* src, int w, int h, int stride, bool bgra,
                      const LetterboxInfo& lb, float* out,
                      int dst = kSegInputSize);

// ---- Aktivasi output: deteksi logit vs probabilitas, softmax bila perlu ----
// true bila keluaran perlu softmax (jumlah kelas per piksel menyimpang dari 1).
bool SegNeedsSoftmax(const float* probs, int n_pix);
void SegSoftmaxInPlace(float* logits, int n_pix, int n_class = kSegClasses);
// Aktivasi otomatis (pakai di atas bila ragu): no-op bila sudah probabilitas.
void SegActivate(float* data, int n_pix, int n_class = kSegClasses);

// P_p kulit dari peta probabilitas model (n_pix piksel, kelas berurutan
// [p0..p5] per piksel) -> out nilai [0,1].
void SegSkinProbability(const float* probs, int n_pix, float* out);

// argmax per piksel -> peta kelas uint8 (untuk statistik + PNG debug seg_).
void SegArgmax(const float* probs, int n_pix, uint8_t* out,
               int n_class = kSegClasses);

// Statistik per kelas (persentase piksel) dari peta argmax.
struct SegClassStats {
  int counts[kSegClasses] = {0, 0, 0, 0, 0, 0};
  int total = 0;
  float pct[kSegClasses] = {0, 0, 0, 0, 0, 0};
};
void SegComputeStats(const uint8_t* argmax_map, int n_pix, SegClassStats* out);

// Warna kelas utk PNG seg_ (RGBA): latar hitam, rambut biru, kulit tubuh
// hijau, kulit wajah oranye-merah, pakaian ungu, aksesori kuning.
void SegClassColor(uint8_t cls, uint8_t rgba[4]);

// ---- Unletterbox peta probabilitas -> kulit di resolusi mask ----
// probs: [dst*dst*6] (model), hasilkan skin [mw*mh] dgn bilinear + potong
// area pad (di luar konten -> 0).
void SegProbsToMaskSkin(const float* probs, const LetterboxInfo& lb, int mw,
                        int mh, float* skin_out, int dst = kSegInputSize);

// ---- Guided/joint-bilateral upsample mask kasar -> resolusi mask ----
// guide_full = luma frame RESOLUSI PENUH (uint8, gw*gh); bobot spasial di
// ruang tujuan, bobot rentang dari perbedaan panduan (luma penuh, titik-ke-
// titik) -> tepi mask mengikuti tepi gambar (bukan tepi kotak mask kasar).
void JointBilateralUpsample(const float* coarse, int cw, int ch,
                            const uint8_t* guide_full, int gw, int gh,
                            int dst_w, int dst_h, float* out,
                            float sigma_space = 2.0f, float sigma_range = 0.08f);

// ---- Union + pengecualian (dipakai builder & uji) ----
// out = max(a, b); lalu out[i]=0 bila exclude[i] != 0 (fitur wajah selalu
// dikecualikan: mata/alis/bibir/lubang hidung/rongga mulut).
void UnionAndExclude(const uint8_t* a, const uint8_t* b,
                     const uint8_t* exclude, int n, uint8_t* out);

}  // namespace kgcore
