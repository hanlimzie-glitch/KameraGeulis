#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <cmath>
#include "kgcore/face_anchors.h"

using Catch::Approx;

TEST_CASE("face anchors: count is 896") {
  const auto anchors = kgcore::GenerateFaceAnchors();
  REQUIRE(anchors.size() == kgcore::kNumFaceAnchors);
  REQUIRE(anchors.size() == 896);
}

TEST_CASE("face anchors: first anchor of the stride-8 group") {
  const auto anchors = kgcore::GenerateFaceAnchors();
  REQUIRE(anchors[0].cx == Approx(0.5f / 16.0f));
  REQUIRE(anchors[0].cy == Approx(0.5f / 16.0f));
  REQUIRE(anchors[0].w == 1.0f);
  REQUIRE(anchors[0].h == 1.0f);
}

TEST_CASE("face anchors: stride-8 group layout is 16x16 cells x 2 anchors") {
  const auto anchors = kgcore::GenerateFaceAnchors();
  // stride-8 group: indices [0, 512)
  // index 0: (0.5, 0.5)/16 ; index 1: same cell, interpolated anchor
  // index 511: last cell (15,15), second anchor
  const auto& last = anchors[511];
  REQUIRE(last.cx == Approx(15.5f / 16.0f));
  REQUIRE(last.cy == Approx(15.5f / 16.0f));
}

TEST_CASE("face anchors: stride-16 groups follow, 8x8 cells x 6 anchors") {
  const auto anchors = kgcore::GenerateFaceAnchors();
  const auto& first16 = anchors[512];
  // stride-16 layer 1, first cell (0,0)
  REQUIRE(first16.cx == Approx(0.5f / 8.0f));
  REQUIRE(first16.cy == Approx(0.5f / 8.0f));
  // last anchor of the whole set: stride-16 layer 4, cell (7,7), interpolated
  const auto& last = anchors[895];
  REQUIRE(last.cx == Approx(7.5f / 8.0f));
  REQUIRE(last.cy == Approx(7.5f / 8.0f));
}

TEST_CASE("face anchors: all centers strictly inside (0,1)") {
  const auto anchors = kgcore::GenerateFaceAnchors();
  for (const auto& a : anchors) {
    REQUIRE(a.cx > 0.0f);
    REQUIRE(a.cx < 1.0f);
    REQUIRE(a.cy > 0.0f);
    REQUIRE(a.cy < 1.0f);
    REQUIRE(a.w == 1.0f);
    REQUIRE(a.h == 1.0f);
  }
}

TEST_CASE("face anchors: deterministic across calls") {
  const auto a = kgcore::GenerateFaceAnchors();
  const auto b = kgcore::GenerateFaceAnchors();
  for (size_t i = 0; i < a.size(); ++i) {
    REQUIRE(a[i].cx == b[i].cx);
    REQUIRE(a[i].cy == b[i].cy);
  }
}
