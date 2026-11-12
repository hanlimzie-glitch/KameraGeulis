// KameraGeulis - M2: GLFW window + live Media Foundation camera preview
// through the GPUPixel pipeline (BeautyFaceFilter: smoothing/whitening,
// landmark-gated effects disabled) + full ImGui UI (tabbed panel, presets,
// camera/resolution/fps picker, settings JSON save/load, debug overlay).
// NOT runtime-verified on real Windows yet (CI builds only; no webcam/GL in
// headless CI). See STATUS.md.
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>

#include <algorithm>
#include <cmath>

#include "camera_capture.h"
#include "beauty_pipeline.h"
#include "kgcore/log.h"
#include "kgcore/settings.h"
#include "kgcore/strings.h"
#include "kgcore/frame.h"
#include "panel.h"
#include "startup.h"

namespace kg = kgcore;

namespace {
using clock_type = std::chrono::steady_clock;
using std::chrono::duration_cast;

std::string WideToUtf8(const std::wstring& wide) {
  if (wide.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                    nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), n,
                      nullptr, nullptr);
  return out;
}


std::string SettingsPath() {
  char dir[MAX_PATH];
  const DWORD n = GetEnvironmentVariableA("APPDATA", dir, MAX_PATH);
  if (n > 0 && n < MAX_PATH) {
    std::string base(dir, n);
    base += "\\KameraGeulis";
    CreateDirectoryA(base.c_str(), nullptr);  // ok bila sudah ada
    return base + "\\settings.json";
  }
  return "settings.json";
}

kg::Settings UiToSettings(const kg::AppUiState& ui) {
  kg::Settings s;
  s.smoothing = ui.smoothing; s.whitening = ui.whitening;
  s.face_slim = ui.face_slim; s.big_eyes = ui.big_eyes;
  s.lipstick = ui.lipstick; s.blusher = ui.blusher;
  s.mirror = ui.mirror; s.landmark_enabled = ui.landmark_enabled;
  s.preset = ui.preset; s.language = ui.language;
  s.camera_device = ui.camera_device;
  s.capture_width = ui.capture_width;
  s.capture_height = ui.capture_height;
  s.capture_fps = ui.capture_fps;
  return s;
}

void ApplySettingsToUi(const kg::Settings& s, kg::AppUiState& ui) {
  ui.smoothing = s.smoothing; ui.whitening = s.whitening;
  ui.face_slim = s.face_slim; ui.big_eyes = s.big_eyes;
  ui.lipstick = s.lipstick; ui.blusher = s.blusher;
  ui.mirror = s.mirror; ui.landmark_enabled = s.landmark_enabled;
  ui.preset = s.preset.empty() ? "Natural" : s.preset;
  ui.language = (s.language == "en") ? "en" : "id";
  ui.camera_device = s.camera_device;
  ui.capture_width = s.capture_width;
  ui.capture_height = s.capture_height;
  ui.capture_fps = s.capture_fps;
}

// Synthetic fallback frame so the app renders something even with no camera
// (dev machines, headless demos, CI smoke region). Gradient bars sweeping.
kg::Frame MakeSyntheticFrame(int width, int height, int64_t ts_ms) {
  kg::Frame f;
  f.width = width;
  f.height = height;
  f.stride_bytes = width * 4;
  f.format = kg::kFormatBGRA;
  f.timestamp_ms = ts_ms;
  f.pixels.assign(static_cast<size_t>(f.stride_bytes) * height, 0);
  const double t = static_cast<double>(ts_ms % 10000) / 10000.0;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      uint8_t* px = f.pixels.data() + static_cast<size_t>(y) * f.stride_bytes + x * 4;
      double u = static_cast<double>(x) / width;
      double v = static_cast<double>(y) / height;
      px[0] = static_cast<uint8_t>(40 + 120 * std::abs(std::sin(3.0 * u + t * 6.0)));   // B
      px[1] = static_cast<uint8_t>(30 + 90 * v);                                       // G
      px[2] = static_cast<uint8_t>(60 + 140 * u * (1.0 - v));                          // R
      px[3] = 255;
    }
  }
  return f;
}

struct PreviewTexture {
  GLuint tex = 0;
  int w = 0, h = 0;
  void Upload(const uint8_t* src, int width, int height, bool mirror,
              bool src_is_bgra) {
    glBindTexture(GL_TEXTURE_2D, tex);
    if (w != width || h != height) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, nullptr);
      w = width; h = height;
    }
    if (!mirror && !src_is_bgra) {
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA,
                      GL_UNSIGNED_BYTE, src);
      return;
    }
    // CPU repack: optional horizontal mirror + optional BGRA->RGBA swap.
    const int row = width * 4;
    flip_.resize(static_cast<size_t>(row) * height);
    for (int y = 0; y < height; ++y) {
      const uint8_t* in = src + static_cast<size_t>(y) * row;
      uint8_t* out_row = flip_.data() + static_cast<size_t>(y) * row;
      for (int x = 0; x < width; ++x) {
        const int sx = mirror ? (width - 1 - x) : x;
        const uint8_t* px = in + sx * 4;
        uint8_t* dst = out_row + x * 4;
        if (src_is_bgra) { dst[0] = px[2]; dst[1] = px[1]; dst[2] = px[0]; }
        else             { dst[0] = px[0]; dst[1] = px[1]; dst[2] = px[2]; }
        dst[3] = px[3];
      }
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA,
                    GL_UNSIGNED_BYTE, flip_.data());
  }
  void Destroy() {
    if (tex) { glDeleteTextures(1, &tex); tex = 0; }
  }
  std::vector<uint8_t> flip_;
};
}  // namespace

// Staged OpenGL context creation. Tried in order; each attempt logs the
// exact GLFW error code + description so a driver that cannot give us a
// 3.3 context is diagnosable from log.txt alone.
struct GlAttempt {
  int major, minor, profile;
  bool forward_compat;
  const char* label;
};

int RunApp(const kg::StartupArgs& args, const kg::StartupPaths& paths) {
  const bool safe = args.safe;
  glfwSetErrorCallback([](int code, const char* desc) {
    kgcore::Log("glfw", "error %d: %s", code, desc ? desc : "(no description)");
  });
  kgcore::Log("app", "glfwInit...");
  if (!glfwInit()) {
    kg::ShowFatalAndExit(L"GLFW init gagal",
                         L"glfwInit() mengembalikan false - GLFW tidak bisa dimulai sama sekali.\n"
                         L"Penyebab umum: driver GPU/OpenGL rusak atau diblokir.\n"
                         L"Coba perbarui driver GPU (Intel/AMD/NVIDIA) lalu jalankan lagi.",
                         12);
  }
  int glfw_major = 0, glfw_minor = 0, glfw_rev = 0;
  glfwGetVersion(&glfw_major, &glfw_minor, &glfw_rev);
  kgcore::Log("app", "GLFW versi %d.%d.%d", glfw_major, glfw_minor, glfw_rev);
  if (GLFWmonitor* mon = glfwGetPrimaryMonitor()) {
    if (const GLFWvidmode* mode = glfwGetVideoMode(mon)) {
      kgcore::Log("app", "monitor utama: %dx%d @ %d Hz", mode->width, mode->height,
                  mode->refreshRate);
    }
  }

  const GlAttempt attempts[] = {
      {3, 3, GLFW_OPENGL_CORE_PROFILE, true,  "OpenGL 3.3 core (forward-compat)"},
      {3, 3, GLFW_OPENGL_ANY_PROFILE,  false, "OpenGL 3.3 any-profile (compatibility)"},
      {4, 6, GLFW_OPENGL_CORE_PROFILE, false, "OpenGL 4.6 core"},
  };
  GLFWwindow* window = nullptr;
  for (const GlAttempt& a : attempts) {
    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, a.major);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, a.minor);
    glfwWindowHint(GLFW_OPENGL_PROFILE, a.profile);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, a.forward_compat ? GL_TRUE : GL_FALSE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    window = glfwCreateWindow(1440, 810, "KameraGeulis", nullptr, nullptr);
    if (window) {
      kgcore::Log("app", "GL context: %s - BERHASIL", a.label);
      break;
    }
    // GLFW 3.3 API: int glfwGetError(const char** description)
    const char* desc = nullptr;
    const int code = glfwGetError(&desc);
    kgcore::Log("app", "GL context: %s - GAGAL (glfw error %d: %s)", a.label, code,
                desc ? desc : "(tidak ada deskripsi)");
  }
  if (!window) {
    kg::ShowFatalAndExit(L"OpenGL context gagal",
                         L"Tidak bisa membuat konteks OpenGL (dicoba: 3.3 core, 3.3 any-profile, "
                         L"4.6 core).\n\n"
                         L"Penyebab umum: (1) driver GPU usang/corrupt - update ke driver terbaru; "
                         L"(2) GPU tidak mendukung OpenGL 3.3 (sangat tua/virtualized/remote desktop); "
                         L"(3) konflik dengan aplikasi overlay (capture/screen-recorder) yang "
                         L"meng-hook OpenGL.\n"
                         L"Lihat log.txt di folder di bawah untuk detail percobaan.",
                         13);
  }
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);
  if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
    kg::ShowFatalAndExit(L"glad gagal memuat fungsi OpenGL",
                         L"Konteks GL dibuat tetapi gladLoadGLLoader gagal - kombinasi driver/"
                         L"konteks tidak menyediakan fungsi GL yang dibutuhkan.\n"
                         L"Update driver GPU Anda.",
                         14);
  }
  kgcore::Log("gl", "VENDOR:   %s", glGetString(GL_VENDOR));
  kgcore::Log("gl", "RENDERER: %s", glGetString(GL_RENDERER));
  kgcore::Log("gl", "VERSION:  %s", glGetString(GL_VERSION));
  kgcore::Log("gl", "GLSL:     %s", glGetString(GL_SHADING_LANGUAGE_VERSION));

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  ImGui_ImplGlfw_InitForOpenGL(window, true);
  const char* glsl_version = "#version 330 core";
  ImGui_ImplOpenGL3_Init(glsl_version);

  const std::string exe_dir = WideToUtf8(paths.exe_dir);
  const std::string settings_path = SettingsPath();
  if (safe) {
    kgcore::Log("app", "SAFE MODE: onnxruntime & GPUPixel TIDAK dimuat (objek pipeline tidak "
                       "dibuat sama sekali); kamera TIDAK dibuka; menampilkan gradien sintetis.");
  }
  // The BeautyPipeline constructor already builds FaceTracker+OrtLandmarkProvider,
  // which loads (and may download) the ONNX models - so in --safe mode we must
  // NOT construct it at all.
  std::unique_ptr<kg::BeautyPipeline> pipeline;
  bool pipeline_ok = false;
  if (!safe) {
    try {
      pipeline = std::make_unique<kg::BeautyPipeline>();
      pipeline_ok = pipeline->Init(exe_dir);
    } catch (const std::exception& e) {
      kgcore::Log("app", "pipeline GPUPixel init exception: %s", e.what());
      pipeline_ok = false;
    } catch (...) {
      kgcore::Log("app", "pipeline GPUPixel init exception: unknown");
      pipeline_ok = false;
    }
    kgcore::Log("app", "pipeline GPUPixel init: %s", pipeline_ok ? "OK" : "GAGAL");
  } else {
    kgcore::Log("app", "pipeline GPUPixel: dinonaktifkan (mode aman)");
  }

  kg::AppUiState ui;
  ui.safe_mode = safe;
  ui.vsync = true;
  kg::Settings settings;
  const bool settings_loaded = settings.Load(settings_path);
  if (settings_loaded) {
    ApplySettingsToUi(settings, ui);
  }
  kg::Log("app", "settings: %s (%s)", settings_loaded ? "loaded" : "defaults",
          settings_path.c_str());
  kg::Strings::Instance().SetLanguage(ui.language == "en" ? kg::Language::kEn : kg::Language::kId);

  kg::CameraCapture capture;
  bool camera_started = false;
  if (!safe) {
    camera_started = capture.Start(ui.camera_device, ui.capture_width,
                                   ui.capture_height, ui.capture_fps);
    kgcore::Log("app", "MF capture start: device=%d %dx%d@%dfps -> %s", ui.camera_device,
                ui.capture_width, ui.capture_height, ui.capture_fps,
                camera_started ? "OK" : "GAGAL - memakai gradien sintetis");
  } else {
    kgcore::Log("app", "MF capture: dinonaktifkan (mode aman)");
  }
  ui.camera_ok = camera_started;

  PreviewTexture preview;
  glGenTextures(1, &preview.tex);
  glBindTexture(GL_TEXTURE_2D, preview.tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  if (!safe && !pipeline_ok && pipeline) {
    ui.notice = pipeline->LastResourceError().empty()
                    ? "Efek GPU dinonaktifkan (lihat log) - preview menampilkan frame mentah"
                    : pipeline->LastResourceError();
    ui.notice_is_error = true;
  }
  ui.landmark_available = pipeline_ok && pipeline && pipeline->Tracker().ProviderAvailable();
  ui.landmark_status = ui.landmark_available
                           ? kg::Strings::Instance().Get("landmark.active")
                           : kg::Strings::Instance().Get("landmark.inactive");
  ui.status_message = safe
                          ? "Mode aman (tanpa kamera/ORT/GPUPixel)"
                          : (camera_started ? kg::Strings::Instance().Get("status.ready")
                                            : kg::Strings::Instance().Get("status.no_camera"));

  auto last_frame_time = clock_type::now();
  float fps_smooth = 0.0f;
  bool synthetic = !camera_started;
  int synthetic_w = 640, synthetic_h = 480;
  bool f3_prev = false;
  bool vsync_applied = true;  // glfwSwapInterval(1) right after creation

  while (!glfwWindowShouldClose(window) && !ui.quit_requested) {
    glfwPollEvents();

    // VSync checkbox (About tab) - apply on change only.
    if (ui.vsync != vsync_applied) {
      glfwSwapInterval(ui.vsync ? 1 : 0);
      vsync_applied = ui.vsync;
      kgcore::Log("app", "vsync %s", ui.vsync ? "ON" : "OFF");
    }
    // F3 toggles the landmark/timings debug overlay.
    const bool f3 = glfwGetKey(window, GLFW_KEY_F3) == GLFW_PRESS;
    if (f3 && !f3_prev) ui.show_debug = !ui.show_debug;
    f3_prev = f3;

    // ---- settings action requests (from the panel, previous frame) ----
    if (ui.request_save_settings) {
      settings = UiToSettings(ui);
      if (settings.Save(settings_path)) {
        ui.notice = kg::Strings::Instance().Get("presets.saved");
        ui.notice_is_error = false;
      } else {
        ui.notice = kg::Strings::Instance().Get("presets.save_failed");
        ui.notice_is_error = true;
      }
      ui.request_save_settings = false;
    }
    if (ui.request_load_settings) {
      kg::Settings loaded;
      if (loaded.Load(settings_path)) {
        ApplySettingsToUi(loaded, ui);
        kg::Strings::Instance().SetLanguage(
            ui.language == "en" ? kg::Language::kEn : kg::Language::kId);
        ui.notice = kg::Strings::Instance().Get("presets.loaded");
        ui.notice_is_error = false;
        ui.request_camera_apply = true;  // apply loaded camera mode
      } else {
        ui.notice = kg::Strings::Instance().Get("presets.load_failed");
        ui.notice_is_error = true;
      }
      ui.request_load_settings = false;
    }
    if (ui.request_reset_defaults) {
      const kg::Settings def;
      ui.smoothing = def.smoothing;
      ui.whitening = def.whitening;
      ui.face_slim = def.face_slim;
      ui.big_eyes = def.big_eyes;
      ui.lipstick = def.lipstick;
      ui.blusher = def.blusher;
      ui.mirror = def.mirror;
      ui.preset = def.preset;  // "Natural"
      ui.request_reset_defaults = false;
    }
    if (ui.request_camera_apply) {
      if (safe) {
        // --safe: camera controls are disabled in the UI, but a stale
        // request (e.g. from "Muat" settings in safe mode) must never open
        // the camera.
        ui.request_camera_apply = false;
      } else {
        capture.Stop();
        camera_started = capture.Start(ui.camera_device, ui.capture_width,
                                       ui.capture_height, ui.capture_fps);
        ui.camera_ok = camera_started;
        synthetic = !camera_started;
        ui.frames_captured = 0;
        ui.status_message = camera_started
                                ? kg::Strings::Instance().Get("camera.applied")
                                : kg::Strings::Instance().Get("camera.apply_failed");
        if (!camera_started) {
          ui.notice = kg::Strings::Instance().Get("camera.apply_failed");
          ui.notice_is_error = true;
        } else {
          ui.notice_is_error = false;
        }
        ui.request_camera_apply = false;
      }
    }

    // Language radio applies immediately.
    kg::Strings::Instance().SetLanguage(
        ui.language == "en" ? kg::Language::kEn : kg::Language::kId);

    // ---- capture ----
    kg::Frame frame;
    bool have_frame = false;
    const auto cap_start = clock_type::now();
    if (capture.LatestFrame(frame)) {
      have_frame = true;
      synthetic = false;
      ui.camera_ok = true;
      ui.status_message = kg::Strings::Instance().Get("status.ready");
    }
    ui.capture_ms = duration_cast<std::chrono::microseconds>(clock_type::now() - cap_start)
                        .count() /
                    1000.0f;
    if (!have_frame && synthetic) {
      auto now_ms = duration_cast<std::chrono::milliseconds>(
                        clock_type::now().time_since_epoch())
                        .count();
      frame = MakeSyntheticFrame(synthetic_w, synthetic_h, now_ms);
      have_frame = true;
    }

    int out_w = 0, out_h = 0;
    bool pipeline_frame_ok = false;
    if (have_frame && pipeline_ok) {
      // Live-apply the two global beauty parameters (cheap; idempotent).
      pipeline->SetBeautyStrength(ui.smoothing, ui.whitening);
      pipeline->ProcessFrame(frame);
      kg::StageTimings st;
      if (pipeline->GetStageTimings(&st)) {
        ui.detect_ms = st.detect_ms;
        ui.mesh_ms = st.mesh_ms;
        ui.track_ms = st.track_ms;
        ui.frames_since_detect = st.frames_since_detect;
      }
      const uint8_t* out = pipeline->OutputRGBA();
      out_w = pipeline->OutputWidth();
      out_h = pipeline->OutputHeight();
      if (out && out_w > 0 && out_h > 0) {
        preview.Upload(out, out_w, out_h, ui.mirror, /*src_is_bgra=*/false);
        pipeline_frame_ok = true;
      }
      ui.cam_width = frame.width;
      ui.cam_height = frame.height;
      ui.frames_captured = capture.FramesCaptured();
    }
    if (have_frame && !pipeline_frame_ok) {
      // GPUPixel off (safe mode or init failed): show the frame as-is.
      preview.Upload(frame.pixels.data(), frame.width, frame.height, ui.mirror,
                     /*src_is_bgra=*/frame.format == kg::kFormatBGRA);
      out_w = frame.width;
      out_h = frame.height;
      ui.cam_width = frame.width;
      ui.cam_height = frame.height;
      ui.frames_captured = safe ? ui.frames_captured + 1 : capture.FramesCaptured();
    }

    // Before/after: hold the compare button to see the raw frame instead of
    // the processed one (pipeline still runs; only the preview changes).
    const uint8_t* raw = have_frame ? frame.pixels.data() : nullptr;
    if (ui.compare_held && raw) {
      preview.Upload(raw, frame.width, frame.height, ui.mirror,
                     /*src_is_bgra=*/frame.format == kg::kFormatBGRA);
      out_w = frame.width;
      out_h = frame.height;
    }

    auto now = clock_type::now();
    float dt = std::chrono::duration<float>(now - last_frame_time).count();
    last_frame_time = now;
    if (dt > 0.0f) {
      float fps = 1.0f / dt;
      fps_smooth = fps_smooth == 0.0f ? fps : fps_smooth * 0.95f + fps * 0.05f;
      ui.fps = fps_smooth;
    }
    if (pipeline) ui.landmark_available = pipeline->Tracker().ProviderAvailable();

    // ---------- Render ----------
    const auto render_start = clock_type::now();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glClearColor(0.07f, 0.07f, 0.09f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    int fb_w = 0, fb_h = 0;
    glfwGetFramebufferSize(window, &fb_w, &fb_h);
    glViewport(0, 0, fb_w, fb_h);

    const float panel_w = std::max(320.0f, static_cast<float>(fb_w) * 0.27f);

    // Preview: fills the area left of the panel.
    ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(fb_w) - panel_w - 24.0f,
                                    static_cast<float>(fb_h) - 16.0f),
                             ImGuiCond_Always);
    ImGui::Begin("Preview", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings);
    ImVec2 image_pos{}, image_size{};
    {
      const ImVec2 avail = ImGui::GetContentRegionAvail();
      const float hud_h = 40.0f;  // compare button + compare hint
      const ImVec2 image_area(avail.x, std::max(64.0f, avail.y - hud_h - 8.0f));
      if (out_w > 0 && out_h > 0) {
        float scale = std::min(image_area.x / out_w, image_area.y / out_h);
        image_size = ImVec2(out_w * scale, out_h * scale);
        // Center the image horizontally in the preview window.
        const float offset_x = (avail.x - image_size.x) * 0.5f;
        if (offset_x > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset_x);
        image_pos = ImGui::GetCursorScreenPos();
        ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(preview.tex)),
                     image_size, ImVec2(0, 1), ImVec2(1, 0));
      } else {
        ImGui::TextUnformatted("Menunggu frame kamera...");
      }

      // Debug overlay: 468 landmark points + bbox + HUD on the preview.
      if (ui.show_debug && image_size.x > 1.0f && image_size.y > 1.0f) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const kg::LandmarkSet lm_default{};
        const auto& lm = pipeline_ok ? pipeline->Tracker().Current() : lm_default;
        auto to_screen = [&](float x, float y) {
          const float sx = ui.mirror ? (image_pos.x + (1.0f - x) * image_size.x)
                                     : (image_pos.x + x * image_size.x);
          const float sy = image_pos.y + y * image_size.y;
          return ImVec2(sx, sy);
        };
        if (lm.valid) {
          float min_x = 1.0f, min_y = 1.0f, max_x = 0.0f, max_y = 0.0f;
          const ImU32 dot = IM_COL32(255, 226, 92, 220);
          for (const auto& p : lm.pts) {
            const ImVec2 sp = to_screen(p.x, p.y);
            dl->AddCircleFilled(sp, 2.0f, dot);
            min_x = std::min(min_x, p.x); max_x = std::max(max_x, p.x);
            min_y = std::min(min_y, p.y); max_y = std::max(max_y, p.y);
          }
          const ImVec2 b0 = to_screen(min_x, min_y);
          const ImVec2 b1 = to_screen(max_x, max_y);
          const float bx0 = std::min(b0.x, b1.x), bx1 = std::max(b0.x, b1.x);
          dl->AddRect(ImVec2(bx0, b0.y), ImVec2(bx1, b1.y), IM_COL32(0, 229, 255, 230),
                      0.0f, 0, 2.0f);
        }
        char hud[512];
        std::snprintf(hud, sizeof(hud),
                      "FPS %.1f | cap %.2f ms | det %.2f ms | mesh %.2f ms | "
                      "trk %.2f ms | render %.2f ms | sejak deteksi %d",
                      ui.fps, ui.capture_ms, ui.detect_ms, ui.mesh_ms, ui.track_ms,
                      ui.render_ms, ui.frames_since_detect);
        const ImVec2 hud_pos(image_pos.x + 8.0f, image_pos.y + 8.0f);
        const ImVec2 text_size = ImGui::CalcTextSize(hud);
        dl->AddRectFilled(hud_pos,
                          ImVec2(hud_pos.x + text_size.x + 12.0f, hud_pos.y + text_size.y + 8.0f),
                          IM_COL32(0, 0, 0, 160));
        dl->AddText(ImVec2(hud_pos.x + 6.0f, hud_pos.y + 4.0f), IM_COL32_WHITE, hud);
      }

      ImGui::Spacing();
      if (ui.compare_held) {
        ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "%s",
                           kg::Strings::Instance().Get("compare.active").c_str());
      } else {
        ImGui::TextUnformatted(" ");
      }
      ImGui::Button(kg::Strings::Instance().Get("compare.hold").c_str(),
                    ImVec2(-1.0f, 0.0f));
      ui.compare_held = ImGui::IsItemActive();
    }
    ImGui::End();

    // Side panel on the right.
    ImGui::SetNextWindowPos(ImVec2(static_cast<float>(fb_w) - panel_w - 8.0f, 8.0f),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(panel_w, static_cast<float>(fb_h) - 16.0f),
                             ImGuiCond_Always);
    RenderPanel(ui);

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window);
    ui.render_ms =
        duration_cast<std::chrono::microseconds>(clock_type::now() - render_start).count() /
        1000.0f;
  }

  capture.Stop();
  if (pipeline) pipeline->Shutdown();  // GL context still current - required
  preview.Destroy();

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}

// Windows GUI subsystem entry point (UNICODE build -> wWinMain).
// Order: paths -> file log (flush per line) -> crash handlers -> (DLL preload)
// -> selftest/safe-mode branch -> window. Every failure path ends either in a
// clear MessageBox + log, or (selftest) a non-zero exit code - never a silent exit.
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
  const kg::StartupArgs args = kg::ParseStartupArgs(GetCommandLineW());

  kg::StartupPaths paths;
  std::wstring path_err;
  if (!kg::ComputeStartupPaths(hInstance, &paths, &path_err)) {
    const std::wstring msg = L"KameraGeulis tidak bisa dimulai.\n\n" + path_err;
    MessageBoxW(nullptr, msg.c_str(), L"KameraGeulis - Gagal Startup",
                MB_ICONERROR | MB_OK | MB_TOPMOST);
    return 10;
  }
  kg::BeginFileLog(paths, args);
  kg::InstallCrashHandlers(paths);

  if (args.selftest) {
    kgcore::Log("startup", "mode --selftest: tidak ada window, tidak ada GL");
    const int code = kg::RunSelftest(paths);
    kgcore::Log("startup", "selftest exit code %d", code);
    kg::EndFileLog();
    return code;
  }
  if (!args.safe) {
    const kg::DllPreloadResult pre = kg::PreloadRuntimeDlls(paths);
    if (!pre.all_ok) {
      kg::ShowFatalAndExit(L"Load DLL runtime gagal", pre.detail, 11);
    }
  } else {
    kgcore::Log("startup", "SAFE MODE: preload DLL (onnxruntime/gpupixel) dilewati");
  }
  const int code = RunApp(args, paths);
  kg::EndFileLog();
  return code;
}
