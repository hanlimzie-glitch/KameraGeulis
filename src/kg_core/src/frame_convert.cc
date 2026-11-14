#include "kgcore/frame_convert.h"
#include <cmath>

namespace kgcore {

void ConvertToRgbaTopDown(const uint8_t* src, int width, int height,
                          size_t src_stride, bool flip_vertical,
                          bool src_is_bgra, uint8_t* dst) {
  if (!src || !dst || width <= 0 || height <= 0) return;
  const size_t row_bytes = static_cast<size_t>(width) * 4;
  if (src_stride < row_bytes) src_stride = row_bytes;
  for (int y = 0; y < height; ++y) {
    const int src_y = flip_vertical ? (height - 1 - y) : y;
    const uint8_t* in = src + static_cast<size_t>(src_y) * src_stride;
    uint8_t* out = dst + static_cast<size_t>(y) * row_bytes;
    if (src_is_bgra) {
      for (size_t x = 0; x < row_bytes; x += 4) {
        out[x + 0] = in[x + 2];  // R
        out[x + 1] = in[x + 1];  // G
        out[x + 2] = in[x + 0];  // B
        out[x + 3] = 255;        // A: camera RGB32 padding byte is undefined
      }
    } else {
      for (size_t x = 0; x < row_bytes; x += 4) {
        out[x + 0] = in[x + 0];
        out[x + 1] = in[x + 1];
        out[x + 2] = in[x + 2];
        out[x + 3] = 255;  // force opaque: alpha 0 makes ImGui blend to black
      }
    }
  }
}

FrameStats ComputeFrameStats(const uint8_t* src, int width, int height,
                             size_t src_stride, bool flip_vertical,
                             bool src_is_bgra) {
  FrameStats st;
  if (!src || width <= 0 || height <= 0) return st;
  const size_t row_bytes = static_cast<size_t>(width) * 4;
  if (src_stride < row_bytes) src_stride = row_bytes;

  double sum = 0.0;
  constexpr int kStep = 8;  // sample every 8th pixel in each axis
  for (int y = 0; y < height; y += kStep) {
    const int src_y = flip_vertical ? (height - 1 - y) : y;
    const uint8_t* row = src + static_cast<size_t>(src_y) * src_stride;
    for (int x = 0; x < width; x += kStep) {
      const uint8_t* px = row + static_cast<size_t>(x) * 4;
      const double r = src_is_bgra ? px[2] : px[0];
      const double g = px[1];
      const double b = src_is_bgra ? px[0] : px[2];
      sum += 0.299 * r + 0.587 * g + 0.114 * b;
      ++st.sampled;
    }
  }
  if (st.sampled > 0) st.mean_luma = sum / st.sampled;

  const int cx = width / 2;
  const int cy = height / 2;
  const int src_cy = flip_vertical ? (height - 1 - cy) : cy;
  const uint8_t* center =
      src + static_cast<size_t>(src_cy) * src_stride + static_cast<size_t>(cx) * 4;
  st.center_alpha = center[3];
  return st;
}
}  // namespace kgcore
