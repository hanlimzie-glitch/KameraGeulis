#include "kgcore/frame_convert.h"
#include <cmath>
#include <cstring>

namespace kgcore {

int NormalizeRotationSetting(int deg) {
  if (deg < 0) return -1;
  return NormalizeEffectiveRotation(deg);
}

int NormalizeEffectiveRotation(int deg) {
  int r = deg % 360;
  if (r < 0) r += 360;
  if (r == 90 || r == 180 || r == 270) return r;
  return 0;
}

bool RotateFrameCW(const Frame& src, int rot_deg_cw, Frame* dst) {
  if (!dst || !src.IsValid()) return false;
  if (dst == &src) {
    Frame tmp = src;
    return RotateFrameCW(tmp, rot_deg_cw, dst);
  }
  const int rot = NormalizeEffectiveRotation(rot_deg_cw);
  const int sw = src.width;
  const int sh = src.height;
  const size_t src_row_bytes = static_cast<size_t>(sw) * 4;
  const size_t src_stride = src.stride_bytes >= static_cast<int>(src_row_bytes)
                                ? static_cast<size_t>(src.stride_bytes)
                                : src_row_bytes;
  if (src.pixels.size() < src_stride * static_cast<size_t>(sh)) return false;

  const int dw = (rot == 90 || rot == 270) ? sh : sw;
  const int dh = (rot == 90 || rot == 270) ? sw : sh;
  dst->width = dw;
  dst->height = dh;
  dst->stride_bytes = dw * 4;
  dst->format = src.format;
  dst->timestamp_ms = src.timestamp_ms;
  dst->pixels.resize(static_cast<size_t>(dw) * dh * 4);

  const uint8_t* sbase = src.pixels.data();
  uint8_t* dbase = dst->pixels.data();

  if (rot == 0) {
    if (src_stride == src_row_bytes) {
      std::memcpy(dbase, sbase, src_row_bytes * static_cast<size_t>(sh));
    } else {
      for (int y = 0; y < sh; ++y) {
        std::memcpy(dbase + static_cast<size_t>(y) * src_row_bytes,
                    sbase + static_cast<size_t>(y) * src_stride, src_row_bytes);
      }
    }
    return true;
  }

  if (rot == 180) {
    for (int dy = 0; dy < dh; ++dy) {
      const uint32_t* srow = reinterpret_cast<const uint32_t*>(
          sbase + static_cast<size_t>(sh - 1 - dy) * src_stride);
      uint32_t* drow = reinterpret_cast<uint32_t*>(
          dbase + static_cast<size_t>(dy) * dw * 4);
      for (int dx = 0; dx < dw; ++dx) {
        drow[dx] = srow[sw - 1 - dx];
      }
    }
    return true;
  }

  if (rot == 90) {
    for (int dy = 0; dy < dh; ++dy) {
      uint32_t* drow = reinterpret_cast<uint32_t*>(
          dbase + static_cast<size_t>(dy) * dw * 4);
      const int sx = dy;
      for (int dx = 0; dx < dw; ++dx) {
        const int sy = sh - 1 - dx;
        const uint32_t* srow = reinterpret_cast<const uint32_t*>(
            sbase + static_cast<size_t>(sy) * src_stride);
        drow[dx] = srow[sx];
      }
    }
    return true;
  }

  // rot == 270
  for (int dy = 0; dy < dh; ++dy) {
    uint32_t* drow = reinterpret_cast<uint32_t*>(
        dbase + static_cast<size_t>(dy) * dw * 4);
    const int sx = sw - 1 - dy;
    for (int dx = 0; dx < dw; ++dx) {
      const int sy = dx;
      const uint32_t* srow = reinterpret_cast<const uint32_t*>(
          sbase + static_cast<size_t>(sy) * src_stride);
      drow[dx] = srow[sx];
    }
  }
  return true;
}

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
