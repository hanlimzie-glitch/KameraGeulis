#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <cmath>
#include "kgcore/roi_align.h"

using Catch::Approx;

TEST_CASE("aligned roi: horizontal eyes give zero rotation") {
  const kgcore::Vec2 left{100.0f, 200.0f}, right{300.0f, 200.0f};
  const auto roi = kgcore::ComputeAlignedRoi(200.0f, 250.0f, 160.0f, 200.0f, left, right, 1.5f);
  REQUIRE(roi.angle_rad == Approx(0.0f).margin(1e-6f));
  REQUIRE(roi.side_px == Approx(200.0f * 1.5f));  // max(w,h) * scale
  REQUIRE(roi.cx_px == Approx(200.0f));
  REQUIRE(roi.cy_px == Approx(250.0f));
}

TEST_CASE("aligned roi: rotated eyes give matching angle") {
  const kgcore::Vec2 left{100.0f, 100.0f}, right{300.0f, 216.0f};  // dy = 116
  const auto roi = kgcore::ComputeAlignedRoi(200.0f, 200.0f, 100.0f, 100.0f, left, right, 1.5f);
  REQUIRE(roi.angle_rad == Approx(std::atan2(116.0f, 200.0f)));
  REQUIRE(roi.side_px == Approx(150.0f));
}

TEST_CASE("frame -> mesh canvas -> frame round trip is identity") {
  const auto roi = kgcore::ComputeAlignedRoi(320.0f, 240.0f, 180.0f, 220.0f,
                                             kgcore::Vec2{280.0f, 220.0f},
                                             kgcore::Vec2{360.0f, 230.0f}, 1.5f);
  const kgcore::Vec2 pt_px{350.0f, 300.0f};
  const int W = 640, H = 480;
  const kgcore::Vec2 canvas = kgcore::FrameToMeshCanvas(pt_px, roi, W, H);
  const float side = roi.side_px;
  const float dx = pt_px.x - roi.cx_px, dy = pt_px.y - roi.cy_px;
  const float ca = std::cos(-roi.angle_rad), sa = std::sin(-roi.angle_rad);
  const float ux = ca * dx - sa * dy, uy = sa * dx + ca * dy;
  REQUIRE(canvas.x == Approx(0.5f + ux / side).margin(1e-5f));
  REQUIRE(canvas.y == Approx(0.5f + uy / side).margin(1e-5f));
  // And the inverse mapping returns the original pixel position.
  const kgcore::Vec2 back_norm = kgcore::MeshCanvasToFrame(canvas, roi, W, H);
  REQUIRE(back_norm.x * W == Approx(pt_px.x).margin(1e-3f));
  REQUIRE(back_norm.y * H == Approx(pt_px.y).margin(1e-3f));
}

TEST_CASE("mesh canvas center maps to roi center in normalized frame coords") {
  const auto roi = kgcore::ComputeAlignedRoi(320.0f, 240.0f, 100.0f, 100.0f,
                                             kgcore::Vec2{300.0f, 230.0f},
                                             kgcore::Vec2{340.0f, 230.0f}, 1.5f);
  const int W = 640, H = 480;
  const kgcore::Vec2 c = kgcore::MeshCanvasToFrame({0.5f, 0.5f}, roi, W, H);
  REQUIRE(c.x == Approx(0.5f).margin(1e-6f));
  REQUIRE(c.y == Approx(0.5f).margin(1e-6f));
}
