#pragma once
#include <string>

namespace kgcore {
// Persistent user settings (%APPDATA%/KameraGeulis/settings.json on Windows).
// Unknown fields in the file are ignored; missing fields get defaults.
struct Settings {
  float smoothing = 0.18f;    // [0,1] - M2i: default Natural (lebih natural)
  float whitening = 0.06f;    // [0,1] - M2i: default Natural
  float chroma_retain = 0.8f; // [0,1] - M2i: retensi chroma MENTAH (anti cast)
  float face_slim = 0.0f;     // [0,1]
  float big_eyes = 0.0f;      // [0,1]
  float lipstick = 0.0f;      // [0,1]
  float blusher = 0.0f;       // [0,1]
  bool mirror = true;
  bool virtual_camera = false;
  bool landmark_enabled = true;  // honored only when a provider is available
  std::string preset = "Natural";   // Natural|Soft|Glam|ClearSkin|Custom
  std::string language = "id";      // id|en
  int camera_device = 0;            // MF device index
  int capture_width = 1280;         // requested capture mode (best effort)
  int capture_height = 720;
  int capture_fps = 30;

  bool operator==(const Settings& o) const;
  bool operator!=(const Settings& o) const { return !(*this == o); }

  // Loads from `path`; on any error returns false and *this stays default.
  // M2i: `err` (opsional) menerima penyebab pasti (path + IO/JSON).
  bool Load(const std::string& path, std::string* err = nullptr);
  // Saves atomically: tulis tmp -> rename menimpa tujuan (std::filesystem,
  // di Windows memakai MoveFileEx REPLACE - rename C89 GAGAL bila tujuan
  // sudah ada, itulah bug "GAGAL menyimpan perangkat" di PC pengguna).
  // Directori tujuan dibuat bila belum ada. `err` menerima penyebab pasti.
  bool Save(const std::string& path, std::string* err = nullptr) const;
  std::string ToJson() const;
  bool FromJson(const std::string& json);
};
}  // namespace kgcore
