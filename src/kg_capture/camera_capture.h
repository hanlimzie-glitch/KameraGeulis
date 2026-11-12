#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include "kgcore/bounded_queue.h"
#include "kgcore/frame.h"

namespace kgcore {
// Media Foundation camera capture (Windows only). Captures on a dedicated
// thread into a bounded queue (drop-oldest, capacity 2) so the render loop
// never waits on the camera.
class CameraCapture {
 public:
  CameraCapture() = default;
  ~CameraCapture() { Stop(); }
  CameraCapture(const CameraCapture&) = delete;
  CameraCapture& operator=(const CameraCapture&) = delete;

  // Starts capture on device `device_index` (0 = default). Returns false and
  // sets LastError() on failure.
  bool Start(int device_index = 0);
  void Stop();
  bool Running() const { return running_.load(); }

  // Copies the newest frame into `out` (returns false if none yet).
  bool LatestFrame(Frame& out);

  uint64_t FramesCaptured() const { return frames_captured_.load(); }
  std::string LastError() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return last_error_;
  }

 private:
  void CaptureThread(int device_index);
  void SetError(const char* fmt, ...);

  std::atomic<bool> running_{false};
  std::atomic<uint64_t> frames_captured_{0};
  std::thread thread_;
  std::unique_ptr<BoundedQueue<Frame>> queue_;

  mutable std::mutex error_mutex_;
  std::string last_error_;
};
}  // namespace kgcore
