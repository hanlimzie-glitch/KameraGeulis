#include "beauty_pipeline.h"
#include <gpupixel/gpupixel.h>
#include <fstream>
#include "kgcore/frame_convert.h"
#include "kgcore/log.h"
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
}

void BeautyPipeline::SetBeautyStrength(float smoothing, float whitening) {
  if (!beauty_filter_) return;
  beauty_filter_->SetBlurAlpha(smoothing);
  beauty_filter_->SetWhite(whitening);
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
}

const uint8_t* BeautyPipeline::OutputRGBA() {
  if (!sink_) return nullptr;
  // GetRgbaBuffer() runs glReadPixels on gpupixel's own GL context and
  // returns CPU memory - the app never touches gpupixel texture ids (M2e
  // requirement 4: official readback path, plain readback; PBO later).
  const uint8_t* src = sink_->GetRgbaBuffer();
  const int w = sink_->GetWidth();
  const int h = sink_->GetHeight();
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
  return true;
}
}  // namespace kgcore
