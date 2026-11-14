#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <array>
#include "kgcore/face_mesh_decode.h"

using Catch::Approx;

TEST_CASE("mesh decode: tensor ramp rescales to [0,1] canvas coords") {
  std::array<float, kgcore::kLandmarkCount * 3> raw{};
  for (size_t i = 0; i < raw.size(); ++i) raw[i] = static_cast<float>(i % 192) + 0.5f;
  std::array<kgcore::Vec2, kgcore::kLandmarkCount> pts{};
  kgcore::DecodeMeshTensor(raw.data(), &pts);
  for (size_t i = 0; i < pts.size(); ++i) {
    const float ex = (static_cast<float>((i * 3 + 0) % 192) + 0.5f) / 192.0f;
    const float ey = (static_cast<float>((i * 3 + 1) % 192) + 0.5f) / 192.0f;
    REQUIRE(pts[i].x == Approx(ex).margin(1e-6f));
    REQUIRE(pts[i].y == Approx(ey).margin(1e-6f));
  }
}

TEST_CASE("mesh decode: known corner values") {
  std::array<float, kgcore::kLandmarkCount * 3> raw{};
  raw[0] = 0.0f;    // pt0.x = 0
  raw[1] = 191.0f;  // pt0.y ~ 0.9948
  raw[2] = 99.0f;   // pt0.z (ignored)
  std::array<kgcore::Vec2, kgcore::kLandmarkCount> pts{};
  kgcore::DecodeMeshTensor(raw.data(), &pts);
  REQUIRE(pts[0].x == Approx(0.0f));
  REQUIRE(pts[0].y == Approx(191.0f / 192.0f));
}

TEST_CASE("mesh decode: null inputs are safe no-ops") {
  std::array<kgcore::Vec2, kgcore::kLandmarkCount> pts{};
  kgcore::DecodeMeshTensor(nullptr, &pts);  // must not crash; pts untouched
  kgcore::DecodeMeshTensor(nullptr, nullptr);  // must not crash
  REQUIRE(pts[0].x == 0.0f);
}

TEST_CASE("mesh face flag passthrough") {
  const float flag = 31.45f;
  REQUIRE(kgcore::ReadMeshFaceFlag(&flag) == Approx(31.45f));
  REQUIRE(kgcore::ReadMeshFaceFlag(nullptr) == Approx(0.0f));
}
