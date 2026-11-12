#include "panel.h"
#include "imgui.h"
#include "kgcore/strings.h"

namespace kgcore {
namespace {
void DisabledWithTooltip(const char* label, const char* tooltip) {
  ImGui::BeginDisabled();
  ImGui::Button(label);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
  ImGui::EndDisabled();
}
}  // namespace

void RenderPanel(AppUiState& state) {
  auto& s = Strings::Instance();
  static float dummy_smoothing = 0.0f, dummy_whitening = 0.0f;
  static float dummy_slim = 0.0f, dummy_eyes = 0.0f;
  static bool dummy_vcam = false;

  ImGui::Begin("KameraGeulis", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
  ImGui::TextUnformatted(s.Get("app.title").c_str());
  ImGui::Separator();

  ImGui::Text("Kamera: %s (%dx%d, %llu frame)", state.camera_ok ? "OK" : "—",
              state.cam_width, state.cam_height,
              static_cast<unsigned long long>(state.frames_captured));
  ImGui::Text("FPS: %.1f", state.fps);
  ImGui::Text("Status: %s", state.status_message.c_str());
  ImGui::Separator();

  // --- Landmark status (honest, always visible) ---
  ImGui::Text("%s:", s.Get("landmark.status").c_str());
  if (state.landmark_available) {
    ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "%s",
                       s.Get("landmark.active").c_str());
  } else {
    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "%s",
                       s.Get("landmark.inactive").c_str());
    ImGui::TextWrapped("%s", s.Get("landmark.unavailable").c_str());
  }
  ImGui::Separator();

  if (ImGui::BeginTabBar("kgeulis_tabs")) {
    if (ImGui::BeginTabItem(s.Get("tab.skin").c_str())) {
      ImGui::BeginDisabled();
      ImGui::SliderFloat(s.Get("skin.smoothing").c_str(), &dummy_smoothing, 0.0f, 1.0f);
      ImGui::SliderFloat(s.Get("skin.whitening").c_str(), &dummy_whitening, 0.0f, 1.0f);
      ImGui::EndDisabled();
      ImGui::SetTooltip("M2 — slider menyusul");
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(s.Get("tab.face").c_str())) {
      ImGui::BeginDisabled();
      ImGui::SliderFloat(s.Get("face.slim").c_str(), &dummy_slim, 0.0f, 1.0f);
      ImGui::SliderFloat(s.Get("face.bigeyes").c_str(), &dummy_eyes, 0.0f, 1.0f);
      ImGui::EndDisabled();
      ImGui::SetTooltip("Belum tersedia di v1 — mapping 468 poin ke filter GPUPixel belum terverifikasi (docs/LANDMARKS.md)");
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(s.Get("tab.makeup").c_str())) {
      ImGui::TextWrapped("M4 — lipstick, blusher, foundation, eyeliner, eyeshadow menyusul.");
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(s.Get("tab.presets").c_str())) {
      DisabledWithTooltip("Natural", "M2");
      ImGui::SameLine();
      DisabledWithTooltip("Soft", "M2");
      ImGui::SameLine();
      DisabledWithTooltip("Glam", "M2");
      ImGui::SameLine();
      DisabledWithTooltip("ClearSkin", "M2");
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }

  ImGui::Separator();
  ImGui::Checkbox(s.Get("common.mirror").c_str(), &state.mirror);
  ImGui::BeginDisabled();
  ImGui::Checkbox(s.Get("vcam.toggle").c_str(), &dummy_vcam);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", s.Get("vcam.need_register").c_str());
  }
  ImGui::EndDisabled();

  if (ImGui::Button(s.Get("common.quit").c_str())) {
    state.quit_requested = true;
  }
  ImGui::End();
}
}  // namespace kgcore
