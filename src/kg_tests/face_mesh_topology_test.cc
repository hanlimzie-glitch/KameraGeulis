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
  auto ring = [&](int base, float cx, float cy, float rx, float ry) {
    for (int k = 0; k < 16; ++k) {
      float a = static_cast<float>(k) / 16.0f * 6.28318f;
      xy[(base + k) * 2] = cx + rx * std::cos(a);
      xy[(base + k) * 2 + 1] = cy + ry * std::sin(a);
    }
  };
  ring(33, 250, 200, 30, 14);
  ring(263, 390, 200, 30, 14);
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
  xy[48 * 2] = 350; xy[48 * 2 + 1] = 290;
  xy[49 * 2] = 336; xy[49 * 2 + 1] = 292;
  xy[278 * 2] = 290; xy[278 * 2 + 1] = 290;
  xy[279 * 2] = 304; xy[279 * 2 + 1] = 292;
  xy[1 * 2] = 320; xy[1 * 2 + 1] = 285;
  xy[4 * 2] = 320; xy[4 * 2 + 1] = 300;
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
  REQUIRE(r.rx > 1.0f);
  REQUIRE(r.ry > 1.0f);
  REQUIRE(r.rx < 40.0f);  // elips kecil — bukan disk besar
  REQUIRE(l.cx < r.cx);   // cermin sumbu hidung
  REQUIRE(std::fabs(l.cy - r.cy) < 1.0f);
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
