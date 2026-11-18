#pragma once
// M2j — penyedia segmentasi kulit SEMUA tubuh (MediaPipe
// selfie_multiclass_256x256 -> ONNX opset 17, ORT CPU).
//
// Kontrak:
//  * Thread TERPISAH (target adaptif 3-5 Hz) — SubmitFrame() tidak pernah menunggu
//    inferensi; render bebas (hasil terbaru yang dipakai, frame lama dibuang).
//  * Model mengeluarkan peta [1,256,256,6]; softmax (bila perlu) dijalankan
//    DI SINI, di luar model — lihat seg_math.h (SegNeedsSoftmax/SegActivate).
//  * Gagal muat/unduh/verifikasi/ inferensi -> Available()==false + alasan
//    jujur; pemanggil wajib fallback ke mask wajah saja (tanpa crash).
//  * Model TIDAK pernah di-commit; unduh pertama kali dari rilis `models-v1`
//    (pola sama dgn OrtLandmarkProvider).
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kgcore/seg_math.h"

namespace Ort {
class Env;
class Session;
}  // namespace Ort

namespace kgcore {
class SegProvider {
 public:
  SegProvider();
  ~SegProvider();
  SegProvider(const SegProvider&) = delete;
  SegProvider& operator=(const SegProvider&) = delete;

  // Mulai/berhenti thread inferensi (Start idempoten).
  void Start();
  void Stop();
  // M2k: set minimum spacing between inferences (200–3000 ms), requested by PerfGuard.
  void SetPeriodMs(int period_ms);
  // M2o: konfigurasi dinamis ORT untuk benchmark matrix & default hemat CPU
  // (`intra_op` dibatasi [1,3], default 2; `allow_spinning` default false).
  void ConfigureSession(int intra_op, bool allow_spinning);
  // M2o: 0 = periode adaptif berdasar gerak kepala; >0 = periode tetap (ms) untuk benchmark.
  void SetBenchmarkPeriodMs(int period_ms);
  // M2n: flag perbandingan FP32 vs INT8 pada frame yang sama, dan pilih INT8 opsional.
  void SetCompareMode(bool enable) { compare_mode_.store(enable); }
  void SetPreferInt8(bool enable) { prefer_int8_.store(enable); }
  bool CompareMode() const { return compare_mode_.load(); }

  struct CompareTelemetry {
    bool active = false;
    uint64_t samples = 0;
    double last_fp32_ms = 0.0;
    double last_int8_ms = 0.0;
    double avg_fp32_ms = 0.0;
    double avg_int8_ms = 0.0;
    float last_argmax_agree = 0.0f;
    float avg_argmax_agree = 0.0f;
    float last_miou = 0.0f;
    float avg_miou = 0.0f;
    uint64_t last_frame_serial = 0;
  };
  CompareTelemetry LatestCompare() const;

  bool Available() const { return available_.load(); }
  // Alasan kegagalan / "ok" (login sekali + notifikasi UI).
  std::string LastError() const;

  // Serahkan frame terbaru (RGBA/BGRA). MENYALIN + letterbox 256 di sini
  // (murah, ~0.1 ms) lalu membangunkan thread — tidak pernah menunggu ORT.
  void SubmitFrame(const uint8_t* pixels, int w, int h, int stride_bytes,
                   bool bgra, uint64_t frame_serial,
                   const SegPoseAnchor& pose = {},
                   float head_speed_px = 0.0f);

  // Salin hasil terakhir (peta probabilitas 256x256x6 + letterbox + serial
  // frame sumber + ms inferensi). false bila belum pernah ada hasil.
  bool GetResult(std::vector<float>* probs, LetterboxInfo* lb,
                 uint64_t* frame_serial, double* infer_ms,
                 SegPoseAnchor* pose = nullptr) const;

  // M2m: salin hanya bila res_serial > last_seen_serial (hemat memcpy 1.57 MB/frame).
  bool GetResultIfNewer(uint64_t last_seen_serial, std::vector<float>* probs,
                        LetterboxInfo* lb, uint64_t* frame_serial,
                        double* infer_ms, SegPoseAnchor* pose = nullptr) const;

  // Telemetri (atomik, aman dari thread mana pun).
  double LastInferMs() const { return last_ms_.load(); }
  double AverageInferMs() const { return average_ms_.load(); }
  uint64_t InferredFrames() const { return inferred_.load(); }
  uint64_t DroppedFrames() const { return dropped_.load(); }  // slot tertimpa
  unsigned HardwareCores() const { return hw_cores_.load(); }
  int IntraOpThreads() const { return intra_threads_.load(); }
  bool AllowSpinning() const { return allow_spinning_.load(); }
  int CurrentPeriodMs() const { return current_period_ms_.load(); }
  const char* ExecutionProviderName() const { return ep_name_; }
  const char* PrecisionName() const { return precision_name_; }

 private:
  void ThreadMain_();
  bool EnsureModelLoaded_();          // di thread: unduh + verifikasi + session
  bool RebuildSession_(int intra_op, bool allow_spinning);
  bool EnsureModelFile_(const std::wstring& dir, const wchar_t* file_name,
                        const char* sha256_hex);
  bool DownloadFile_(const wchar_t* host_path, const std::wstring& dest);

  // ---- slot input terbaru (dipakai thread) ----
  mutable std::mutex mu_;
  std::condition_variable cv_;
  bool slot_pending_ = false;
  std::vector<float> slot_input_;  // 256*256*3 (RGB ternormalisasi)
  LetterboxInfo slot_lb_{};
  uint64_t slot_serial_ = 0;
  SegPoseAnchor slot_pose_{};

  // ---- hasil terakhir (dibaca render) ----
  bool res_valid_ = false;
  std::vector<float> res_probs_;  // 256*256*6
  LetterboxInfo res_lb_{};
  uint64_t res_serial_ = 0;
  double res_ms_ = 0.0;
  SegPoseAnchor res_pose_{};

  std::atomic<bool> running_{false};
  std::atomic<bool> stop_{false};
  std::atomic<bool> available_{false};
  std::atomic<bool> dead_{false};  // gagal permanen -> SubmitFrame no-op
  std::atomic<bool> compare_mode_{false};
  std::atomic<bool> prefer_int8_{false};
  std::atomic<double> last_ms_{0.0};
  std::atomic<double> average_ms_{0.0};
  std::atomic<int> requested_period_ms_{250};
  std::atomic<int> benchmark_period_ms_{0};
  std::atomic<float> head_speed_px_{0.0f};
  std::atomic<int> current_period_ms_{1000};
  std::atomic<uint64_t> inferred_{0};
  std::atomic<uint64_t> dropped_{0};
  std::atomic<unsigned> hw_cores_{0};
  std::atomic<int> intra_threads_{2};
  std::atomic<bool> allow_spinning_{false};
  std::atomic<int> target_intra_threads_{2};
  std::atomic<bool> target_allow_spinning_{false};
  std::atomic<bool> reconfigure_pending_{false};
  std::wstring loaded_model_path_;
  std::wstring loaded_int8_path_;
  const char* ep_name_ = "cpu";
  const char* precision_name_ = "fp32";
  std::thread th_;
  mutable std::mutex err_mu_;
  std::string last_error_ = "belum dimuat";
  mutable std::mutex cmp_mu_;
  CompareTelemetry cmp_stats_{};

  std::unique_ptr<Ort::Env> env_;
  std::unique_ptr<Ort::Session> session_;
  std::unique_ptr<Ort::Session> compare_int8_session_;
};
}  // namespace kgcore
