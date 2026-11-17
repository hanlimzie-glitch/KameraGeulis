// M2k — uji topologi wajah (Linux, tanpa GPU): triangulasi resmi, urutan
// loop pengecualian, geometri elips hidung. Data = hasil generate dari
// face_mesh_connections.py MediaPipe (Apache-2.0).
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <utility>

#include "kgcore/face_mesh_topology.h"
#include "kgcore/mask_layers.h"

namespace {

void MakeSynthetic(float* xy) {
  for (int i = 0; i < 468; ++i) {
    float a = static_cast<float>(i) / 468.0f * 6.28318f;
    xy[i * 2] = 320.0f + 150.0f * std::cos(a);
    xy[i * 2 + 1] = 240.0f + 200.0f * std::sin(a);
  }
  // Mata: isi semua 16 vertex resmi kRightEyeRing & kLeftEyeRing secara teratur.
  static const int reye_ring[16] = {33, 7, 163, 144, 145, 153, 154, 155,
                                    133, 173, 157, 158, 159, 160, 161, 246};
  static const int leye_ring[16] = {263, 249, 390, 373, 374, 380, 381, 382,
                                    362, 398, 384, 385, 386, 387, 388, 466};
  for (int k = 0; k < 16; ++k) {
    float a = 3.14159265f - static_cast<float>(k) / 16.0f * 6.2831853f;
    xy[reye_ring[k] * 2] = 250.0f + 30.0f * std::cos(a);
    xy[reye_ring[k] * 2 + 1] = 200.0f + 14.0f * std::sin(a);
    float al = static_cast<float>(k) / 16.0f * 6.2831853f;
    xy[leye_ring[k] * 2] = 390.0f + 30.0f * std::cos(al);
    xy[leye_ring[k] * 2 + 1] = 200.0f + 14.0f * std::sin(al);
  }
  // Sudut mata resmi (right eye: 33 luar, 133 dalam; left eye: 362 dalam, 263 luar)
  // sehingga pusat mata kanan = (250, 200), pusat mata kiri = (390, 200), IOD = 140.
  xy[33 * 2] = 220;  xy[33 * 2 + 1] = 200;
  xy[133 * 2] = 280; xy[133 * 2 + 1] = 200;
  xy[362 * 2] = 360; xy[362 * 2 + 1] = 200;
  xy[263 * 2] = 420; xy[263 * 2 + 1] = 200;
  int rb[5] = {46, 53, 52, 65, 55};
  int rb2[5] = {70, 63, 105, 66, 107};
  for (int k = 0; k < 5; ++k) {
    xy[rb[k] * 2] = 225.0f + k * 14.0f;
    xy[rb[k] * 2 + 1] = 165.0f;
    xy[rb2[k] * 2] = 225.0f + k * 14.0f;
    xy[rb2[k] * 2 + 1] = 178.0f;
  }
  int lb[5] = {276, 283, 282, 295, 300};
  int lb2[5] = {334, 296, 336, 293, 335};
  for (int k = 0; k < 5; ++k) {
    xy[lb[k] * 2] = 365.0f + k * 14.0f;
    xy[lb[k] * 2 + 1] = 165.0f;
    xy[lb2[k] * 2] = 365.0f + k * 14.0f;
    xy[lb2[k] * 2 + 1] = 178.0f;
  }
  static const int lip[20] = {61, 146, 91, 181, 84,  17,  314, 405, 321, 375,
                              291, 185, 40,  39,  37,  0,   267, 269, 270, 409};
  for (int k = 0; k < 20; ++k) {
    float a = static_cast<float>(k) / 20.0f * 6.28318f;
    xy[lip[k] * 2] = 320.0f + 55.0f * std::cos(a);
    xy[lip[k] * 2 + 1] = 330.0f + 22.0f * std::sin(a);
  }
  // Sayap hidung (48/49 & 278/279) ada di (±0.30, +0.55) * IOD = (278/362, 277)
  xy[48 * 2] = 278; xy[48 * 2 + 1] = 277;
  xy[49 * 2] = 285; xy[49 * 2 + 1] = 275;
  xy[278 * 2] = 362; xy[278 * 2 + 1] = 277;
  xy[279 * 2] = 355; xy[279 * 2 + 1] = 275;
  xy[1 * 2] = 320; xy[1 * 2 + 1] = 272;
  xy[2 * 2] = 320; xy[2 * 2 + 1] = 290;
  xy[4 * 2] = 320; xy[4 * 2 + 1] = 282;
  // Titik pembuka lubang hidung kanonik (±0.15, +0.64) * IOD (IOD=140 -> 320±21, 200+89.6):
  // kanan: {20, 60, 99, 242} sekitar (299, 289.6)
  xy[20 * 2] = 298.0f;  xy[20 * 2 + 1] = 288.0f;
  xy[60 * 2] = 296.0f;  xy[60 * 2 + 1] = 291.0f;
  xy[99 * 2] = 299.0f;  xy[99 * 2 + 1] = 292.5f;
  xy[242 * 2] = 303.0f; xy[242 * 2 + 1] = 287.0f;
  // kiri: {250, 290, 328, 462} sekitar (341, 289.6)
  xy[250 * 2] = 342.0f; xy[250 * 2 + 1] = 288.0f;
  xy[290 * 2] = 344.0f; xy[290 * 2 + 1] = 291.0f;
  xy[328 * 2] = 341.0f; xy[328 * 2 + 1] = 292.5f;
  xy[462 * 2] = 337.0f; xy[462 * 2 + 1] = 287.0f;
}

}  // namespace

TEST_CASE("topologi: teselasi resmi 852 segitiga manifold, indeks < 468") {
  const int n = kgcore::TessTriangleCount();
  REQUIRE(n == 852);
  const kgcore::TessTriangle* t = kgcore::TessTriangles();
  std::map<std::pair<int, int>, int> edge_use;
  for (int i = 0; i < n; ++i) {
    REQUIRE(t[i].a < 468);
    REQUIRE(t[i].b < 468);
    REQUIRE(t[i].c < 468);
    REQUIRE(t[i].a != t[i].b);
    int v[3] = {t[i].a, t[i].b, t[i].c};
    for (int k = 0; k < 3; ++k) {
      auto key = std::minmax(v[k], v[(k + 1) % 3]);
      ++edge_use[key];
    }
  }
  for (const auto& kv : edge_use) REQUIRE(kv.second <= 2);
}

TEST_CASE("topologi: loop pengecualian tertutup & berukuran benar") {
  float xy[468 * 2];
  MakeSynthetic(xy);
  auto sh = kgcore::BuildExclusionShapes(xy);
  REQUIRE(sh.loops.size() == 5);
  REQUIRE(sh.loops[0].size() == 16);  // mata kanan
  REQUIRE(sh.loops[1].size() == 16);  // mata kiri
  REQUIRE((sh.loops[2].size() >= 8 && sh.loops[2].size() <= 12));  // alis
  REQUIRE((sh.loops[3].size() >= 8 && sh.loops[3].size() <= 12));
  REQUIRE(sh.loops[4].size() == 20);  // bibir luar
  for (const auto& L : sh.loops) {
    REQUIRE(!L.empty());
    std::set<int> uniq(L.begin(), L.end());
    REQUIRE(uniq.size() == L.size());
    for (int v : L) REQUIRE((v >= 0 && v < 468));
  }
  REQUIRE(sh.ellipses.size() == 2);
  const auto& r = sh.ellipses[0];
  const auto& l = sh.ellipses[1];
  const auto basis = kgcore::ComputeFaceEyeBasis(xy);
  REQUIRE(basis.valid);
  REQUIRE(std::fabs(basis.iod - 140.0f) < 1.0f);
  float r_nx = 0, r_ny = 0, l_nx = 0, l_ny = 0;
  REQUIRE(kgcore::ToFaceNormalizedCoords(basis, r.cx, r.cy, &r_nx, &r_ny));
  REQUIRE(kgcore::ToFaceNormalizedCoords(basis, l.cx, l.cy, &l_nx, &l_ny));
  // Verifikasi terhadap koordinat ternormalisasi kanonik (±0.15, +0.64) * IOD.
  REQUIRE(std::fabs(r_nx - (-0.15f)) < 0.03f);
  REQUIRE(std::fabs(r_ny - 0.64f) < 0.03f);
  REQUIRE(std::fabs(l_nx - 0.15f) < 0.03f);
  REQUIRE(std::fabs(l_ny - 0.64f) < 0.03f);
  REQUIRE(std::fabs(r.rx - 0.035f * basis.iod) < 0.5f);
  REQUIRE(std::fabs(r.ry - 0.035f * basis.iod) < 0.5f);
  REQUIRE(r.cx < l.cx);
  REQUIRE(std::fabs(l.cy - r.cy) < 1.0f);
}

TEST_CASE("M2m topologi: lubang hidung tidak di sayap 48/278, eksklusi pusat >=0.95, dan ikut rotasi") {
  float xy[468 * 2];
  MakeSynthetic(xy);
  // Skala 3x ke resolusi 1920x1080 (IOD = 420 -> atau skala 1.26x IOD = 177 px).
  const float sx = 177.0f / 140.0f;
  const float cx0 = 320.0f, cy0 = 200.0f;
  for (int i = 0; i < 468; ++i) {
    xy[i * 2] = 949.0f + (xy[i * 2] - cx0) * sx;
    xy[i * 2 + 1] = 520.0f + (xy[i * 2 + 1] - cy0) * sx;
  }
  const int fw = 1920, fh = 1080, mw = 384, mh = 216;
  const auto sh = kgcore::BuildExclusionShapes(xy);
  REQUIRE(sh.ellipses.size() == 2);
  std::vector<uint8_t> excl;
  kgcore::RasterizeExclusionMaskCpu(xy, fw, fh, mw, mh, &excl);
  for (const auto& e : sh.ellipses) {
    const int mx = std::clamp(static_cast<int>(e.cx * mw / fw), 0, mw - 1);
    const int my = std::clamp(static_cast<int>(e.cy * mh / fh), 0, mh - 1);
    // Nilai pengecualian di pusat >= 0.95 (242/255).
    REQUIRE(excl[static_cast<size_t>(my) * mw + mx] >= 242);
  }
  // Titik sayap hidung lama (48/278) TIDAK boleh terlubangi di luar lubang asli.
  const int wing_x = std::clamp(static_cast<int>(xy[48 * 2] * mw / fw), 0, mw - 1);
  const int wing_y = std::clamp(static_cast<int>(xy[48 * 2 + 1] * mh / fh), 0, mh - 1);
  REQUIRE(excl[static_cast<size_t>(wing_y) * mw + wing_x] == 0);

  // Putar wajah +25 derajat di sekitar titik tengah mata: lubang tetap di (±0.15, +0.64).
  const float ang = 25.0f * 3.14159265f / 180.0f;
  const float ca = std::cos(ang), sa = std::sin(ang);
  for (int i = 0; i < 468; ++i) {
    const float dx = xy[i * 2] - 949.0f, dy = xy[i * 2 + 1] - 520.0f;
    xy[i * 2] = 949.0f + ca * dx - sa * dy;
    xy[i * 2 + 1] = 520.0f + sa * dx + ca * dy;
  }
  const auto sh_rot = kgcore::BuildExclusionShapes(xy);
  const auto basis_rot = kgcore::ComputeFaceEyeBasis(xy);
  float nx = 0, ny = 0;
  REQUIRE(kgcore::ToFaceNormalizedCoords(basis_rot, sh_rot.ellipses[0].cx,
                                         sh_rot.ellipses[0].cy, &nx, &ny));
  REQUIRE(std::fabs(nx - (-0.15f)) < 0.03f);
  REQUIRE(std::fabs(ny - 0.64f) < 0.03f);

  // Fallback kanonik bila titik lubang hidung tidak terisi (0,0).
  for (int id : {20, 60, 99, 242, 250, 290, 328, 462}) {
    xy[id * 2] = 0.0f;
    xy[id * 2 + 1] = 0.0f;
  }
  const auto sh_fb = kgcore::BuildExclusionShapes(xy);
  REQUIRE(kgcore::ToFaceNormalizedCoords(basis_rot, sh_fb.ellipses[0].cx,
                                         sh_fb.ellipses[0].cy, &nx, &ny));
  REQUIRE(std::fabs(nx - (-0.15f)) < 1e-4f);
  REQUIRE(std::fabs(ny - 0.64f) < 1e-4f);
}

TEST_CASE("topologi: raster triangulasi + exclusion mata/bibir di CPU referensi") {
  float xy[468 * 2];
  MakeSynthetic(xy);
  constexpr int fw = 640, fh = 480, mw = 64, mh = 48;
  std::vector<uint8_t> face, excl;
  kgcore::RasterizeFaceMaskCpu(xy, fw, fh, mw, mh, &face);
  kgcore::RasterizeExclusionMaskCpu(xy, fw, fh, mw, mh, &excl);
  auto at = [=](int x, int y) { return static_cast<size_t>(y) * mw + x; };
  size_t face_n = 0, excl_n = 0;
  for (auto v : face) face_n += v != 0;
  for (auto v : excl) excl_n += v != 0;
  REQUIRE(face_n > 100);
  REQUIRE(excl_n > 10);
  // Pixel center di mata dan mulut: pengecualian benar-benar ter-raster,
  // bukan sekadar loop landmark yang valid.
  REQUIRE(excl[at(25, 20)] == 255);
  REQUIRE(excl[at(32, 33)] == 255);
}

TEST_CASE("topologi: OrderLoop menolak rantai terbuka") {
  std::pair<uint16_t, uint16_t> sq[4] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}};
  REQUIRE(kgcore::OrderLoopFromEdges(sq, 4).size() == 4);
  std::pair<uint16_t, uint16_t> ch[3] = {{0, 1}, {1, 2}, {2, 3}};
  REQUIRE(kgcore::OrderLoopFromEdges(ch, 3).empty());
}

TEST_CASE("M2n topologi: ROI longgar melacak blob gelap lubang hidung saat yaw") {
  float xy[468 * 2];
  MakeSynthetic(xy);
  constexpr int fw = 640, fh = 480;
  const auto basis = kgcore::ComputeFaceEyeBasis(xy);
  const auto base_shapes = kgcore::BuildExclusionShapes(xy);
  REQUIRE(base_shapes.ellipses.size() == 2);

  // Frame kulit seragam (Y ~ 150) + dua lubang hidung gelap nyata yang bergeser
  // (+10, -5) px dari jangkar landmark akibat kepala menoleh (yaw).
  std::vector<uint8_t> rgba(static_cast<size_t>(fw) * fh * 4);
  for (int i = 0; i < fw * fh; ++i) {
    rgba[i * 4 + 0] = 180;
    rgba[i * 4 + 1] = 145;
    rgba[i * 4 + 2] = 120;
    rgba[i * 4 + 3] = 255;
  }
  const float target_x[2] = {base_shapes.ellipses[0].cx + 10.0f,
                             base_shapes.ellipses[1].cx + 10.0f};
  const float target_y[2] = {base_shapes.ellipses[0].cy - 5.0f,
                             base_shapes.ellipses[1].cy - 5.0f};
  for (int y = 0; y < fh; ++y) {
    for (int x = 0; x < fw; ++x) {
      for (int k = 0; k < 2; ++k) {
        const float dx = (x + 0.5f) - target_x[k];
        const float dy = (y + 0.5f) - target_y[k];
        if (dx * dx + dy * dy <= 16.0f) {
          uint8_t* p = rgba.data() + (static_cast<size_t>(y) * fw + x) * 4;
          p[0] = 25; p[1] = 20; p[2] = 18;
        }
      }
    }
  }

  kgcore::ExclusionShapes refined;
  kgcore::NostrilRoiInfo info[2]{};
  kgcore::RefineNostrilEllipsesFromLuma(base_shapes, rgba.data(), fw, fh,
                                        fw * 4, 150.0f, basis.iod, &refined,
                                        info);
  REQUIRE(refined.ellipses.size() == 2);
  for (int k = 0; k < 2; ++k) {
    REQUIRE(info[k].found_dark_blob);
    REQUIRE(std::hypot(refined.ellipses[k].cx - target_x[k],
                       refined.ellipses[k].cy - target_y[k]) < 2.0f);
    REQUIRE(info[k].offset_px < 1.5f);
  }
}

