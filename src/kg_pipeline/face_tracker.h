#pragma once
#include <array>
#include <memory>
#include <string>
#include <vector>
#include "kgcore/frame.h"
#include "kgcore/landmark.h"
#include "kgcore/one_euro.h"

namespace kgcore {
// Abstraction over face-landmark providers (see docs/ARCHITECTURE.md §5).
// Implementations: NullLandmarkProvider (model missing / unavailable),
// OrtLandmarkProvider (M1b — ONNX Runtime CPU, 468 pts).
class ILandmarkProvider {
 public:
  virtual ~ILandmarkProvider() = default;
  virtual bool Available() const = 0;
  virtual const char* Name() const = 0;
  // Returns a LandmarkSet; valid==false when no face or model unavailable.
  virtual LandmarkSet Track(const Frame& frame) = 0;
};

class NullLandmarkProvider : public ILandmarkProvider {
 public:
  bool Available() const override { return false; }
  const char* Name() const override { return "none"; }
  LandmarkSet Track(const Frame&) override { return {}; }
};

// Wraps a provider with OneEuro smoothing + fade gain. Thread-confined to the
// render thread (no internal locking).
class FaceTracker {
 public:
  explicit FaceTracker(std::unique_ptr<ILandmarkProvider> provider);

  void Track(const Frame& frame);
  const LandmarkSet& Current() const { return current_; }
  float Gain() const { return fade_.Gain(); }  // smooth [0,1] for effect fade
  bool ProviderAvailable() const { return provider_->Available(); }
  const char* ProviderName() const { return provider_->Name(); }

 private:
  std::unique_ptr<ILandmarkProvider> provider_;
  std::array<OneEuroVec2, kLandmarkCount> smoothers_;
  LandmarkFade fade_{300.0f};
  LandmarkSet current_;
};
}  // namespace kgcore
