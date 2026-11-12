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
  bool Start(int device_index = 0, int width = 0, int height = 0, int fps = 0);
  void Stop();
  bool Running() const { return running_.load(); }

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

  mutable std::mutex error_mutex_;
  std::string last_error_;
};
}  // namespace kgcore
