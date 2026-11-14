#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <vector>
#include "kgcore/skin_mask.h"

using namespace kgcore;

namespace {

// Wajah sintetis: oval lingkaran, dua kotak mata, kotak bibir, garis alis,
// titik lubang hidung - semua indeks sesuai topologi asli.
LandmarkSet MakeSyntheticFace() {
  LandmarkSet lm;
  lm.valid = true;
  const float cx = 0.5f, cy = 0.5f, r = 0.42f;
  for (int i = 0; i < 36; ++i) {
    const float a = -3.14159265f * 0.5f +
                    static_cast<float>(i) * (2.0f * 3.14159265f / 36.0f);
    lm.pts[topology::kFaceOvalRing[i]] =
        Vec2(cx + r * std::cos(a), cy + r * std::sin(a));
  }
  auto box_ring = [&](const int* ring, int n, float x0, float y0, float x1,
                      float y1) {
    for (int i = 0; i < n; ++i) {
      float t = static_cast<float>(i) / static_cast<float>(n);
      float x, y;
      if (t < 0.25f) { x = x0 + (x1 - x0) * (t / 0.25f); y = y0; }
      else if (t < 0.5f) { x = x1; y = y0 + (y1 - y0) * ((t - 0.25f) / 0.25f); }
      else if (t < 0.75f) { x = x1 - (x1 - x0) * ((t - 0.5f) / 0.25f); y = y1; }
      else { x = x0; y = y1 - (y1 - y0) * ((t - 0.75f) / 0.25f); }
      lm.pts[ring[i]] = Vec2(x, y);
    }
  };
  // Mata: kiri-layar pada kotak kiri, kanan-layar pada kotak kanan.
  box_ring(topology::kRightEyeRing, 16, 0.27f, 0.39f, 0.39f, 0.45f);
  box_ring(topology::kLeftEyeRing, 16, 0.61f, 0.39f, 0.73f, 0.45f);
  box_ring(topology::kLipsOuterRing, 20, 0.40f, 0.68f, 0.60f, 0.76f);
  // Alis: 2 rantai mendatar per sisi (atas+bawah alis).
  const float brow_x[4][5] = {{0.28f, 0.32f, 0.35f, 0.38f, 0.40f},
                              {0.28f, 0.32f, 0.35f, 0.38f, 0.40f},
                              {0.60f, 0.62f, 0.65f, 0.68f, 0.72f},
                              {0.60f, 0.62f, 0.65f, 0.68f, 0.72f}};
  const float brow_y[4] = {0.325f, 0.365f, 0.325f, 0.365f};
  for (int c = 0; c < 4; ++c)
    for (int i = 0; i < 5; ++i)
      lm.pts[topology::kBrowChains[c][i]] = Vec2(brow_x[c][i], brow_y[c]);
  lm.pts[topology::kNostrilPts[0]] = Vec2(0.47f, 0.60f);
  lm.pts[topology::kNostrilPts[1]] = Vec2(0.53f, 0.60f);
  return lm;
}

// Frame RGBA: warna kulit + satu patch abu-abu ("janggut/bayangan").
std::vector<uint8_t> MakeFaceRgba(int w, int h) {
  std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const float u = (static_cast<float>(x) + 0.5f) / w;
      const float v = (static_cast<float>(y) + 0.5f) / h;
      const bool grey = u >= 0.42f && u <= 0.58f && v >= 0.80f && v <= 0.86f;
      uint8_t* p = px.data() + (static_cast<size_t>(y) * w + x) * 4;
      p[0] = grey ? 110 : 220;  // R
      p[1] = grey ? 110 : 170;  // G
      p[2] = grey ? 110 : 145;  // B
      p[3] = 255;
    }
  }
  return px;
}

// Rata-rata mask di jendela kecil (koordinat normalisasi).
float WindowMean(const std::vector<uint8_t>& m, int mw, int mh, float u,
                 float v, float rad) {
  double sum = 0.0;
  int n = 0;
  const int x0 = std::max(0, static_cast<int>((u - rad) * mw));
  const int x1 = std::min(mw - 1, static_cast<int>((u + rad) * mw));
  const int y0 = std::max(0, static_cast<int>((v - rad) * mh));
  const int y1 = std::min(mh - 1, static_cast<int>((v + rad) * mh));
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x) { sum += m[y * mw + x]; ++n; }
  return n ? static_cast<float>(sum / n) : 0.0f;
}

}  // namespace

TEST_CASE("skin mask: rasterisasi poligon - dalam penuh, luar kosong") {
  LandmarkSet lm;
  lm.valid = true;
  lm.pts[0] = Vec2(0.2f, 0.2f);
  lm.pts[1] = Vec2(0.8f, 0.2f);
  lm.pts[2] = Vec2(0.8f, 0.8f);
  lm.pts[3] = Vec2(0.2f, 0.8f);
  const int ring[4] = {0, 1, 2, 3};
  std::vector<uint8_t> m(16 * 16, 0);
  RasterizeRing(ring, 4, lm, 16, 16, m.data(), 255);
  REQUIRE(m[8 * 16 + 8] == 255);    // pusat
  REQUIRE(m[6 * 16 + 6] == 255);    // di dalam (0.375*16=6 -> >=4)
  REQUIRE(m[1 * 16 + 1] == 0);      // luar pojok
  REQUIRE(m[15 * 16 + 15] == 0);    // luar pojok kanan-bawah
}

TEST_CASE("skin mask: goresan alis (capsule) memotong hanya di sekitar garis") {
  LandmarkSet lm;
  lm.valid = true;
  lm.pts[0] = Vec2(0.1f, 0.5f);
  lm.pts[1] = Vec2(0.9f, 0.5f);
  const int chain[2] = {0, 1};
  std::vector<uint8_t> m(16 * 16, 1);
  StrokeChain(chain, 2, lm, 16, 16, 2.0f, m.data(), 0);
  REQUIRE(m[8 * 16 + 8] == 0);   // tepat di garis
  REQUIRE(m[9 * 16 + 8] == 0);   // masih dalam radius 2
  REQUIRE(m[4 * 16 + 8] == 1);   // 4 px dari garis - tidak tersentuh
}

TEST_CASE("skin mask: feather membuat gradien lembut, interior & luar utuh") {
  std::vector<uint8_t> m(32 * 32, 0);
  for (int y = 0; y < 32; ++y)
    for (int x = 0; x < 16; ++x) m[y * 32 + x] = 255;
  FeatherMask(m.data(), 32, 32, 2, 2);
  REQUIRE(m[16 * 32 + 1] == 255);   // jauh di dalam
  REQUIRE(m[16 * 32 + 31] == 0);    // jauh di luar
  const uint8_t edge = m[16 * 32 + 15];
  REQUIRE(edge > 0);                // ada nilai antara (gradien lembut)
  REQUIRE(edge < 255);
}

TEST_CASE("skin mask: temporal blend = EMA persis") {
  const uint8_t prev[4] = {0, 255, 100, 50};
  uint8_t cur[4] = {255, 0, 200, 150};
  TemporalBlend(prev, cur, 4, 0.5f);
  REQUIRE(cur[0] == 128);  // round(0.5*255 + 0.5*0)
  REQUIRE(cur[1] == 128);
  REQUIRE(cur[2] == 150);  // round(0.5*200 + 0.5*100)
  REQUIRE(cur[3] == 100);
}

TEST_CASE("skin mask: disc lubang hidung") {
  std::vector<uint8_t> m(16 * 16, 1);
  FillDiscN(Vec2(0.5f, 0.5f), 0.125f, 16, 16, m.data(), 0);  // r = 2 px
  REQUIRE(m[8 * 16 + 8] == 0);
  REQUIRE(m[14 * 16 + 8] == 1);  // jauh di luar disc
}

TEST_CASE("skin mask: rumus komposit - piksel LUAR mask tidak berubah sama sekali") {
  const int w = 16, h = 16;
  std::vector<uint8_t> raw(w * h * 4), beauty(w * h * 4);
  for (int i = 0; i < w * h; ++i) {
    raw[i * 4 + 0] = 200; raw[i * 4 + 1] = 100; raw[i * 4 + 2] = 50;
    beauty[i * 4 + 0] = 40; beauty[i * 4 + 1] = 80; beauty[i * 4 + 2] = 210;
    raw[i * 4 + 3] = beauty[i * 4 + 3] = 255;
  }
  std::vector<uint8_t> mask(8 * 8, 0);
  for (int y = 2; y < 6; ++y)
    for (int x = 2; x < 6; ++x) mask[y * 8 + x] = 255;
  std::vector<uint8_t> out(w * h * 4, 0);
  MixMasked(raw.data(), beauty.data(), mask.data(), 8, 8, w, h, 1.0f, out.data());
  // LUAR mask: byte-identik dengan raw (syarat M2h butuh ketat).
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const float u = (x + 0.5f) / w, v = (y + 0.5f) / h;
      const float t = SampleMaskBilinear(mask.data(), 8, 8, u, v);
      if (t > 0.0f) continue;  // hanya kelas mask=0 yang dicek ketat
      for (int c = 0; c < 4; ++c) {
        REQUIRE(out[(y * w + x) * 4 + c] == raw[(y * w + x) * 4 + c]);
      }
    }
  }
  // Di dalam mask penuh (piksel jauh dari tepi patch): hasil = beauty.
  const int ci = (8 * w + 8) * 4;
  REQUIRE(out[ci + 0] == 40);
  REQUIRE(out[ci + 1] == 80);
  REQUIRE(out[ci + 2] == 210);
  REQUIRE(out[ci + 3] == 255);
}

TEST_CASE("skin mask: gain 0 = efek mati total (tanpa wajah -> fade)") {
  const int w = 8, h = 8;
  std::vector<uint8_t> raw(w * h * 4, 10), beauty(w * h * 4, 250);
  for (int i = 0; i < w * h; ++i) raw[i * 4 + 3] = beauty[i * 4 + 3] = 255;
  std::vector<uint8_t> mask(4 * 4, 255);
  std::vector<uint8_t> out(w * h * 4, 0);
  MixMasked(raw.data(), beauty.data(), mask.data(), 4, 4, w, h, 0.0f, out.data());
  for (size_t i = 0; i < out.size(); ++i) REQUIRE(out[i] == raw[i]);
}

TEST_CASE("skin mask: statistik selisih - luar nol bila hanya dalam mask berubah") {
  const int w = 16, h = 16;
  std::vector<uint8_t> raw(w * h * 4), out(w * h * 4);
  std::vector<uint8_t> mask(8 * 8, 0);
  for (int y = 3; y < 6; ++y)
    for (int x = 3; x < 6; ++x) mask[y * 8 + x] = 255;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const bool inside = x >= 6 && x <= 11 && y >= 6 && y <= 11;
      uint8_t* r = raw.data() + (y * w + x) * 4;
      uint8_t* o = out.data() + (y * w + x) * 4;
      r[0] = o[0] = 120; r[3] = o[3] = 255;
      r[1] = 120; o[1] = inside ? 220 : 120;  // hanya dalam patch berubah
      r[2] = 120; o[2] = inside ? 180 : 120;
    }
  }
  float outside = -1.0f, inside = -1.0f;
  MaskDiffStats(raw.data(), out.data(), mask.data(), 8, 8, w, h, 1.0f, &outside,
                &inside);
  REQUIRE(outside == 0.0f);  // luar mask: NOL mutlak
  REQUIRE(inside > 30.0f);   // dalam mask: ada selisih nyata
}

TEST_CASE("skin mask: upsample mask -> grayscale RGBA") {
  std::vector<uint8_t> mask(2 * 2, 0);
  mask[0] = 255;  // hanya pojok kiri-atas
  std::vector<uint8_t> rgba(4 * 4 * 4, 0);
  UpsampleMaskToGrayRgba(mask.data(), 2, 2, 4, 4, rgba.data());
  REQUIRE(rgba[3] == 255);          // alpha selalu 255
  REQUIRE(rgba[0] > 200);           // pojok kiri-atas ~ mask 255
  const size_t br = (3 * 4 + 3) * 4;
  REQUIRE(rgba[br + 0] < 60);       // pojok kanan-bawah ~ mask 0
  REQUIRE(rgba[br + 3] == 255);
}

TEST_CASE("skin mask builder E2h: oval berisi, mata/bibir/alis/hitung terbuang, warna non-kulit keluar") {
  const int fw = 192, fh = 192;
  const int mw = 96, mh = 96;  // 1/4 resolusi - geometri cukup besar utk feather
  SkinMaskBuilder b;
  b.Configure(mw, mh);
  REQUIRE(b.configured());
  LandmarkSet lm = MakeSyntheticFace();
  std::vector<uint8_t> rgba = MakeFaceRgba(fw, fh);
  REQUIRE(b.Update(lm, /*face_present=*/true, rgba.data(), fw, fh, fw * 4));
  REQUIRE(b.has_face());
  const auto& m = b.mask();
  REQUIRE(m.size() == static_cast<size_t>(mw) * mh);
  // Kulit pipi (jauh dari fitur): tinggi.
  REQUIRE(WindowMean(m, mw, mh, 0.17f, 0.55f, 0.03f) > 150.0f);
  // Mata kiri-layar: terbuang.
  REQUIRE(WindowMean(m, mw, mh, 0.33f, 0.42f, 0.03f) < 80.0f);
  // Bibir: terbuang.
  REQUIRE(WindowMean(m, mw, mh, 0.50f, 0.72f, 0.03f) < 80.0f);
  // Alis kiri-layar: terbuang.
  REQUIRE(WindowMean(m, mw, mh, 0.34f, 0.345f, 0.03f) < 120.0f);
  // Lubang hidung: terbuang (jendela ketat di titik 48).
  REQUIRE(WindowMean(m, mw, mh, 0.47f, 0.60f, 0.015f) < 100.0f);
  // Patch abu-abu (bukan warna kulit) di dalam oval: rendah (inti patch saja).
  REQUIRE(WindowMean(m, mw, mh, 0.50f, 0.83f, 0.015f) < 80.0f);
  // Di luar oval (pojok frame): 0.
  REQUIRE(WindowMean(m, mw, mh, 0.03f, 0.03f, 0.02f) == 0.0f);
}

TEST_CASE("skin mask builder: wajah hilang -> mask TIDAK diubah (gain yang fade)") {
  const int fw = 64, fh = 64;
  SkinMaskBuilder b;
  b.Configure(16, 16);
  LandmarkSet lm = MakeSyntheticFace();
  std::vector<uint8_t> rgba = MakeFaceRgba(fw, fh);
  REQUIRE(b.Update(lm, true, rgba.data(), fw, fh, fw * 4));
  std::vector<uint8_t> before = b.mask();
  // Frame berikutnya: wajah hilang.
  REQUIRE_FALSE(b.Update(lm, false, rgba.data(), fw, fh, fw * 4));
  REQUIRE_FALSE(b.has_face());
  REQUIRE(std::memcmp(before.data(), b.mask().data(), before.size()) == 0);
}
