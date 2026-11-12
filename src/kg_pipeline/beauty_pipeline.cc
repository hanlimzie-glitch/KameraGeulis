#include "beauty_pipeline.h"
#include <gpupixel/gpupixel.h>
#include "kgcore/log.h"

namespace kgcore {
constexpr char kTag[] = "pipeline";

BeautyPipeline::BeautyPipeline()
    : tracker_(std::make_unique<FaceTracker>(std::make_unique<NullLandmarkProvider>())) {}

BeautyPipeline::~BeautyPipeline() { Shutdown(); }

bool BeautyPipeline::Init(const std::string& resource_path) {
  if (initialized_) return true;
  gpupixel::GPUPixel::SetResourcePath(resource_path);

  beauty_filter_ = gpupixel::BeautyFaceFilter::Create();
  if (!beauty_filter_) {
    Log(kTag, "BeautyFaceFilter::Create failed");
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
}  // namespace kgcore
