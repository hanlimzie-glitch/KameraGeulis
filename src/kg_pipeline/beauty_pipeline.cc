#include "beauty_pipeline.h"
#include <gpupixel/gpupixel.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include "kgcore/frame_convert.h"
#include "kgcore/log.h"
#include "kgcore/reader_fallback.h"
#include "ort_landmark_provider.h"

namespace kgcore {
constexpr char kTag[] = "pipeline";

BeautyPipeline::BeautyPipeline()
    : tracker_(std::make_unique<FaceTracker>(std::make_unique<OrtLandmarkProvider>())) {}

BeautyPipeline::~BeautyPipeline() { Shutdown(); }

bool BeautyPipeline::Init(const std::string& resource_path) {
  if (initialized_) return true;
  gpupixel::GPUPixel::SetResourcePath(resource_path);

  // Pre-verify every lookup texture the GPUPixel filters load (see
  // beauty_face_unit_filter.cc in the pinned gpupixel ref). GPUPixel's
  // SourceImage::Create returns nullptr for a missing file (asserts are
  // compiled out in Release) and its Init() still reports success — the
  // first frame then crashes on a virtual call with a null source image.
  // Missing resources must disable the pipeline with a clear log/UI
  // message, never crash.
  const char* required[] = {"lookup_gray.png", "lookup_origin.png", "lookup_skin.png",
                            "lookup_light.png"};
  bool all_present = true;
  for (const char* name : required) {
    const std::string path = resource_path + "res/" + name;
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
      Log(kTag, "resource HILANG: res/%s (dicari di %s)", name, path.c_str());
      all_present = false;
      continue;
    }
    const long long size = static_cast<long long>(f.tellg());
    Log(kTag, "resource res/%s: OK (%lld B)", name, size);
  }
  if (!all_present) {
    last_resource_error_ =
        "resource gpupixel di 'res/' tidak lengkap - efek GPU dinonaktifkan (lihat log)";
    Log(kTag, "%s", last_resource_error_.c_str());
    return false;
  }

  beauty_filter_ = gpupixel::BeautyFaceFilter::Create();
  if (!beauty_filter_) {
    Log(kTag, "BeautyFaceFilter::Create failed (filter/shader init) - efek GPU dinonaktifkan");
    last_resource_error_ = "BeautyFaceFilter::Create gagal - lihat log (shader/GLib?)";
    return false;
  }
  sink_ = gpupixel::SinkRawData::Create();
  source_ = gpupixel::SourceRawData::Create();
  if (!source_ || !sink_) {
    Log(kTag, "SourceRawData/SinkRawData::Create failed");
    return false;
  }
  beauty_filter_->SetBlurAlpha(0.35f);
  beauty_filter_->SetWhite(0.125f);
  source_->AddSink(beauty_filter_)->AddSink(sink_);
  if (!source_ || !sink_) {  // belt-and-braces (checked above too)
    last_resource_error_ = "SourceRawData/SinkRawData null setelah Create";
    return false;
  }
  initialized_ = true;
  Log(kTag, "pipeline initialized (landmark provider: %s)", tracker_->ProviderName());
  return true;
}

void BeautyPipeline::Shutdown() {
  // Release in reverse add order; GPUPixel objects are shared_ptr-based.
  sink_.reset();
  beauty_filter_.reset();
  source_.reset();
  initialized_ = false;
  frames_produced_ = 0;
  readback_blocked_logged_ = false;
}

void BeautyPipeline::SetBeautyStrength(float smoothing, float whitening) {
  smoothing_ = smoothing;
  whitening_ = whitening;
  if (!beauty_filter_) return;
  if (skin_mask_enabled_) {
    // M2j kualitas (F5): jalur LUT kulit = penguat HF (ratio 1.326) dan
    // lift putih gpupixel terlalu kuat (+19.5 luma @22%). Pada mode kulit:
    //   * sharpen/LUT putih DIMATIKAN -> smoothing dijalankan sbg guided
    //     smoothing murni (radius dari slider Haluskan) di mask_blender;
    //   * Cerahkan = lift proporsional: dv = k*(255-v), k = 0.9*whitening
    //     (22% -> +6..8 di inti terang, 100% -> ~+30) - lihat mask_blender.
    beauty_filter_->SetSharpen(0.0f);
    beauty_filter_->SetBlurAlpha(0.0f);  // koefisien rumus sharpen = 0
    beauty_filter_->SetWhite(0.0f);
    // Lift Cerahan + guided smoothing TIDAK di sini: keduanya masuk lewat
    // BeautyComposeConfig (MakeComposeConfig di main.cc — GPU & CPU identik).
  } else {
    beauty_filter_->SetBlurAlpha(smoothing);
    beauty_filter_->SetWhite(whitening);
  }
}

void BeautyPipeline::ProcessFrame(const Frame& frame) {
  if (!initialized_) return;
  tracker_->Track(frame);  // ORT provider consumes the original frame format
  const int w = frame.width;
  const int h = frame.height;
  if (w <= 0 || h <= 0 || frame.pixels.empty()) return;
  const size_t row_bytes = static_cast<size_t>(w) * 4;
  const size_t src_stride =
      frame.stride_bytes >= static_cast<int>(row_bytes)
          ? static_cast<size_t>(frame.stride_bytes)
          : row_bytes;
  if (frame.pixels.size() < src_stride * static_cast<size_t>(h)) {
    Log(kTag, "ProcessFrame: buffer terlalu kecil (%zu < %zu) - frame dilewati",
        frame.pixels.size(), src_stride * static_cast<size_t>(h));
    return;
  }
  // M2e: always hand gpupixel a TOP-DOWN RGBA buffer with alpha=255.
  // Pinned gpupixel (SourceRawData::GenerateTextureWithPixels) wraps its
  // BGRA glTexImage2D in #if GPUPIXEL_IOS||GPUPIXEL_MAC -> on Windows the
  // BGRA upload is a NO-OP and the whole chain renders an empty texture
  // (black preview). The RGBA branch is unguarded and works everywhere.
  convert_buf_.resize(row_bytes * static_cast<size_t>(h));
  ConvertToRgbaTopDown(frame.pixels.data(), w, h, src_stride,
                       /*flip_vertical=*/false,
                       /*src_is_bgra=*/frame.format != kFormatRGBA,
                       convert_buf_.data());
  source_->ProcessData(convert_buf_.data(), w, h, static_cast<int>(row_bytes),
                       gpupixel::GPUPIXEL_FRAME_TYPE_RGBA);
  // M2h: mask kulit dari landmark + warna frame ini (resolusi rendah, CPU
  // murni - lihat kgcore/skin_mask.h). Tidak dihitung bila checkbox mati.
  mask_ms_ = 0.0f;
  if (skin_mask_enabled_) {
    const int mw = std::max(8, w / 4);
    const int mh = std::max(8, h / 4);
    if (!mask_builder_.configured() || mask_builder_.w() != mw ||
        mask_builder_.h() != mh) {
      mask_builder_.Configure(mw, mh);
      Log(kTag, "skin mask: dikonfigurasi %dx%d (frame %dx%d)", mw, mh, w, h);
    }
    // M2j: segmen semua-kulit di thread sendiri (tidak memblokir render).
    // Hasil dipakai SYNCHRONOUS dgn serial frame yang sedang diproses (bila
    // seg lebih lambat, lag-nya dicatat dalam satuan frame - jangan bohong).
    ++frame_serial_;
    if (!seg_start_logged_) {
      seg_.Start();
      seg_start_logged_ = true;
      Log(kTag, "seg thread: mulai (target 10-15 Hz, ORT CPU)");
    }
    seg_.SubmitFrame(convert_buf_.data(), w, h, static_cast<int>(row_bytes),
                     /*bgra=*/false, frame_serial_);
    // Buffer hasil hidup SAMPAI Update() (seg_in.letterbox = &seg_lb).
    std::vector<float> seg_probs;
    LetterboxInfo seg_lb{};
    uint64_t seg_serial = 0;
    double seg_ms_val = 0.0;
    SegMaskInput seg_in;
    const SegMaskInput* seg_ptr = nullptr;
    if (seg_.Available() &&
        seg_.GetResult(&seg_probs, &seg_lb, &seg_serial, &seg_ms_val)) {
      seg_in.probs = seg_probs.data();
      seg_in.letterbox = &seg_lb;
      seg_in.frame_serial = static_cast<int64_t>(seg_serial);
      seg_in.valid = true;
      seg_ptr = &seg_in;
      seg_ms_ = static_cast<float>(seg_ms_val);
      const int64_t lag = static_cast<int64_t>(frame_serial_) -
                          static_cast<int64_t>(seg_serial);
      seg_lag_frames_ = static_cast<float>(std::clamp<int64_t>(lag, 0, 60));
      seg_result_ok_ = true;
    } else if (seg_.Available()) {
      seg_result_ok_ = false;
      seg_lag_frames_ = -1.0f;
    } else {
      // Gagal/ belum siap -> WAJAH SAJA + notifikasi sekali (jujur, no crash).
      seg_result_ok_ = false;
      seg_lag_frames_ = -1.0f;
      if (!seg_fail_logged_) {
        seg_fail_logged_ = true;
        Log(kTag, "seg tidak tersedia (%s) -> mask wajah saja",
            seg_.LastError().c_str());
      }
    }
    const auto t0 = std::chrono::steady_clock::now();
    mask_builder_.Update(tracker_->Current(), tracker_->Current().valid,
                         convert_buf_.data(), w, h, static_cast<int>(row_bytes),
                         seg_ptr);
    mask_ms_ = std::chrono::duration<float, std::milli>(
                   std::chrono::steady_clock::now() - t0)
                   .count();
    // Log latensi ujung-ke-ujung berkala (mesh/landmark, seg, mask) - M2j.
    static thread_local uint64_t s_lag_log_n = 0;
    if ((frame_serial_ % 90) == 0) {
      StageTimings st{};
      const bool has_st = GetStageTimings(&st);
      Log(kTag,
          "lag frame serial=%llu: mesh=%.1fms (~%.1ffr@30) seg=%.1fms "
          "(lag %.0f fr) mask=%.2fms%s",
          static_cast<unsigned long long>(frame_serial_),
          has_st ? st.mesh_ms : -1.0,
          has_st ? st.mesh_ms / 33.3f : -1.0f, seg_ms_, seg_lag_frames_,
          mask_ms_, seg_result_ok_ ? "" : " [seg mati -> wajah saja]");
    }
    (void)s_lag_log_n;
  }
  // M2f: sink mengisi width_/height_ bersamaan dengan framebuffer pertamanya
  // (SinkRawData::Render) - sink berarti sudah pernah menghasilkan output.
  if (sink_ && sink_->GetWidth() > 0 && sink_->GetHeight() > 0) {
    ++frames_produced_;
  }
}

const uint8_t* BeautyPipeline::OutputRGBA() {
  if (!sink_) return nullptr;
  const int w = sink_->GetWidth();
  const int h = sink_->GetHeight();
  // M2f guard: GetRgbaBuffer() me-run glReadPixels di framebuffer yang hanya
  // dibuat pada Render() pertama - sebelum ada frame output, framebuffer_
  // null dan readback crash (bukti F5 di PC pengguna: 0xC0000005 di
  // gpupixel.dll). Ditolak tanpa menyentuh gpupixel sama sekali.
  if (!SinkReadbackAllowed(w, h, frames_produced_)) {
    if (!readback_blocked_logged_) {
      Log(kTag, "readback dilewati: tidak ada frame keluaran (sink %dx%d, out=%llu)",
          w, h, static_cast<unsigned long long>(frames_produced_));
      readback_blocked_logged_ = true;
    }
    return nullptr;
  }
  readback_blocked_logged_ = false;
  // GetRgbaBuffer() runs glReadPixels on gpupixel's own GL context and
  // returns CPU memory - the app never touches gpupixel texture ids (M2e
  // requirement 4: official readback path, plain readback; PBO later).
  const uint8_t* src = sink_->GetRgbaBuffer();
  if (!src || w <= 0 || h <= 0) return nullptr;
  out_buf_.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
  // Copy + force alpha 255 in one place. Sink buffer is treated as top-down
  // (gpupixel quads map t=0 -> framebuffer bottom, glReadPixels starts at the
  // bottom, so the CPU buffer comes back in input order). Orientation is
  // UNVERIFIED on real hardware - flip_vertical=true here is the one-line
  // fix if the user reports an upside-down output (see STATUS.md).
  ConvertToRgbaTopDown(src, w, h, static_cast<size_t>(w) * 4,
                       /*flip_vertical=*/false, /*src_is_bgra=*/false,
                       out_buf_.data());
  return out_buf_.data();
}
int BeautyPipeline::OutputWidth() const { return sink_ ? sink_->GetWidth() : 0; }
int BeautyPipeline::OutputHeight() const { return sink_ ? sink_->GetHeight() : 0; }

bool BeautyPipeline::GetStageTimings(StageTimings* out) const {
  if (!out || !tracker_) return false;
  bool ok = false;
  if (tracker_->Provider()) ok = tracker_->Provider()->GetStageTimings(out);
  else *out = StageTimings{};
  out->track_ms = tracker_->LastTrackMs();
  out->mask_ms = mask_ms_;
  return true;
}
}  // namespace kgcore
