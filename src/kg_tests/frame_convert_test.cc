#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <cstring>
#include <vector>
#include "kgcore/frame_convert.h"

using Catch::Approx;

namespace {
// 2x2 BGRA source, bottom-up memory (row 0 in memory = visual bottom).
// Visual top row: red(255,0,0) blue(0,0,255); visual bottom: green white.
// BGRA bytes: B,G,R,A.
const uint8_t kBottomUpBgra[2 * 2 * 4] = {
    // memory row 0 = visual bottom: green (B=255? no: G=255 -> B0,G255,R0), white
    0, 255, 0, 7,    255, 255, 255, 9,
    // memory row 1 = visual top: red, blue
    0, 0, 255, 11,   255, 0, 0, 13,
};
}  // namespace

TEST_CASE("convert: BGRA bottom-up -> top-down RGBA, alpha forced 255") {
  uint8_t dst[2 * 2 * 4];
  kgcore::ConvertToRgbaTopDown(kBottomUpBgra, 2, 2, 2 * 4,
                               /*flip_vertical=*/true, /*src_is_bgra=*/true, dst);
  // dst row 0 = visual top: red then blue, alpha 255 (was 11/13)
  REQUIRE(dst[0] == 255); REQUIRE(dst[1] == 0); REQUIRE(dst[2] == 0);
  REQUIRE(dst[3] == 255);
  REQUIRE(dst[4] == 0); REQUIRE(dst[5] == 0); REQUIRE(dst[6] == 255);
  REQUIRE(dst[7] == 255);
  // dst row 1 = visual bottom: green then white
  REQUIRE(dst[8] == 0); REQUIRE(dst[9] == 255); REQUIRE(dst[10] == 0);
  REQUIRE(dst[11] == 255);
  REQUIRE(dst[12] == 255); REQUIRE(dst[13] == 255); REQUIRE(dst[14] == 255);
  REQUIRE(dst[15] == 255);
}

TEST_CASE("convert: RGBA top-down keeps channels, alpha forced 255") {
  // One row, RGBA with alpha=0 (would be invisible when blended).
  const uint8_t src[8] = {10, 20, 30, 0, 40, 50, 60, 0};
  uint8_t dst[8];
  kgcore::ConvertToRgbaTopDown(src, 2, 1, 8, /*flip=*/false,
                               /*src_is_bgra=*/false, dst);
  REQUIRE(dst[0] == 10); REQUIRE(dst[1] == 20); REQUIRE(dst[2] == 30);
  REQUIRE(dst[3] == 255);
  REQUIRE(dst[4] == 40); REQUIRE(dst[5] == 50); REQUIRE(dst[6] == 60);
  REQUIRE(dst[7] == 255);
}

TEST_CASE("convert: padded source stride is honoured") {
  // 2 pixels wide but stride 16 (8 bytes padding per row).
  uint8_t src[16 * 2];
  std::memset(src, 0xAA, sizeof(src));
  // row0 (top): pixel0 = B=1,G=2,R=3, pixel1 = B=4,G=5,R=6, padding 0xEE
  const uint8_t row0[8] = {1, 2, 3, 0, 4, 5, 6, 0};
  std::memcpy(src, row0, 8);
  std::memset(src + 8, 0xEE, 8);
  // row1: pixel0 = B=7,G=8,R=9 ...
  src[16] = 7; src[17] = 8; src[18] = 9; src[19] = 0;
  src[20] = 10; src[21] = 11; src[22] = 12; src[23] = 0;

  uint8_t dst[2 * 2 * 4];
  kgcore::ConvertToRgbaTopDown(src, 2, 2, 16, false, true, dst);
  REQUIRE(dst[0] == 3); REQUIRE(dst[1] == 2); REQUIRE(dst[2] == 1);  // swapped
  REQUIRE(dst[4] == 6); REQUIRE(dst[5] == 5); REQUIRE(dst[6] == 4);
  // row1 must come from offset 16, not 8 (padding skipped)
  REQUIRE(dst[8] == 9); REQUIRE(dst[9] == 8); REQUIRE(dst[10] == 7);
  REQUIRE(dst[12] == 12); REQUIRE(dst[13] == 11); REQUIRE(dst[14] == 10);
}

TEST_CASE("stats: mean luma and center alpha") {
  // 16x16 BGRA, top-down: all pixels B=0,G=0,R=100 (luma=0.299*100=29.9),
  // center pixel alpha = 42.
  std::vector<uint8_t> src(16 * 16 * 4, 0);
  for (int y = 0; y < 16; ++y)
    for (int x = 0; x < 16; ++x) {
      uint8_t* px = src.data() + (y * 16 + x) * 4;
      px[2] = 100;  // R (BGRA layout)
      px[3] = 255;
    }
  src[(8 * 16 + 8) * 4 + 3] = 42;
  const auto st = kgcore::ComputeFrameStats(src.data(), 16, 16, 16 * 4,
                                            /*flip=*/false, /*bgra=*/true);
  REQUIRE(st.sampled > 0);
  REQUIRE(st.mean_luma == Approx(0.299 * 100.0).margin(0.01));
  REQUIRE(st.center_alpha == 42);
}

TEST_CASE("stats: bottom-up source reads the right center row") {
  // 4x4, bottom-up: memory row 0 (first) = visual bottom with alpha 7;
  // visual center row (y=1) has alpha 99 in memory row (4-1-1)=2.
  std::vector<uint8_t> src(4 * 4 * 4, 0);
  auto px = [&](int mem_row, int x) { return src.data() + (mem_row * 4 + x) * 4; };
  for (int r = 0; r < 4; ++r)
    for (int x = 0; x < 4; ++x) px(r, x)[3] = (r == 0) ? 7 : 0;
  // visual center: cy = 2 for height 4 -> memory row = 4-1-2 = 1
  px(1, 2)[3] = 99;
  const auto st = kgcore::ComputeFrameStats(src.data(), 4, 4, 4 * 4,
                                            /*flip=*/true, /*bgra=*/true);
  REQUIRE(st.center_alpha == 99);
}
