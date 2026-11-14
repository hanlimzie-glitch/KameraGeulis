#include "softcam_sender.h"
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include "kgcore/frame.h"
#include "kgcore/log.h"

namespace kgcore {
constexpr char kTag[] = "vcam";

SoftcamSender::~SoftcamSender() { Close(); }

void SoftcamSender::SetError(const char* fmt, ...) {
  char buf[512];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  last_error_ = buf;
  Log(kTag, "%s", buf);
}

bool SoftcamSender::Open(int width, int height, float framerate,
                         const std::string& dll_directory) {
  if (camera_) return true;
  if (width <= 0 || height <= 0 || width > 16384 || height > 16384 ||
      width % 4 != 0 || height % 4 != 0) {
    SetError("vcam size %dx%d unsupported (must be 4..16384, multiples of 4)",
             width, height);
    return false;
  }

  std::string path = dll_directory.empty() ? "softcam.dll" : dll_directory + "\\softcam.dll";
  dll_ = reinterpret_cast<void*>(LoadLibraryW(std::wstring(path.begin(), path.end()).c_str()));
  if (!dll_) {
    SetError("softcam.dll not found (searched '%s'). The virtual camera "
             "requires the softcam driver; see README (one-time admin "
             "registration).", path.c_str());
    return false;
  }
  fn_create_ = reinterpret_cast<ScCreateCamera>(GetProcAddress(reinterpret_cast<HMODULE>(dll_), "scCreateCamera"));
  fn_delete_ = reinterpret_cast<ScDeleteCamera>(GetProcAddress(reinterpret_cast<HMODULE>(dll_), "scDeleteCamera"));
  fn_send_ = reinterpret_cast<ScSendFrame>(GetProcAddress(reinterpret_cast<HMODULE>(dll_), "scSendFrame"));
  fn_is_connected_ = reinterpret_cast<ScIsConnected>(GetProcAddress(reinterpret_cast<HMODULE>(dll_), "scIsConnected"));
  if (!fn_create_ || !fn_delete_ || !fn_send_ || !fn_is_connected_) {
    SetError("softcam.dll loaded but exports missing (wrong/partial build)");
    Close();
    return false;
  }

  camera_ = fn_create_(width, height, framerate);
  if (!camera_) {
    SetError("scCreateCamera failed — another KameraGeulis instance may own "
             "the virtual camera, or the driver is not registered in the "
             "registry (needs the one-time admin step).");
    Close();
    return false;
  }
  width_ = width;
  height_ = height;
  bgr_buffer_.resize(static_cast<size_t>(width) * height * 3);
  Log(kTag, "virtual camera open: %dx%d @ %.1f fps", width, height, framerate);
  return true;
}

void SoftcamSender::Close() {
  if (camera_ && fn_delete_) {
    fn_delete_(camera_);
  }
  camera_ = nullptr;
  fn_create_ = nullptr;
  fn_delete_ = nullptr;
  fn_send_ = nullptr;
  fn_is_connected_ = nullptr;
  if (dll_) {
    FreeLibrary(reinterpret_cast<HMODULE>(dll_));
    dll_ = nullptr;
  }
  width_ = height_ = 0;
}

bool SoftcamSender::SendBgr24(const uint8_t* bgr_top_down, size_t bytes) {
  if (!camera_ || !fn_send_) return false;
  if (bytes != bgr_buffer_.size()) {
    SetError("BGR frame size mismatch (%zu vs %zu)", bytes, bgr_buffer_.size());
    return false;
  }
  fn_send_(camera_, bgr_top_down);
  return true;
}

bool SoftcamSender::SendFromFramePixels(const uint8_t* pixels, int width, int height,
                                        int stride_bytes, int source_format_bgra) {
  if (!camera_) return false;
  if (width != width_ || height != height_) return false;
  for (int y = 0; y < height; ++y) {
    const uint8_t* src = pixels + static_cast<size_t>(y) * stride_bytes;
    uint8_t* dst = bgr_buffer_.data() + static_cast<size_t>(y) * width * 3;
    for (int x = 0; x < width; ++x) {
      if (source_format_bgra) {  // B,G,R,A -> B,G,R
        dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
      } else {                   // R,G,B,A -> B,G,R
        dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0];
      }
      src += 4;
      dst += 3;
    }
  }
  return SendBgr24(bgr_buffer_.data(), bgr_buffer_.size());
}

bool SoftcamSender::ConsumerConnected() const {
  if (!camera_ || !fn_is_connected_) return false;
  return fn_is_connected_(camera_);
}
}  // namespace kgcore
