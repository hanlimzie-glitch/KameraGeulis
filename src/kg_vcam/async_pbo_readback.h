#pragma once
#include <glad/glad.h>
#include <cstdint>

namespace kgcore {
class VcamService;

// Readback GPU asinkron dengan double-buffered PBO + glFenceSync (timeout=0):
// - Tidak pernah memanggil glReadPixels sinkron yang memblokir thread render.
// - Hanya memanen slot PBO yang fence-nya sudah selesai (non-blocking), lalu
//   menyerahkan buffer RGBA ke VcamService di thread terpisah.
class AsyncPboReadback {
 public:
  AsyncPboReadback() = default;
  ~AsyncPboReadback();

  AsyncPboReadback(const AsyncPboReadback&) = delete;
  AsyncPboReadback& operator=(const AsyncPboReadback&) = delete;

  bool Init();
  void Destroy();
  bool IsInitialized() const { return initialized_; }

  // Panggil dari thread GL utama dengan tekstur keluaran bersih (tanpa F3/F6).
  void CaptureAndSubmit(GLuint src_tex, int width, int height,
                        int64_t capture_ts_us, VcamService* service);

 private:
  bool initialized_ = false;
  GLuint fbo_ = 0;
  GLuint pbo_[2]{0, 0};
  GLsync fence_[2]{nullptr, nullptr};
  int pbo_w_[2]{0, 0};
  int pbo_h_[2]{0, 0};
  int64_t pbo_ts_[2]{0, 0};
  int slot_idx_ = 0;
};

}  // namespace kgcore
