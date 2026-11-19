#pragma once
// M2p — Deteksi orientasi kamera otomatis (0/90/180/270 derajat CW) dengan
// memori prior dan hysteresis anti ping-pong. Murni C++17 (diuji Linux).
#include <array>
#include <string>

namespace kgcore {

// Indeks rotasi: 0=0°, 1=90°, 2=180°, 3=270° (searah jarum jam).
inline constexpr std::array<int, 4> kRotationAnglesDeg = {0, 90, 180, 270};

int RotationToIndex(int deg);
int IndexToRotation(int idx);

struct OrientationScores {
  std::array<float, 4> score{0.0f, 0.0f, 0.0f, 0.0f};  // 0, 90, 180, 270 deg
  bool ok = false;
};

struct OrientationConfig {
  float min_detect_score = 0.55f;   // skor minimum detektor agar dianggap wajah valid
  float switch_margin = 0.10f;      // keunggulan skor minimum saat rotasi aktif masih melihat wajah
  int confirm_searches = 2;         // pencarian berturut-turut yang sepakat sebelum pindah rotasi
  int switch_cooldown_frames = 15;  // jeda frame minimum antar pergantian rotasi
  int search_cooldown_frames = 10;  // jeda frame antar pencarian saat tidak ada wajah
  int fast_retry_frames = 2;        // jeda singkat saat konfirmasi kandidat sedang menunggu
  int fail_trigger_frames = 4;      // jumlah frame gagal berurutan sebelum pemicu pencarian
};

struct OrientationSearchDecision {
  bool switched = false;
  int prev_rot = 0;
  int new_rot = 0;
  int best_rot = 0;
  float best_score = 0.0f;
  float current_score = 0.0f;
  std::string reason;
};

class AutoOrientationController {
 public:
  explicit AutoOrientationController(OrientationConfig cfg = {});

  void SetPrior(int deg);
  int Prior() const { return prior_rot_; }

  // Mengembalikan rotasi efektif untuk `manual_setting`:
  // -1 (Otomatis) -> `effective_rot_`, selain itu -> 0/90/180/270 manual.
  int EffectiveRotation(int manual_setting) const;

  // Catat hasil frame terbaru (wajah terlacak valid atau gagal/reset).
  void NoteFrameResult(bool face_valid, bool roi_reset);

  // Apakah pencarian 4-rotasi sebaiknya dijalankan pada frame ini?
  bool ShouldSearch(int manual_setting) const;

  // Evaluasi skor BlazeFace pada 4 rotasi (0/90/180/270), terapkan hysteresis
  // & prior, lalu kembalikan keputusan rotasi efektif.
  OrientationSearchDecision ApplySearchScores(const OrientationScores& scores);

  // Reset streak/cooldown tanpa menghapus prior terbaik.
  void ResetTransient();

  int FailStreak() const { return fail_streak_; }
  int CandidateRotation() const { return candidate_rot_; }
  int CandidateStreak() const { return candidate_streak_; }
  int FramesSinceSwitch() const { return frames_since_switch_; }
  bool HasEverFoundFace() const { return has_ever_found_face_; }

 private:
  OrientationConfig cfg_;
  int prior_rot_ = 0;
  int effective_rot_ = 0;
  int candidate_rot_ = -1;
  int candidate_streak_ = 0;
  int fail_streak_ = 0;
  int reset_streak_ = 0;
  int frames_since_switch_ = 1000;
  int frames_since_search_ = 1000;
  bool has_ever_found_face_ = false;
  bool has_searched_once_ = false;
};

}  // namespace kgcore
