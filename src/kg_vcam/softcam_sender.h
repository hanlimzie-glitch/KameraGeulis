#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace kgcore {
// DirectShow virtual camera sender backed by softcam (MIT, tshino) — loaded
// at RUNTIME via LoadLibrary. If softcam.dll is missing next to the exe, or
// the virtual camera was never registered (one-time admin step), the sender
// reports Unavailable and the app keeps running without it.
//
// Frame contract: 24-bit packed BGR, rows top-down (as softcam expects).
// Dimensions must be positive, <=16384 and multiples of 4 (softcam hard
// requirement); otherwise Open() fails with a clear message.
class SoftcamSender {
 public:
  SoftcamSender() = default;
  ~SoftcamSender();
  SoftcamSender(const SoftcamSender&) = delete;
  SoftcamSender& operator=(const SoftcamSender&) = delete;

  // Loads softcam.dll (next to the executable, or an explicit directory) and
  // creates the virtual camera. Returns false + LastError() on failure.
  bool Open(int width, int height, float framerate,
            const std::string& dll_directory = "");
  void Close();

  bool IsOpen() const { return camera_ != nullptr; }
  bool SendBgr24(const uint8_t* bgr_top_down, size_t bytes);
  // Convenience: convert BGRA/RGBA (rows top-down) -> BGR24 and send.
  bool SendFromFramePixels(const uint8_t* pixels, int width, int height,
                           int stride_bytes, int source_format_bgra);
  bool ConsumerConnected() const;

  int Width() const { return width_; }
  int Height() const { return height_; }
  std::string LastError() const { return last_error_; }

 private:
  void SetError(const char* fmt, ...);

  void* dll_ = nullptr;                    // HMODULE
  void* camera_ = nullptr;                 // scCamera
  int width_ = 0, height_ = 0;
  std::string last_error_;
  std::vector<uint8_t> bgr_buffer_;

  using ScCreateCamera = void* (__cdecl*)(int, int, float);
  using ScDeleteCamera = void (__cdecl*)(void*);
  using ScSendFrame = void (__cdecl*)(void*, const void*);
  using ScIsConnected = bool (__cdecl*)(void*);
  ScCreateCamera fn_create_ = nullptr;
  ScDeleteCamera fn_delete_ = nullptr;
  ScSendFrame fn_send_ = nullptr;
  ScIsConnected fn_is_connected_ = nullptr;
};
}  // namespace kgcore
