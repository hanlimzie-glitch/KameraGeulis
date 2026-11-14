#pragma once
#include <cstdint>
#include <vector>

namespace kgcore {
// Pixel formats for Frame::pixels.
enum FrameFormat : int {
  kFormatBGRA = 0,  // 4 bytes/px, matches Media Foundation RGB32/ARGB32 and softcam source conversion
  kFormatRGBA = 1,  // 4 bytes/px, GPUPixel readback output
};

struct Frame {
  std::vector<uint8_t> pixels;  // packed rows, row stride = stride_bytes
  int width = 0;
  int height = 0;
  int stride_bytes = 0;
  int format = kFormatBGRA;
  int64_t timestamp_ms = 0;    // chrono steady_clock-based monotonic ms

  bool IsValid() const {
    return width > 0 && height > 0 && stride_bytes >= width * 4 &&
           pixels.size() >= static_cast<size_t>(stride_bytes) * height;
  }
};
}  // namespace kgcore
