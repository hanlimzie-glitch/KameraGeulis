#pragma once
// M2l — diagnosis & swa-uji berkelanjutan (jendela geser) untuk mendeteksi
// kedipan / frame keluaran blend hitam (luma < 8 padahal mentah > 20).
// Bila laju frame hitam > 5% selama 1 detik, memicu fallback otomatis ke
// jalur komposit sebelumnya. Murni logika (unit-tested di Linux).
#include <cstddef>
#include <deque>

namespace kgcore {

class BlackFrameGuard {
 public:
  static constexpr float kRawBrightMinLuma = 20.0f;
  static constexpr float kOutDarkMaxLuma = 8.0f;
  static constexpr double kWindowMs = 1000.0;
  static constexpr double kMaxDarkRatio = 0.05;  // > 5%
  static constexpr size_t kMinSamples = 10;

  struct Sample {
    double ts_ms = 0.0;
    bool dark = false;
  };

  struct SecondSummary {
    int total = 0;
    int dark = 0;
    double window_dark_ratio = 0.0;
    size_t window_samples = 0;
  };

  static bool IsDarkAnomaly(float raw_luma, float out_luma) {
    return raw_luma > kRawBrightMinLuma && out_luma < kOutDarkMaxLuma;
  }

  void ResetWindow();
  void ResetAll();

  // Catat satu iterasi render. Jika blend tidak aktif, jendela di-reset agar
  // sampel lama tidak terbawa saat mode dihidupkan lagi.
  void RecordSample(double now_ms, float raw_luma, float out_luma,
                    bool blend_active);

  // true bila jendela geser sudah mencakup >= 1 detik (dan >= kMinSamples)
  // dengan laju frame hitam > 5%.
  bool ShouldFallback(double now_ms) const;

  double WindowDarkRatio(double now_ms) const;
  size_t WindowSampleCount() const { return samples_.size(); }

  // Ambil hitungan per-detik untuk log [diag] dan reset counter detik.
  SecondSummary TakeSecondSummary(double now_ms);

 private:
  void PruneOld_(double now_ms);

  std::deque<Sample> samples_;
  double window_start_ms_ = -1.0;
  bool was_active_ = false;
  int sec_total_ = 0;
  int sec_dark_ = 0;
};

}  // namespace kgcore
