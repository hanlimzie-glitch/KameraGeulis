#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <algorithm>
#include <cmath>
#include <vector>
#include "kgcore/face_detection_decode.h"

using Catch::Approx;

TEST_CASE("sigmoid is exact at 0 and symmetric") {
  REQUIRE(kgcore::Sigmoid(0.0f) == Approx(0.5f));
  REQUIRE(kgcore::Sigmoid(100.0f) == Approx(1.0f).margin(1e-7f));
  REQUIRE(kgcore::Sigmoid(-100.0f) == Approx(0.0f).margin(1e-7f));
  REQUIRE(kgcore::Sigmoid(2.0f) == Approx(1.0f - kgcore::Sigmoid(-2.0f)));
}

TEST_CASE("box iou sanity") {
  REQUIRE(kgcore::BoxIoU(0, 0, 1, 1, 0, 0, 1, 1) == Approx(1.0f));
  REQUIRE(kgcore::BoxIoU(0, 0, 1, 1, 2, 2, 3, 3) == Approx(0.0f));
  REQUIRE(kgcore::BoxIoU(0, 0, 2, 2, 1, 1, 3, 3) == Approx(1.0f / 7.0f));
}

TEST_CASE("letterbox keeps aspect ratio with symmetric zero border") {
  // Landscape source: limited by width.
  const auto lb = kgcore::ComputeLetterbox(200, 100, 128);
  REQUIRE(lb.scale == Approx(128.0f / 200.0f));
  REQUIRE(lb.pad_x == Approx(0.0f));
  REQUIRE(lb.pad_y == Approx((128.0f - 100.0f * (128.0f / 200.0f)) * 0.5f));
  // Portrait source: limited by height.
  const auto lb2 = kgcore::ComputeLetterbox(100, 200, 128);
  REQUIRE(lb2.scale == Approx(128.0f / 200.0f));
  REQUIRE(lb2.pad_y == Approx(0.0f));
}

TEST_CASE("letterbox maps tensor corner <-> frame corners round trip") {
  const auto lb = kgcore::ComputeLetterbox(640, 480, 128);
  const kgcore::Vec2 a{0.25f, 0.75f};           // normalized frame pt
  const kgcore::Vec2 t = lb.MapToTensor(a);
  const kgcore::Vec2 b = lb.MapToFrame(t);
  REQUIRE(b.x == Approx(a.x).margin(1e-4f));
  REQUIRE(b.y == Approx(a.y).margin(1e-4f));
  // Center of a 128x128 letterbox maps to the center of the frame.
  const kgcore::Vec2 c = lb.MapToFrame({64.0f, 64.0f});
  REQUIRE(c.x == Approx(0.5f).margin(1e-4f));
  REQUIRE(c.y == Approx(0.5f).margin(1e-4f));
}

TEST_CASE("decode: single high-score anchor yields one detection at expected place") {
  const auto anchors = kgcore::GenerateFaceAnchors();
  std::vector<float> regressors(896 * 16, 0.0f);
  std::vector<float> classificators(896, -100.0f);  // all background
  // Put a strong face in anchor 0. Anchor 0 center is (0.5/16, 0.5/16), w=h=1.
  // Want decoded box centered at (0.5, 0.5) with size 0.25 (raw = (target-anchor)*128).
  const float ax = anchors[0].cx, ay = anchors[0].cy;
  regressors[0] = (0.5f - ax) * 128.0f;
  regressors[1] = (0.5f - ay) * 128.0f;
  regressors[2] = 0.25f * 128.0f;   // w
  regressors[3] = 0.25f * 128.0f;   // h
  // Keypoints: left eye at (0.45, 0.45), right eye at (0.55, 0.45) — arbitrary.
  regressors[4] = (0.45f - ax) * 128.0f;
  regressors[5] = (0.45f - ay) * 128.0f;
  regressors[6] = (0.55f - ax) * 128.0f;
  regressors[7] = (0.45f - ay) * 128.0f;
  classificators[0] = 100.0f;       // sigmoid ~ 1.0

  const auto dets = kgcore::DecodeDetections(regressors.data(), classificators.data(),
                                             anchors, 0.5f, 0.3f);
  REQUIRE(dets.size() == 1);
  REQUIRE(dets[0].cx == Approx(0.5f).margin(1e-5f));
  REQUIRE(dets[0].cy == Approx(0.5f).margin(1e-5f));
  REQUIRE(dets[0].w == Approx(0.25f).margin(1e-5f));
  REQUIRE(dets[0].h == Approx(0.25f).margin(1e-5f));
  REQUIRE(dets[0].score == Approx(1.0f).margin(1e-6f));
  REQUIRE(dets[0].keypoints[0].x == Approx(0.45f).margin(1e-5f));
  REQUIRE(dets[0].keypoints[0].y == Approx(0.45f).margin(1e-5f));
  REQUIRE(dets[0].keypoints[1].x == Approx(0.55f).margin(1e-5f));
}

TEST_CASE("decode: all-background yields no detections") {
  const auto anchors = kgcore::GenerateFaceAnchors();
  std::vector<float> regressors(896 * 16, 0.0f);
  std::vector<float> classificators(896, -100.0f);
  const auto dets = kgcore::DecodeDetections(regressors.data(), classificators.data(),
                                             anchors, 0.5f, 0.3f);
  REQUIRE(dets.empty());
}

TEST_CASE("decode: near-identical duplicate anchors are merged by weighted NMS") {
  const auto anchors = kgcore::GenerateFaceAnchors();
  std::vector<float> regressors(896 * 16, 0.0f);
  std::vector<float> classificators(896, -100.0f);
  // Anchors 0 and 1 share the same cell center (stride-8, anchors 0/1).
  // Give both the same box; scores 0.9 and 0.8 respectively.
  for (int idx : {0, 1}) {
    const float ax = anchors[idx].cx, ay = anchors[idx].cy;
    regressors[idx * 16 + 0] = (0.5f - ax) * 128.0f;
    regressors[idx * 16 + 1] = (0.5f - ay) * 128.0f;
    regressors[idx * 16 + 2] = 0.25f * 128.0f;
    regressors[idx * 16 + 3] = 0.25f * 128.0f;
  }
  classificators[0] = 2.1972f;   // sigmoid = 0.9
  classificators[1] = 1.3863f;   // sigmoid = 0.8
  const auto dets = kgcore::DecodeDetections(regressors.data(), classificators.data(),
                                             anchors, 0.5f, 0.3f);
  REQUIRE(dets.size() == 1);
  REQUIRE(dets[0].score == Approx(0.9f).margin(1e-3f));
  REQUIRE(dets[0].cx == Approx(0.5f).margin(1e-5f));
}
