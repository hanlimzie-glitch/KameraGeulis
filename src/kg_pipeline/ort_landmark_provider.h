#pragma once
// Windows-only: ONNX Runtime CPU provider for the 468-pt FaceMesh landmark
// pipeline (BlazeFace short-range detector -> eye-aligned ROI -> FaceMesh).
// See docs/LANDMARKS.md. On any failure (models missing, download/verify
// error, session creation error) the provider reports Available()==false and
// an honest status string — the app never crashes and landmark effects stay
// disabled.
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "face_tracker.h"
#include "kgcore/landmark.h"
#include "kgcore/roi_align.h"

namespace Ort {
class Env;
class Session;
}  // namespace Ort

namespace kgcore {
class OrtLandmarkProvider : public ILandmarkProvider {
 public:
  OrtLandmarkProvider();
  ~OrtLandmarkProvider() override;
  OrtLandmarkProvider(const OrtLandmarkProvider&) = delete;
  OrtLandmarkProvider& operator=(const OrtLandmarkProvider&) = delete;

  bool Available() const override;
  const char* Name() const override { return "ort-cpu"; }
  LandmarkSet Track(const Frame& frame) override;
  bool GetStageTimings(StageTimings* out) const override;
  bool GetDebugRoi(std::vector<uint8_t>* rgba_out, int* w, int* h,
                   AlignedRoi* roi_out) const override;

  // Human-readable reason when Available()==false ("models missing",
  // "download failed (…)", "hash mismatch …", "session error …", "ok").
  // Logged once at startup and shown in the UI.
  const std::string& LastError() const { return last_error_; }

 private:
  LandmarkSet TrackImpl_(const Frame& frame);  // Track() = wrapper ber-timing
  bool DetectionStage_(const Frame& frame, AlignedRoi* roi_out, float* face_score_out);
  bool RunMeshOnRoi_(const Frame& frame, const AlignedRoi& roi,
                     std::array<Vec2, kLandmarkCount>* canvas_pts,
                     float* face_flag_out, std::vector<uint8_t>* roi_rgba_out);
  bool EnsureModelsLoaded_();          // locate/download/verify/load; idempotent
  bool LoadSessions_(const std::wstring& dir);  // actually creates the sessions
  bool EnsureModelFile_(const std::wstring& dir, const wchar_t* file_name,
                        const char* sha256_hex);  // verify or download+verify
  bool DownloadFile_(const wchar_t* host_path, const std::wstring& dest);
  std::wstring ModelDir_() const;

  std::unique_ptr<Ort::Env> env_;
  std::unique_ptr<Ort::Session> det_;
  std::unique_ptr<Ort::Session> mesh_;
  bool load_attempted_ = false;
  bool available_ = false;
  bool logged_failure_ = false;
  std::string last_error_ = "model belum dimuat";

  // State tracking antar-frame (M2l: ROI dihitung langsung dari bbox
  // landmark ter-rotasi + detektor periodik adaptif 10-15 frame + uji sanity).
  bool have_prev_roi_ = false;
  AlignedRoi prev_roi_{};
  LandmarkSet prev_lm_{};
  float det_ref_side_px_ = 0.0f;
  float prev_face_score_ = 0.0f;
  float prev_face_flag_ = 10.0f;
  float last_roi_shift_frac_ = 0.0f;
  float last_angle_delta_rad_ = 0.0f;
  int lost_streak_ = 0;          // frame gagal berurutan
  int frames_since_detect_ = 0;  // frame sejak detektor terakhir jalan
  int reset_count_ = 0;
  std::string last_reset_reason_;
  StageTimings stage_;               // debug-overlay per-stage timings

  // Potongan ROI 192x192 RGBA terakhir yang dikirim ke FaceMesh (untuk F5).
  std::vector<uint8_t> last_roi_rgba_;
  AlignedRoi last_roi_used_{};
};
}  // namespace kgcore
