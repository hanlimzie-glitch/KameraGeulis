#include "panel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "imgui.h"
#include "camera_capture.h"  // kg_capture: EnumerateDevices / EnumerateModes
#include "kgcore/presets.h"
#include "kgcore/strings.h"

namespace kgcore {
namespace {
// Cached device/mode lists (camera open/close is expensive; only refresh
// on Refresh button / selection change).
struct CameraEnumCache {
  bool loaded = false;
  std::vector<VideoDeviceInfo> devices;
  std::vector<VideoMode> modes;  // for `modes_for_device`
  int modes_for_device = -1;
  void RefreshDevices() { devices = CameraCapture::EnumerateDevices(); }
  void RefreshModes(int device) {
    modes = CameraCapture::EnumerateModes(device);
    modes_for_device = device;
  }
};
CameraEnumCache& CamCache() {
  static CameraEnumCache cache;
  return cache;
}

void SliderPct(const char* label, float* v01) {
  int pct = static_cast<int>(std::lround(*v01 * 100.0f));
  if (ImGui::SliderInt(label, &pct, 0, 100, "%d %%")) {
    *v01 = static_cast<float>(pct) / 100.0f;
  }
}

void DisabledComingSoon(const char* label) {
  auto& s = Strings::Instance();
  float dummy = 0.0f;
  ImGui::BeginDisabled();
  ImGui::SliderFloat(label, &dummy, 0.0f, 1.0f, "0%%");
  ImGui::SameLine();
  ImGui::Button(s.Get("common.coming_soon").c_str());
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("%s", s.Get("common.coming_soon_tip").c_str());
  }
  ImGui::EndDisabled();
}

void ApplyPreset(AppUiState& ui, const char* name) {
  if (const PresetDef* p = FindPreset(name)) {
    ui.smoothing = p->smoothing;
    ui.whitening = p->whitening;
    ui.preset = p->name;
  }
}

void DrawPresetsTab(AppUiState& ui) {
  auto& s = Strings::Instance();
  const char* names[] = {"Natural", "Soft", "Glam", "ClearSkin"};
  for (int i = 0; i < 4; ++i) {
    if (i > 0) ImGui::SameLine();
    if (ImGui::Button(names[i])) ApplyPreset(ui, names[i]);
  }
  ImGui::Separator();
  const bool is_custom = ui.preset == "Custom";
  ImGui::Text("%s:", s.Get("presets.active").c_str());
  ImGui::SameLine();
  if (is_custom) {
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "%s", s.Get("presets.custom").c_str());
  } else {
    ImGui::TextColored(ImVec4(0.55f, 0.9f, 0.55f, 1.0f), "%s", ui.preset.c_str());
  }
  ImGui::Separator();
  if (ImGui::Button(s.Get("presets.reset").c_str())) {
    ui.request_reset_defaults = true;
  }
  ImGui::Separator();
  if (ImGui::Button(s.Get("common.save").c_str(), ImVec2(-1.0f, 0.0f))) {
    ui.request_save_settings = true;
  }
  if (ImGui::Button(s.Get("common.load").c_str(), ImVec2(-1.0f, 0.0f))) {
    ui.request_load_settings = true;
  }
}

void DrawCameraTab(AppUiState& ui) {
  auto& s = Strings::Instance();
  auto& cache = CamCache();
  const bool cam_disabled = ui.safe_mode;
  if (cam_disabled) {
    ImGui::TextWrapped("Kamera dinonaktifkan (mode aman). Jalankan tanpa --safe untuk memakai kamera.");
    ImGui::Separator();
    ImGui::BeginDisabled();
  } else if (!cache.loaded) {
    cache.RefreshDevices();
    cache.RefreshModes(ui.camera_device);
    cache.loaded = true;
  }

  if (cache.devices.empty()) {
    ImGui::TextWrapped("%s", s.Get("camera.none").c_str());
  } else {
    const std::string preview =
        (ui.camera_device >= 0 && ui.camera_device < static_cast<int>(cache.devices.size()))
            ? cache.devices[ui.camera_device].name
            : ("Kamera #" + std::to_string(ui.camera_device));
    if (ImGui::BeginCombo(s.Get("camera.device").c_str(), preview.c_str())) {
      for (size_t i = 0; i < cache.devices.size(); ++i) {
        const bool sel = (static_cast<int>(i) == ui.camera_device);
        if (ImGui::Selectable(cache.devices[i].name.c_str(), sel)) {
          ui.camera_device = static_cast<int>(i);
          cache.RefreshModes(ui.camera_device);
        }
        if (sel) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
  }

  // Resolution list from native modes (unique wxh), or a safe fallback.
  struct Res { int w, h; };
  std::vector<Res> resolutions;
  for (const auto& m : cache.modes) {
    bool dup = false;
    for (const auto& r : resolutions) {
      if (r.w == m.width && r.h == m.height) { dup = true; break; }
    }
    if (!dup) resolutions.push_back({m.width, m.height});
  }
  const bool using_fallback_modes = resolutions.empty();
  if (using_fallback_modes) {
    resolutions = {{1280, 720}, {640, 480}};
  }
  std::string res_preview =
      std::to_string(ui.capture_width) + "x" + std::to_string(ui.capture_height);
  if (ImGui::BeginCombo(s.Get("camera.resolution").c_str(), res_preview.c_str())) {
    for (const auto& r : resolutions) {
      const bool sel = (r.w == ui.capture_width && r.h == ui.capture_height);
      char label[32];
      std::snprintf(label, sizeof(label), "%dx%d", r.w, r.h);
      if (ImGui::Selectable(label, sel)) {
        ui.capture_width = r.w;
        ui.capture_height = r.h;
        // Pick the highest fps available for the new resolution.
        int best_fps = 0;
        for (const auto& m : cache.modes) {
          if (m.width == r.w && m.height == r.h && m.fps > best_fps) best_fps = m.fps;
        }
        ui.capture_fps = best_fps > 0 ? best_fps : 30;
      }
      if (sel) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }

  // FPS list for the chosen resolution.
  std::vector<int> fps_values;
  for (const auto& m : cache.modes) {
    if (m.width == ui.capture_width && m.height == ui.capture_height && m.fps > 0) {
      bool dup = false;
      for (int f : fps_values) if (f == m.fps) { dup = true; break; }
      if (!dup) fps_values.push_back(m.fps);
    }
  }
  if (fps_values.empty()) fps_values = {30, 60};
  std::sort(fps_values.begin(), fps_values.end());
  char fps_preview[16];
  std::snprintf(fps_preview, sizeof(fps_preview), "%d", ui.capture_fps);
  if (ImGui::BeginCombo(s.Get("camera.fps").c_str(), fps_preview)) {
    for (int f : fps_values) {
      const bool sel = (f == ui.capture_fps);
      char label[16];
      std::snprintf(label, sizeof(label), "%d", f);
      if (ImGui::Selectable(label, sel)) ui.capture_fps = f;
      if (sel) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  if (using_fallback_modes) {
    ImGui::TextWrapped("%s", s.Get("camera.no_modes").c_str());
  }

  if (ImGui::Button(s.Get("camera.apply").c_str())) {
    ui.request_camera_apply = true;
  }
  ImGui::SameLine();
  if (ImGui::Button(s.Get("camera.refresh").c_str())) {
    cache.RefreshDevices();
    cache.RefreshModes(ui.camera_device);
  }

  // M2f: status kamera jujur - kegagalan membuka reader (mis. 0x80070057)
  // tampil di sini + tombol coba manual (ulang otomatis dengan backoff jalan
  // terus di latar belakang).
  if (!ui.camera_error_line.empty()) {
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s",
                       ui.camera_error_line.c_str());
    // M2g requirement 4: petunjuk "dipakai aplikasi lain" HANYA untuk kode
    // yang memang bermakna itu (E_ACCESSDENIED / ERROR_SHARING_VIOLATION) -
    // jangan menyalahkan aplikasi lain bila sebabnya E_INVALIDARG/SHUTDOWN.
    if (ui.camera_busy_hint) {
      ImGui::TextWrapped(
          "Kamera mungkin dipakai aplikasi lain atau akses kamera diblokir.");
    } else {
      ImGui::TextDisabled("Lihat log untuk rincian tiap percobaan tingkat.");
    }
    if (ImGui::Button("Coba lagi")) {
      ui.request_camera_apply = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(ulang otomatis aktif, jeda 1-8 detik)");
  }
  ImGui::Separator();
  ImGui::Text("%s: %dx%d", s.Get("camera.current").c_str(), ui.cam_width, ui.cam_height);
  ImGui::Text("FPS render: %.1f", ui.fps);
  ImGui::Separator();

  if (cam_disabled) {
    ImGui::EndDisabled();
  }
  // Virtual camera - disabled until M3 (softcam + registrasi admin).
  ImGui::BeginDisabled();
  bool vcam = false;
  ImGui::Checkbox(s.Get("vcam.toggle").c_str(), &vcam);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("%s", s.Get("vcam.need_register").c_str());
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::TextDisabled("%s", s.Get("vcam.coming_soon").c_str());
}

void DrawAboutTab(AppUiState& ui) {
  auto& s = Strings::Instance();
  ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.8f, 1.0f), "KameraGeulis 0.2.0");
  ImGui::Separator();

  ImGui::Text("%s:", s.Get("landmark.status").c_str());
  if (ui.landmark_available) {
    ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "%s", s.Get("landmark.active").c_str());
  } else {
    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "%s", s.Get("landmark.inactive").c_str());
    ImGui::TextWrapped("%s", s.Get("landmark.unavailable").c_str());
  }
  ImGui::Separator();

  ImGui::Checkbox("VSync", &ui.vsync);
  ImGui::Checkbox(s.Get("debug.overlay").c_str(), &ui.show_debug);
  ImGui::TextWrapped("%s", s.Get("debug.note").c_str());
  ImGui::Spacing();

  // ---- M2e diagnosis (F4/F5 hotkeys mirror these controls) ----
  ImGui::Separator();
  ImGui::Text("Diagnosis (M2e):");
  ImGui::Checkbox("Tampilkan kamera mentah (tanpa GPUPixel) (F4)",
                  &ui.raw_preview);
  if (ui.raw_preview) {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                       "RAW: frame capture diupload langsung (tanpa efek)");
  }
  if (ImGui::Button("Simpan frame debug PNG (F5)")) {
    ++ui.debug_frame_requests;
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Simpan 1 frame mentah + 1 frame keluaran pipeline sebagai PNG\n"
        "ke folder %APPDATA%\\KameraGeulis\\debug\\ (path dicatat di log)");
  }
  if (!ui.camera_mode_line.empty()) {
    ImGui::Text("Kamera: %s", ui.camera_mode_line.c_str());
  }
  ImGui::Spacing();
  ImGui::Text("capture  %.2f ms", ui.capture_ms);
  ImGui::Text("detect   %.2f ms (%s: %d)", ui.detect_ms,
              s.Get("debug.since_detect").c_str(), ui.frames_since_detect);
  ImGui::Text("mesh     %.2f ms", ui.mesh_ms);
  ImGui::Text("track    %.2f ms", ui.track_ms);
  ImGui::Text("render   %.2f ms", ui.render_ms);
  ImGui::TextWrapped("%s", s.Get("debug.unverified").c_str());
  ImGui::Separator();

  ImGui::BeginDisabled();
  ImGui::Button(s.Get("common.snapshot").c_str());
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("%s", s.Get("common.coming_soon").c_str());
  }
  ImGui::EndDisabled();
  ImGui::Separator();
  ImGui::TextWrapped("%s", s.Get("about.license_note").c_str());
}
}  // namespace

void RenderPanel(AppUiState& ui) {
  auto& s = Strings::Instance();

  ImGui::Begin("KameraGeulis", nullptr,
               ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);
  ImGui::TextUnformatted(s.Get("app.title").c_str());
  ImGui::Separator();

  // --- Status line ---
  ImGui::Text("%s: %s (%dx%d, %llu frame)", s.Get("status.camera").c_str(),
              ui.camera_ok ? "OK" : "-", ui.cam_width, ui.cam_height,
              static_cast<unsigned long long>(ui.frames_captured));
  ImGui::Text("FPS (render): %.1f", ui.fps);
  ImGui::Text("Status: %s", ui.status_message.c_str());
  ImGui::Separator();

  if (ImGui::BeginTabBar("kgeulis_tabs")) {
    if (ImGui::BeginTabItem(s.Get("tab.skin").c_str())) {
      const float prev_smoothing = ui.smoothing;
      const float prev_whitening = ui.whitening;
      SliderPct(s.Get("skin.smoothing").c_str(), &ui.smoothing);
      SliderPct(s.Get("skin.whitening").c_str(), &ui.whitening);
      if (ui.smoothing != prev_smoothing || ui.whitening != prev_whitening) {
        ui.preset = "Custom";
      }
      // M2h butuh 3: mask kulit (default aktif) - matikan = perilaku lama
      // (efek ke seluruh frame).
      ImGui::Checkbox(s.Get("skin.only").c_str(), &ui.skin_only);
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", s.Get("skin.only_tip").c_str());
      }
      if (!ui.skin_only) {
        ImGui::TextDisabled("%s", s.Get("skin.only_off_note").c_str());
      }
      ImGui::Spacing();
      ImGui::TextWrapped("%s", s.Get("skin.coming_soon_note").c_str());
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(s.Get("tab.face").c_str())) {
      DisabledComingSoon(s.Get("face.slim").c_str());
      DisabledComingSoon(s.Get("face.bigeyes").c_str());
      ImGui::Spacing();
      ImGui::TextWrapped("%s", s.Get("common.coming_soon_tip").c_str());
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(s.Get("tab.makeup").c_str())) {
      DisabledComingSoon(s.Get("makeup.lipstick").c_str());
      DisabledComingSoon(s.Get("makeup.blusher").c_str());
      DisabledComingSoon(s.Get("makeup.foundation").c_str());
      DisabledComingSoon(s.Get("makeup.eyeliner").c_str());
      DisabledComingSoon(s.Get("makeup.eyeshadow").c_str());
      DisabledComingSoon(s.Get("makeup.browtint").c_str());
      ImGui::Spacing();
      ImGui::TextWrapped("%s", s.Get("common.coming_soon_tip").c_str());
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(s.Get("tab.presets").c_str())) {
      DrawPresetsTab(ui);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(s.Get("tab.camera").c_str())) {
      DrawCameraTab(ui);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(s.Get("tab.about").c_str())) {
      DrawAboutTab(ui);
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }

  ImGui::Separator();
  ImGui::Checkbox(s.Get("common.mirror").c_str(), &ui.mirror);
  ImGui::SameLine();
  ImGui::Text("%s:", s.Get("common.language").c_str());
  ImGui::SameLine();
  if (ImGui::RadioButton("ID", ui.language == "id")) ui.language = "id";
  ImGui::SameLine();
  if (ImGui::RadioButton("EN", ui.language == "en")) ui.language = "en";
  ImGui::Separator();

  if (!ui.notice.empty()) {
    if (ui.notice_is_error) {
      ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", ui.notice.c_str());
    } else {
      ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "%s", ui.notice.c_str());
    }
    ImGui::Separator();
  }
  if (ImGui::Button(s.Get("common.quit").c_str(), ImVec2(-1.0f, 0.0f))) {
    ui.quit_requested = true;
  }
  ImGui::End();
}
}  // namespace kgcore
