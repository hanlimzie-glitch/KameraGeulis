#pragma once
#include <string>
#include "kgcore/virtual_camera.h"
#include "softcam_sender.h"

namespace kgcore {

// Implementasi IVirtualCameraSink berbasis DirectShow (tshino/softcam).
// Memeriksa ketersediaan softcam.dll + registrasi CLSID di Windows Registry
// secara read-only (tanpa hak admin).
class SoftcamSink final : public IVirtualCameraSink {
 public:
  explicit SoftcamSink(std::string dll_directory = "");
  ~SoftcamSink() override;

  const char* BackendName() const override { return "DirectShow (softcam)"; }
  bool IsRegistered(std::string* detail_out = nullptr) const override;
  bool Open(int width, int height, float fps,
            std::string* err_out = nullptr) override;
  void Close() override;
  bool IsOpen() const override { return sender_.IsOpen(); }
  VcamPixelFormat PreferredFormat() const override {
    return VcamPixelFormat::kBgr24TopDown;
  }
  bool SendFrame(const uint8_t* data, size_t bytes, int width, int height,
                 int64_t timestamp_us) override;
  int ConsumerCount() const override;

  const std::string& DllDirectory() const { return dll_directory_; }

 private:
  std::string dll_directory_;
  SoftcamSender sender_;
};

}  // namespace kgcore
