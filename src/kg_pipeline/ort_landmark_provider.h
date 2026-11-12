#pragma once
// Windows-only: ONNX Runtime CPU provider for the 468-pt FaceMesh landmark
// pipeline (BlazeFace short-range detector -> eye-aligned ROI -> FaceMesh).
// See docs/LANDMARKS.md. On any failure (models missing, download/verify
// error, session creation error) the provider reports Available()==false and
// an honest status string — the app never crashes and landmark effects stay
// disabled.
#include <memory>
#include <string>
#include "face_tracker.h"

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
};
}  // namespace kgcore
