// KameraGeulis — M1: GLFW window + live Media Foundation camera preview
// through the GPUPixel pipeline (BeautyFaceFilter at defaults) + ImGui panel.
// NOT runtime-verified on real Windows yet (CI builds only; no webcam/GL in
// headless CI). See STATUS.md.
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>

#include <algorithm>
#include <cmath>

#include "camera_capture.h"
#include "beauty_pipeline.h"
#include "kgcore/log.h"
#include "kgcore/strings.h"
#include "kgcore/frame.h"
#include "panel.h"

namespace kg = kgcore;

namespace {
using clock_type = std::chrono::steady_clock;

std::string ExecutableDir() {
  char buf[MAX_PATH];
  DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
  if (n == 0) return ".";
  std::string path(buf, n);
  auto pos = path.find_last_of("\\/");
  return (pos == std::string::npos) ? "." : path.substr(0, pos);
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
  void Upload(const uint8_t* rgba, int width, int height, bool mirror) {
    glBindTexture(GL_TEXTURE_2D, tex);
    if (w != width || h != height) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, nullptr);
      w = width; h = height;
    }
    if (!mirror) {
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA,
                      GL_UNSIGNED_BYTE, rgba);
      return;
    }
    // CPU mirror (cheap at preview sizes) — correct orientation for selfie view.
    const int row = width * 4;
    flip_.resize(static_cast<size_t>(row) * height);
    for (int y = 0; y < height; ++y) {
      const uint8_t* src = rgba + static_cast<size_t>(y) * row;
      uint8_t* dst = flip_.data() + static_cast<size_t>(y) * row;
      for (int x = 0; x < width; ++x) {
        std::memcpy(dst + (width - 1 - x) * 4, src + x * 4, 4);
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

int RunApp() {
  glfwSetErrorCallback([](int code, const char* desc) {
    kg::Log("glfw", "error %d: %s", code, desc);
  });
  if (!glfwInit()) {
    kg::Log("app", "glfwInit failed");
    return 1;
  }
#if defined(__APPLE__)
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
#endif
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);

  GLFWwindow* window = glfwCreateWindow(1280, 720, "KameraGeulis", nullptr, nullptr);
  if (!window) {
    kg::Log("app", "glfwCreateWindow failed");
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);
  if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
    kg::Log("app", "gladLoadGLLoader failed");
    glfwDestroyWindow(window);
    glfwTerminate();
    return 1;
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  ImGui_ImplGlfw_InitForOpenGL(window, true);
  const char* glsl_version = "#version 330 core";
  ImGui_ImplOpenGL3_Init(glsl_version);

  const std::string exe_dir = ExecutableDir();
  kg::BeautyPipeline pipeline;
  pipeline.Init(exe_dir);

  kg::CameraCapture capture;
  const bool camera_started = capture.Start(0);

  PreviewTexture preview;
  glGenTextures(1, &preview.tex);
  glBindTexture(GL_TEXTURE_2D, preview.tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  kg::Strings::Instance().SetLanguage(kg::Language::kId);

  kg::AppUiState ui;
  ui.mirror = true;
  ui.camera_ok = false;
  ui.status_message = camera_started ? kg::Strings::Instance().Get("status.starting") : kg::Strings::Instance().Get("status.no_camera");
  ui.landmark_available = pipeline.Tracker().ProviderAvailable();
  ui.landmark_status = ui.landmark_available
                           ? kg::Strings::Instance().Get("landmark.active")
                           : kg::Strings::Instance().Get("landmark.inactive");

  auto last_frame_time = clock_type::now();
  float fps_smooth = 0.0f;
  bool synthetic = !camera_started;
  int synthetic_w = 640, synthetic_h = 480;

  while (!glfwWindowShouldClose(window) && !ui.quit_requested) {
    glfwPollEvents();

    kg::Frame frame;
    bool have_frame = false;
    if (capture.LatestFrame(frame)) {
      have_frame = true;
      synthetic = false;
      ui.camera_ok = true;
      ui.status_message = kg::Strings::Instance().Get("status.ready");
    } else if (synthetic) {
      auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        clock_type::now().time_since_epoch())
                        .count();
      frame = MakeSyntheticFrame(synthetic_w, synthetic_h, now_ms);
      have_frame = true;
    }

    int out_w = 0, out_h = 0;
    if (have_frame) {
      pipeline.ProcessFrame(frame);
      const uint8_t* out = pipeline.OutputRGBA();
      out_w = pipeline.OutputWidth();
      out_h = pipeline.OutputHeight();
      if (out && out_w > 0 && out_h > 0) {
        preview.Upload(out, out_w, out_h, ui.mirror);
      }
      ui.cam_width = frame.width;
      ui.cam_height = frame.height;
      ui.frames_captured = capture.FramesCaptured();
    }

    auto now = clock_type::now();
    float dt = std::chrono::duration<float>(now - last_frame_time).count();
    last_frame_time = now;
    if (dt > 0.0f) {
      float fps = 1.0f / dt;
      fps_smooth = fps_smooth == 0.0f ? fps : fps_smooth * 0.95f + fps * 0.05f;
      ui.fps = fps_smooth;
    }

    // ---------- Render ----------
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glClearColor(0.07f, 0.07f, 0.09f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    int fb_w = 0, fb_h = 0;
    glfwGetFramebufferSize(window, &fb_w, &fb_h);
    glViewport(0, 0, fb_w, fb_h);

    // Preview: left ~75% of the window as an ImGui image.
    ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(fb_w) * 0.72f,
                                    static_cast<float>(fb_h) - 16.0f),
                             ImGuiCond_Always);
    ImGui::Begin("Preview", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings);
    {
      const ImVec2 avail = ImGui::GetContentRegionAvail();
      if (out_w > 0 && out_h > 0) {
        float scale = std::min(avail.x / out_w, avail.y / out_h);
        ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(preview.tex)),
                     ImVec2(out_w * scale, out_h * scale), ImVec2(0, 1), ImVec2(1, 0));
      } else {
        ImGui::Text("Menunggu frame kamera…");
      }
    }
    ImGui::End();

    // Side panel on the right.
    ImGui::SetNextWindowPos(ImVec2(static_cast<float>(fb_w) * 0.72f + 16.0f, 8.0f),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(fb_w) * 0.28f - 24.0f,
                                    static_cast<float>(fb_h) - 16.0f),
                             ImGuiCond_Always);
    RenderPanel(ui);

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window);
  }

  capture.Stop();
  pipeline.Shutdown();  // GL context still current — required
  preview.Destroy();

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}

// Windows GUI subsystem entry point (UNICODE build → wWinMain).
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  return RunApp();
}
