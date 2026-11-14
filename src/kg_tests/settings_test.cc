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
  // M2i: default diturunkan ke nilai Natural (lebih natural).
  REQUIRE(s.smoothing == 0.18f);
  REQUIRE(s.whitening == 0.06f);
  REQUIRE(s.chroma_retain == 0.8f);
  REQUIRE(s.mirror);
  REQUIRE_FALSE(s.virtual_camera);
  REQUIRE(s.language == "id");
  REQUIRE(s.preset == "Natural");
}

TEST_CASE("settings round-trip through JSON") {
  kgcore::Settings a;
  a.smoothing = 0.8f; a.whitening = 0.1f; a.face_slim = 0.5f;
  a.chroma_retain = 0.55f;
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

TEST_CASE("camera fields round-trip and clamp") {
  kgcore::Settings a;
  REQUIRE(a.camera_device == 0);
  REQUIRE(a.capture_width == 1280);
  REQUIRE(a.capture_height == 720);
  REQUIRE(a.capture_fps == 30);
  a.camera_device = 3;
  a.capture_width = 640;
  a.capture_height = 480;
  a.capture_fps = 60;
  const std::string path = TempPath();
  REQUIRE(a.Save(path));
  kgcore::Settings b;
  REQUIRE(b.Load(path));
  REQUIRE(b.camera_device == 3);
  REQUIRE(b.capture_width == 640);
  REQUIRE(b.capture_height == 480);
  REQUIRE(b.capture_fps == 60);
  std::remove(path.c_str());
}

TEST_CASE("camera fields clamp out-of-range and junk") {
  kgcore::Settings s;
  REQUIRE(s.FromJson(R"({"camera_device": 99, "capture_width": -5, "capture_height": 99999, "capture_fps": 1000})"));
  REQUIRE(s.camera_device == 16);
  REQUIRE(s.capture_width == 16);
  REQUIRE(s.capture_height == 4320);
  REQUIRE(s.capture_fps == 240);
  kgcore::Settings s2;
  REQUIRE(s2.FromJson(R"({"camera_device": "x", "capture_fps": "30"})"));
  REQUIRE(s2.camera_device == 0);
  REQUIRE(s2.capture_fps == 30);
}

TEST_CASE("M2i: Save menimpa tujuan yang sudah ada + err menyebut penyebab") {
  // Bug PC pengguna: save pertama OK, save berikutnya GAGAL di Windows
  // (rename C89 tidak menimpa tujuan). std::filesystem::rename harus bisa.
  kgcore::Settings a;
  a.smoothing = 0.7f;
  a.camera_device = 3;
  const std::string path = TempPath();
  REQUIRE(a.Save(path));          // tujuan belum ada
  a.camera_device = 4;
  REQUIRE(a.Save(path));          // tujuan SUDAH ADA -> wajib tetap OK
  kgcore::Settings b;
  REQUIRE(b.Load(path));
  REQUIRE(b.camera_device == 4);
  REQUIRE(b.smoothing == 0.7f);

  // Load file korup: false + err memuat path (penyebab JSON).
  {
    std::ofstream o(path, std::ios::binary | std::ios::trunc);
    o << "{ not json !!";
  }
  std::string err;
  kgcore::Settings c;
  REQUIRE_FALSE(c.Load(path, &err));
  REQUIRE(err.find(path) != std::string::npos);

  // Save ke direktori tak-valid (parent adalah FILE): false + err non-kosong.
  std::string err2;
  kgcore::Settings d;
  REQUIRE_FALSE(d.Save(path + "/sub/x.json", &err2));
  REQUIRE_FALSE(err2.empty());
  std::remove(path.c_str());
}
