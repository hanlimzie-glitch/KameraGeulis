#pragma once
#include <string>

namespace kgcore {
// Persistent user settings (%APPDATA%/KameraGeulis/settings.json on Windows).
// Unknown fields in the file are ignored; missing fields get defaults.
struct Settings {
  float smoothing = 0.35f;    // [0,1]
  float whitening = 0.25f;    // [0,1]
  float face_slim = 0.0f;     // [0,1]
  float big_eyes = 0.0f;      // [0,1]
  float lipstick = 0.0f;      // [0,1]
  float blusher = 0.0f;       // [0,1]
  bool mirror = true;
  bool virtual_camera = false;
  bool landmark_enabled = true;  // honored only when a provider is available
  std::string preset = "Natural";   // Natural|Soft|Glam|ClearSkin|Custom
  std::string language = "id";      // id|en

  bool operator==(const Settings& o) const;
  bool operator!=(const Settings& o) const { return !(*this == o); }

  // Loads from `path`; on any error returns false and *this stays default.
  bool Load(const std::string& path);
  // Saves atomically-ish (write tmp + rename). Returns false on IO error.
  bool Save(const std::string& path) const;
  std::string ToJson() const;
  bool FromJson(const std::string& json);
};
}  // namespace kgcore
