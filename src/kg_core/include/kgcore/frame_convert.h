#pragma once
#include <cstddef>
#include <cstdint>

namespace kgcore {
// Pure CPU frame helpers - no GL/OS dependencies, unit-tested on Linux.
//
// Convention: destination is always tightly packed (stride = width*4),
// top-down (row 0 = visual top), RGBA with alpha FORCED to 255. Camera
// frames (MF RGB32 = BGRA, possibly bottom-up, possibly padded stride) and
// GPUPixel readback (RGBA, top-down) both go through here before display.

// Convert one 4-bytes-per-pixel source frame into `dst` (width*height*4).
//   src_stride  - source row stride in bytes (>= width*4); use abs value.
//   flip_vertical - true when memory row 0 is the visual BOTTOM row
//                   (classic DIB / negative MF stride).
//   src_is_bgra - true for MF RGB32 (byte order B,G,R,A), false for RGBA.
// Alpha in `dst` is always 255: camera RGB32 leaves the 4th byte undefined
// (often 0), which would make the frame invisible when blended.
void ConvertToRgbaTopDown(const uint8_t* src, int width, int height,
                          size_t src_stride, bool flip_vertical,
                          bool src_is_bgra, uint8_t* dst);

struct FrameStats {
  double mean_luma = 0.0;   // 0..255, 0.299R+0.587G+0.114B over a sample grid
  uint8_t center_alpha = 0; // alpha byte of the center pixel (BGRA or RGBA)
  int sampled = 0;          // pixels sampled for mean_luma
};

// Sampled diagnostics for the per-second log line. Reads the source frame
// with the same layout rules as ConvertToRgbaTopDown (stride/flip/format).
FrameStats ComputeFrameStats(const uint8_t* src, int width, int height,
                             size_t src_stride, bool flip_vertical,
                             bool src_is_bgra);
}  // namespace kgcore
