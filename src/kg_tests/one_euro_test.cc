#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <cmath>
#include <vector>
#include "kgcore/one_euro.h"

using Catch::Approx;

TEST_CASE("one euro filter converges to a constant signal") {
  kgcore::OneEuroFilter f(1.0f, 0.007f, 1.0f);
  float last = 0.0f;
  for (int i = 0; i < 300; ++i) last = f.Filter(1.0f, i * 16);
  REQUIRE(last == Approx(1.0f).margin(1e-3f));
}

TEST_CASE("one euro filter suppresses jitter around a constant") {
  kgcore::OneEuroFilter f(1.0f, 0.007f, 1.0f);
  const float raw[] = {0.5f, 0.54f, 0.44f, 0.52f, 0.47f, 0.53f, 0.48f, 0.51f};
  float acc = 0.0f;
  for (size_t i = 0; i < 8; ++i) acc = f.Filter(raw[i], static_cast<int64_t>(i) * 16);
  // Filtered output stays well inside the raw jitter envelope.
  const float lo = 0.44f + 0.02f, hi = 0.54f - 0.02f;
  for (size_t i = 0; i < 8; ++i) {}
  REQUIRE(acc > lo);
  REQUIRE(acc < hi);
}

TEST_CASE("one euro filter tracks a step with bounded lag") {
  kgcore::OneEuroFilter f(1.0f, 0.05f, 1.0f);
  int64_t t = 0;
  for (int i = 0; i < 60; ++i, t += 16) f.Filter(0.0f, t);
  float v = 0.0f;
  for (int i = 0; i < 60; ++i, t += 16) v = f.Filter(1.0f, t);
  REQUIRE(v > 0.9f);  // settled within ~1 s of 60 fps frames
}

TEST_CASE("one euro filter reset reinitializes state") {
  kgcore::OneEuroFilter f(1.0f, 0.007f, 1.0f);
  f.Filter(5.0f, 0);
  f.Filter(5.0f, 16);
  f.Reset();
  REQUIRE(f.Filter(2.0f, 32) == 2.0f);  // first sample after reset passes through
}

TEST_CASE("one euro vec2 filters both components") {
  kgcore::OneEuroVec2 f(1.0f, 0.007f, 1.0f);
  kgcore::Vec2 v{0.0f, 0.0f};
  for (int i = 0; i < 200; ++i) v = f.Filter({1.0f, 1.0f}, i * 16);
  REQUIRE(v.x == Approx(1.0f).margin(1e-3f));
  REQUIRE(v.y == Approx(1.0f).margin(1e-3f));
}

TEST_CASE("one euro filter tolerates non-monotonic timestamps") {
  kgcore::OneEuroFilter f(1.0f, 0.007f, 1.0f);
  f.Filter(1.0f, 100);
  float v = f.Filter(1.0f, 50);  // backwards timestamp must not crash/NaN
  REQUIRE(std::isfinite(v));
}
