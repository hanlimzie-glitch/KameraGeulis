#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace kgcore {

// Format piksel top-down yang didukung oleh sink kamera virtual.
enum class VcamPixelFormat {
  kBgr24TopDown = 0,   // 24-bit BGR packed, baris 0 di atas (softcam)
  kBgra32TopDown = 1,  // 32-bit BGRA packed, baris 0 di atas
  kNv12TopDown = 2,    // Y plane W*H + interleaved UV plane W*(H/2) (MF)
};

enum class VcamDriverState {
  kNotRegistered = 0,   // Belum terdaftar (DLL hilang atau belum registrasi COM)
  kRegisteredIdle = 1,  // Terdaftar (tidak mengirim)
  kStreaming = 2,       // Mengirim frame
};

struct VcamConfig {
  int width = 1280;
  int height = 720;
  int fps = 30;
  bool mirror = false;  // Default MATI: mirror hanya untuk preview
};

struct VcamStats {
  bool registered = false;
  bool active = false;
  VcamDriverState state = VcamDriverState::kNotRegistered;
  int consumer_count = -1;  // -1=tidak diketahui, 0=belum ada, >=1=terhubung
  uint64_t frames_sent = 0;
  uint64_t frames_repeated = 0;
  uint64_t frames_dropped = 0;
  uint64_t pbo_busy_skips = 0;
  float output_fps = 0.0f;
  float worker_ms = 0.0f;   // rata-rata waktu resize+konversi+send per frame
  float readback_ms = 0.0f; // waktu map PBO di render thread (non-blocking)
  float latency_ms = 0.0f;  // estimasi umur frame saat dikirim ke sink
  std::string last_error;
};

// Antarmuka abstrak sink kamera virtual sehingga backend kedua (mis.
// MFCreateVirtualCamera Windows 11) dapat ditambahkan di belakang interface
// yang sama tanpa menyentuh pipeline PBO/pacer/UI.
class IVirtualCameraSink {
 public:
  virtual ~IVirtualCameraSink() = default;
  virtual const char* BackendName() const = 0;
  virtual bool IsRegistered(std::string* detail_out = nullptr) const = 0;
  virtual bool Open(int width, int height, float fps,
                    std::string* err_out = nullptr) = 0;
  virtual void Close() = 0;
  virtual bool IsOpen() const = 0;
  virtual VcamPixelFormat PreferredFormat() const = 0;
  virtual bool SendFrame(const uint8_t* data, size_t bytes, int width,
                         int height, int64_t timestamp_us) = 0;
  // -1 bila tidak didukung, 0 bila belum ada konsumen, >=1 bila ada konsumen.
  virtual int ConsumerCount() const = 0;
};

struct VcamRect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
};

// Normalisasi ukuran keluaran ke kelipatan 4 yang valid (default 1280x720).
void NormalizeVcamConfig(VcamConfig* cfg);

// Ukuran buffer dalam byte untuk format tertentu (0 jika ukuran tidak valid).
size_t VcamOutputByteSize(VcamPixelFormat fmt, int width, int height);

// Hitung kotak tujuan aspect-preserving (letterbox/pillarbox terpusat).
VcamRect ComputeLetterboxRect(int src_w, int src_h, int dst_w, int dst_h);

// Konversi frame RGBA (top-down secara default; set flip_vertical=true jika
// sumber bottom-up) ke format sink target dengan resize aspect-preserving dan
// mirror horizontal opsional. Mengembalikan false jika input/ukuran tidak valid.
bool ConvertRgbaToVcamFormat(const uint8_t* src_rgba, int src_w, int src_h,
                             int src_stride_bytes, int dst_w, int dst_h,
                             VcamPixelFormat dst_fmt, bool flip_vertical,
                             bool mirror_horizontal,
                             std::vector<uint8_t>* out_buf);

// Pengatur tempo konstan-FPS (murni, deterministic untuk unit test):
// - SubmitFrame tidak pernah memblokir; frame baru menimpa slot pending
//   (drop-oldest) dan menaikkan penghitung drop.
// - PollFrame melepaskan frame pada interval konstan `1e6 / target_fps` us,
//   mengulang frame aktif terakhir bila sumber lebih lambat (mis. 20 fps -> 30 fps),
//   dan menjamin timestamp PTS selalu naik monoton.
class VcamFramePacer {
 public:
  explicit VcamFramePacer(int target_fps = 30);

  void Reset(int target_fps);
  int TargetFps() const { return target_fps_; }
  int64_t FrameIntervalUs() const { return interval_us_; }

  // Mengembalikan true jika menggantikan frame pending yang belum terkirim (drop).
  bool SubmitFrame(const uint8_t* rgba, int w, int h, int64_t capture_ts_us);

  // Periksa apakah waktunya mengirim frame baru pada `now_us`.
  bool PollFrame(int64_t now_us, std::vector<uint8_t>* out_rgba, int* out_w,
                 int* out_h, int64_t* out_pts_us, int64_t* out_capture_ts_us,
                 bool* out_is_repeat);

  bool HasFrame() const { return has_active_ || has_pending_; }
  uint64_t FramesSent() const { return frames_sent_; }
  uint64_t FramesRepeated() const { return frames_repeated_; }
  uint64_t FramesDropped() const { return frames_dropped_; }

 private:
  int target_fps_ = 30;
  int64_t interval_us_ = 33333;
  bool has_pending_ = false;
  std::vector<uint8_t> pending_rgba_;
  int pending_w_ = 0;
  int pending_h_ = 0;
  int64_t pending_capture_ts_us_ = 0;

  bool has_active_ = false;
  std::vector<uint8_t> active_rgba_;
  int active_w_ = 0;
  int active_h_ = 0;
  int64_t active_capture_ts_us_ = 0;

  bool schedule_started_ = false;
  int64_t next_due_us_ = 0;
  int64_t last_pts_us_ = -1;

  uint64_t frames_sent_ = 0;
  uint64_t frames_repeated_ = 0;
  uint64_t frames_dropped_ = 0;
};

struct VcamStatusInfo {
  VcamDriverState state = VcamDriverState::kNotRegistered;
  const char* status_key = "vcam.status_not_registered";
  int consumer_count = -1;
};

VcamStatusInfo ComputeVcamStatus(bool registered, bool open, int consumer_count,
                                 bool has_error);

}  // namespace kgcore
