#pragma once
#include <string>

namespace kgcore {
// UI state shared between main loop and panel rendering.
struct AppUiState {
  bool mirror = true;
  bool quit_requested = false;
  bool camera_ok = false;
  int cam_width = 0, cam_height = 0;
  uint64_t frames_captured = 0;
  float fps = 0.0f;
  std::string status_message;     // e.g. status.ready / status.no_camera
  std::string landmark_status;    // landmark.active / landmark.inactive
  bool landmark_available = false;
};

// Draws the side panel (tabs Face/Skin/Makeup/Presets). Call between
// ImGui::NewFrame() and ImGui::Render().
void RenderPanel(AppUiState& state);
}  // namespace kgcore
