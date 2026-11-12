#include "kgcore/settings.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <nlohmann/json.hpp>

namespace kgcore {
using nlohmann::json;

bool Settings::operator==(const Settings& o) const {
  auto feq = [](float a, float b) { return std::fabs(a - b) < 1e-6f; };
  return feq(smoothing, o.smoothing) && feq(whitening, o.whitening) &&
         feq(face_slim, o.face_slim) && feq(big_eyes, o.big_eyes) &&
         feq(lipstick, o.lipstick) && feq(blusher, o.blusher) &&
         mirror == o.mirror && virtual_camera == o.virtual_camera &&
         landmark_enabled == o.landmark_enabled && preset == o.preset &&
         language == o.language && camera_device == o.camera_device &&
         capture_width == o.capture_width && capture_height == o.capture_height &&
         capture_fps == o.capture_fps;
}

static float ClampUnit(const json& j, const char* key, float def) {
  if (!j.contains(key)) return def;
  float v = def;
  try { v = j.at(key).get<float>(); } catch (const std::exception&) { return def; }
  if (!std::isfinite(v)) return def;
  return std::clamp(v, 0.0f, 1.0f);
}
static bool ClampBool(const json& j, const char* key, bool def) {
  if (!j.contains(key)) return def;
  try { return j.at(key).get<bool>(); } catch (const std::exception&) { return def; }
}
static int ClampInt(const json& j, const char* key, int def, int lo, int hi) {
  if (!j.contains(key)) return def;
  int v = def;
  try { v = j.at(key).get<int>(); } catch (const std::exception&) { return def; }
  return std::clamp(v, lo, hi);
}
static std::string ClampString(const json& j, const char* key,
                               const std::string& def, size_t max_len) {
  if (!j.contains(key)) return def;
  try {
    auto s = j.at(key).get<std::string>();
    if (s.size() > max_len) s.resize(max_len);
    return s;
  } catch (const std::exception&) { return def; }
}

bool Settings::FromJson(const std::string& text) {
  json j;
  try { j = json::parse(text); } catch (const std::exception&) { return false; }
  if (!j.is_object()) return false;
  smoothing = ClampUnit(j, "smoothing", smoothing);
  whitening = ClampUnit(j, "whitening", whitening);
  face_slim = ClampUnit(j, "face_slim", face_slim);
  big_eyes = ClampUnit(j, "big_eyes", big_eyes);
  lipstick = ClampUnit(j, "lipstick", lipstick);
  blusher = ClampUnit(j, "blusher", blusher);
  mirror = ClampBool(j, "mirror", mirror);
  virtual_camera = ClampBool(j, "virtual_camera", virtual_camera);
  landmark_enabled = ClampBool(j, "landmark_enabled", landmark_enabled);
  preset = ClampString(j, "preset", preset, 32);
  language = ClampString(j, "language", language, 8);
  if (language != "id" && language != "en") language = "id";
  camera_device = ClampInt(j, "camera_device", camera_device, 0, 16);
  capture_width = ClampInt(j, "capture_width", capture_width, 16, 7680);
  capture_height = ClampInt(j, "capture_height", capture_height, 16, 4320);
  capture_fps = ClampInt(j, "capture_fps", capture_fps, 1, 240);
  return true;
}

std::string Settings::ToJson() const {
  json j;
  j["smoothing"] = smoothing;
  j["whitening"] = whitening;
  j["face_slim"] = face_slim;
  j["big_eyes"] = big_eyes;
  j["lipstick"] = lipstick;
  j["blusher"] = blusher;
  j["mirror"] = mirror;
  j["virtual_camera"] = virtual_camera;
  j["landmark_enabled"] = landmark_enabled;
  j["preset"] = preset;
  j["language"] = language;
  j["camera_device"] = camera_device;
  j["capture_width"] = capture_width;
  j["capture_height"] = capture_height;
  j["capture_fps"] = capture_fps;
  return j.dump(2);
}

bool Settings::Load(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  return FromJson(ss.str());
}

bool Settings::Save(const std::string& path) const {
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << ToJson();
    if (!out.good()) return false;
  }
  // Atomic-ish replace (best effort across platforms).
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    std::remove(tmp.c_str());
    return false;
  }
  return true;
}
}  // namespace kgcore
