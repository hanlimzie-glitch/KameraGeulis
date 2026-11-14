#pragma once
// M2j — penyedia segmentasi kulit SEMUA tubuh (MediaPipe
// selfie_multiclass_256x256 -> ONNX opset 17, ORT CPU).
//
// Kontrak:
//  * Thread TERPISAH (target 10-15 Hz) — SubmitFrame() tidak pernah menunggu
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

  bool Available() const { return available_.load(); }
  // Alasan kegagalan / "ok" (login sekali + notifikasi UI).
  std::string LastError() const;

  // Serahkan frame terbaru (RGBA/BGRA). MENYALIN + letterbox 256 di sini
  // (murah, ~0.1 ms) lalu membangunkan thread — tidak pernah menunggu ORT.
  void SubmitFrame(const uint8_t* pixels, int w, int h, int stride_bytes,
                   bool bgra, uint64_t frame_serial);

  // Salin hasil terakhir (peta probabilitas 256x256x6 + letterbox + serial
  // frame sumber + ms inferensi). false bila belum pernah ada hasil.
  bool GetResult(std::vector<float>* probs, LetterboxInfo* lb,
                 uint64_t* frame_serial, double* infer_ms) const;

  // Telemetri (atomik, aman dari thread mana pun).
  double LastInferMs() const { return last_ms_.load(); }
  uint64_t InferredFrames() const { return inferred_.load(); }
  uint64_t DroppedFrames() const { return dropped_.load(); }  // slot tertimpa

 private:
  void ThreadMain_();
  bool EnsureModelLoaded_();          // di thread: unduh + verifikasi + session
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

  // ---- hasil terakhir (dibaca render) ----
  bool res_valid_ = false;
  std::vector<float> res_probs_;  // 256*256*6
  LetterboxInfo res_lb_{};
  uint64_t res_serial_ = 0;
  double res_ms_ = 0.0;

  std::atomic<bool> running_{false};
  std::atomic<bool> stop_{false};
  std::atomic<bool> available_{false};
  std::atomic<bool> dead_{false};  // gagal permanen -> SubmitFrame no-op
  std::atomic<double> last_ms_{0.0};
  std::atomic<uint64_t> inferred_{0};
  std::atomic<uint64_t> dropped_{0};
  std::thread th_;
  mutable std::mutex err_mu_;
  std::string last_error_ = "belum dimuat";

  std::unique_ptr<Ort::Env> env_;
  std::unique_ptr<Ort::Session> session_;
};
}  // namespace kgcore
