#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <vector>
#include "kgcore/seg_math.h"
#include "kgcore/skin_mask.h"

using namespace kgcore;

// ===================== M2j - matematika segmentasi =====================

TEST_CASE("M2j: letterbox 16:9 -> 256 dan unletterbox roundtrip") {
  const LetterboxInfo lb = ComputeSegLetterbox(1920, 1080, 256);
  REQUIRE(lb.dst == 256);
  REQUIRE(lb.src_w == 1920);
  // skala min(256/1920, 256/1080) = 256/1920
  REQUIRE(std::fabs(lb.scale - 256.0f / 1920.0f) < 1e-6f);
  // konten 256x144, pad vertikal (256-144)/2 = 56
  REQUIRE(std::fabs(lb.pad_x) < 1e-6f);
  REQUIRE(std::fabs(lb.pad_y - 56.0f) < 1e-6f);

  // Roundtrip: titik pusat frame -> model -> balik.
  float mx = 1920 * 0.5f * lb.scale + lb.pad_x;
  float my = 1080 * 0.5f * lb.scale + lb.pad_y;
  float sx = 0, sy = 0;
  REQUIRE(UnletterboxPoint(lb, mx, my, &sx, &sy));
  REQUIRE(std::fabs(sx - 960.0f) < 1.0f);
  REQUIRE(std::fabs(sy - 540.0f) < 1.0f);
  // Titik di area pad -> false.
  REQUIRE_FALSE(UnletterboxPoint(lb, 10.0f, 10.0f, &sx, &sy));
}

TEST_CASE("M2j: letterbox input normalisasi [-1,1] + pad replikasi tepi") {
  const int w = 4, h = 2;
  std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4, 0);
  // Kiri merah (255,0,0), kanan biru (0,0,255).
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      uint8_t* p = rgba.data() + (static_cast<size_t>(y) * w + x) * 4;
      if (x < w / 2) { p[0] = 255; p[1] = 0; p[2] = 0; }
      else { p[0] = 0; p[1] = 0; p[2] = 255; }
      p[3] = 255;
    }
  }
  const LetterboxInfo lb = ComputeSegLetterbox(w, h, 16);
  std::vector<float> out(16 * 16 * 3, -9.0f);
  LetterboxToInput(rgba.data(), w, h, w * 4, /*bgra=*/false, lb, out.data(), 16);
  // Sampel kiri-atas konten (area pad atas: replikasi -> tetap warna tepi).
  auto at = [&](int x, int y) { return &out[(y * 16 + x) * 3]; };
  const int scaled_w = static_cast<int>(std::round(w * lb.scale));
  const int scaled_h = static_cast<int>(std::round(h * lb.scale));
  // Nilai RGB ter-normalisasi: R=255 -> 1.0; B=255 -> 1.0; R=0 -> -1.
  REQUIRE(std::fabs(lb.pad_y - 4.0f) < 1e-5f);  // (16-8)/2
  // Titik di dalam konten: kiri-atas (merah) dan kanan-bawah (biru).
  {
    const float* p = at(static_cast<int>(lb.pad_x), static_cast<int>(lb.pad_y));
    REQUIRE(std::fabs(p[0] - 1.0f) < 1e-5f);   // R merah
    REQUIRE(std::fabs(p[1] - (-1.0f)) < 1e-5f);
  }
  {
    const float* p = at(static_cast<int>(lb.pad_x) + scaled_w - 1,
                        static_cast<int>(lb.pad_y) + scaled_h - 1);
    REQUIRE(std::fabs(p[2] - 1.0f) < 1e-5f);   // B biru
  }
  // Area pad atas tereplikasi dari tepi (bukan hitam 0/255 mentah):
  {
    const float* p = at(4, 0);  // di atas kolom merah (sx=1)
    REQUIRE(std::fabs(p[0] - 1.0f) < 1e-5f);   // tetap merah (replikasi)
  }
}

TEST_CASE("M2j: deteksi logit + softmax -> probabilitas; P_kulit = p2+p3") {
  // 2 piksel: satu sudah probabilitas (jumlah 1), satu logit.
  std::vector<float> data(2 * kSegClasses, 0.0f);
  float* a = data.data();
  a[0] = 0.2f; a[1] = 0.1f; a[2] = 0.3f; a[3] = 0.4f; a[4] = 0.0f; a[5] = 0.0f;
  // jumlah = 1.0 -> tidak perlu softmax untuk seluruh peta bila semua begini;
  // uji per fungsi:
  REQUIRE_FALSE(SegNeedsSoftmax(data.data(), 1));
  float* b = data.data() + kSegClasses;
  b[0] = 2.0f; b[1] = -1.0f; b[2] = 3.0f; b[3] = 1.0f; b[4] = 0.0f; b[5] = -2.0f;
  // Pakai piksel logit saja utk keputusan softmax:
  REQUIRE(SegNeedsSoftmax(b, 1));
  SegActivate(data.data(), 2);  // deteksi dari sampel campuran -> softmax total
  // Setelah aktivasi: jumlah tiap piksel ~1.
  for (int i = 0; i < 2; ++i) {
    float s = 0;
    for (int c = 0; c < kSegClasses; ++c) s += data[i * kSegClasses + c];
    REQUIRE(std::fabs(s - 1.0f) < 1e-4f);
  }
  // P_kulit = p2+p3 (dibatasi 1).
  std::vector<float> probs = {0.05f, 0.10f, 0.40f, 0.45f, 0.0f, 0.0f};  // sum=1
  float skin = 0;
  SegSkinProbability(probs.data(), 1, &skin);
  REQUIRE(std::fabs(skin - 0.85f) < 1e-6f);
  probs[2] = 0.7f; probs[3] = 0.7f;  // overflow -> dipotong 1
  SegSkinProbability(probs.data(), 1, &skin);
  REQUIRE(skin == 1.0f);
}

TEST_CASE("M2j: argmax + statistik per kelas + warna PNG seg_") {
  std::vector<float> probs(4 * kSegClasses, 0.0f);
  auto set = [&](int i, int cls) {
    for (int c = 0; c < kSegClasses; ++c) probs[i * kSegClasses + c] = 0.01f;
    probs[i * kSegClasses + cls] = 1.0f;
  };
  set(0, 0); set(1, 3); set(2, 3); set(3, 2);
  std::vector<uint8_t> am(4);
  SegArgmax(probs.data(), 4, am.data());
  REQUIRE(am[0] == 0);
  REQUIRE(am[1] == 3);
  REQUIRE(am[3] == 2);
  SegClassStats st;
  SegComputeStats(am.data(), 4, &st);
  REQUIRE(st.total == 4);
  REQUIRE(st.counts[3] == 2);
  REQUIRE(st.counts[2] == 1);
  REQUIRE(std::fabs(st.pct[3] - 50.0f) < 1e-5f);
  float sum = 0;
  for (int c = 0; c < kSegClasses; ++c) sum += st.pct[c];
  REQUIRE(std::fabs(sum - 100.0f) < 1e-4f);
  uint8_t col[4];
  SegClassColor(3, col);
  REQUIRE(col[0] == 255);  // kulit wajah oranye-merah
  REQUIRE(col[3] == 255);
  SegClassColor(1, col);
  REQUIRE(col[2] == 220);  // rambut biru
}

TEST_CASE("M2j: guided upsampling mempertahankan tepi panduan (luma)") {
  // Panduan: tepi tegak di x = gw/2 (kiri gelap 30, kanan terang 220).
  const int gw = 64, gh = 64;
  std::vector<uint8_t> guide(gw * gh);
  for (int y = 0; y < gh; ++y)
    for (int x = 0; x < gw; ++x)
      guide[y * gw + x] = static_cast<uint8_t>(x < gw / 2 ? 30 : 220);
  // Kasar: mask tepat di tengah tapi SAMAR (transisi lebar 12 px di cw).
  const int cw = 16, ch = 16;
  std::vector<float> coarse(cw * ch, 0.0f);
  for (int y = 0; y < ch; ++y)
    for (int x = 0; x < cw; ++x) {
      // langkah keras di x=8 -> dst x=32 = posisi tepi panduan.
      coarse[y * cw + x] = x < 8 ? 0.0f : 1.0f;
    }
  std::vector<float> fine(gw * gh, 0.0f);
  JointBilateralUpsample(coarse.data(), cw, ch, guide.data(), gw, gh, gw, gh,
                         fine.data(), /*sigma_space=*/1.5f,
                         /*sigma_range=*/0.05f);
  // Tepi hasil harus SEJAJAR dgn tepi panduan (bukan tepi mask kasar):
  // di kiri tepi (x < 32-3) nilai kecil; di kanan tepi (x > 32+3) besar.
  double left = 0, right = 0;
  int ln = 0, rn = 0;
  for (int y = 20; y < 44; ++y) {
    for (int x = 20; x < 29; ++x) { left += fine[y * gw + x]; ++ln; }
    for (int x = 35; x < 44; ++x) { right += fine[y * gw + x]; ++rn; }
  }
  REQUIRE(left / ln < 0.15);
  REQUIRE(right / rn > 0.85);
  // Band transisi sempit: jumlah piksel antara 0.2..0.8 di baris tengah
  // lebih kecil dari transisi blur kasar (12 px pada skala halus ~ 48 px).
  int mid_n = 0;
  for (int x = 0; x < gw; ++x) {
    const float v = fine[32 * gw + x];
    if (v > 0.2f && v < 0.8f) ++mid_n;
  }
  REQUIRE(mid_n < 24);
}

TEST_CASE("M2j: union + pengecualian fitur (mata/alis/bibir selalu nol)") {
  const int n = 16;
  std::vector<uint8_t> face(n, 0), seg(n, 200), excl(n, 0), out(n, 1);
  excl[3] = 255;  // mis. inti mata
  excl[9] = 255;
  face[5] = 180;
  seg[5] = 90;
  UnionAndExclude(face.data(), seg.data(), excl.data(), n, out.data());
  REQUIRE(out[3] == 0);
  REQUIRE(out[9] == 0);
  REQUIRE(out[5] == 180);   // max(face, seg)
  REQUIRE(out[0] == 200);   // seg mengisi luar wajah
}

TEST_CASE("M2j: unletterbox peta seg -> kulit mask (pad di luar = 0)") {
  const int dst = kSegInputSize;
  const LetterboxInfo lb = ComputeSegLetterbox(1920, 1080, dst);
  // Peta: separuh kiri konten = kulit penuh (p2=1), selebihnya 0.
  std::vector<float> probs(static_cast<size_t>(dst) * dst * kSegClasses, 0.0f);
  for (int y = 0; y < dst; ++y) {
    for (int x = 0; x < dst; ++x) {
      float* p = probs.data() + (static_cast<size_t>(y) * dst + x) * kSegClasses;
      if (x >= lb.pad_x && x < lb.pad_x + dst / 2) p[kSegClassBodySkin] = 1.0f;
    }
  }
  const int mw = 48, mh = 27;
  std::vector<float> skin(static_cast<size_t>(mw) * mh, -1.0f);
  SegProbsToMaskSkin(probs.data(), lb, mw, mh, skin.data());
  // Kiri konten (x<mw/2) -> ~1; kanan -> ~0; baris paling atas frame juga
  // ter-mapping ke pita konten model (pad = batas luar frame, bukan bar hitam).
  REQUIRE(skin[mh / 2 * mw + mw / 4] > 0.9f);
  REQUIRE(skin[mh / 2 * mw + mw - 1] < 0.1f);
  REQUIRE(skin[1] >= 0.0f);  // baris atas: pad vertikal -> 0 (di luar konten)
  // Setiap nilai dalam [0,1].
  for (float v : skin) {
    REQUIRE(v >= 0.0f);
    REQUIRE(v <= 1.0f);
  }
}
