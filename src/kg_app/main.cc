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
#include "kgcore/perf_guard.h"  // M2k: adaptive load guard
#include "kgcore/mask_layers.h"  // M2k: F5 same-pass diff report
#include "kgcore/black_frame_guard.h"  // M2l: swa-uji berkelanjutan frame hitam
#include "kgcore/camera_orientation.h" // M2p: rotasi kamera + auto-orientasi
#include "kgcore/landmark_tracking.h"  // M2l: overlay landmark ber-indeks F5

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


std::string SettingsPathFromAppData(const std::wstring& appdata_dir) {
  if (!appdata_dir.empty()) {
    return WideToUtf8(appdata_dir) + "\\settings.json";
  }
  return "settings.json";
}

float ComputeMeanLumaRgba(const uint8_t* rgba, int w, int h) {
  if (!rgba || w <= 0 || h <= 0) return 0.0f;
  double sum = 0.0;
  long n = 0;
  for (int y = 0; y < h; y += 8) {
    const uint8_t* row = rgba + static_cast<size_t>(y) * w * 4;
    for (int x = 0; x < w; x += 8) {
      const uint8_t* p = row + static_cast<size_t>(x) * 4;
      sum += 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
      ++n;
    }
  }
  return n > 0 ? static_cast<float>(sum / n) : 0.0f;
}

float ComputeMeanLumaFrame(const kg::Frame& f) {
  if (!f.IsValid()) return 0.0f;
  double sum = 0.0;
  long n = 0;
  const bool bgra = (f.format != kg::kFormatRGBA);
  for (int y = 0; y < f.height; y += 8) {
    const uint8_t* row = f.pixels.data() + static_cast<size_t>(y) * f.stride_bytes;
    for (int x = 0; x < f.width; x += 8) {
      const uint8_t* p = row + static_cast<size_t>(x) * 4;
      const float r = bgra ? p[2] : p[0];
      const float g = p[1];
      const float b = bgra ? p[0] : p[2];
      sum += 0.299 * r + 0.587 * g + 0.114 * b;
      ++n;
    }
  }
  return n > 0 ? static_cast<float>(sum / n) : 0.0f;
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
  s.camera_rotation = ui.camera_rotation;
  s.auto_rotation_prior = ui.auto_rotation_prior;
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
  ui.camera_rotation = s.camera_rotation;
  ui.auto_rotation_prior = s.auto_rotation_prior;
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

// M2e/M2l (F5): %APPDATA%\KameraGeulis\debug\ (pakai appdata_dir UTF-16 agar konsisten).
std::string DebugDumpDir(const std::wstring& appdata_dir) {
  if (appdata_dir.empty()) return {};
  const std::wstring wdir = appdata_dir + L"\\debug";
  CreateDirectoryW(wdir.c_str(), nullptr);
  return WideToUtf8(wdir);
}

// M2h/M2l: simpan pasangan raw_*.png + out_*.png + mask_*.png + seg_*.png +
// diff_*.png (960x540 grayscale) + roi_*.png + landmarks_*.png (ber-indeks).
// `tex_out` GPU kini DIJAMIN bersih tanpa overlay F6/F3.
void DumpDebugFrames(const std::wstring& appdata_dir, const kg::Frame* raw,
                     kg::BeautyPipeline* pipeline, MaskBlender* blender,
                     bool blend_current, bool pipeline_ok, bool skin_only,
                     float smooth_amount, float whitening,
                     float chroma_retain) {
  const std::string dir = DebugDumpDir(appdata_dir);
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
    if (kgcore::WritePngRgbFromRgba(path.c_str(), raw_rgba.data(), raw_w, raw_h)) {
      kgcore::Log("app", "F5: frame mentah -> %s (%dx%d, RGB terkompresi)",
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
  // --- M2l: roi_*.png (potongan 192x192 yang dikirim ke FaceMesh) & landmarks_*.png ---
  {
    std::vector<uint8_t> roi_rgba;
    int roi_w = 0, roi_h = 0;
    kgcore::AlignedRoi roi_used{};
    const bool have_roi = pipeline->Tracker().Provider()->GetDebugRoi(
        &roi_rgba, &roi_w, &roi_h, &roi_used);
    if (have_roi && !roi_rgba.empty()) {
      const std::string path = dir + "\\roi_" + stamp + ".png";
      if (kgcore::WritePngRgbFromRgba(path.c_str(), roi_rgba.data(), roi_w, roi_h)) {
        kgcore::Log("app", "F5: potongan ROI FaceMesh -> %s (%dx%d, cx=%.1f cy=%.1f side=%.1f rot=%.1f deg)",
                    path.c_str(), roi_w, roi_h, roi_used.cx_px, roi_used.cy_px,
                    roi_used.side_px, roi_used.angle_rad * 57.29578f);
      }
    }
    const auto& lm = pipeline->Tracker().Current();
    if (!raw_rgba.empty() && (lm.valid || have_roi)) {
      std::vector<uint8_t> lm_rgba;
      if (kgcore::RenderLandmarkDebugOverlayRgba(
              raw_rgba.data(), raw_w, raw_h, lm,
              have_roi ? &roi_used : nullptr, &lm_rgba)) {
        const std::string path = dir + "\\landmarks_" + stamp + ".png";
        if (kgcore::WritePngRgbFromRgba(path.c_str(), lm_rgba.data(), raw_w, raw_h)) {
          kgcore::Log("app", "F5: overlay landmark ber-indeks -> %s (%dx%d)",
                      path.c_str(), raw_w, raw_h);
        }
      }
    }
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
  std::vector<uint8_t> gpu_mask, gpu_out;
  const bool have_gpu_mask = blend_current && skin_only && blender &&
      blender->mask_w > 0 && blender->mask_h > 0 &&
      blender->ReadbackMask(&gpu_mask) &&
      gpu_mask.size() == static_cast<size_t>(blender->mask_w) * blender->mask_h;
  const bool have_gpu_out = blend_current && skin_only && blender && blender->ReadbackOutput(&gpu_out) &&
      gpu_out.size() == static_cast<size_t>(w) * h * 4;
  const int src_mw = have_gpu_mask ? blender->mask_w : mb.w();
  const int src_mh = have_gpu_mask ? blender->mask_h : mb.h();
  const uint8_t* mask_data = have_gpu_mask ? gpu_mask.data() : mb.mask().data();
  const bool mask_ok = have_gpu_mask ||
      (mb.configured() &&
       mb.mask().size() == static_cast<size_t>(mb.w()) * static_cast<size_t>(mb.h()));
  // M2n/M2o: sinkronkan mask efektif resolusi penuh dengan penyaring fitur gelap
  // (luma blur 5x5) di kFsFinal supaya mask_*.png dan metrik F5 sesuai 1:1.
  std::vector<uint8_t> effective_mask_full;
  int eff_mw = src_mw, eff_mh = src_mh;
  const uint8_t* eff_mask_ptr = mask_data;
  long face_pre_n = 0, dark_excl_n = 0, dark_atten_n = 0;
  const bool apply_fullres_dark =
      mask_ok && have_gpu_mask && blender && blender->last_skin_luma > 0.0f &&
      !raw_rgba.empty() && raw_w == w && raw_h == h;
  kgcore::NostrilRoiInfo nostril_info[2]{};
  bool have_nostril_info = false;
  int nostril_found_count = 0;
  kgcore::LandmarkSet f5_lm{};
  if (mask_ok && !raw_rgba.empty() && raw_w == w && raw_h == h) {
    const auto& lm_raw = pipeline->Tracker().CurrentRaw();
    const auto& lm = lm_raw.valid ? lm_raw : pipeline->Tracker().Current();
    f5_lm = lm;
    if (lm.valid) {
      std::vector<float> f5_xy(kgcore::kLandmarkCount * 2);
      for (size_t i = 0; i < kgcore::kLandmarkCount; ++i) {
        f5_xy[i * 2] = lm.pts[i].x * w;
        f5_xy[i * 2 + 1] = lm.pts[i].y * h;
      }
      const auto basis = kgcore::ComputeFaceEyeBasis(f5_xy.data());
      const auto base_shapes = kgcore::BuildExclusionShapes(f5_xy.data());
      kgcore::ExclusionShapes refined = base_shapes;
      const float ref_luma =
          mb.has_luma_calibration()
              ? mb.skin_luma0()
              : (blender ? blender->last_skin_luma : 0.0f);
      kgcore::RefineNostrilEllipsesFromLuma(
          base_shapes, raw_rgba.data(), w, h, w * 4, ref_luma, basis.iod,
          &refined, nostril_info, f5_xy.data());
      have_nostril_info = !base_shapes.ellipses.empty();
      for (int k = 0; k < 2; ++k) {
        if (nostril_info[k].found_dark_blob) ++nostril_found_count;
      }
    }
  }
  if (apply_fullres_dark) {
    effective_mask_full.resize(static_cast<size_t>(w) * h);
    kgcore::SkinMaskConfig dcfg = mb.config();
    dcfg.dark_luma_low_ratio = blender->last_dark_low_ratio;
    dcfg.dark_luma_high_ratio = blender->last_dark_high_ratio;
    const float r0 = blender->last_nostril_roi[0][2];
    const float r1 = blender->last_nostril_roi[1][2];
    const float r0_sq = r0 * r0;
    const float r1_sq = r1 * r1;
    const bool same_res_mask = (src_mw == w && src_mh == h);
    for (int y = 0; y < h; ++y) {
      const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(h);
      const float fy = static_cast<float>(y) + 0.5f;
      for (int x = 0; x < w; ++x) {
        const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(w);
        const float fx = static_cast<float>(x) + 0.5f;
        const size_t idx = static_cast<size_t>(y) * w + x;
        const float m_pre = same_res_mask
            ? (static_cast<float>(mask_data[idx]) / 255.0f)
            : kgcore::SampleMaskBilinear(mask_data, src_mw, src_mh, u, v);
        if (m_pre <= 0.0f) {
          effective_mask_full[idx] = 0;
          continue;
        }
        const float y_px = kgcore::ComputeBlurredLuma5x5(
            raw_rgba.data(), w, h, w * 4, x, y);
        bool in_n = false;
        if (r0 > 0.0f) {
          const float dx = fx - blender->last_nostril_roi[0][0];
          const float dy = fy - blender->last_nostril_roi[0][1];
          if (dx * dx + dy * dy <= r0_sq) in_n = true;
        }
        if (!in_n && r1 > 0.0f) {
          const float dx = fx - blender->last_nostril_roi[1][0];
          const float dy = fy - blender->last_nostril_roi[1][1];
          if (dx * dx + dy * dy <= r1_sq) in_n = true;
        }
        const float w_dark = kgcore::ComputeDarkFeatureWeight(
            y_px, blender->last_skin_luma, dcfg, in_n);
        if (m_pre >= 0.5f) {
          ++face_pre_n;
          if (w_dark < 0.5f) ++dark_excl_n;
          if (w_dark < 0.95f) ++dark_atten_n;
        }
        const float m_eff = std::clamp(m_pre * w_dark, 0.0f, 1.0f);
        effective_mask_full[idx] = static_cast<uint8_t>(m_eff * 255.0f + 0.5f);
      }
    }
    eff_mask_ptr = effective_mask_full.data();
    eff_mw = w;
    eff_mh = h;
  }
  // --- mask_*.png (grayscale 8-bit terkompresi ukuran frame) ---
  if (mask_ok) {
    std::vector<uint8_t> gray;
    if (!effective_mask_full.empty()) {
      gray = effective_mask_full;
    } else if (src_mw == w && src_mh == h) {
      gray.assign(mask_data, mask_data + static_cast<size_t>(w) * h);
    } else {
      std::vector<uint8_t> gray_rgba(static_cast<size_t>(w) * h * 4);
      kgcore::UpsampleMaskToGrayRgba(mask_data, src_mw, src_mh, w, h,
                                     gray_rgba.data());
      gray.resize(static_cast<size_t>(w) * h);
      for (size_t i = 0; i < gray.size(); ++i) gray[i] = gray_rgba[i * 4];
    }
    const std::string path = dir + "\\mask_" + stamp + ".png";
    if (kgcore::WritePngGray(path.c_str(), gray.data(), w, h)) {
      kgcore::Log("app",
                  "F5: mask blend aktual -> %s (sumber=%s, mask %dx%d, gray8 %dx%d)",
                  path.c_str(),
                  apply_fullres_dark ? "tex_mask GPU + filter gelap full-res"
                                     : (have_gpu_mask ? "tex_mask GPU" : "CPU upload buffer"),
                  eff_mw, eff_mh, w, h);
    } else {
      kgcore::Log("app", "F5: GAGAL menulis %s", path.c_str());
    }
  } else {
    kgcore::Log("app",
                "F5: mask kulit tidak tersedia (checkbox mati atau belum ada frame wajah) - mask_*.png dilewati");
  }
  // Argmax seg dengan pemetaan yang sama ke resolusi frame untuk korelasi F5.
  std::vector<uint8_t> argmax_full;
  // --- seg_*.png: peta kelas BERWARNA + statistik per kelas + ms + lag ---
  {
    const std::vector<float>& probs = pipeline->SegUsedProbabilities();
    const uint64_t seg_serial = pipeline->SegUsedSerial();
    const double seg_ms = pipeline->SegUsedInferMs();
    if (pipeline->SegActive() &&
        probs.size() == static_cast<size_t>(kgcore::kSegInputSize) *
                        kgcore::kSegInputSize * kgcore::kSegClasses) {
      const int sw = kgcore::kSegInputSize, sh = kgcore::kSegInputSize;
      std::vector<uint8_t> am(static_cast<size_t>(sw) * sh);
      kgcore::SegArgmax(probs.data(), sw * sh, am.data());
      argmax_full.resize(static_cast<size_t>(w) * h);
      if (pipeline->SegUsedSimilarity().valid) {
        kgcore::SegProbabilitiesToFrameArgmaxSimilarity(
            probs.data(), pipeline->SegUsedLetterbox(), w, h,
            argmax_full.data(), pipeline->SegUsedSimilarity(), sw);
      } else {
        kgcore::SegProbabilitiesToFrameArgmax(
            probs.data(), pipeline->SegUsedLetterbox(), w, h,
            argmax_full.data(), sw, pipeline->SegUsedShiftX(),
            pipeline->SegUsedShiftY());
      }
      std::vector<uint8_t> up(static_cast<size_t>(w) * h * 4);
      for (int i = 0; i < w * h; ++i) {
        uint8_t col[4];
        kgcore::SegClassColor(argmax_full[static_cast<size_t>(i)], col);
        uint8_t* d = up.data() + static_cast<size_t>(i) * 4;
        d[0] = col[0]; d[1] = col[1]; d[2] = col[2]; d[3] = 255;
      }
      const std::string path = dir + "\\seg_" + stamp + ".png";
      if (kgcore::WritePngRgbFromRgba(path.c_str(), up.data(), w, h)) {
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
                  "F5: seg snapshot yang dipakai frame ini belum ada (%s) - seg_*.png dilewati",
                  pipeline->Seg().LastError().c_str());
    }
  }
  // --- statistik selisih raw vs out (SELALU dari output bersih tanpa overlay F6/F3) ---
  auto log_stats = [&](const uint8_t* cmp_raw, const uint8_t* cmp_out,
                       const char* mode) {
    if (raw_rgba.empty() || raw_w != w || raw_h != h) {
      kgcore::Log("app", "F5: statistik selisih dilewati (raw %dx%d vs out %dx%d)",
                  raw_w, raw_h, w, h);
      return;
    }
    float outside = 0.0f, inside = 0.0f;
    kgcore::MaskDiffStats(cmp_raw, cmp_out, eff_mask_ptr, eff_mw, eff_mh, w,
                          h, gain, &outside, &inside);
    kgcore::Log("app",
                "F5: selisih rata-rata LUAR mask = %.3f (harus mendekati 0), "
                "DALAM mask = %.3f | gain=%.2f | mode=%s | mask %dx%d",
                outside, inside, gain, mode, eff_mw, eff_mh);
    kgcore::InMaskMetrics met;
    kgcore::ComputeInMaskMetrics(cmp_raw, cmp_out, eff_mask_ptr, eff_mw,
                                 eff_mh, w, h, gain, &met);
    kgcore::Log("app",
                "F5 metrik M2i [%s]: delta luma inti = %+.1f | delta chroma inti = %.1f | "
                "rasio detail halus = %.3f | diff pita tepi = %.1f | "
                "n core=%d tepi=%d luar=%d",
                mode, met.delta_luma, met.delta_chroma, met.detail_ratio,
                met.edge_band_diff, met.core_n, met.edge_n, met.outside_n);
    kgcore::F5DiffReport rep;
    kgcore::ComputeF5DiffReport(cmp_raw, cmp_out, eff_mask_ptr, eff_mw, eff_mh, w, h,
                                gain, argmax_full.empty() ? nullptr : argmax_full.data(),
                                &rep);
    static const char* bands[5] = {"<0.01", "0.01-0.1", "0.1-0.5", "0.5-0.9", ">0.9"};
    for (int i = 0; i < 5; ++i) {
      const auto& r = rep.per_range[i];
      kgcore::Log("app", "F5 diff band mask %s: mean=%.3f n=%ld", bands[i],
                  r.n ? r.sum / r.n : 0.0, r.n);
    }
    static const char* classes[6] = {"latar", "rambut", "kulit_tubuh", "kulit_wajah",
                                      "pakaian", "aksesoris"};
    for (int i = 0; i < 6; ++i) {
      const auto& c = rep.per_class[i];
      kgcore::Log("app", "F5 diff seg class %s: mean=%.3f n=%ld", classes[i],
                  c.n ? c.sum / c.n : 0.0, c.n);
    }
    kgcore::Log("app", "F5 outside-mask (mask*gain < 8/255): mean=%.3f n=%ld "
                       "[target <=0.1 levels]", rep.outside_mean, rep.outside_n);
    {
      const auto& hc = rep.per_class[1];
      const double hair_mean = hc.n ? hc.sum / hc.n : 0.0;
      const double hair_ge3_pct = hc.n ? (100.0 * hc.ge3_n / hc.n) : 0.0;
      kgcore::Log("app",
                  "F5 metrik M2n rambut: mean=%.3f [target <=0.15] | "
                  "berubah >=3 level=%.2f%% (%ld/%ld) [target <1%%]",
                  hair_mean, hair_ge3_pct, hc.ge3_n, hc.n);
    }
    if (face_pre_n > 0) {
      kgcore::Log("app",
                  "F5 metrik M2n filter_gelap: skin_luma=%.1f excl(<0.5)=%.2f%% "
                  "(%ld/%ld) atten(<0.95)=%.2f%% (%ld/%ld)",
                  blender->last_skin_luma, 100.0 * dark_excl_n / face_pre_n,
                  dark_excl_n, face_pre_n, 100.0 * dark_atten_n / face_pre_n,
                  dark_atten_n, face_pre_n);
    } else {
      kgcore::Log("app",
                  "F5 metrik M2n filter_gelap: skin_luma=%.1f (belum ada piksel wajah full-res)",
                  blender ? blender->last_skin_luma : 0.0f);
    }
    if (have_nostril_info) {
      kgcore::Log("app",
                  "F5 metrik M2o lubang_hidung: found=%d/2 | Kanan anchor=(%.1f,%.1f) "
                  "blob=(%.1f,%.1f) excl=(%.1f,%.1f) anchor_blob=%.2fpx excl_blob=%.2fpx found=%d | "
                  "Kiri anchor=(%.1f,%.1f) blob=(%.1f,%.1f) excl=(%.1f,%.1f) "
                  "anchor_blob=%.2fpx excl_blob=%.2fpx found=%d (search=(%.1f,%.1f) roi_r=%.1f px)",
                  nostril_found_count,
                  nostril_info[0].anchor_cx, nostril_info[0].anchor_cy,
                  nostril_info[0].dark_cx, nostril_info[0].dark_cy,
                  nostril_info[0].excl_cx, nostril_info[0].excl_cy,
                  nostril_info[0].anchor_blob_offset_px,
                  nostril_info[0].offset_px, nostril_info[0].found_dark_blob ? 1 : 0,
                  nostril_info[1].anchor_cx, nostril_info[1].anchor_cy,
                  nostril_info[1].dark_cx, nostril_info[1].dark_cy,
                  nostril_info[1].excl_cx, nostril_info[1].excl_cy,
                  nostril_info[1].anchor_blob_offset_px,
                  nostril_info[1].offset_px, nostril_info[1].found_dark_blob ? 1 : 0,
                  nostril_info[0].search_cx, nostril_info[0].search_cy,
                  nostril_info[0].roi_radius_px);
    }
    if (f5_lm.valid) {
      kgcore::HairlineMetrics hlm{};
      kgcore::ComputeHairlineMetrics(
          cmp_raw, cmp_out, eff_mask_ptr, eff_mw, eff_mh, w, h, gain,
          f5_lm, &hlm);
      if (hlm.band_n > 0) {
        kgcore::Log("app",
                    "F5 metrik M2o hairline: band=%d rough(0.05-0.95)=%.1f%% (%d) | "
                    "strip6_raw=%.2f strip6_out=%.2f | interior_raw=%.2f interior_out=%.2f | "
                    "halo_out=%.2f halo_delta=%+.2f | guided=%d",
                    hlm.band_n, hlm.roughness_pct, hlm.rough_n,
                    hlm.strip_raw_luma, hlm.strip_out_luma,
                    hlm.interior_raw_luma, hlm.interior_out_luma,
                    hlm.halo_out_diff, hlm.halo_filter_delta,
                    (blender && blender->last_mask_guided) ? 1 : 0);
      }
    }
    const auto cmp_tel = pipeline->Seg().LatestCompare();
    if (cmp_tel.active) {
      kgcore::Log("app",
                  "F5 seg_compare (n=%llu): fp32_avg=%.1fms int8_avg=%.1fms | "
                  "argmax_agree=%.2f%% mIoU=%.4f",
                  static_cast<unsigned long long>(cmp_tel.samples),
                  cmp_tel.avg_fp32_ms, cmp_tel.avg_int8_ms,
                  cmp_tel.avg_argmax_agree * 100.0f, cmp_tel.avg_miou);
    }
    const int dw = w > 960 ? 960 : w;
    const int dh = h > 540 ? 540 : h;
    std::vector<uint8_t> diff_gray;
    kgcore::WriteDiffAmplifiedGray(cmp_raw, cmp_out, w, h, 8.0f, dw, dh,
                                    &diff_gray);
    const std::string diff_path = dir + "\\diff_" + stamp + ".png";
    if (kgcore::WritePngGray(diff_path.c_str(), diff_gray.data(), dw, dh))
      kgcore::Log("app", "F5: diff amplified x8 -> %s (grayscale %dx%d)",
                  diff_path.c_str(), dw, dh);
  };
  // --- out_*.png ---
  if (have_gpu_out) {
    const std::string path = dir + "\\out_" + stamp + ".png";
    if (kgcore::WritePngRgbFromRgba(path.c_str(), gpu_out.data(), w, h))
      kgcore::Log("app", "F5: output bersih dari tex_out GPU -> %s (%dx%d)",
                  path.c_str(), w, h);
    else
      kgcore::Log("app", "F5: GAGAL menulis output GPU %s", path.c_str());
    log_stats(raw_rgba.data(), gpu_out.data(), "tex_out GPU (same pass, bersih)");
  } else if (skin_only && mask_ok && !raw_rgba.empty() && raw_w == w && raw_h == h) {
    std::vector<uint8_t> mixed(raw_rgba.size());
    const kgcore::BeautyComposeConfig bcfg =
        MakeComposeConfig(skin_only, smooth_amount, whitening, chroma_retain);
    kgcore::ComposeBeautyRaw(raw_rgba.data(), out, mb.mask().data(), mb.w(),
                             mb.h(), w, h, gain, bcfg, mixed.data());
    const std::string path = dir + "\\out_" + stamp + ".png";
    if (kgcore::WritePngRgbFromRgba(path.c_str(), mixed.data(), w, h)) {
      kgcore::Log("app",
                  "F5: frame keluaran (komposit raw+beauty x mask) -> %s (%dx%d)",
                  path.c_str(), w, h);
    } else {
      kgcore::Log("app", "F5: GAGAL menulis %s", path.c_str());
    }
    log_stats(raw_rgba.data(), mixed.data(), "skin-only");
  } else {
    const std::string path = dir + "\\out_" + stamp + ".png";
    if (kgcore::WritePngRgbFromRgba(path.c_str(), out, w, h)) {
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
    glActiveTexture(GL_TEXTURE0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glBindTexture(GL_TEXTURE_2D, tex);
    if (w != width || h != height) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
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
  const std::string settings_path = SettingsPathFromAppData(paths.appdata_dir);
  const std::wstring settings_wpath = paths.appdata_dir + L"\\settings.json";
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
      pipeline->Seg().SetCompareMode(args.seg_compare);
      pipeline->Seg().SetPreferInt8(args.seg_int8);
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
  const DWORD settings_attr_before = GetFileAttributesW(settings_wpath.c_str());
  const bool settings_file_existed = settings_attr_before != INVALID_FILE_ATTRIBUTES;
  std::string settings_load_error;
  const bool settings_loaded = settings.Load(settings_path, &settings_load_error);
  if (settings_loaded) {
    ApplySettingsToUi(settings, ui);
  } else if (settings_file_existed) {
    const std::wstring bad_wpath = settings_wpath + L".bad";
    if (GetFileAttributesW(bad_wpath.c_str()) == INVALID_FILE_ATTRIBUTES) {
      if (MoveFileW(settings_wpath.c_str(), bad_wpath.c_str())) {
        kg::Log("app", "settings: berkas rusak dipindah ke %ls", bad_wpath.c_str());
      } else {
        const DWORD backup_error = GetLastError();
        kg::Log("app", "settings: berkas rusak dipertahankan di %s (backup gagal, Win32=%lu)",
                settings_path.c_str(), static_cast<unsigned long>(backup_error));
      }
    } else {
      kg::Log("app", "settings: berkas rusak dipertahankan di %s (.bad sudah ada)",
              settings_path.c_str());
    }
  }
  kg::Log("app", "settings: %s (%s) cause=%s",
          settings_loaded ? "loaded" : "defaults", settings_path.c_str(),
          settings_loaded ? "none" : settings_load_error.c_str());
  kg::Strings::Instance().SetLanguage(ui.language == "en" ? kg::Language::kEn : kg::Language::kId);

  kg::CameraCapture capture;
  bool camera_started = false;
  if (!safe) {
    camera_started = capture.Start(ui.camera_device, ui.capture_width,
                                   ui.capture_height, ui.capture_fps,
                                   args.capture_plain);
    if (camera_started && !safe) {
      kg::Settings dev = settings;
      dev.camera_device = ui.camera_device;
      dev.capture_width = ui.capture_width;
      dev.capture_height = ui.capture_height;
      dev.capture_fps = ui.capture_fps;
      std::string dev_err;
      kg::Settings::SaveInfo sinfo;
      // File corrupt sudah di-backup ke .bad di atas; simpan file baru bila
      // settings terbaca atau belum ada file (atau sudah dipindah ke .bad).
      const bool can_write_settings =
          settings_loaded || !settings_file_existed ||
          (GetFileAttributesW(settings_wpath.c_str()) == INVALID_FILE_ATTRIBUTES);
      if (can_write_settings) {
        if (dev.SaveAndVerify(settings_path, &sinfo, &dev_err)) {
          kgcore::Log("app",
                      "settings: perangkat awal tersimpan & terverifikasi (%s, %zu byte, device=%d %dx%d@%d)",
                      sinfo.resolved_path.c_str(), sinfo.bytes_written,
                      dev.camera_device, dev.capture_width, dev.capture_height,
                      dev.capture_fps);
        } else {
          kgcore::Log("app", "settings: GAGAL menyimpan perangkat awal - %s",
                      dev_err.c_str());
        }
      } else {
        kgcore::Log("app", "settings: perangkat awal tidak disimpan karena file lama gagal dibaca dan masih ada");
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
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
  glBindTexture(GL_TEXTURE_2D, 0);

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
  kgcore::PerfGuard perf_guard;
  kgcore::AutoOrientationController orient_ctrl;
  orient_ctrl.SetPrior(ui.auto_rotation_prior);
  ui.effective_rotation = orient_ctrl.EffectiveRotation(ui.camera_rotation);
  int prev_effective_rot = ui.effective_rotation;
  kgcore::Log("orient", "inisialisasi rotasi: mode=%d prior=%d° efektif=%d°",
              ui.camera_rotation, orient_ctrl.Prior(), ui.effective_rotation);
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

  // ---- M2h/M2l state (mask kulit + swa-uji berkelanjutan frame hitam) ----
  MaskBlender blender;             // stage mix(raw, beauty, mask*gain) GPU
  kg::Frame last_camera_raw;       // salinan frame mentah TERAKHIR utk F5 & fallback
  bool last_camera_raw_valid = false;
  int64_t last_frame_ts_ms = 0;
  bool frame_is_synthetic = false; // frame iterasi ini bukan dari kamera
  bool preview_show_blend = false; // dipertahankan antar iterasi !have_frame
  bool blender_warned = false;
  bool s_selftest_done = false;
  int s_blend_frames = 0;
  bool s_blend_logged_first = false;
  const bool force_no_mask_gl = args.no_mask_gl;
  bool gpu_mask_gen_disabled = args.no_mask_gen_gpu;
  kgcore::BlackFrameGuard black_guard;
  float last_raw_luma = 0.0f;
  float last_blend_out_luma = 0.0f;
  float last_nonblend_out_luma = 0.0f;
  bool compare_uploaded_nonframe = false;
  bool prev_show_mask = ui.show_mask;
  int dark_streak = 0;             // detik berturut gambar nyaris hitam
  bool dark_hint_shown = false;
  // M2m: akumulator telemetri lag pelacakan & latensi kamera-ke-layar per detik.
  std::vector<float> win_oneeuro_mean_px;
  std::vector<float> win_oneeuro_p95_px;
  std::vector<float> win_mask_feat_diff_px;
  std::vector<float> win_cam2screen_ms;
  int64_t pending_cam_ts_ms = -1;
  float last_mask_feat_diff_px = 0.0f;
  // M2o: state benchmark matrix 12 konfigurasi (--seg-bench / tab Info) +
  // deteksi MF camera FPS drop.
  const std::vector<kgcore::SegBenchConfig> bench_matrix =
      kgcore::BuildSegBenchMatrix();
  std::vector<kgcore::SegBenchResult> bench_results;
  bool bench_running = args.seg_bench && !safe;
  size_t bench_idx = 0;
  int64_t bench_step_start_ms = 0;
  uint64_t bench_step_cam_recv0 = 0;
  uint64_t bench_step_sent0 = 0;
  uint64_t bench_step_pipe0 = 0;
  std::vector<float> bench_fps_samples;
  std::vector<float> bench_c2s_samples;
  double bench_seg_ms_sum = 0.0;
  double bench_seg_lag_sum = 0.0;
  int bench_seg_n = 0;
  bool bench_cfg_applied = false;
  if (bench_running) {
    ui.seg_bench_active = true;
    ui.seg_bench_status = "Benchmark seg dimulai (12 konfigurasi x 10 dtk)...";
    kgcore::Log("bench", "mode --seg-bench aktif: 12 konfigurasi x 10 detik");
  }
  bool mf_low_light_warned = false;
  int mf_underdeliver_streak = 0;

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
      kg::Settings::SaveInfo sinfo;
      if (settings.SaveAndVerify(settings_path, &sinfo, &save_err)) {
        ui.notice = kg::Strings::Instance().Get("presets.saved");
        ui.notice_is_error = false;
        kgcore::Log("app",
                    "settings: tersimpan & terverifikasi (%s, %zu byte, device=%d %dx%d@%d)",
                    sinfo.resolved_path.c_str(), sinfo.bytes_written,
                    settings.camera_device, settings.capture_width,
                    settings.capture_height, settings.capture_fps);
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
        orient_ctrl.SetPrior(ui.auto_rotation_prior);
        orient_ctrl.ResetTransient();
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
      ui.camera_rotation = def.camera_rotation;
      ui.preset = def.preset;  // "Natural"
      orient_ctrl.ResetTransient();
      ui.request_reset_defaults = false;
    }
    if (ui.request_rotation_save) {
      ui.request_rotation_save = false;
      kg::Settings rot_s;
      rot_s.Load(settings_path);
      rot_s.camera_rotation = ui.camera_rotation;
      rot_s.auto_rotation_prior = orient_ctrl.Prior();
      std::string rot_err;
      kg::Settings::SaveInfo sinfo;
      if (rot_s.SaveAndVerify(settings_path, &sinfo, &rot_err)) {
        kgcore::Log("orient",
                    "settings: putar kamera tersimpan (%d°, prior=%d°)",
                    rot_s.camera_rotation, rot_s.auto_rotation_prior);
      } else {
        kgcore::Log("orient", "settings: gagal menyimpan putar kamera - %s",
                    rot_err.c_str());
      }
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
          kg::Settings dev;
          dev.Load(settings_path);  // pertahankan nilai lain; default bila hilang
          dev.camera_device = ui.camera_device;
          dev.capture_width = ui.capture_width;
          dev.capture_height = ui.capture_height;
          dev.capture_fps = ui.capture_fps;
          std::string save_err;
          kg::Settings::SaveInfo sinfo;
          if (dev.SaveAndVerify(settings_path, &sinfo, &save_err)) {
            kgcore::Log("app",
                        "settings: perangkat kamera tersimpan & terverifikasi (%s, %zu byte, index %d %dx%d@%d)",
                        sinfo.resolved_path.c_str(), sinfo.bytes_written,
                        dev.camera_device, dev.capture_width,
                        dev.capture_height, dev.capture_fps);
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

    // ---- capture + M2p rotasi orientasi ----
    kg::Frame raw_frame;
    kg::Frame frame;
    bool have_frame = false;
    const auto cap_start = clock_type::now();
    frame_is_synthetic = false;
    if (capture.LatestFrame(raw_frame)) {
      have_frame = true;
      synthetic = false;
      ui.camera_ok = true;
      ui.status_message = kg::Strings::Instance().Get("status.ready");
      pending_cam_ts_ms = raw_frame.timestamp_ms;
    }
    ui.capture_ms = duration_cast<std::chrono::microseconds>(clock_type::now() - cap_start)
                        .count() /
                    1000.0f;
    if (!have_frame && synthetic) {
      auto now_ms = duration_cast<std::chrono::milliseconds>(
                        clock_type::now().time_since_epoch())
                        .count();
      raw_frame = MakeSyntheticFrame(synthetic_w, synthetic_h, now_ms);
      have_frame = true;
      frame_is_synthetic = true;
    }

    if (have_frame) {
      // M2p (1b): deteksi orientasi otomatis saat wajah tidak terdeteksi
      // atau tracking gagal berulang -> evaluasi 4 rotasi (0/90/180/270).
      if (!frame_is_synthetic && !safe && pipeline_ok && pipeline &&
          orient_ctrl.ShouldSearch(ui.camera_rotation)) {
        kg::OrientationScores scores;
        if (pipeline->ProbeOrientations(raw_frame, &scores) && scores.ok) {
          const auto dec = orient_ctrl.ApplySearchScores(scores);
          kgcore::Log(
              "orient",
              "search: 0°=%.2f 90°=%.2f 180°=%.2f 270°=%.2f | cur=%d° prior=%d° -> eff=%d° (%s)",
              scores.score[0], scores.score[1], scores.score[2], scores.score[3],
              dec.prev_rot, orient_ctrl.Prior(), dec.new_rot,
              dec.reason.c_str());
          if (dec.switched) {
            ui.auto_rotation_prior = orient_ctrl.Prior();
            kg::Settings ps;
            ps.Load(settings_path);
            ps.camera_rotation = ui.camera_rotation;
            ps.auto_rotation_prior = ui.auto_rotation_prior;
            ps.Save(settings_path);
          }
        }
      }

      const int eff_rot = orient_ctrl.EffectiveRotation(ui.camera_rotation);
      ui.effective_rotation = eff_rot;
      if (eff_rot != prev_effective_rot) {
        kgcore::Log("orient", "rotasi efektif: %d° -> %d° (mode=%s)",
                    prev_effective_rot, eff_rot,
                    ui.camera_rotation < 0 ? "Otomatis" : "Manual");
        prev_effective_rot = eff_rot;
        if (pipeline) pipeline->ResetTracking();
        last_camera_raw_valid = false;
        preview_show_blend = false;
      }

      if (eff_rot == 0) {
        frame = std::move(raw_frame);
      } else if (!kg::RotateFrameCW(raw_frame, eff_rot, &frame)) {
        frame = std::move(raw_frame);
      }
      last_frame_ts_ms = frame.timestamp_ms;
      if (!frame_is_synthetic) {
        // M2h butuh 4 & M2p: simpan frame mentah TERORIENTASI terakhir.
        last_camera_raw = frame;
        last_camera_raw_valid = true;
        last_raw_luma = ComputeMeanLumaFrame(frame);
      }
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

    // M2l ROOT FIX: JANGAN reset preview_show_blend = false di setiap iterasi
    // render! Kamera (~30 fps) lebih jarang dari loop render (~85+ iter/s);
    // mereset preview_show_blend pada iterasi !have_frame membuat ImGui
    // menggambar preview.tex yang 0x0 (hitam) -> kedipan 67-87% di M2k.
    int out_w = 0, out_h = 0;  // dims of THIS iteration's pipeline output
    const bool want_raw = ui.raw_preview;  // F4: tampilkan kamera mentah
    bool pipeline_frame_ok = false;
    const bool blend_mode_allowed =
        ui.skin_only && !want_raw && pipeline_ok && !force_no_mask_gl &&
        !blender.disabled && !blender.init_failed;
    if (!blend_mode_allowed) {
      preview_show_blend = false;
      if (!have_frame && preview.w == 0) {
        if (!want_raw && pipeline_ok && pipeline && pipeline->OutputRGBA() &&
            pipeline->OutputWidth() > 0 && pipeline->OutputHeight() > 0) {
          const uint8_t* prev_out = pipeline->OutputRGBA();
          const int pw = pipeline->OutputWidth(), ph = pipeline->OutputHeight();
          preview.Upload(prev_out, pw, ph, ui.mirror, /*src_is_bgra=*/false);
          last_nonblend_out_luma = ComputeMeanLumaRgba(prev_out, pw, ph);
          shown_w = pw;
          shown_h = ph;
        } else if (last_camera_raw_valid) {
          preview.Upload(last_camera_raw.pixels.data(), last_camera_raw.width,
                         last_camera_raw.height, ui.mirror,
                         /*src_is_bgra=*/last_camera_raw.format == kg::kFormatBGRA);
          last_nonblend_out_luma = last_raw_luma;
          shown_w = last_camera_raw.width;
          shown_h = last_camera_raw.height;
        }
      }
    }
    if (have_frame && pipeline_ok && !want_raw) {
      // Live-apply the two global beauty parameters (cheap; idempotent).
      pipeline->SetBeautyStrength(ui.smoothing, ui.whitening);
      const bool mask_gl_ready = ui.skin_only && blender.Init();
      pipeline->SetSkinMaskEnabled(ui.skin_only);  // M2h: mask hanya bila aktif
      pipeline->SetGpuMaskMode(mask_gl_ready && !force_no_mask_gl &&
                               !blender.disabled && !gpu_mask_gen_disabled);
      pipeline->SetSkinCoverage(perf_guard.ForceFaceOnly() ||
                                (ui.skin_only && ui.skin_coverage == 1));
      if (!bench_running) {
        pipeline->Seg().SetPeriodMs(perf_guard.SegPeriodMs());
      }
      pipeline->ProcessFrame(frame);
      if (!frame_is_synthetic) {
        orient_ctrl.NoteFrameResult(pipeline->Tracker().CurrentRaw().valid,
                                    pipeline->Tracker().CurrentRaw().roi_reset);
      }
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
        // M2i/M2l: `--no-mask-gl` / swa-uji gagal (blender.disabled) -> jalur
        // blend dimatikan; `--no-mask-gen-gpu` / tahap-1 -> mask CPU M2j.
        if (ui.skin_only && out_w == frame.width && out_h == frame.height &&
            !blender.init_failed && !force_no_mask_gl && !blender.disabled) {
          const kgcore::BeautyComposeConfig bcfg = MakeComposeConfig(
              ui.skin_only, ui.smoothing, ui.whitening, ui.chroma_retain);
          // M2m (3a) & M2p: kontur mask memakai landmark mentah frame ini,
          // atau landmark baik terakhir yang dibekukan saat hold/fade-out
          // sehingga eksklusi mata/alis/bibir/hidung tidak pernah jatuh.
          const auto& raw_lm = pipeline->Tracker().CurrentRaw();
          const auto& mask_lm = pipeline->Tracker().MaskLandmarks();
          std::vector<float> mask_landmarks;
          if (mask_lm.valid) {
            mask_landmarks.resize(kgcore::kLandmarkCount * 2);
            double diff_sum = 0.0;
            for (size_t i = 0; i < kgcore::kLandmarkCount; ++i) {
              mask_landmarks[i * 2] = mask_lm.pts[i].x * frame.width;
              mask_landmarks[i * 2 + 1] = mask_lm.pts[i].y * frame.height;
              if (raw_lm.valid) {
                diff_sum += std::hypot(
                    (mask_lm.pts[i].x - raw_lm.pts[i].x) * frame.width,
                    (mask_lm.pts[i].y - raw_lm.pts[i].y) * frame.height);
              }
            }
            last_mask_feat_diff_px =
                static_cast<float>(diff_sum / kgcore::kLandmarkCount);
          }
          blended = blender.Blend(bcfg, pipeline->InputRGBA(), frame.width,
                                  frame.height, out, mb.mask().data(), mb.w(),
                                  mb.h(), pipeline->Tracker().Gain(),
                                  ui.show_mask,
                                  mask_landmarks.empty() ? nullptr : mask_landmarks.data(),
                                  static_cast<int>(mask_landmarks.size() / 2),
                                  mb.face_probability().empty() ? nullptr
                                                                : mb.face_probability().data(),
                                  pipeline->SegActive(), mb.skin_cb0(),
                                  mb.skin_cr0(), mb.has_color_calibration(),
                                  &mb.config(), !gpu_mask_gen_disabled,
                                  pipeline->SegAttenuation(),
                                  mb.hair_probability().empty() ? nullptr
                                                                : mb.hair_probability().data(),
                                  mb.skin_luma0(), mb.has_luma_calibration(),
                                  pipeline->SegAttenuation(),
                                  pipeline->Tracker().IsMaskFrozen());
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
          preview_show_blend = true;  // digambar dari tex_out blender
          ++s_blend_frames;
          if (!blender.ProbeOutputLuma(&last_blend_out_luma)) {
            last_blend_out_luma = 0.0f;
          }
          if (!s_blend_logged_first) {
            s_blend_logged_first = true;
            kgcore::Log("blend",
                        "aktif pertama: tex_out=%u %dx%d has_stats=%d gpu_mask=%d",
                        blender.tex_out, blender.out_w, blender.out_h,
                        blender.has_stats ? 1 : 0,
                        gpu_mask_gen_disabled ? 0 : 1);
          }
          // M2i (1) swa-uji awal: setelah ~30 frame, luma keluaran vs mentah.
          if (!s_selftest_done && s_blend_frames >= 30) {
            s_selftest_done = true;
            float out_y = 0.0f;
            double raw_sum = 0.0;
            long raw_n = 0;
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
                preview_show_blend = false;
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
          last_nonblend_out_luma = ComputeMeanLumaRgba(out, out_w, out_h);
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
      if (!frame_is_synthetic && pipeline->Tracker().CurrentRaw().valid) {
        win_oneeuro_mean_px.push_back(pipeline->Tracker().LastFilterLagMeanPx());
        win_oneeuro_p95_px.push_back(pipeline->Tracker().LastFilterLagP95Px());
        win_mask_feat_diff_px.push_back(last_mask_feat_diff_px);
      }
    } else if (!have_frame && preview_show_blend && ui.show_mask != prev_show_mask &&
               blender.ready && pipeline) {
      // F6 toggled di antara dua frame kamera: segarkan tex_out_debug segera.
      if (ui.show_mask) blender.UpdateDebugTexture(pipeline->Tracker().Gain());
      else blender.debug_ready = false;
    }
    prev_show_mask = ui.show_mask;
    if (have_frame && (want_raw || !pipeline_frame_ok)) {
      preview.Upload(frame.pixels.data(), frame.width, frame.height, ui.mirror,
                     /*src_is_bgra=*/frame.format == kg::kFormatBGRA);
      last_nonblend_out_luma = ComputeMeanLumaFrame(frame);
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
    if (ui.compare_held) {
      if (raw) {
        preview.Upload(raw, frame.width, frame.height, ui.mirror,
                       /*src_is_bgra=*/frame.format == kg::kFormatBGRA);
        shown_w = frame.width;
        shown_h = frame.height;
        if (!frame_is_synthetic) ++uploaded_raw;
      } else if (!compare_uploaded_nonframe && last_camera_raw_valid) {
        preview.Upload(last_camera_raw.pixels.data(), last_camera_raw.width,
                       last_camera_raw.height, ui.mirror,
                       /*src_is_bgra=*/last_camera_raw.format == kg::kFormatBGRA);
        shown_w = last_camera_raw.width;
        shown_h = last_camera_raw.height;
        compare_uploaded_nonframe = true;
      }
    } else {
      compare_uploaded_nonframe = false;
    }
    // M2e requirement 1: glGetError after every preview upload operation.
    drain_gl_errors();

    // M2e/M2l (F5): dump PNG + metrik bersih.
    if (ui.debug_frame_requests != debug_req_seen) {
      debug_req_seen = ui.debug_frame_requests;
      DumpDebugFrames(paths.appdata_dir,
                      last_camera_raw_valid ? &last_camera_raw : nullptr,
                      pipeline.get(), &blender, preview_show_blend,
                      pipeline_ok, ui.skin_only, ui.smoothing,
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

    auto now = clock_type::now();
    float dt = std::chrono::duration<float>(now - last_frame_time).count();
    last_frame_time = now;
    if (dt > 0.0f) {
      float fps = 1.0f / dt;
      fps_smooth = fps_smooth == 0.0f ? fps : fps_smooth * 0.95f + fps * 0.05f;
      ui.fps = fps_smooth;
    }
    const double perf_now_ms = std::chrono::duration<double, std::milli>(
                                   now.time_since_epoch()).count();
    if (ui.request_seg_bench) {
      ui.request_seg_bench = false;
      if (!safe && pipeline_ok && pipeline && !bench_running) {
        bench_running = true;
        bench_idx = 0;
        bench_results.clear();
        bench_cfg_applied = false;
        ui.seg_bench_active = true;
        ui.seg_bench_status = "Benchmark seg dimulai (12 konfig x 10 dtk)...";
        kgcore::Log("bench", "benchmark seg dimulai dari UI (12 konfig x 10 dtk)");
      }
    }
    if (bench_running && pipeline_ok && pipeline) {
      const int64_t now_ms_b = static_cast<int64_t>(perf_now_ms);
      if (!bench_cfg_applied) {
        const auto& cfg = bench_matrix[bench_idx];
        pipeline->Seg().ConfigureSession(cfg.intra_op, cfg.allow_spinning);
        pipeline->Seg().SetBenchmarkPeriodMs(cfg.period_ms);
        bench_step_start_ms = now_ms_b;
        bench_step_cam_recv0 = capture.GetStats().received;
        bench_step_sent0 = sent_to_pipeline;
        bench_step_pipe0 = pipe_out;
        bench_fps_samples.clear();
        bench_c2s_samples.clear();
        bench_seg_ms_sum = 0.0;
        bench_seg_lag_sum = 0.0;
        bench_seg_n = 0;
        bench_cfg_applied = true;
        char sbuf[160];
        std::snprintf(sbuf, sizeof(sbuf),
                      "Benchmark %zu/%zu: intra=%d spin=%d period=%dms",
                      bench_idx + 1, bench_matrix.size(), cfg.intra_op,
                      cfg.allow_spinning ? 1 : 0, cfg.period_ms);
        ui.seg_bench_status = sbuf;
        kgcore::Log("bench", "mulai #%zu/%zu: intra_op=%d allow_spinning=%d period=%dms",
                    bench_idx + 1, bench_matrix.size(), cfg.intra_op,
                    cfg.allow_spinning ? 1 : 0, cfg.period_ms);
      } else {
        const int64_t elapsed_b = now_ms_b - bench_step_start_ms;
        // 1 dtk warmup setelah rebuild session; 9 dtk pengukuran
        if (elapsed_b >= 1000) {
          if (ui.fps > 0.0f) bench_fps_samples.push_back(ui.fps);
          if (pipeline->SegActive()) {
            bench_seg_ms_sum += pipeline->SegMs();
            bench_seg_lag_sum += std::max(0.0f, pipeline->SegLagFrames());
            ++bench_seg_n;
          }
        } else {
          bench_step_cam_recv0 = capture.GetStats().received;
          bench_step_sent0 = sent_to_pipeline;
          bench_step_pipe0 = pipe_out;
          bench_c2s_samples.clear();
        }
        if (elapsed_b >= 10000) {
          const double dt_s = std::max(1.0, static_cast<double>(elapsed_b - 1000) / 1000.0);
          const kg::CameraStats cst_b = capture.GetStats();
          const uint64_t recv_d = cst_b.received - bench_step_cam_recv0;
          const uint64_t sent_d = sent_to_pipeline - bench_step_sent0;
          const uint64_t pipe_d = pipe_out - bench_step_pipe0;
          auto mean_vec = [](const std::vector<float>& v) {
            if (v.empty()) return 0.0;
            double s = 0.0;
            for (float x : v) s += x;
            return s / v.size();
          };
          auto pct_vec = [](std::vector<float> v, float q) {
            if (v.empty()) return 0.0;
            std::sort(v.begin(), v.end());
            const size_t idx = static_cast<size_t>(
                std::clamp(q, 0.0f, 1.0f) * static_cast<float>(v.size() - 1));
            return static_cast<double>(v[idx]);
          };
          kgcore::SegBenchResult res;
          res.cfg = bench_matrix[bench_idx];
          res.recv_fps = static_cast<double>(recv_d) / dt_s;
          res.proc_fps = static_cast<double>(pipe_d) / dt_s;
          res.cam2screen_med_ms = pct_vec(bench_c2s_samples, 0.50f);
          res.cam2screen_p95_ms = pct_vec(bench_c2s_samples, 0.95f);
          res.seg_infer_avg_ms = bench_seg_n > 0 ? (bench_seg_ms_sum / bench_seg_n) : 0.0;
          res.seg_samples = bench_seg_n;
          res.meets_criteria = kgcore::EvaluateSegBenchCriteria(res);
          bench_results.push_back(res);
          const double drop_pct = recv_d > 0
              ? (1.0 - static_cast<double>(std::min(sent_d, recv_d)) / recv_d) * 100.0
              : 0.0;
          kgcore::Log("bench",
                      "selesai #%zu/%zu: intra=%d spin=%d period=%dms | render=%.1f fps "
                      "cam=%.1f/s proc=%.1f/s drop=%.1f%% c2s_med=%.1fms c2s_p95=%.1fms "
                      "seg=%.1fms (n=%d) | pass=%d",
                      bench_idx + 1, bench_matrix.size(), res.cfg.intra_op,
                      res.cfg.allow_spinning ? 1 : 0, res.cfg.period_ms,
                      mean_vec(bench_fps_samples), res.recv_fps, res.proc_fps,
                      drop_pct, res.cam2screen_med_ms, res.cam2screen_p95_ms,
                      res.seg_infer_avg_ms, res.seg_samples,
                      res.meets_criteria ? 1 : 0);
          ++bench_idx;
          bench_cfg_applied = false;
          if (bench_idx >= bench_matrix.size()) {
            const int best_i = kgcore::SelectBestSegBenchConfig(bench_results);
            const auto& win_cfg = bench_results[static_cast<size_t>(best_i)].cfg;
            pipeline->Seg().ConfigureSession(win_cfg.intra_op, win_cfg.allow_spinning);
            pipeline->Seg().SetBenchmarkPeriodMs(0);  // kembali ke adaptif
            bench_running = false;
            ui.seg_bench_active = false;
            char done_buf[200];
            std::snprintf(done_buf, sizeof(done_buf),
                          "Selesai: pemenang #%d intra=%d spin=%d period=%dms (pass=%d)",
                          best_i + 1, win_cfg.intra_op,
                          win_cfg.allow_spinning ? 1 : 0, win_cfg.period_ms,
                          bench_results[static_cast<size_t>(best_i)].meets_criteria ? 1 : 0);
            ui.seg_bench_status = done_buf;
            kgcore::Log("bench", "PEMENANG MATRIKS: %s", done_buf);
          }
        }
      }
    }
    if (!bench_running) {
      perf_guard.Update(perf_now_ms, ui.fps);
    }
    if (perf_guard.JustChanged()) {
      const std::string notice = perf_guard.TakeNotice();
      kgcore::Log("perf", "%s | state=%d seg_period=%dms fps=%.1f",
                  notice.c_str(), static_cast<int>(perf_guard.state()),
                  perf_guard.SegPeriodMs(), ui.fps);
      ui.notice = notice;
      ui.notice_is_error = false;
    }
    if (pipeline) ui.landmark_available = pipeline->Tracker().ProviderAvailable();

    // M2l: diagnosis per-iterasi render + swa-uji berkelanjutan frame hitam.
    const bool showing_blend_frame =
        preview_show_blend && !want_raw && !ui.compare_held &&
        shown_w > 0 && shown_h > 0;
    const float iter_out_luma =
        showing_blend_frame
            ? ((blender.ready && blender.DisplayTex(ui.show_mask) != 0 &&
                blender.out_w > 0 && blender.out_h > 0)
                   ? last_blend_out_luma
                   : 0.0f)
            : ((preview.w > 0 && preview.h > 0 && preview.tex != 0)
                   ? last_nonblend_out_luma
                   : 0.0f);
    const bool guard_active =
        !safe && ui.skin_only && !want_raw && !ui.compare_held &&
        !frame_is_synthetic && pipeline_ok && !force_no_mask_gl &&
        !blender.disabled && last_camera_raw_valid;
    black_guard.RecordSample(perf_now_ms, last_raw_luma, iter_out_luma,
                             guard_active);
    if (guard_active && black_guard.ShouldFallback(perf_now_ms)) {
      const double ratio_pct = black_guard.WindowDarkRatio(perf_now_ms) * 100.0;
      if (!gpu_mask_gen_disabled) {
        gpu_mask_gen_disabled = true;
        black_guard.ResetWindow();
        kgcore::Log("blend",
                    "SWA-UJI BERKELANJUTAN GAGAL: frame gelap %.1f%% (>5%% selama 1 dtk) -> fallback ke mask CPU M2j",
                    ratio_pct);
        ui.notice =
            "Kedipan hitam terdeteksi pada mask GPU - beralih otomatis ke mask CPU.";
        ui.notice_is_error = true;
      } else {
        blender.disabled = true;
        preview_show_blend = false;
        black_guard.ResetWindow();
        kgcore::Log("blend",
                    "SWA-UJI BERKELANJUTAN GAGAL: frame gelap %.1f%% (>5%% selama 1 dtk) -> fallback ke jalur seluruh-frame",
                    ratio_pct);
        ui.notice =
            "Preview blend GL hitam berulang - beralih otomatis ke seluruh frame.";
        ui.notice_is_error = true;
      }
    }

    // M2e/M2l: diagnosis ringkas tiap detik (semua tahap + frame gelap blend).
    const int64_t diag_now_ms = duration_cast<std::chrono::milliseconds>(
                                    now.time_since_epoch())
                                    .count();
    if (diag_now_ms - diag_last_ms >= 1000) {
      diag_last_ms = diag_now_ms;
      const auto bsum = black_guard.TakeSecondSummary(perf_now_ms);
      if (!safe) {
        const kg::CameraStats cst = capture.GetStats();
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
              if (ui.notice == kg::Strings::Instance().Get("camera.black_frames"))
                ui.notice.clear();
            }
            dark_streak = 0;
            dark_hint_shown = false;
          }
        }
        auto mean_of = [](const std::vector<float>& v) {
          if (v.empty()) return 0.0f;
          double s = 0.0;
          for (float x : v) s += x;
          return static_cast<float>(s / v.size());
        };
        auto pct_of = [](std::vector<float> v, float q) {
          if (v.empty()) return 0.0f;
          std::sort(v.begin(), v.end());
          const size_t idx = static_cast<size_t>(
              std::clamp(q, 0.0f, 1.0f) * static_cast<float>(v.size() - 1));
          return v[idx];
        };
        const float oe_mean = mean_of(win_oneeuro_mean_px);
        const float oe_p95 = pct_of(win_oneeuro_p95_px, 0.95f);
        const float mask_diff = mean_of(win_mask_feat_diff_px);
        const float c2s_med = pct_of(win_cam2screen_ms, 0.50f);
        const float c2s_p95 = pct_of(win_cam2screen_ms, 0.95f);
        win_oneeuro_mean_px.clear();
        win_oneeuro_p95_px.clear();
        win_mask_feat_diff_px.clear();
        win_cam2screen_ms.clear();
        kg::StageTimings diag_st{};
        if (pipeline) pipeline->GetStageTimings(&diag_st);
        const float seg_age_fr = pipeline ? pipeline->SegLagFrames() : -1.0f;
        const float seg_ms_cur = pipeline ? pipeline->SegMs() : 0.0f;
        const size_t qlen = capture.QueueSize();
        const double neg_fps = (cst.fps_num > 0 && cst.fps_den > 0)
            ? static_cast<double>(cst.fps_num) / static_cast<double>(cst.fps_den)
            : 0.0;
        if (neg_fps > 0.0 && cst.actual_fps > 0.0 && cst.actual_fps < 0.75 * neg_fps) {
          if (++mf_underdeliver_streak >= 3 && !mf_low_light_warned) {
            mf_low_light_warned = true;
            kgcore::Log("diag",
                        "PERINGATAN kamera MF di bawah FPS negosiasi (eksposur rendah / USB?): "
                        "actual=%.1f fps ts=%.1f fps vs negosiasi=%.1f fps (luma=%.1f)",
                        cst.actual_fps, cst.mf_ts_fps, neg_fps, cst.mean_luma);
          }
        } else {
          mf_underdeliver_streak = 0;
          mf_low_light_warned = false;
        }

        kgcore::Log(
            "diag",
            "MF: recv=%llu %dx%d stride=%d subtype=0x%08X fps=%d/%d actual_fps=%.1f ts_fps=%.1f luma=%.1f alpha=%d | "
            "pipeline: sent=%llu out=%llu err=%llu | upload: out=%llu raw=%llu | "
            "glErr(last=0x%X total=%u) tex=%u %dx%d RGBA8 | ctx=%p | camRunning=%d pipelineOK=%d | "
            "blend=%u %dx%d%s gpu_mask=%d guided=%d | blend_dark=%d/%d (win=%.1f%%) rawY=%.1f outY=%.1f | "
            "lag: oneeuro(mean=%.2f p95=%.2f px) mask_feat_diff=%.2f px seg_age=%.0f fr (%.1fms period=%dms intra=%d spin=%d) "
            "cam2screen(med=%.1f p95=%.1f ms) qlen=%zu rot=%ddeg(%s) roi_resets=%d (60s=%d/m) hold=%d/%dfr fade_out=%.0fms state=%s",
            static_cast<unsigned long long>(cst.received), cst.width, cst.height, cst.stride,
            static_cast<unsigned>(cst.subtype), cst.fps_num, cst.fps_den,
            cst.actual_fps, cst.mf_ts_fps, cst.mean_luma,
            cst.center_alpha, static_cast<unsigned long long>(sent_to_pipeline),
            static_cast<unsigned long long>(pipe_out),
            static_cast<unsigned long long>(pipe_err_count),
            static_cast<unsigned long long>(uploaded_out),
            static_cast<unsigned long long>(uploaded_raw), static_cast<unsigned>(last_gl_err),
            static_cast<unsigned>(gl_err_count), preview.tex, preview.w, preview.h,
            static_cast<void*>(glfwGetCurrentContext()), capture.Running() ? 1 : 0,
            pipeline_ok ? 1 : 0, blender.tex_out, blender.out_w, blender.out_h,
            blender.disabled ? " (OFF)" : "", gpu_mask_gen_disabled ? 0 : 1,
            blender.last_mask_guided ? 1 : 0,
            bsum.dark, bsum.total, bsum.window_dark_ratio * 100.0,
            last_raw_luma, iter_out_luma,
            oe_mean, oe_p95, mask_diff, seg_age_fr, seg_ms_cur,
            pipeline ? pipeline->Seg().CurrentPeriodMs() : 0,
            pipeline ? pipeline->Seg().IntraOpThreads() : 0,
            (pipeline && pipeline->Seg().AllowSpinning()) ? 1 : 0,
            c2s_med, c2s_p95, qlen, ui.effective_rotation,
            ui.camera_rotation < 0 ? "auto" : "manual",
            diag_st.reset_count, diag_st.resets_in_last_min,
            diag_st.hold_frames, diag_st.total_hold_frames,
            diag_st.last_fade_out_ms,
            kgcore::TrackingPhaseName(diag_st.tracking_phase));
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
        if (showing_blend_frame) {
          ImGui::Image(static_cast<ImTextureID>(
                           static_cast<uintptr_t>(blender.DisplayTex(ui.show_mask))),
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
                      "sejak deteksi %d | landmark age %.1f fr | frame #%llu",
                      ui.fps, ui.capture_ms, ui.detect_ms, ui.mesh_ms, ui.track_ms,
                      ui.mask_ms, seg_part, ui.render_ms, ui.frames_since_detect,
                      (lm.valid && last_frame_ts_ms >= lm.ts_ms)
                          ? static_cast<float>(last_frame_ts_ms - lm.ts_ms) / 33.333f
                          : -1.0f,
                      static_cast<unsigned long long>(ui.frames_captured));
        const ImVec2 hud_pos(image_pos.x + 8.0f, image_pos.y + 8.0f);
        const ImVec2 text_size = ImGui::CalcTextSize(hud);
        dl->AddRectFilled(hud_pos,
                          ImVec2(hud_pos.x + text_size.x + 12.0f, hud_pos.y + text_size.y + 8.0f),
                          IM_COL32(0, 0, 0, 160));
        dl->AddText(ImVec2(hud_pos.x + 6.0f, hud_pos.y + 4.0f), IM_COL32_WHITE, hud);
      }

      // F6: gambar kontur pengecualian GPU (mata, batas alis atas/bawah,
      // bibir luar, lubang hidung) di atas preview; titik berasal dari frame
      // landmark yang sama dengan rasterizer mask.
      if (ui.show_mask && pipeline_ok && pipeline && image_size.x > 1.0f &&
          image_size.y > 1.0f) {
        const auto& lm = pipeline->Tracker().MaskLandmarks();
        if (lm.valid) {
          std::vector<float> xy(kgcore::kLandmarkCount * 2);
          for (size_t i = 0; i < kgcore::kLandmarkCount; ++i) {
            xy[i * 2] = lm.pts[i].x * shown_w;
            xy[i * 2 + 1] = lm.pts[i].y * shown_h;
          }
          const auto base_shapes = kgcore::BuildExclusionShapes(xy.data());
          const auto basis = kgcore::ComputeFaceEyeBasis(xy.data());
          kgcore::ExclusionShapes shapes = base_shapes;
          if (pipeline->InputRGBA() && pipeline->MaskBuilder().has_luma_calibration()) {
            kgcore::RefineNostrilEllipsesFromLuma(
                base_shapes, pipeline->InputRGBA(), shown_w, shown_h,
                shown_w * 4, pipeline->MaskBuilder().skin_luma0(), basis.iod,
                &shapes, nullptr, xy.data());
          }
          ImDrawList* dl = ImGui::GetWindowDrawList();
          const ImU32 col = IM_COL32(50, 255, 230, 245);
          auto point_screen = [&](float x, float y) {
            const float nx = x / shown_w, ny = y / shown_h;
            return ImVec2(image_pos.x + (ui.mirror ? (1.0f - nx) : nx) * image_size.x,
                          image_pos.y + ny * image_size.y);
          };
          for (const auto& loop : shapes.loops) {
            for (size_t i = 0; i < loop.size(); ++i) {
              const int a = loop[i], b = loop[(i + 1) % loop.size()];
              dl->AddLine(point_screen(xy[a * 2], xy[a * 2 + 1]),
                          point_screen(xy[b * 2], xy[b * 2 + 1]), col, 1.5f);
            }
          }
          for (const auto& e : shapes.ellipses) {
            ImVec2 prev = point_screen(e.cx + e.rx, e.cy);
            for (int i = 1; i <= 24; ++i) {
              const float a = 6.2831853f * i / 24.0f;
              ImVec2 next = point_screen(e.cx + e.rx * std::cos(a),
                                         e.cy + e.ry * std::sin(a));
              dl->AddLine(prev, next, col, 1.5f);
              prev = next;
            }
          }
        }
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
    const auto present_done = clock_type::now();
    if (pending_cam_ts_ms > 0 && !frame_is_synthetic) {
      const int64_t present_ms =
          duration_cast<std::chrono::milliseconds>(present_done.time_since_epoch())
              .count();
      const float c2s = static_cast<float>(
          std::max<int64_t>(0, present_ms - pending_cam_ts_ms));
      win_cam2screen_ms.push_back(c2s);
      if (bench_running && bench_cfg_applied) {
        bench_c2s_samples.push_back(c2s);
      }
      pending_cam_ts_ms = -1;
    }
    ui.render_ms =
        duration_cast<std::chrono::microseconds>(present_done - render_start).count() /
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
