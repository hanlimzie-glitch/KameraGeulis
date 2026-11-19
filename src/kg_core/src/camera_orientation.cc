#include "kgcore/camera_orientation.h"
#include <algorithm>
#include <cstdio>
#include "kgcore/frame_convert.h"

namespace kgcore {

int RotationToIndex(int deg) {
  const int r = NormalizeEffectiveRotation(deg);
  switch (r) {
    case 90:
      return 1;
    case 180:
      return 2;
    case 270:
      return 3;
    default:
      return 0;
  }
}

int IndexToRotation(int idx) {
  if (idx < 0 || idx >= 4) return 0;
  return kRotationAnglesDeg[static_cast<size_t>(idx)];
}

AutoOrientationController::AutoOrientationController(OrientationConfig cfg)
    : cfg_(cfg) {}

void AutoOrientationController::SetPrior(int deg) {
  const int norm = NormalizeEffectiveRotation(deg);
  prior_rot_ = norm;
  effective_rot_ = norm;
}

int AutoOrientationController::EffectiveRotation(int manual_setting) const {
  const int norm_setting = NormalizeRotationSetting(manual_setting);
  if (norm_setting >= 0) return norm_setting;
  return effective_rot_;
}

void AutoOrientationController::NoteFrameResult(bool face_valid, bool roi_reset) {
  ++frames_since_switch_;
  ++frames_since_search_;
  if (face_valid) {
    has_ever_found_face_ = true;
    fail_streak_ = 0;
    if (roi_reset) {
      ++reset_streak_;
    } else {
      reset_streak_ = 0;
    }
  } else {
    ++fail_streak_;
    reset_streak_ = 0;
  }
}

bool AutoOrientationController::ShouldSearch(int manual_setting) const {
  if (NormalizeRotationSetting(manual_setting) >= 0) return false;
  // Pencarian perdana saat start jika frame pertama belum punya wajah.
  if (!has_searched_once_ && fail_streak_ >= 1) return true;
  const bool failing = fail_streak_ >= cfg_.fail_trigger_frames ||
                         reset_streak_ >= cfg_.fail_trigger_frames;
  if (!failing) return false;
  const int min_wait =
      (candidate_rot_ >= 0 && candidate_streak_ > 0) ? cfg_.fast_retry_frames
                                                     : cfg_.search_cooldown_frames;
  return frames_since_search_ >= min_wait;
}

OrientationSearchDecision AutoOrientationController::ApplySearchScores(
    const OrientationScores& scores) {
  has_searched_once_ = true;
  frames_since_search_ = 0;

  OrientationSearchDecision dec;
  dec.prev_rot = effective_rot_;
  dec.new_rot = effective_rot_;

  const int cur_idx = RotationToIndex(effective_rot_);
  const float cur_score = scores.score[static_cast<size_t>(cur_idx)];
  dec.current_score = cur_score;

  // Pilih skor tertinggi dengan tie-breaker mengutamakan rotasi efektif saat
  // ini, lalu prior tersimpan.
  int best_idx = cur_idx;
  float best_score = cur_score;
  const int prior_idx = RotationToIndex(prior_rot_);
  if (scores.score[static_cast<size_t>(prior_idx)] > best_score + 1e-4f) {
    best_idx = prior_idx;
    best_score = scores.score[static_cast<size_t>(prior_idx)];
  }
  for (int i = 0; i < 4; ++i) {
    if (scores.score[static_cast<size_t>(i)] > best_score + 1e-4f) {
      best_idx = i;
      best_score = scores.score[static_cast<size_t>(i)];
    }
  }
  const int best_rot = IndexToRotation(best_idx);
  dec.best_rot = best_rot;
  dec.best_score = best_score;

  if (!scores.ok || best_score < cfg_.min_detect_score) {
    candidate_rot_ = -1;
    candidate_streak_ = 0;
    dec.reason = "tidak ada wajah melebihi ambang";
    return dec;
  }

  if (best_rot == effective_rot_) {
    candidate_rot_ = -1;
    candidate_streak_ = 0;
    prior_rot_ = effective_rot_;
    has_ever_found_face_ = true;
    dec.reason = "rotasi saat ini tetap terbaik";
    return dec;
  }

  // Hysteresis margin: jika rotasi aktif masih mendeteksi wajah, kandidat baru
  // harus unggul signifikan (+switch_margin).
  if (cur_score >= cfg_.min_detect_score &&
      best_score < cur_score + cfg_.switch_margin) {
    candidate_rot_ = -1;
    candidate_streak_ = 0;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "hysteresis margin (< +%.2f)",
                  cfg_.switch_margin);
    dec.reason = buf;
    return dec;
  }

  // Switch cooldown setelah perpindahan terbaru agar tidak ping-pong.
  if (has_ever_found_face_ && frames_since_switch_ < cfg_.switch_cooldown_frames) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "menunggu cooldown switch (%d/%d fr)",
                  frames_since_switch_, cfg_.switch_cooldown_frames);
    dec.reason = buf;
    return dec;
  }

  if (candidate_rot_ == best_rot) {
    ++candidate_streak_;
  } else {
    candidate_rot_ = best_rot;
    candidate_streak_ = 1;
  }

  if (candidate_streak_ < cfg_.confirm_searches) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "menunggu konfirmasi %d/%d",
                  candidate_streak_, cfg_.confirm_searches);
    dec.reason = buf;
    return dec;
  }

  // Konfirmasi tercapai: ganti rotasi efektif & simpan sebagai prior.
  effective_rot_ = best_rot;
  prior_rot_ = best_rot;
  dec.switched = true;
  dec.new_rot = effective_rot_;
  frames_since_switch_ = 0;
  fail_streak_ = 0;
  reset_streak_ = 0;
  candidate_rot_ = -1;
  candidate_streak_ = 0;
  has_ever_found_face_ = true;
  char buf[96];
  std::snprintf(buf, sizeof(buf), "beralih %d->%d (skor %.2f vs %.2f)",
                dec.prev_rot, dec.new_rot, best_score, cur_score);
  dec.reason = buf;
  return dec;
}

void AutoOrientationController::ResetTransient() {
  candidate_rot_ = -1;
  candidate_streak_ = 0;
  fail_streak_ = 0;
  reset_streak_ = 0;
  frames_since_switch_ = cfg_.switch_cooldown_frames;
  frames_since_search_ = cfg_.search_cooldown_frames;
}

}  // namespace kgcore
