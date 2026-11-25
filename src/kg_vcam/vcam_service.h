#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kgcore/virtual_camera.h"

namespace kgcore {

// Layanan kamera virtual pada thread pekerja terpisah:
// - Tidak pernah memblokir thread render GL (SubmitFrame hanya menimpa slot
//   terbaru dengan drop-oldest policy).
// - Menjaga FPS konstan dengan VcamFramePacer (mengulang frame terakhir bila
//   kamera fisik lebih lambat).
// - Menangani resize aspect-preserving, mirror opsional (default MATI),
//   konversi format, serta log diagnostik [vcam].
class VcamService {
 public:
  explicit VcamService(std::unique_ptr<IVirtualCameraSink> sink);
  ~VcamService();

  VcamService(const VcamService&) = delete;
  VcamService& operator=(const VcamService&) = delete;

  // Periksa status registrasi di registry (tanpa hak admin).
  bool RefreshRegistration();

  // Nyalakan / matikan streaming dengan konfigurasi saat ini.
  void SetEnabled(bool enabled, const VcamConfig& cfg);
  void UpdateConfig(const VcamConfig& cfg);
  bool IsEnabled() const { return want_enabled_.load(); }

  // Dipanggil dari thread render setelah PBO selesai di-map.
  void SubmitFrame(const uint8_t* rgba_top_down, int width, int height,
                   int64_t capture_ts_us, float readback_ms);

  void NotePboBusySkip() { pbo_busy_skips_.fetch_add(1, std::memory_order_relaxed); }

  VcamStats GetStats() const;
  std::string RegistrationDetail() const;

  void Shutdown();

 private:
  void WorkerLoop();

  std::unique_ptr<IVirtualCameraSink> sink_;
  std::thread worker_;
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> want_enabled_{false};
  std::atomic<uint64_t> pbo_busy_skips_{0};

  mutable std::mutex mu_;
  std::condition_variable cv_;
  VcamConfig cfg_{};
  bool cfg_dirty_ = false;

  bool has_pending_ = false;
  std::vector<uint8_t> pending_rgba_;
  int pending_w_ = 0;
  int pending_h_ = 0;
  int64_t pending_ts_us_ = 0;
  float last_readback_ms_ = 0.0f;

  VcamStats stats_{};
  std::string reg_detail_;
  bool prev_registered_ = false;
};

}  // namespace kgcore
