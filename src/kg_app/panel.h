#pragma once
#include <cstdint>
#include <string>

namespace kgcore {
// UI state shared between the main loop and the panel. Persisted effect/
// camera fields mirror Settings; runtime-only fields are transient.
struct AppUiState {
  // ---- persisted (mirror of Settings) ----
  float smoothing = 0.35f;    // [0,1] - Haluskan Kulit
  float whitening = 0.25f;    // [0,1] - Cerahkan Kulit
  float face_slim = 0.0f;     // [0,1] - disabled ("Segera hadir")
  float big_eyes = 0.0f;      // [0,1] - disabled
  float lipstick = 0.0f;      // [0,1] - disabled
  float blusher = 0.0f;       // [0,1] - disabled
  std::string preset = "Natural";      // Natural|Soft|Glam|ClearSkin|Custom
  bool mirror = true;
  bool landmark_enabled = true;
  std::string language = "id";         // id|en
  int camera_device = 0;
  int capture_width = 1280;
  int capture_height = 720;
  int capture_fps = 30;

  // ---- runtime ----
  bool quit_requested = false;
  bool compare_held = false;   // UI-compares against the raw frame
  bool show_debug = false;     // F3 / Info tab: landmark overlay + timings
  bool safe_mode = false;      // --safe: camera/ORT/GPUPixel disabled
  bool vsync = true;           // About tab checkbox (runtime-only)
  bool raw_preview = false;    // M2e F4 / Info tab: tampilkan kamera mentah
  uint64_t debug_frame_requests = 0;  // M2e F5 / Info tab: simpan PNG debug
  std::string camera_mode_line;       // mode MF aktual (read-back) untuk UI
  std::string camera_error_line;       // M2f: "Kamera gagal: 0x... (mencoba lagi...)"
  bool camera_busy_hint = false;        // M2g: true HANYA bila hr = aplikasi lain/izin
  bool camera_ok = false;
  int cam_width = 0, cam_height = 0;
  uint64_t frames_captured = 0;
  float fps = 0.0f;
  float capture_ms = 0.0f;     // time inside LatestFrame()
  float detect_ms = 0.0f;      // detector stage (0 when tracking)
  float mesh_ms = 0.0f;        // mesh stage (incl. crop)
  float track_ms = 0.0f;       // total provider Track() (FaceTracker)
  float render_ms = 0.0f;      // GL/UI render block
  int frames_since_detect = 0;
  std::string status_message;
  std::string landmark_status;
  bool landmark_available = false;
  std::string notice;          // transient user notice ("saved/loaded/...")
  bool notice_is_error = false;

  // ---- action requests (consumed by the main loop) ----
  bool request_save_settings = false;
  bool request_load_settings = false;
  bool request_reset_defaults = false;
  bool request_camera_apply = false;
};

// Draws the full UI (large preview handled by the main loop; tabbed panel
// + overlay helpers here). Call between ImGui::NewFrame() and ImGui::Render().
void RenderPanel(AppUiState& state);
}  // namespace kgcore
