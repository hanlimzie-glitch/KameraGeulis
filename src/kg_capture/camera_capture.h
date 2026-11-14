#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <thread>
#include "kgcore/bounded_queue.h"
#include "kgcore/frame.h"

namespace kgcore {
struct VideoDeviceInfo {
  int index = 0;
  std::string name;  // MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME
};
struct VideoMode {
  int width = 0, height = 0, fps = 0;
};

// Per-second diagnosis snapshot (M2e). Filled by the capture thread,
// copied out under stats_mutex_ by the render loop for log/UI.
struct CameraStats {
  uint64_t received = 0;        // frames copied from MF samples
  int width = 0, height = 0;    // last frame dims
  int stride = 0;               // last negotiated stride, SIGNED (negative = bottom-up)
  int subtype = 0;              // MF_MT_SUBTYPE fourcc (e.g. 'RGB32')
  int fps_num = 0, fps_den = 0; // negotiated MF_MT_FRAME_RATE
  double mean_luma = 0.0;       // sampled mean luma of last frame (0..255)
  int center_alpha = 0;         // alpha byte at frame center of last frame
  bool mode_from_device = false;// true once GetCurrentMediaType read-back succeeded
};

// Media Foundation camera capture (Windows only). Captures on a dedicated
// thread into a bounded queue (drop-oldest, capacity 2) so the render loop
// never waits on the camera.
class CameraCapture {
 public:
  CameraCapture() = default;
  ~CameraCapture() { Stop(); }
  CameraCapture(const CameraCapture&) = delete;
  CameraCapture& operator=(const CameraCapture&) = delete;

  // Starts capture on device `device_index` (0 = default). If width/height
  // are > 0, requests that resolution (and fps, if > 0); falls back to the
  // camera's default RGB32 mode with a log line when the exact request is
  // rejected. Returns false and sets LastError() on failure.
  // M2f: `plain_attrs` = --capture-plain -> source reader TANPA atribut
  // (tingkat (c) ladder) langsung dari awal. Reader creation di thread worker
  // memakai tangga fallback (a)->(b)->(c) dan mengulang dengan backoff bila
  // gagal; "OK" hanya dicetak setelah reader jadi + frame pertama diterima.
  bool Start(int device_index = 0, int width = 0, int height = 0, int fps = 0,
             bool plain_attrs = false);
  void Stop();
  bool Running() const { return running_.load(); }

  // M2f status jujur untuk UI/log (snapshot atomik).
  struct CaptureStatus {
    bool thread_active = false;  // thread jalan (bisa sedang backoff)
    bool streaming = false;      // reader jadi DAN frame pertama sudah masuk
    bool had_failure = false;    // minimal satu percobaan gagal sejauh ini
    int64_t hr = 0;              // hr kegagalan terakhir (0 bila belum ada)
    unsigned attempt = 0;        // jumlah percobaan buka
    unsigned failure_count = 0;  // jumlah kegagalan
  };
  CaptureStatus GetStatus() const;

  // Enumerates capture devices (video capture sources). Static so the
  // settings UI can populate pickers before any capture starts; cheap, but
  // does touch MF — call off the hot path (e.g. on demand / startup).
  static std::vector<VideoDeviceInfo> EnumerateDevices();
  // Enumerates (width,height,fps) modes reported by the device's native
  // media types, deduplicated and sorted by pixels desc. Empty on failure.
  static std::vector<VideoMode> EnumerateModes(int device_index);

  // Copies the newest frame into `out` (returns false if none yet).
  bool LatestFrame(Frame& out);

  uint64_t FramesCaptured() const { return frames_captured_.load(); }
  // Snapshot of the M2e diagnosis counters (thread-safe copy).
  CameraStats GetStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
  }
  std::string LastError() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return last_error_;
  }

 private:
  void CaptureThread(int device_index, int width, int height, int fps);
  void SetError(const char* fmt, ...);

  std::atomic<bool> running_{false};
  std::atomic<uint64_t> frames_captured_{0};
  std::thread thread_;
  std::unique_ptr<BoundedQueue<Frame>> queue_;

  mutable std::mutex stats_mutex_;
  CameraStats stats_;

  // M2f: status buka-kamera untuk UI (atomik, tanpa lock).
  std::atomic<bool> first_frame_seen_{false};
  std::atomic<uint32_t> open_attempt_{0};
  std::atomic<uint32_t> open_failure_count_{0};
  std::atomic<int64_t> last_open_hr_{0};  // 0 = belum ada kegagalan
  bool plain_attrs_ = false;              // --capture-plain (dibaca thread)

  mutable std::mutex error_mutex_;
  std::string last_error_;
};
}  // namespace kgcore
