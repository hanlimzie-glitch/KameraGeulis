#include "kgcore/virtual_camera.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace kgcore {
namespace {

inline uint8_t ClampByte(int v) {
  return static_cast<uint8_t>(std::clamp(v, 0, 255));
}

// Sampel RGBA bilinear dengan clamp ke batas gambar sumber.
inline void SampleBilinearRgba(const uint8_t* src, int src_w, int src_h,
                               int src_stride, float sx, float sy,
                               uint8_t* out_r, uint8_t* out_g,
                               uint8_t* out_b) {
  const float fx = std::clamp(sx, 0.0f, static_cast<float>(src_w - 1));
  const float fy = std::clamp(sy, 0.0f, static_cast<float>(src_h - 1));
  const int x0 = static_cast<int>(fx);
  const int y0 = static_cast<int>(fy);
  const int x1 = std::min(x0 + 1, src_w - 1);
  const int y1 = std::min(y0 + 1, src_h - 1);
  const float tx = fx - static_cast<float>(x0);
  const float ty = fy - static_cast<float>(y0);
  const float itx = 1.0f - tx;
  const float ity = 1.0f - ty;

  const uint8_t* r0 = src + static_cast<size_t>(y0) * src_stride;
  const uint8_t* r1 = src + static_cast<size_t>(y1) * src_stride;
  const uint8_t* p00 = r0 + static_cast<size_t>(x0) * 4;
  const uint8_t* p10 = r0 + static_cast<size_t>(x1) * 4;
  const uint8_t* p01 = r1 + static_cast<size_t>(x0) * 4;
  const uint8_t* p11 = r1 + static_cast<size_t>(x1) * 4;

  const float w00 = itx * ity;
  const float w10 = tx * ity;
  const float w01 = itx * ty;
  const float w11 = tx * ty;

  *out_r = ClampByte(static_cast<int>(std::lround(
      p00[0] * w00 + p10[0] * w10 + p01[0] * w01 + p11[0] * w11)));
  *out_g = ClampByte(static_cast<int>(std::lround(
      p00[1] * w00 + p10[1] * w10 + p01[1] * w01 + p11[1] * w11)));
  *out_b = ClampByte(static_cast<int>(std::lround(
      p00[2] * w00 + p10[2] * w10 + p01[2] * w01 + p11[2] * w11)));
}

inline void RgbToYuvBt601(uint8_t r, uint8_t g, uint8_t b, uint8_t* y_out,
                           uint8_t* u_out, uint8_t* v_out) {
  const int ri = r, gi = g, bi = b;
  const int y = ((66 * ri + 129 * gi + 25 * bi + 128) >> 8) + 16;
  const int u = ((-38 * ri - 74 * gi + 112 * bi + 128) >> 8) + 128;
  const int v = ((112 * ri - 94 * gi - 18 * bi + 128) >> 8) + 128;
  *y_out = ClampByte(y);
  *u_out = ClampByte(u);
  *v_out = ClampByte(v);
}

}  // namespace

void NormalizeVcamConfig(VcamConfig* cfg) {
  if (!cfg) return;
  cfg->width = std::clamp(cfg->width, 16, 4096);
  cfg->height = std::clamp(cfg->height, 16, 4096);
  cfg->width = std::max(16, (cfg->width / 4) * 4);
  cfg->height = std::max(16, (cfg->height / 4) * 4);
  cfg->fps = std::clamp(cfg->fps, 1, 60);
}

size_t VcamOutputByteSize(VcamPixelFormat fmt, int width, int height) {
  if (width <= 0 || height <= 0) return 0;
  const size_t px = static_cast<size_t>(width) * static_cast<size_t>(height);
  switch (fmt) {
    case VcamPixelFormat::kBgr24TopDown:
      return px * 3;
    case VcamPixelFormat::kBgra32TopDown:
      return px * 4;
    case VcamPixelFormat::kNv12TopDown:
      if ((width % 2) != 0 || (height % 2) != 0) return 0;
      return px + px / 2;
  }
  return 0;
}

VcamRect ComputeLetterboxRect(int src_w, int src_h, int dst_w, int dst_h) {
  VcamRect r{};
  if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return r;
  const int64_t lhs = static_cast<int64_t>(src_w) * dst_h;
  const int64_t rhs = static_cast<int64_t>(src_h) * dst_w;
  if (lhs == rhs) {
    return {0, 0, dst_w, dst_h};
  }
  if (lhs > rhs) {
    // Sumber lebih lebar -> pas selebar dst_w, bar hitam atas/bawah.
    r.w = dst_w;
    r.h = std::max(1, static_cast<int>(std::lround(
                          static_cast<double>(dst_w) * src_h / src_w)));
    r.h = std::min(r.h, dst_h);
    r.x = 0;
    r.y = (dst_h - r.h) / 2;
  } else {
    // Sumber lebih tinggi (mis. rotasi 90° potret -> 1280x720) -> bar kiri/kanan.
    r.h = dst_h;
    r.w = std::max(1, static_cast<int>(std::lround(
                          static_cast<double>(dst_h) * src_w / src_h)));
    r.w = std::min(r.w, dst_w);
    r.x = (dst_w - r.w) / 2;
    r.y = 0;
  }
  return r;
}

bool ConvertRgbaToVcamFormat(const uint8_t* src_rgba, int src_w, int src_h,
                             int src_stride_bytes, int dst_w, int dst_h,
                             VcamPixelFormat dst_fmt, bool flip_vertical,
                             bool mirror_horizontal,
                             std::vector<uint8_t>* out_buf) {
  if (!src_rgba || !out_buf || src_w <= 0 || src_h <= 0 || dst_w <= 0 ||
      dst_h <= 0 || src_stride_bytes < src_w * 4) {
    return false;
  }
  const size_t need = VcamOutputByteSize(dst_fmt, dst_w, dst_h);
  if (need == 0) return false;
  out_buf->resize(need);

  // Jalur cepat 1:1 (tanpa resize) untuk BGR24/BGRA32.
  if (src_w == dst_w && src_h == dst_h &&
      dst_fmt != VcamPixelFormat::kNv12TopDown) {
    for (int y = 0; y < dst_h; ++y) {
      const int sy = flip_vertical ? (src_h - 1 - y) : y;
      const uint8_t* srow = src_rgba + static_cast<size_t>(sy) * src_stride_bytes;
      if (dst_fmt == VcamPixelFormat::kBgr24TopDown) {
        uint8_t* drow = out_buf->data() + static_cast<size_t>(y) * dst_w * 3;
        if (!mirror_horizontal) {
          for (int x = 0; x < dst_w; ++x) {
            const uint8_t* p = srow + static_cast<size_t>(x) * 4;
            drow[x * 3 + 0] = p[2];
            drow[x * 3 + 1] = p[1];
            drow[x * 3 + 2] = p[0];
          }
        } else {
          for (int x = 0; x < dst_w; ++x) {
            const int sx = dst_w - 1 - x;
            const uint8_t* p = srow + static_cast<size_t>(sx) * 4;
            drow[x * 3 + 0] = p[2];
            drow[x * 3 + 1] = p[1];
            drow[x * 3 + 2] = p[0];
          }
        }
      } else {  // kBgra32TopDown
        uint8_t* drow = out_buf->data() + static_cast<size_t>(y) * dst_w * 4;
        for (int x = 0; x < dst_w; ++x) {
          const int sx = mirror_horizontal ? (dst_w - 1 - x) : x;
          const uint8_t* p = srow + static_cast<size_t>(sx) * 4;
          drow[x * 4 + 0] = p[2];
          drow[x * 4 + 1] = p[1];
          drow[x * 4 + 2] = p[0];
          drow[x * 4 + 3] = 255;
        }
      }
    }
    return true;
  }

  // Jalur umum dengan aspect-preserving letterbox + bilinear scaling.
  const VcamRect box = ComputeLetterboxRect(src_w, src_h, dst_w, dst_h);
  const float scale_x = static_cast<float>(src_w) / static_cast<float>(box.w);
  const float scale_y = static_cast<float>(src_h) / static_cast<float>(box.h);

  if (dst_fmt == VcamPixelFormat::kNv12TopDown) {
    uint8_t* y_plane = out_buf->data();
    uint8_t* uv_plane = y_plane + static_cast<size_t>(dst_w) * dst_h;
    std::memset(y_plane, 16, static_cast<size_t>(dst_w) * dst_h);
    std::memset(uv_plane, 128, static_cast<size_t>(dst_w) * dst_h / 2);

    // Hitung Y untuk semua baris; akumulasi UV 2x2 hanya di area box.
    for (int y = 0; y < dst_h; y += 2) {
      for (int sub = 0; sub < 2; ++sub) {
        const int dy = y + sub;
        if (dy < box.y || dy >= box.y + box.h) continue;
        float sy = (static_cast<float>(dy - box.y) + 0.5f) * scale_y - 0.5f;
        if (flip_vertical) sy = static_cast<float>(src_h - 1) - sy;
        uint8_t* y_row = y_plane + static_cast<size_t>(dy) * dst_w;
        for (int dx = box.x; dx < box.x + box.w; ++dx) {
          float sx = (static_cast<float>(dx - box.x) + 0.5f) * scale_x - 0.5f;
          if (mirror_horizontal) sx = static_cast<float>(src_w - 1) - sx;
          uint8_t r = 0, g = 0, b = 0;
          SampleBilinearRgba(src_rgba, src_w, src_h, src_stride_bytes, sx, sy,
                             &r, &g, &b);
          uint8_t yy = 16, uu = 128, vv = 128;
          RgbToYuvBt601(r, g, b, &yy, &uu, &vv);
          y_row[dx] = yy;
          if (sub == 0 && (dx % 2) == 0) {
            uv_plane[static_cast<size_t>(y / 2) * dst_w + dx] = uu;
            uv_plane[static_cast<size_t>(y / 2) * dst_w + dx + 1] = vv;
          }
        }
      }
    }
    return true;
  }

  // BGR24 atau BGRA32 dengan resize/letterbox.
  if (dst_fmt == VcamPixelFormat::kBgr24TopDown) {
    std::fill(out_buf->begin(), out_buf->end(), 0);
  } else {
    for (size_t i = 0; i < need; i += 4) {
      (*out_buf)[i + 0] = 0;
      (*out_buf)[i + 1] = 0;
      (*out_buf)[i + 2] = 0;
      (*out_buf)[i + 3] = 255;
    }
  }
  const int bpp = (dst_fmt == VcamPixelFormat::kBgr24TopDown) ? 3 : 4;
  for (int dy = box.y; dy < box.y + box.h; ++dy) {
    float sy = (static_cast<float>(dy - box.y) + 0.5f) * scale_y - 0.5f;
    if (flip_vertical) sy = static_cast<float>(src_h - 1) - sy;
    uint8_t* drow = out_buf->data() + static_cast<size_t>(dy) * dst_w * bpp;
    for (int dx = box.x; dx < box.x + box.w; ++dx) {
      float sx = (static_cast<float>(dx - box.x) + 0.5f) * scale_x - 0.5f;
      if (mirror_horizontal) sx = static_cast<float>(src_w - 1) - sx;
      uint8_t r = 0, g = 0, b = 0;
      SampleBilinearRgba(src_rgba, src_w, src_h, src_stride_bytes, sx, sy, &r,
                         &g, &b);
      uint8_t* p = drow + static_cast<size_t>(dx) * bpp;
      p[0] = b;
      p[1] = g;
      p[2] = r;
      if (bpp == 4) p[3] = 255;
    }
  }
  return true;
}

VcamFramePacer::VcamFramePacer(int target_fps) { Reset(target_fps); }

void VcamFramePacer::Reset(int target_fps) {
  target_fps_ = std::clamp(target_fps, 1, 60);
  interval_us_ = 1000000LL / target_fps_;
  has_pending_ = false;
  pending_rgba_.clear();
  pending_w_ = pending_h_ = 0;
  pending_capture_ts_us_ = 0;
  has_active_ = false;
  active_rgba_.clear();
  active_w_ = active_h_ = 0;
  active_capture_ts_us_ = 0;
  schedule_started_ = false;
  next_due_us_ = 0;
  last_pts_us_ = -1;
  frames_sent_ = 0;
  frames_repeated_ = 0;
  frames_dropped_ = 0;
}

bool VcamFramePacer::SubmitFrame(const uint8_t* rgba, int w, int h,
                                 int64_t capture_ts_us) {
  if (!rgba || w <= 0 || h <= 0) return false;
  const bool dropped_old = has_pending_;
  if (dropped_old) {
    ++frames_dropped_;
  }
  const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
  pending_rgba_.assign(rgba, rgba + bytes);
  pending_w_ = w;
  pending_h_ = h;
  pending_capture_ts_us_ = capture_ts_us;
  has_pending_ = true;
  return dropped_old;
}

bool VcamFramePacer::PollFrame(int64_t now_us, std::vector<uint8_t>* out_rgba,
                               int* out_w, int* out_h, int64_t* out_pts_us,
                               int64_t* out_capture_ts_us,
                               bool* out_is_repeat) {
  if (!has_pending_ && !has_active_) return false;
  if (!schedule_started_) {
    schedule_started_ = true;
    next_due_us_ = now_us;
  }
  if (now_us < next_due_us_) return false;

  bool is_repeat = false;
  if (has_pending_) {
    active_rgba_.swap(pending_rgba_);
    active_w_ = pending_w_;
    active_h_ = pending_h_;
    active_capture_ts_us_ = pending_capture_ts_us_;
    has_pending_ = false;
    has_active_ = true;
  } else {
    is_repeat = true;
    ++frames_repeated_;
  }

  const int64_t pts_us = std::max(now_us, last_pts_us_ + 1);
  last_pts_us_ = pts_us;
  ++frames_sent_;

  if (now_us - next_due_us_ > 2 * interval_us_) {
    next_due_us_ = now_us + interval_us_;
  } else {
    next_due_us_ += interval_us_;
  }

  if (out_rgba) *out_rgba = active_rgba_;
  if (out_w) *out_w = active_w_;
  if (out_h) *out_h = active_h_;
  if (out_pts_us) *out_pts_us = pts_us;
  if (out_capture_ts_us) *out_capture_ts_us = active_capture_ts_us_;
  if (out_is_repeat) *out_is_repeat = is_repeat;
  return true;
}

VcamStatusInfo ComputeVcamStatus(bool registered, bool open, int consumer_count,
                                 bool has_error) {
  VcamStatusInfo info{};
  info.consumer_count = consumer_count;
  if (!registered) {
    info.state = VcamDriverState::kNotRegistered;
    info.status_key = "vcam.status_not_registered";
    info.consumer_count = -1;
    return info;
  }
  if (!open || has_error) {
    info.state = VcamDriverState::kRegisteredIdle;
    info.status_key = "vcam.status_registered_idle";
    return info;
  }
  info.state = VcamDriverState::kStreaming;
  info.status_key = "vcam.status_streaming";
  return info;
}

}  // namespace kgcore
