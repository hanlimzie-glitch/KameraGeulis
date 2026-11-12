#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "kgcore/landmark.h"

using Catch::Approx;

TEST_CASE("flatten landmarks yields 936 floats in x,y order") {
  kgcore::LandmarkSet set;
  set.valid = true;
  for (size_t i = 0; i < kgcore::kLandmarkCount; ++i) {
    set.pts[i] = {float(i) / 1000.0f, float(i) / 2000.0f};
  }
  auto flat = kgcore::FlattenLandmarks(set);
  REQUIRE(flat.size() == kgcore::kLandmarkCount * 2);
  REQUIRE(flat[0] == Approx(0.0f));
  REQUIRE(flat[1] == Approx(0.0f));
  REQUIRE(flat[2] == Approx(0.001f));
  REQUIRE(flat[3] == Approx(0.0005f));
}

TEST_CASE("landmark fade ramps up and down over the fade window") {
  kgcore::LandmarkFade fade(300.0f);
  fade.Update(false, 0);
  REQUIRE(fade.Gain() == Approx(0.0f));
  fade.Update(true, 100);   // +100ms of 300ms window
  float mid = fade.Gain();
  REQUIRE(mid == Approx(1.0f / 3.0f).margin(1e-3f));
  fade.Update(true, 300);   // total 300ms -> full gain
  REQUIRE(fade.Gain() == Approx(1.0f));
  fade.Update(false, 450);  // 150ms of fade-out
  REQUIRE(fade.Gain() == Approx(0.5f).margin(1e-3f));
}

TEST_CASE("landmark fade never exceeds [0,1] and tolerates dt=0") {
  kgcore::LandmarkFade fade(300.0f);
  fade.Update(true, 0);
  fade.Update(true, 0);
  REQUIRE(fade.Gain() == Approx(1.0f));
  fade.Update(false, 10000);
  REQUIRE(fade.Gain() == Approx(0.0f));
}

TEST_CASE("default landmark set is invalid") {
  kgcore::LandmarkSet set;
  REQUIRE_FALSE(set.valid);
  REQUIRE(set.face_score == 0.0f);
}
