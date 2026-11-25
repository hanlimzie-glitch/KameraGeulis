#include "kgcore/settings.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <nlohmann/json.hpp>

namespace kgcore {
using nlohmann::json;

bool Settings::operator==(const Settings& o) const {
  auto feq = [](float a, float b) { return std::fabs(a - b) < 1e-6f; };
  return feq(smoothing, o.smoothing) && feq(whitening, o.whitening) &&
         feq(chroma_retain, o.chroma_retain) &&
         skin_coverage == o.skin_coverage &&
         feq(face_slim, o.face_slim) && feq(big_eyes, o.big_eyes) &&
         feq(lipstick, o.lipstick) && feq(blusher, o.blusher) &&
         mirror == o.mirror && virtual_camera == o.virtual_camera &&
         vcam_width == o.vcam_width && vcam_height == o.vcam_height &&
         vcam_fps == o.vcam_fps && vcam_mirror == o.vcam_mirror &&
         landmark_enabled == o.landmark_enabled && preset == o.preset &&
         language == o.language && camera_device == o.camera_device &&
         capture_width == o.capture_width && capture_height == o.capture_height &&
         capture_fps == o.capture_fps &&
         camera_rotation == o.camera_rotation &&
         auto_rotation_prior == o.auto_rotation_prior;
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
  chroma_retain = ClampUnit(j, "chroma_retain", chroma_retain);
  face_slim = ClampUnit(j, "face_slim", face_slim);
  big_eyes = ClampUnit(j, "big_eyes", big_eyes);
  lipstick = ClampUnit(j, "lipstick", lipstick);
  blusher = ClampUnit(j, "blusher", blusher);
  mirror = ClampBool(j, "mirror", mirror);
  virtual_camera = ClampBool(j, "virtual_camera", virtual_camera);
  vcam_width = ClampInt(j, "vcam_width", vcam_width, 16, 4096);
  vcam_height = ClampInt(j, "vcam_height", vcam_height, 16, 4096);
  vcam_width = std::max(16, (vcam_width / 4) * 4);
  vcam_height = std::max(16, (vcam_height / 4) * 4);
  vcam_fps = ClampInt(j, "vcam_fps", vcam_fps, 1, 60);
  vcam_mirror = ClampBool(j, "vcam_mirror", vcam_mirror);
  landmark_enabled = ClampBool(j, "landmark_enabled", landmark_enabled);
  preset = ClampString(j, "preset", preset, 32);
  language = ClampString(j, "language", language, 8);
  if (language != "id" && language != "en") language = "id";
  camera_device = ClampInt(j, "camera_device", camera_device, 0, 16);
  capture_width = ClampInt(j, "capture_width", capture_width, 16, 7680);
  capture_height = ClampInt(j, "capture_height", capture_height, 16, 4320);
  capture_fps = ClampInt(j, "capture_fps", capture_fps, 1, 240);
  skin_coverage = ClampInt(j, "skin_coverage", skin_coverage, 0, 1);
  if (j.contains("camera_rotation")) {
    try {
      const int r = j.at("camera_rotation").get<int>();
      if (r == -1 || r == 0 || r == 90 || r == 180 || r == 270) {
        camera_rotation = r;
      }
    } catch (const std::exception&) {}
  }
  if (j.contains("auto_rotation_prior")) {
    try {
      const int p = j.at("auto_rotation_prior").get<int>();
      if (p == 0 || p == 90 || p == 180 || p == 270) {
        auto_rotation_prior = p;
      }
    } catch (const std::exception&) {}
  }
  return true;
}

std::string Settings::ToJson() const {
  json j;
  j["smoothing"] = smoothing;
  j["whitening"] = whitening;
  j["chroma_retain"] = chroma_retain;
  j["face_slim"] = face_slim;
  j["big_eyes"] = big_eyes;
  j["lipstick"] = lipstick;
  j["blusher"] = blusher;
  j["mirror"] = mirror;
  j["virtual_camera"] = virtual_camera;
  j["vcam_width"] = vcam_width;
  j["vcam_height"] = vcam_height;
  j["vcam_fps"] = vcam_fps;
  j["vcam_mirror"] = vcam_mirror;
  j["landmark_enabled"] = landmark_enabled;
  j["preset"] = preset;
  j["language"] = language;
  j["camera_device"] = camera_device;
  j["capture_width"] = capture_width;
  j["capture_height"] = capture_height;
  j["capture_fps"] = capture_fps;
  j["skin_coverage"] = skin_coverage;
  j["camera_rotation"] = camera_rotation;
  j["auto_rotation_prior"] = auto_rotation_prior;
  return j.dump(2);
}

bool Settings::Load(const std::string& path, std::string* err) {
  if (err) err->clear();
  namespace fs = std::filesystem;
  const fs::path p = fs::u8path(path);
  std::ifstream in(p, std::ios::binary);
  if (!in) {
    if (err) {
      std::error_code exists_ec;
      const bool exists = fs::exists(p, exists_ec);
      *err = !exists && !exists_ec
                 ? "file tidak ditemukan: " + path
                 : "tidak bisa membuka " + path + " (" +
                       std::string(std::strerror(errno)) + ")";
    }
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  std::string text = ss.str();
  try {
    json j = json::parse(text);
    if (!j.is_object()) {
      if (err) *err = "JSON di " + path + " bukan object";
      return false;
    }
  } catch (const std::exception& e) {
    if (err) *err = "JSON tidak valid di " + path + ": " + e.what();
    return false;
  }
  return FromJson(text);
}

bool Settings::Save(const std::string& path, std::string* err) const {
  SaveInfo info;
  return SaveAndVerify(path, &info, err);
}

bool Settings::SaveAndVerify(const std::string& path, SaveInfo* info,
                             std::string* err) const {
  if (err) err->clear();
  if (info) *info = SaveInfo{};
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path p = fs::u8path(path);
  const fs::path abs_p = fs::absolute(p, ec);
  const std::string resolved = ec ? path : abs_p.u8string();
  ec.clear();

  if (p.has_parent_path()) {
    fs::create_directories(p.parent_path(), ec);
    if (ec && !fs::is_directory(p.parent_path())) {
      if (err)
        *err = "gagal membuat direktori " + p.parent_path().u8string() + ": " +
               ec.message();
      return false;
    }
    ec.clear();
  }

  const std::string json_str = ToJson();
  const fs::path tmp_p = fs::u8path(path + ".tmp");
  {
    std::ofstream out(tmp_p, std::ios::binary | std::ios::trunc);
    if (!out) {
      if (err)
        *err = "gagal menulis " + tmp_p.u8string() + " (" +
               std::string(std::strerror(errno)) + ")";
      return false;
    }
    out.write(json_str.data(), static_cast<std::streamsize>(json_str.size()));
    out.flush();
    if (!out.good()) {
      if (err)
        *err = "stream rusak saat menulis " + tmp_p.u8string() + " (" +
               std::string(std::strerror(errno)) + ")";
      out.close();
      fs::remove(tmp_p, ec);
      return false;
    }
  }

  // Rename atomik menimpa tujuan (coba ulang singkat bila antivirus/indexer
  // Windows mengunci tujuan sesaat).
  for (int attempt = 0; attempt < 3; ++attempt) {
    ec.clear();
    fs::rename(tmp_p, p, ec);
    if (!ec) break;
    if (attempt < 2) std::this_thread::sleep_for(std::chrono::milliseconds(15));
  }
  if (ec) {
    fs::remove(tmp_p, ec);
    if (err)
      *err = "rename " + tmp_p.u8string() + " -> " + resolved + ": " +
             ec.message();
    return false;
  }

  // Verifikasi keberadaan, jumlah byte di disk, dan baca-ulang JSON.
  std::error_code sz_ec;
  const uintmax_t disk_bytes = fs::file_size(p, sz_ec);
  if (sz_ec || disk_bytes != json_str.size()) {
    if (err)
      *err = "verifikasi ukuran disk gagal di " + resolved +
             " (expected=" + std::to_string(json_str.size()) + ", got=" +
             (sz_ec ? std::string("err:") + sz_ec.message()
                    : std::to_string(disk_bytes)) +
             ")";
    return false;
  }

  Settings verify;
  std::string verify_err;
  if (!verify.Load(path, &verify_err)) {
    if (err)
      *err = "verifikasi baca-ulang gagal untuk " + resolved + ": " +
             verify_err;
    return false;
  }

  if (info) {
    info->resolved_path = resolved;
    info->bytes_written = json_str.size();
    info->verified = true;
  }
  return true;
}
}  // namespace kgcore
