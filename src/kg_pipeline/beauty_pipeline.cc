#include "beauty_pipeline.h"
#include <gpupixel/gpupixel.h>
#include <fstream>
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
  tracker_->Track(frame);
  const auto type = frame.format == kFormatRGBA
                        ? gpupixel::GPUPIXEL_FRAME_TYPE_RGBA
                        : gpupixel::GPUPIXEL_FRAME_TYPE_BGRA;
  source_->ProcessData(frame.pixels.data(), frame.width, frame.height,
                       frame.stride_bytes, type);
}

const uint8_t* BeautyPipeline::OutputRGBA() const {
  return sink_ ? sink_->GetRgbaBuffer() : nullptr;
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
