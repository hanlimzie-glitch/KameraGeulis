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
#include <ctime>
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
#include "kgcore/frame_convert.h"
#include "kgcore/png_write.h"
#include "kgcore/reader_fallback.h"  // M2g: MfErrorName + WantsCameraBusyHint
#include "mask_blender.h"  // M2h: stage blending mask kulit (GL aplikasi)
#include "kgcore/skin_mask.h"  // M2i: BeautyComposeConfig + metrik F5

// M2j kualitas: parameter komposit per mode. Mode kulit:
//  * smooth_amount DITINGKATKAN (0.45 + 0.55*slider) - guided benar-benar
//    menghaluskan (target rasio detail halus <= 0.85 pada kekuatan default);
//  * lift = 0.9 * whitening (22% -> +6..8 luma inti, 100% -> ~+30).
// Mode seluruh frame: dipertahankan persis M2i (tanpa lift).
static kgcore::BeautyComposeConfig MakeComposeConfig(bool skin_only,
                                                     float smoothing,
                                                     float whitening,
                                                     float chroma_retain) {
  kgcore::BeautyComposeConfig c;
  c.chroma_retain = chroma_retain;
  if (skin_only) {
    c.smooth_amount = 0.45f + 0.55f * smoothing;
    c.lift = whitening * 0.90f;
  } else {
    c.smooth_amount = smoothing;
  }
  return c;
}
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
  s.chroma_retain = ui.chroma_retain;
  s.face_slim = ui.face_slim; s.big_eyes = ui.big_eyes;
  s.lipstick = ui.lipstick; s.blusher = ui.blusher;
  s.mirror = ui.mirror; s.landmark_enabled = ui.landmark_enabled;
  s.preset = ui.preset; s.language = ui.language;
  s.camera_device = ui.camera_device;
  s.capture_width = ui.capture_width;
  s.capture_height = ui.capture_height;
  s.capture_fps = ui.capture_fps;
  s.skin_coverage = ui.skin_coverage;  // M2j
  return s;
}

void ApplySettingsToUi(const kg::Settings& s, kg::AppUiState& ui) {
  ui.smoothing = s.smoothing; ui.whitening = s.whitening;
  ui.chroma_retain = s.chroma_retain;
  ui.face_slim = s.face_slim; ui.big_eyes = s.big_eyes;
  ui.lipstick = s.lipstick; ui.blusher = s.blusher;
  ui.mirror = s.mirror; ui.landmark_enabled = s.landmark_enabled;
  ui.preset = s.preset.empty() ? "Natural" : s.preset;
  ui.language = (s.language == "en") ? "en" : "id";
  ui.camera_device = s.camera_device;
  ui.capture_width = s.capture_width;
  ui.capture_height = s.capture_height;
  ui.capture_fps = s.capture_fps;
  ui.skin_coverage = s.skin_coverage;  // M2j: 0 semua kulit, 1 wajah saja
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

// M2e requirement 3 (F5): %APPDATA%\KameraGeulis\debug\ (created on demand).
std::string DebugDumpDir() {
  char dir[MAX_PATH] = {};
  const DWORD n = GetEnvironmentVariableA("APPDATA", dir, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return {};
  std::string base(dir, n);
  base += "\\KameraGeulis";
  CreateDirectoryA(base.c_str(), nullptr);  // ok bila sudah ada
  base += "\\debug";
  CreateDirectoryA(base.c_str(), nullptr);
  return base;
}

// M2h: simpan pasangan raw_*.png + out_*.png + mask_*.png dan catat selisih
// rata-rata piksel LUAR mask (harus ~0 saat mode kulit aktif) & DALAM mask.
// `raw` = salinan frame mentah TERAKHIR (disimpan walau F4 mati - butuh 4).
void DumpDebugFrames(const kg::Frame* raw, kg::BeautyPipeline* pipeline,
                     bool pipeline_ok, bool skin_only, float smooth_amount,
                     float whitening, float chroma_retain) {
  const std::string dir = DebugDumpDir();
  if (dir.empty()) {
    kgcore::Log("app", "F5: APPDATA tidak tersedia - PNG TIDAK disimpan");
    return;
  }
  char stamp[32];
  {
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _MSC_VER
    localtime_s(&tmv, &t);
#else
    tmv = *std::localtime(&t);
#endif
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tmv);
  }
  // --- raw (SELALU, walau F4 mati - requirement M2h butuh 4) ---
  std::vector<uint8_t> raw_rgba;
  int raw_w = 0, raw_h = 0;
  if (raw && raw->IsValid()) {
    raw_w = raw->width;
    raw_h = raw->height;
    const size_t row = static_cast<size_t>(raw_w) * 4;
    raw_rgba.resize(row * static_cast<size_t>(raw_h));
    kgcore::ConvertToRgbaTopDown(raw->pixels.data(), raw_w, raw_h,
                                 static_cast<size_t>(raw->stride_bytes),
                                 /*flip_vertical=*/false,
                                 /*src_is_bgra=*/raw->format != kg::kFormatRGBA,
                                 raw_rgba.data());
    const std::string path = dir + "\\raw_" + stamp + ".png";
    if (kgcore::WritePngRgba(path.c_str(), raw_rgba.data(), raw_w, raw_h)) {
      kgcore::Log("app", "F5: frame mentah -> %s (%dx%d, BGRA->RGBA, alpha=255)",
                  path.c_str(), raw_w, raw_h);
    } else {
      kgcore::Log("app", "F5: GAGAL menulis %s", path.c_str());
    }
  } else {
    kgcore::Log("app", "F5: tidak ada frame mentah tersedia untuk disimpan");
  }
  if (!pipeline_ok || !pipeline) {
    kgcore::Log("app", "F5: pipeline nonaktif - tidak ada frame keluaran");
    return;
  }
  const uint8_t* out = pipeline->OutputRGBA();
  const int w = pipeline->OutputWidth();
  const int h = pipeline->OutputHeight();
  if (!out || w <= 0 || h <= 0) {
    kgcore::Log("app",
                "F5: tidak ada frame keluaran (pipeline belum pernah memproses frame) - dilewati");
    return;
  }
  const kg::SkinMaskBuilder& mb = pipeline->MaskBuilder();
  const float gain = pipeline->Tracker().Gain();
  const bool mask_ok =
      mb.configured() &&
      mb.mask().size() == static_cast<size_t>(mb.w()) * static_cast<size_t>(mb.h());
  // --- mask_*.png (grayscale diperbesar ke ukuran frame) ---
  if (mask_ok) {
    std::vector<uint8_t> gray(static_cast<size_t>(w) * h * 4);
    kgcore::UpsampleMaskToGrayRgba(mb.mask().data(), mb.w(), mb.h(), w, h,
                                   gray.data());
    const std::string path = dir + "\\mask_" + stamp + ".png";
    if (kgcore::WritePngRgba(path.c_str(), gray.data(), w, h)) {
      kgcore::Log("app",
                  "F5: mask kulit -> %s (mask %dx%d, gray diperbesar ke %dx%d)",
                  path.c_str(), mb.w(), mb.h(), w, h);
    } else {
      kgcore::Log("app", "F5: GAGAL menulis %s", path.c_str());
    }
  } else {
    kgcore::Log("app",
                "F5: mask kulit tidak tersedia (checkbox mati atau belum ada frame wajah) - mask_*.png dilewati");
  }
  // --- seg_*.png (M2j): peta kelas BERWARNA + statistik per kelas + ms + lag ---
  {
    std::vector<float> probs;
    kgcore::LetterboxInfo lb{};
    uint64_t seg_serial = 0;
    double seg_ms = 0.0;
    if (pipeline->Seg().GetResult(&probs, &lb, &seg_serial, &seg_ms) &&
        probs.size() == static_cast<size_t>(kgcore::kSegInputSize) *
                           kgcore::kSegInputSize * kgcore::kSegClasses) {
      const int sw = kgcore::kSegInputSize, sh = kgcore::kSegInputSize;
      std::vector<uint8_t> am(static_cast<size_t>(sw) * sh);
      kgcore::SegArgmax(probs.data(), sw * sh, am.data());
      // Warna per kelas (256x256) -> upscale nearest ke ukuran frame.
      std::vector<uint8_t> up(static_cast<size_t>(w) * h * 4);
      for (int y = 0; y < h; ++y) {
        const int sy = y * sh / h;
        for (int x = 0; x < w; ++x) {
          const int sx = x * sw / w;
          uint8_t col[4];
          kgcore::SegClassColor(am[static_cast<size_t>(sy) * sw + sx], col);
          uint8_t* d = up.data() + (static_cast<size_t>(y) * w + x) * 4;
          d[0] = col[0]; d[1] = col[1]; d[2] = col[2]; d[3] = 255;
        }
      }
      const std::string path = dir + "\\seg_" + stamp + ".png";
      if (kgcore::WritePngRgba(path.c_str(), up.data(), w, h)) {
        kgcore::Log("app", "F5: peta kelas seg -> %s (%dx%d)", path.c_str(), w, h);
      } else {
        kgcore::Log("app", "F5: GAGAL menulis %s", path.c_str());
      }
      kgcore::SegClassStats st;
      kgcore::SegComputeStats(am.data(), static_cast<int>(am.size()), &st);
      kgcore::Log("app",
                  "F5 seg: kelas%% latar=%.1f rambut=%.1f kulit_tubuh=%.1f "
                  "kulit_wajah=%.1f pakaian=%.1f aksesoris=%.1f | infer %.1f ms | "
                  "lag = %lld frame (serial frame %llu - serial seg %llu)",
                  st.pct[0], st.pct[1], st.pct[2], st.pct[3], st.pct[4],
                  st.pct[5], seg_ms,
                  static_cast<long long>(pipeline->FrameSerial()) -
                      static_cast<long long>(seg_serial),
                  static_cast<unsigned long long>(pipeline->FrameSerial()),
                  static_cast<unsigned long long>(seg_serial));
    } else {
      kgcore::Log("app",
                  "F5: seg tidak tersedia (%s) - seg_*.png dilewati, mask final "
                  "tetap dari wajah landmark",
                  pipeline->Seg().LastError().c_str());
    }
  }
  // --- statistik selisih raw vs out (untuk out yang dibandingkan) ---
  auto log_stats = [&](const uint8_t* cmp_raw, const uint8_t* cmp_out,
                       const char* mode) {
    if (raw_rgba.empty() || raw_w != w || raw_h != h) {
      kgcore::Log("app", "F5: statistik selisih dilewati (raw %dx%d vs out %dx%d)",
                  raw_w, raw_h, w, h);
      return;
    }
    float outside = 0.0f, inside = 0.0f;
    kgcore::MaskDiffStats(cmp_raw, cmp_out, mb.mask().data(), mb.w(), mb.h(), w,
                          h, gain, &outside, &inside);
    kgcore::Log("app",
                "F5: selisih rata-rata LUAR mask = %.3f (harus mendekati 0), "
                "DALAM mask = %.3f | gain=%.2f | mode=%s | mask %dx%d",
                outside, inside, gain, mode, mb.w(), mb.h());
    // M2i (4): metrik tambahan di mask INTI (ter-erosi) + pita tepi.
    kgcore::InMaskMetrics met;
    kgcore::ComputeInMaskMetrics(cmp_raw, cmp_out, mb.mask().data(), mb.w(),
                                 mb.h(), w, h, gain, &met);
    kgcore::Log("app",
                "F5 metrik M2i [%s]: delta luma inti = %+.1f | delta chroma inti = %.1f | "
                "rasio detail halus = %.3f | diff pita tepi = %.1f | "
                "n core=%d tepi=%d luar=%d",
                mode, met.delta_luma, met.delta_chroma, met.detail_ratio,
                met.edge_band_diff, met.core_n, met.edge_n, met.outside_n);
  };
  // --- out_*.png ---
  if (skin_only && mask_ok && !raw_rgba.empty() && raw_w == w && raw_h == h) {
    // Komposit CPU dengan rumus identik shader GPU (tampilan di layar).
    std::vector<uint8_t> mixed(raw_rgba.size());
    // M2i (3)+(4): komposit CPU dgn rumus identik shader GPU - guided
    // smoothing luma + retensi chroma mentah (anti cast) + mix mask.
    const kgcore::BeautyComposeConfig bcfg =
        MakeComposeConfig(skin_only, smooth_amount, whitening, chroma_retain);
    kgcore::ComposeBeautyRaw(raw_rgba.data(), out, mb.mask().data(), mb.w(),
                             mb.h(), w, h, gain, bcfg, mixed.data());
    const std::string path = dir + "\\out_" + stamp + ".png";
    if (kgcore::WritePngRgba(path.c_str(), mixed.data(), w, h)) {
      kgcore::Log("app",
                  "F5: frame keluaran (komposit raw+beauty x mask) -> %s (%dx%d)",
                  path.c_str(), w, h);
    } else {
      kgcore::Log("app", "F5: GAGAL menulis %s", path.c_str());
    }
    log_stats(raw_rgba.data(), mixed.data(), "skin-only");
  } else {
    const std::string path = dir + "\\out_" + stamp + ".png";
    if (kgcore::WritePngRgba(path.c_str(), out, w, h)) {
      kgcore::Log("app", "F5: frame keluaran pipeline -> %s (%dx%d)", path.c_str(),
                  w, h);
    } else {
      kgcore::Log("app", "F5: GAGAL menulis %s", path.c_str());
    }
    if (mask_ok) {
      log_stats(raw_rgba.data(), out,
                skin_only ? "skin-only (raw/out beda ukuran)"
                          : "seluruh-frame (checkbox kulit mati)");
    }
  }
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
    // M2e: alpha is FORCED to 255 here - camera RGB32 padding bytes and
    // sink buffers can carry alpha 0, which ImGui would blend to black.
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
        dst[3] = 255;
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
    // M2e requirement 4: konteks GL pemilik texture keluaran.
    kgcore::Log("app",
                "GL konteks aplikasi (preview texture) = %p; gpupixel SinkRawData "
                "membaca frame ke buffer CPU lewat konteks kerjanya sendiri "
                "(GetRgbaBuffer) - id tekstur gpupixel TIDAK dipakai di konteks aplikasi",
                static_cast<void*>(glfwGetCurrentContext()));
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
                                   ui.capture_height, ui.capture_fps,
                                   args.capture_plain);
    if (camera_started && !safe) {
      // M2h butuh 6: buat/pertahankan catatan perangkat terpakai (default
      // sesi berikutnya). Nilai lain di settings.json tidak diubah.
      kg::Settings dev;
      dev.Load(settings_path);
      dev.camera_device = ui.camera_device;
      std::string dev_err;
      if (!dev.Save(settings_path, &dev_err)) {
        kgcore::Log("app", "settings: GAGAL menyimpan perangkat awal - %s",
                    dev_err.c_str());
      }
    }
    // M2f: "OK" TIDAK dicetak di sini - hanya setelah reader jadi + frame
    // pertama diterima (lihat log "MF capture start: OK" dari thread capture).
    kgcore::Log("app", "MF capture start: device=%d %dx%d@%dfps%s -> %s",
                ui.camera_device, ui.capture_width, ui.capture_height,
                ui.capture_fps, args.capture_plain ? " [--capture-plain]" : "",
                camera_started
                    ? "thread dijalankan (menunggu reader + frame pertama)"
                    : "GAGAL (preflight) - memakai gradien sintetis");
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
  ui.status_message =
      safe ? "Mode aman (tanpa kamera/ORT/GPUPixel)"
           : (camera_started ? "Membuka kamera (menunggu frame pertama)..."
                             : kg::Strings::Instance().Get("status.no_camera"));

  auto last_frame_time = clock_type::now();
  float fps_smooth = 0.0f;
  bool synthetic = !camera_started;
  int synthetic_w = 640, synthetic_h = 480;
  bool f3_prev = false;
  bool f4_prev = false;  // M2e: raw preview toggle
  bool f5_prev = false;  // M2e: debug PNG dump
  bool f6_prev = false;  // M2h: overlay mask kulit
  bool vsync_applied = true;  // glfwSwapInterval(1) right after creation

  // ---- M2e diagnosis state (requirement 1: per-second log) ----
  int shown_w = 0, shown_h = 0;   // dims of the LAST uploaded preview frame
  int cam_stream_w = 0, cam_stream_h = 0;  // stream dims for size-change detect
  int64_t diag_last_ms = 0;
  uint64_t sent_to_pipeline = 0;  // frames handed to GPUPixel
  uint64_t pipe_out = 0;          // frames GPUPixel returned (OutputRGBA ok)
  uint64_t pipe_err_count = 0;    // ProcessFrame ran but produced no output
  uint64_t uploaded_out = 0;      // pipeline frames uploaded to preview tex
  uint64_t uploaded_raw = 0;      // raw frames uploaded (F4 / fallback)
  uint64_t last_gl_err = 0;       // last glGetError after an upload
  uint64_t gl_err_count = 0;      // total GL errors seen after uploads
  uint64_t debug_req_seen = 0;    // last consumed ui.debug_frame_requests

  // ---- M2h state (mask kulit + penghitung jujur + deteksi frame gelap) ----
  MaskBlender blender;             // stage mix(raw, beauty, mask*gain) GPU
  kg::Frame last_camera_raw;       // salinan frame mentah TERAKHIR utk F5
  bool last_camera_raw_valid = false;
  bool frame_is_synthetic = false; // frame iterasi ini bukan dari kamera
  bool preview_show_blend = false; // preview menampilkan tex_out blender
  bool blender_warned = false;     // warning shader sudah sekali
  // M2i (1): swa-uji blend GL (sekali per sesi) + flag jalur lama.
  bool s_selftest_done = false;
  int s_blend_frames = 0;
  bool s_blend_logged_first = false;
  const bool force_no_mask_gl = args.no_mask_gl;
  int dark_streak = 0;             // detik berturut gambar nyaris hitam
  bool dark_hint_shown = false;

  // glGetError must run on the context that owns preview.tex (= this thread).
  auto drain_gl_errors = [&] {
    GLenum e;
    while ((e = glGetError()) != GL_NO_ERROR) {
      last_gl_err = e;
      ++gl_err_count;
      kgcore::Log("gl", "glGetError=0x%04X setelah upload (tex=%u %dx%d)",
                  static_cast<unsigned>(e), preview.tex, preview.w, preview.h);
    }
  };

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
    // M2e: F4 toggles raw camera preview (bypass GPUPixel), F5 dumps PNGs.
    const bool f4 = glfwGetKey(window, GLFW_KEY_F4) == GLFW_PRESS;
    if (f4 && !f4_prev) {
      ui.raw_preview = !ui.raw_preview;
      kgcore::Log("app", "F4: tampilkan kamera mentah = %s",
                  ui.raw_preview ? "ON (tanpa GPUPixel)" : "OFF");
    }
    f4_prev = f4;
    const bool f5 = glfwGetKey(window, GLFW_KEY_F5) == GLFW_PRESS;
    if (f5 && !f5_prev) {
      ++ui.debug_frame_requests;
      kgcore::Log("app", "F5: permintaan dump PNG diterima");
    }
    f5_prev = f5;
    // M2h butuh 4: F6 - tampilkan mask kulit sebagai lapisan merah transparan.
    const bool f6 = glfwGetKey(window, GLFW_KEY_F6) == GLFW_PRESS;
    if (f6 && !f6_prev) {
      ui.show_mask = !ui.show_mask;
      kgcore::Log("app", "F6: tampilkan mask kulit = %s%s",
                  ui.show_mask ? "ON (lapisan merah transparan)" : "OFF",
                  (!ui.skin_only && ui.show_mask)
                      ? " [perhatian: checkbox kulit mati - mask tidak dipakai blending]"
                      : "");
    }
    f6_prev = f6;

    // ---- settings action requests (from the panel, previous frame) ----
    if (ui.request_save_settings) {
      settings = UiToSettings(ui);
      std::string save_err;
      if (settings.Save(settings_path, &save_err)) {
        ui.notice = kg::Strings::Instance().Get("presets.saved");
        ui.notice_is_error = false;
      } else {
        ui.notice = kg::Strings::Instance().Get("presets.save_failed");
        ui.notice_is_error = true;
        kgcore::Log("app", "settings: GAGAL menyimpan - %s", save_err.c_str());
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
      ui.skin_coverage = def.skin_coverage;  // M2j
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
        // M2h butuh 6: Start() membuat stats_ + queue BARU (recv=0) - seluruh
        // penghitung app ikut direset supaya "sent tidak pernah > recv"
        // setelah ganti perangkat (bukti log-5: selisih +30 setelah pindah
        // dari DroidCam ke WebCamera).
        sent_to_pipeline = 0;
        pipe_out = 0;
        pipe_err_count = 0;
        uploaded_out = 0;
        uploaded_raw = 0;
        last_camera_raw_valid = false;
        preview_show_blend = false;
        dark_streak = 0;
        dark_hint_shown = false;
        capture.Stop();
        camera_started = capture.Start(ui.camera_device, ui.capture_width,
                                       ui.capture_height, ui.capture_fps,
                                       args.capture_plain);
        ui.camera_ok = camera_started;
        synthetic = !camera_started;
        ui.frames_captured = 0;
        if (camera_started && !safe) {
          // M2h butuh 6: simpan perangkat yang terakhir DIPAKAI sebagai
          // default sesi berikutnya (settings.json) - indeks 0 bawaan
          // ("DroidCam Video") bisa memberi frame hitam di PC pengguna.
          kg::Settings dev;
          dev.Load(settings_path);  // pertahankan nilai lain; default bila hilang
          dev.camera_device = ui.camera_device;
          std::string save_err;
          if (dev.Save(settings_path, &save_err)) {
            kgcore::Log("app",
                        "settings: perangkat kamera terakhir (index %d) disimpan sebagai default",
                        ui.camera_device);
            // M2i (2): perangkat BERUBAH -> notifikasi "nyaris hitam" dicabut.
            if (ui.notice == kg::Strings::Instance().Get("camera.black_frames"))
              ui.notice.clear();
          } else {
            kgcore::Log("app",
                        "settings: GAGAL menyimpan perangkat kamera (index %d) - %s",
                        ui.camera_device, save_err.c_str());
          }
        }
        // M2i (1): apply baru -> swa-uji blend dijalankan ulang.
        s_selftest_done = false;
        s_blend_frames = 0;
        s_blend_logged_first = false;
        ui.status_message =
            camera_started ? "Membuka kamera (menunggu frame pertama)..."
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
    frame_is_synthetic = false;
    if (capture.LatestFrame(frame)) {
      have_frame = true;
      synthetic = false;
      ui.camera_ok = true;
      ui.status_message = kg::Strings::Instance().Get("status.ready");
      // M2h butuh 4: simpan frame mentah TERAKHIR - F5 harus selalu bisa
      // menulis raw_*.png walau F4 tidak pernah dinyalakan.
      last_camera_raw = frame;
      last_camera_raw_valid = true;
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
      frame_is_synthetic = true;
    }

    // ---- M2f/M2g: status kamera jujur; M2g: kode AWAL + TERAKHIR siklus ----
    if (!safe) {
      const kg::CameraCapture::CaptureStatus cs = capture.GetStatus();
      if (cs.streaming) {
        ui.camera_error_line.clear();
        ui.camera_busy_hint = false;
        ui.camera_ok = true;
      } else if (cs.had_failure) {
        // M2g requirement 4: satu baris berisi KODE TERAKHIR + KODE AWAL
        // siklus, dengan nama simbolis bila dikenal (mis. MF_E_SHUTDOWN).
        auto fmt_hr = [](int64_t hr, char* out, size_t n) {
          const char* nm = kgcore::MfErrorName(hr);
          if (nm[0])
            std::snprintf(out, n, "0x%08llX (%s)",
                          static_cast<unsigned long long>(hr & 0xFFFFFFFFull), nm);
          else
            std::snprintf(out, n, "0x%08llX",
                          static_cast<unsigned long long>(hr & 0xFFFFFFFFull));
        };
        char last_buf[64];
        fmt_hr(cs.hr, last_buf, sizeof(last_buf));
        char kbuf[256];
        if (cs.first_hr == 0 || cs.first_hr == cs.hr) {
          std::snprintf(kbuf, sizeof(kbuf), "Kamera gagal: %s (mencoba lagi...)",
                        last_buf);
        } else {
          char first_buf[64];
          fmt_hr(cs.first_hr, first_buf, sizeof(first_buf));
          std::snprintf(kbuf, sizeof(kbuf),
                        "Kamera gagal: %s, awal %s (mencoba lagi...)",
                        last_buf, first_buf);
        }
        ui.camera_error_line = kbuf;  // tab Kamera: baris merah lengkap
        char sbuf[160];
        std::snprintf(sbuf, sizeof(sbuf), "Kamera gagal: %s (mencoba lagi...)",
                      last_buf);
        ui.status_message = sbuf;  // baris status: versi pendek (satu baris)
        ui.camera_busy_hint = kgcore::WantsCameraBusyHint(cs.hr);
        ui.camera_ok = false;
      }
    }

    // ---- M2e requirement 5: ukuran frame berubah di tengah jalan ----
    // Rebuild the gpupixel chain with the new size (per spec) instead of
    // letting filters keep stale framebuffers.
    if (have_frame && pipeline &&
        (frame.width != cam_stream_w || frame.height != cam_stream_h)) {
      if (cam_stream_w > 0) {
        kgcore::Log("app",
                    "ukuran frame kamera berubah %dx%d -> %dx%d; membuat ulang pipeline",
                    cam_stream_w, cam_stream_h, frame.width, frame.height);
        pipeline->Shutdown();
        pipeline_ok = pipeline->Init(exe_dir);
        kgcore::Log("app", "pipeline setelah perubahan ukuran: %s",
                    pipeline_ok ? "OK" : "GAGAL - preview pakai frame mentah");
        if (!pipeline_ok) {
          ui.notice = pipeline->LastResourceError().empty()
                          ? "Efek GPU dinonaktifkan setelah perubahan ukuran kamera"
                          : pipeline->LastResourceError();
          ui.notice_is_error = true;
        }
      } else {
        kgcore::Log("app", "frame kamera pertama: %dx%d", frame.width, frame.height);
      }
      cam_stream_w = frame.width;
      cam_stream_h = frame.height;
    }

    int out_w = 0, out_h = 0;  // dims of THIS iteration's pipeline output
    const bool want_raw = ui.raw_preview;  // F4: tampilkan kamera mentah
    bool pipeline_frame_ok = false;
    if (have_frame && pipeline_ok && !want_raw) {
      // Live-apply the two global beauty parameters (cheap; idempotent).
      pipeline->SetBeautyStrength(ui.smoothing, ui.whitening);
      pipeline->SetSkinMaskEnabled(ui.skin_only);  // M2h: mask hanya bila aktif
      pipeline->SetSkinCoverage(ui.skin_only && ui.skin_coverage == 1);
      pipeline->ProcessFrame(frame);
      // M2j: telemetri seg ke HUD (thread sendiri; lag dalam satuan frame).
      ui.seg_ms = pipeline->SegMs();
      ui.seg_lag = pipeline->SegLagFrames();
      ui.seg_ok = pipeline->SegActive();
      ui.seg_status = pipeline->Seg().LastError();
      if (!frame_is_synthetic) ++sent_to_pipeline;  // frame kamera saja (M2h but.6)
      kg::StageTimings st;
      if (pipeline->GetStageTimings(&st)) {
        ui.detect_ms = st.detect_ms;
        ui.mesh_ms = st.mesh_ms;
        ui.track_ms = st.track_ms;
        ui.mask_ms = st.mask_ms;
        ui.frames_since_detect = st.frames_since_detect;
      }
      const uint8_t* out = pipeline->OutputRGBA();
      out_w = pipeline->OutputWidth();
      out_h = pipeline->OutputHeight();
      if (out && out_w > 0 && out_h > 0) {
        // M2h butuh 2: komposit lewat mask (GPU) - out = mix(raw, beauty,
        // mask*gain); tanpa wajah gain->0 (fade ke raw, bukan frame penuh).
        bool blended = false;
        const kg::SkinMaskBuilder& mb = pipeline->MaskBuilder();
        // M2i (1): `--no-mask-gl` / swa-uji gagal (blender.disabled) -> jalur
        // blend dimatikan; F4 tetap mentah (lihat blok want_raw).
        if (ui.skin_only && out_w == frame.width && out_h == frame.height &&
            !blender.init_failed && !force_no_mask_gl && !blender.disabled) {
          const kgcore::BeautyComposeConfig bcfg = MakeComposeConfig(
              ui.skin_only, ui.smoothing, ui.whitening, ui.chroma_retain);
          blended = blender.Blend(bcfg, pipeline->InputRGBA(), frame.width,
                                  frame.height, out, mb.mask().data(), mb.w(),
                                  mb.h(), pipeline->Tracker().Gain(),
                                  ui.show_mask);
          if (!blended && blender.init_failed && !blender_warned) {
            blender_warned = true;
            kgcore::Log("app",
                        "mask_blender GL gagal (%s) - efek kembali ke seluruh frame",
                        blender.last_error.c_str());
            ui.notice =
                "Shader blend mask gagal - efek sementara berlaku ke seluruh frame (lihat log)";
            ui.notice_is_error = true;
          }
        }
        if (blended) {
          preview_show_blend = true;  // digambar dari tex_out blender (F6 juga)
          ++s_blend_frames;
          if (!s_blend_logged_first) {
            s_blend_logged_first = true;
            kgcore::Log("blend",
                        "aktif pertama: tex_out=%u %dx%d has_stats=%d",
                        blender.tex_out, blender.out_w, blender.out_h,
                        blender.has_stats ? 1 : 0);
          }
          // M2i (1) swa-uji: setelah ~30 frame, luma keluaran vs mentah.
          if (!s_selftest_done && s_blend_frames >= 30) {
            s_selftest_done = true;
            float out_y = 0.0f;
            double raw_sum = 0.0;
            long raw_n = 0;
            // InputRGBA() = buffer RGBA mentah (row-packed w*4); dimensi sama
            // dengan out_* (syarat blend: out_w == frame.width).
            const uint8_t* raw_rgba_ptr = pipeline->InputRGBA();
            if (raw_rgba_ptr) {
              for (int yy = 0; yy < out_h; yy += 4) {
                const uint8_t* row =
                    raw_rgba_ptr + static_cast<size_t>(yy) * out_w * 4;
                for (int xx = 0; xx < out_w; xx += 4) {
                  const uint8_t* px = row + static_cast<size_t>(xx) * 4;
                  raw_sum += 0.299 * px[0] + 0.587 * px[1] + 0.114 * px[2];
                  ++raw_n;
                }
              }
            }
            const float raw_y =
                raw_n ? static_cast<float>(raw_sum / raw_n) : 0.0f;
            if (blender.ReadOutputLuma(&out_y)) {
              if (raw_y >= 40.0f && out_y < raw_y * 0.10f) {
                blender.disabled = true;
                kgcore::Log("blend",
                            "SWA-UJI GAGAL: luma keluaran %.1f jauh di bawah mentah %.1f -> kembali ke jalur sebelum M2h",
                            out_y, raw_y);
                ui.notice =
                    "Preview blend GL hitam - kembali ke jalur lama. Lihat log (tag blend).";
                ui.notice_is_error = true;
              } else {
                kgcore::Log("blend", "SWA-UJI OK: luma keluaran %.1f vs mentah %.1f",
                            out_y, raw_y);
              }
            } else {
              kgcore::Log("blend", "swa-uji dilewati (readback keluaran gagal)");
            }
          }
        } else {
          preview.Upload(out, out_w, out_h, ui.mirror, /*src_is_bgra=*/false);
          preview_show_blend = false;
        }
        pipeline_frame_ok = true;
        shown_w = out_w;
        shown_h = out_h;
        if (!frame_is_synthetic) {
          ++pipe_out;
          ++uploaded_out;
        }
      } else {
        if (!frame_is_synthetic) ++pipe_err_count;
      }
      ui.cam_width = frame.width;
      ui.cam_height = frame.height;
      ui.frames_captured = capture.FramesCaptured();
    }
    if (have_frame && (want_raw || !pipeline_frame_ok)) {
      // Raw path: F4 toggle, OR GPUPixel off/init failed, OR pipeline
      // produced no output this frame. Upload the capture frame as-is
      // (BGRA handled, stride tight, rows top-down, alpha forced 255).
      preview.Upload(frame.pixels.data(), frame.width, frame.height, ui.mirror,
                     /*src_is_bgra=*/frame.format == kg::kFormatBGRA);
      preview_show_blend = false;
      shown_w = frame.width;
      shown_h = frame.height;
      if (!frame_is_synthetic) ++uploaded_raw;
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
      shown_w = frame.width;
      shown_h = frame.height;
      if (!frame_is_synthetic) ++uploaded_raw;
    }
    // M2e requirement 1: glGetError after every preview upload operation.
    drain_gl_errors();

    // M2e requirement 3 (F5): dump raw + pipeline frames as PNG.
    if (ui.debug_frame_requests != debug_req_seen) {
      debug_req_seen = ui.debug_frame_requests;
      // M2h: pakai salinan frame mentah TERAKHIR (walau F4 mati / frame ini
      // sintetis), dan komposit out lewat mask + statistik luar/dalam mask.
      DumpDebugFrames(last_camera_raw_valid ? &last_camera_raw : nullptr,
                      pipeline.get(), pipeline_ok, ui.skin_only, ui.smoothing,
                      ui.whitening, ui.chroma_retain);
    }

    // Mode kamera aktual (read-back) untuk tab Info (requirement 5).
    if (!safe) {
      const kg::CameraStats cst = capture.GetStats();
      if (cst.width > 0 && cst.height > 0) {
        char mline[192];
        std::snprintf(mline, sizeof(mline),
                      "%dx%d @ %d/%d fps, stride %d%s, subtype 0x%08X, luma %.1f, alpha %d",
                      cst.width, cst.height, cst.fps_num, cst.fps_den, cst.stride,
                      cst.stride < 0 ? " (bottom-up)" : "", static_cast<unsigned>(cst.subtype),
                      cst.mean_luma, cst.center_alpha);
        ui.camera_mode_line = mline;
      }
    }

    // M2e requirement 1: diagnosis ringkas tiap detik (semua tahap).
    const int64_t diag_now_ms = duration_cast<std::chrono::milliseconds>(
                                    clock_type::now().time_since_epoch())
                                    .count();
    if (diag_now_ms - diag_last_ms >= 1000) {
      diag_last_ms = diag_now_ms;
      if (!safe) {
        const kg::CameraStats cst = capture.GetStats();
        // M2h butuh 6: gambar nyaris hitam beberapa detik -> petunjuk sekali
        // (reset saat gambar pulih). Bukti: DroidCam index 0 luma=0.3 ~7 dtk.
        {
          const kg::CameraCapture::CaptureStatus cstat = capture.GetStatus();
          const bool dark =
              cstat.streaming && cst.width > 0 && cst.mean_luma >= 0.0f &&
              cst.mean_luma < 8.0f;
          if (dark) {
            if (++dark_streak >= 3 && !dark_hint_shown) {
              dark_hint_shown = true;
              ui.notice = kg::Strings::Instance().Get("camera.black_frames");
              ui.notice_is_error = true;
              kgcore::Log("app",
                          "kamera: gambar nyaris hitam (luma %.1f) ~%d dtk - petunjuk ditampilkan",
                          cst.mean_luma, dark_streak);
            }
          } else {
            if (dark_streak >= 3) {
              // M2i (2): luma pulih -> notifikasi "nyaris hitam" dicabut.
              if (ui.notice == kg::Strings::Instance().Get("camera.black_frames"))
                ui.notice.clear();
            }
            dark_streak = 0;
            dark_hint_shown = false;
          }
        }
        kgcore::Log(
            "diag",
            "MF: recv=%llu %dx%d stride=%d subtype=0x%08X fps=%d/%d luma=%.1f alpha=%d | "
            "pipeline: sent=%llu out=%llu err=%llu | upload: out=%llu raw=%llu | "
            "glErr(last=0x%X total=%u) tex=%u %dx%d RGBA8 | ctx=%p | camRunning=%d pipelineOK=%d | "
            "blend=%u %dx%d%s",
            static_cast<unsigned long long>(cst.received), cst.width, cst.height, cst.stride,
            static_cast<unsigned>(cst.subtype), cst.fps_num, cst.fps_den, cst.mean_luma,
            cst.center_alpha, static_cast<unsigned long long>(sent_to_pipeline),
            static_cast<unsigned long long>(pipe_out),
            static_cast<unsigned long long>(pipe_err_count),
            static_cast<unsigned long long>(uploaded_out),
            static_cast<unsigned long long>(uploaded_raw), static_cast<unsigned>(last_gl_err),
            static_cast<unsigned>(gl_err_count), preview.tex, preview.w, preview.h,
            static_cast<void*>(glfwGetCurrentContext()), capture.Running() ? 1 : 0,
            pipeline_ok ? 1 : 0, blender.tex_out, blender.out_w, blender.out_h,
            blender.disabled ? " (OFF)" : "");
      } else {
        kgcore::Log("diag",
                    "safe mode: MF/pipeline nonaktif | upload: out=%llu raw=%llu | "
                    "glErr(last=0x%X) tex=%u %dx%d | ctx=%p",
                    static_cast<unsigned long long>(uploaded_out),
                    static_cast<unsigned long long>(uploaded_raw),
                    static_cast<unsigned>(last_gl_err), preview.tex, preview.w, preview.h,
                    static_cast<void*>(glfwGetCurrentContext()));
      }
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
      // M2e requirement 6: status frame. "Menunggu frame kamera..." hanya
      // sampai frame pertama tampil; sesudah itu selalu "N ... M diproses".
      const uint64_t cam_recv = safe ? 0 : capture.GetStats().received;
      char fstatus[160];
      std::snprintf(fstatus, sizeof(fstatus), "%llu frame kamera diterima, %llu diproses",
                    static_cast<unsigned long long>(cam_recv),
                    static_cast<unsigned long long>(pipe_out));
      if (shown_w > 0 && shown_h > 0) {
        float scale = std::min(image_area.x / shown_w, image_area.y / shown_h);
        image_size = ImVec2(shown_w * scale, shown_h * scale);
        // Center the image horizontally in the preview window.
        const float offset_x = (avail.x - image_size.x) * 0.5f;
        if (offset_x > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset_x);
        image_pos = ImGui::GetCursorScreenPos();
        // UV (0,0)-(1,1): semua sumber yang kita upload (capture, readback
        // pipeline, sintetis) sudah baris atas-dibawah - tanpa flip (M2e).
        // M2h: tex blend disimpan TANPA mirror (ruang frame) - mirror saat
        // gambar lewat pembalikan U; overlay landmark sudah mirror-aware.
        const bool showing_blend =
            preview_show_blend && !want_raw && !ui.compare_held &&
            shown_w > 0 && shown_h > 0;
        if (showing_blend) {
          ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(blender.tex_out)),
                       image_size, ui.mirror ? ImVec2(1, 0) : ImVec2(0, 0),
                       ui.mirror ? ImVec2(0, 1) : ImVec2(1, 1));
        } else {
          ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(preview.tex)),
                       image_size, ImVec2(0, 0), ImVec2(1, 1));
        }
        // Status overlay di sudut bawah kiri gambar.
        // M2f: F4 tanpa frame kamera wajib bicara ("Tidak ada frame kamera").
        const bool f4_no_frame = ui.raw_preview && cam_recv == 0;
        if (f4_no_frame || cam_recv > 0 || pipe_out > 0 || ui.raw_preview) {
          char line[192];
          if (f4_no_frame) {
            std::snprintf(line, sizeof(line), "Tidak ada frame kamera");
          } else if (cam_recv > 0 || pipe_out > 0) {
            std::snprintf(line, sizeof(line), "%s%s", ui.raw_preview ? "RAW (F4) | " : "",
                          fstatus);
          } else {
            std::snprintf(line, sizeof(line), "RAW (F4)");
          }
          ImDrawList* dl = ImGui::GetWindowDrawList();
          const ImVec2 ts = ImGui::CalcTextSize(line);
          const ImVec2 sp(image_pos.x + 8.0f,
                          image_pos.y + image_size.y - ts.y - 12.0f);
          dl->AddRectFilled(sp, ImVec2(sp.x + ts.x + 12.0f, sp.y + ts.y + 8.0f),
                            IM_COL32(0, 0, 0, 160));
          dl->AddText(ImVec2(sp.x + 6.0f, sp.y + 4.0f),
                      f4_no_frame ? IM_COL32(255, 180, 80, 255) : IM_COL32_WHITE, line);
        }
      } else if (ui.raw_preview && cam_recv == 0) {
        // M2f: F4 aktif tapi belum ada frame kamera sama sekali.
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Tidak ada frame kamera");
      } else if (cam_recv == 0) {
        ImGui::TextUnformatted("Menunggu frame kamera...");
      } else {
        // Frame kamera sudah masuk tetapi belum ada yang tampil: laporkan
        // hitungan (termasuk "0 diproses" bila pipeline belum menghasilkan).
        ImGui::Text("%s", fstatus);
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
        char hud[640];
        char seg_part[64];
        if (ui.seg_ok) {
          std::snprintf(seg_part, sizeof(seg_part), "seg %.1f ms (%+.0f fr)",
                        ui.seg_ms, ui.seg_lag);
        } else {
          std::snprintf(seg_part, sizeof(seg_part), "seg mati");
        }
        std::snprintf(hud, sizeof(hud),
                      "FPS %.1f | cap %.2f ms | det %.2f ms | mesh %.2f ms | "
                      "trk %.2f ms | msk %.2f ms | %s | render %.2f ms | "
                      "sejak deteksi %d | frame #%llu",
                      ui.fps, ui.capture_ms, ui.detect_ms, ui.mesh_ms, ui.track_ms,
                      ui.mask_ms, seg_part, ui.render_ms, ui.frames_since_detect,
                      static_cast<unsigned long long>(ui.frames_captured));
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
  blender.Destroy();  // M2h: tekstur/shader blend mask (GL konteks masih ada)
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
