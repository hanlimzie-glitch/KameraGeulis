#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <fstream>
#include <sstream>
#include "kgcore/settings.h"

static std::string TempPath() {
  static int counter = 0;
  return std::string("kg_settings_test_") + std::to_string(counter++) + ".json";
}

TEST_CASE("default settings are sensible") {
  kgcore::Settings s;
  REQUIRE(s.smoothing == 0.35f);
  REQUIRE(s.whitening == 0.25f);
  REQUIRE(s.mirror);
  REQUIRE_FALSE(s.virtual_camera);
  REQUIRE(s.language == "id");
  REQUIRE(s.preset == "Natural");
}

TEST_CASE("settings round-trip through JSON") {
  kgcore::Settings a;
  a.smoothing = 0.8f; a.whitening = 0.1f; a.face_slim = 0.5f;
  a.big_eyes = 0.25f; a.lipstick = 0.6f; a.blusher = 0.4f;
  a.mirror = false; a.virtual_camera = true; a.preset = "Glam";
  a.language = "en"; a.landmark_enabled = false;
  const std::string path = TempPath();
  REQUIRE(a.Save(path));
  kgcore::Settings b;
  REQUIRE(b.Load(path));
  REQUIRE(a == b);
  std::remove(path.c_str());
}

TEST_CASE("corrupt settings file leaves defaults intact") {
  kgcore::Settings s;
  REQUIRE_FALSE(s.FromJson("{ not json !!"));
  kgcore::Settings def;
  REQUIRE(s == def);
  REQUIRE_FALSE(s.FromJson("[1,2,3]"));
  REQUIRE(s == def);
}

TEST_CASE("out-of-range values are clamped, junk types ignored") {
  kgcore::Settings s;
  REQUIRE(s.FromJson(R"({"smoothing": 5.0, "whitening": -1.0, "lipstick": "x", "language": "fr"})"));
  REQUIRE(s.smoothing == 1.0f);
  REQUIRE(s.whitening == 0.0f);
  REQUIRE(s.lipstick == kgcore::Settings{}.lipstick);  // untouched default
  REQUIRE(s.language == "id");
}

TEST_CASE("missing file load returns false and keeps defaults") {
  kgcore::Settings s;
  REQUIRE_FALSE(s.Load("definitely_no_such_file_kg.json"));
  REQUIRE(s == kgcore::Settings{});
}

TEST_CASE("unknown extra fields are ignored") {
  kgcore::Settings s;
  REQUIRE(s.FromJson(R"({"smoothing": 0.9, "future_field": {"nested": true}})"));
  REQUIRE(s.smoothing == 0.9f);
}
