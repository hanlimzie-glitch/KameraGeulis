#pragma once
// Windows-only: ONNX Runtime CPU provider for the 468-pt FaceMesh landmark
// pipeline (BlazeFace short-range detector -> eye-aligned ROI -> FaceMesh).
// See docs/LANDMARKS.md. On any failure (models missing, download/verify
// error, session creation error) the provider reports Available()==false and
// an honest status string — the app never crashes and landmark effects stay
// disabled.
#include <array>
#include <memory>
#include <string>
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

  // Human-readable reason when Available()==false ("models missing",
  // "download failed (…)", "hash mismatch …", "session error …", "ok").
  // Logged once at startup and shown in the UI.
  const std::string& LastError() const { return last_error_; }

 private:
  // Detector cadence (docs/ARCHITECTURE.md §5.1): BlazeFace runs on the
  // first frame and whenever the tracked face is lost; while a face is
  // tracked, the mesh ROI is carried forward (previous-frame ROI tracking)
  // using the eye-center delta and the eye-axis angle from the previous
  // frame's landmarks. A long safety re-detection fires after
  // kSafetyRedetectFrames tracked frames.
  static constexpr int kSafetyRedetectFrames = 300;  // ~10 s @ 30 fps

  bool DetectionStage_(const Frame& frame, AlignedRoi* roi_out, float* face_score_out);
  AlignedRoi TrackRoiFromPreviousLandmarks_(int src_w, int src_h) const;
  void NoteMeshOutcome_(const LandmarkSet& result, const AlignedRoi& roi_used,
                        int src_w, int src_h);
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

  // Previous-frame tracking state (ROI carry-over). Landmarks are stored in
  // normalized [0,1] frame coordinates (the same space as LandmarkSet::pts).
  bool have_prev_roi_ = false;
  AlignedRoi prev_roi_{};
  std::array<Vec2, kLandmarkCount> prev_pts_{};
  Vec2 roi_center_offset_{};  // tracked center = landmark centroid + offset
  float prev_face_score_ = 0.0f;
  int lost_streak_ = 0;          // consecutive invalid frames (flag<=0 / inference failure)
  int frames_since_detect_ = 0;  // telemetry: frames tracked without detector
  bool pending_roi_anchor_ = false;  // true after a detection; offset set on next mesh ok
};
}  // namespace kgcore
