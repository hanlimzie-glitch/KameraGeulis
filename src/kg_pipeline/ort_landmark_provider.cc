#include "ort_landmark_provider.h"

#include <onnxruntime_cxx_api.h>

#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "kgcore/face_anchors.h"
#include "kgcore/face_detection_decode.h"
#include "kgcore/face_mesh_decode.h"
#include "kgcore/log.h"
#include "kgcore/roi_align.h"
#include "windows_paths.h"

namespace kgcore {
namespace {
constexpr char kTag[] = "landmark";

// Model provenance: docs/LANDMARKS.md §2.3 (SHA-256 of the two ONNX files).
// Models are NEVER committed; they are downloaded on first run from the
// project GitHub Release `models-v1` (or taken from KAMERAGEULIS_MODELS_DIR).
constexpr wchar_t kDetFile[] = L"face_detection_short_range.onnx";
constexpr wchar_t kMeshFile[] = L"face_mesh.onnx";
constexpr char kDetSha256[] = "82ac962b24dfd78a5b6a3331e8c4091e377d699c6e4fdaca158fba03b5af8e3d";
constexpr char kMeshSha256[] = "ac72f85b77368a2ee1d065257674cb25099699d98376685af549df50d6a40337";
constexpr wchar_t kDownloadBase[] =
    L"github.com";
constexpr wchar_t kDownloadPathPrefix[] =
    L"/hanlimzie-glitch/KameraGeulis/releases/download/models-v1/";
constexpr wchar_t kUserAgent[] = L"KameraGeulis/0.2 (+https://github.com/hanlimzie-glitch/KameraGeulis)";

bool EnsureDirectoryExists(const std::wstring& dir) {
  size_t pos = 0;
  while (pos != std::wstring::npos) {
    pos = dir.find_first_of(L"\\/", pos);
    const std::wstring part = (pos == std::wstring::npos) ? dir : dir.substr(0, pos);
    if (!part.empty() && part.back() != L':') {
      if (CreateDirectoryW(part.c_str(), nullptr) == 0 &&
          GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
      }
    }
    if (pos == std::wstring::npos) break;
    ++pos;
  }
  return true;
}

std::string WideToUtf8(const std::wstring& s) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
  std::string out(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n, nullptr, nullptr);
  return out;
}

std::string Sha256HexOfFile(const std::wstring& path, bool* ok) {
  *ok = false;
  BCRYPT_ALG_HANDLE hAlg = nullptr;
  if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
    return {};
  }
  DWORD objLen = 0, cb = 0;
  BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objLen), sizeof(objLen), &cb, 0);
  std::vector<UCHAR> hash_object(objLen);
  BCRYPT_HASH_HANDLE hHash = nullptr;
  if (BCryptCreateHash(hAlg, &hHash, hash_object.data(), objLen, nullptr, 0, 0) != 0) {
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return {};
  }
  std::ifstream in(path.c_str(), std::ios::binary);
  std::vector<uint8_t> buf(1 << 16);
  bool read_ok = true;
  while (in) {
    in.read(reinterpret_cast<char*>(buf.data()), buf.size());
    const std::streamsize n = in.gcount();
    if (n > 0 && BCryptHashData(hHash, buf.data(), static_cast<ULONG>(n), 0) != 0) {
      read_ok = false;
      break;
    }
  }
  BYTE hash[32]{};
  bool fin_ok = (BCryptFinishHash(hHash, hash, sizeof(hash), 0) == 0);
  BCryptDestroyHash(hHash);
  BCryptCloseAlgorithmProvider(hAlg, 0);
  if (!read_ok || !fin_ok) return {};
  char hex[65]{};
  for (int i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02x", hash[i]);
  *ok = true;
  return std::string(hex, 64);
}

bool FilesEqualHash(const std::wstring& path, const char* sha256_hex) {
  bool ok = false;
  const std::string actual = Sha256HexOfFile(path, &ok);
  if (!ok) return false;
  return actual == sha256_hex;
}
}  // namespace

OrtLandmarkProvider::OrtLandmarkProvider() { EnsureModelsLoaded_(); }

OrtLandmarkProvider::~OrtLandmarkProvider() = default;

bool OrtLandmarkProvider::Available() const { return available_; }

// Model search order (shared with the app --selftest via windows_paths.h):
//   1) KAMERAGEULIS_MODELS_DIR — authoritative (download target if writable)
//   2) <exe_dir>\models — shipped in the CI artifact (read-only)
//   3) %LOCALAPPDATA%\KameraGeulis\models — download cache / download target
// Returns the first directory where BOTH models exist and verify. If none
// does, returns the directory the app should DOWNLOAD into (env dir if set,
// else LOCALAPPDATA).
std::wstring OrtLandmarkProvider::ModelDir_() const {
  const std::wstring env = KgEnvModelsDirW();
  const std::wstring exe = KgExeModelsDirW();
  const std::wstring local = KgLocalModelsDirW();
  auto verified = [&](const std::wstring& dir) {
    if (dir.empty()) return false;
    return FilesEqualHash(dir + L"\\" + kDetFile, kDetSha256) &&
           FilesEqualHash(dir + L"\\" + kMeshFile, kMeshSha256);
  };
  if (!env.empty() && verified(env)) {
    Log(kTag, "model dir: %ls (KAMERAGEULIS_MODELS_DIR, verified)", env.c_str());
    return env;
  }
  if (verified(exe)) {
    Log(kTag, "model dir: %ls (shipped next to exe, verified)", exe.c_str());
    return exe;
  }
  if (!local.empty() && verified(local)) {
    Log(kTag, "model dir: %ls (LOCALAPPDATA cache, verified)", local.c_str());
    return local;
  }
  if (!env.empty()) {
    Log(kTag, "model dir: %ls (KAMERAGEULIS_MODELS_DIR; download target)", env.c_str());
    return env;  // env authoritative: download into it
  }
  Log(kTag, "model dir: %ls (tidak ada model terverifikasi - download)", local.c_str());
  return local;
}

bool OrtLandmarkProvider::DownloadFile_(const wchar_t* host_path, const std::wstring& dest) {
  const std::wstring tmp = dest + L".tmp";
  HINTERNET hSession =
      WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                  WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hSession) return false;
  std::wstring host = kDownloadBase;
  std::wstring path = std::wstring(kDownloadPathPrefix) + host_path;
  bool ok = false;
  for (int redirect = 0; redirect < 6 && !ok; ++redirect) {
    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) break;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), nullptr,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            WINHTTP_FLAG_SECURE);
    if (!hRequest) {
      WinHttpCloseHandle(hConnect);
      break;
    }
    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
                            0, 0, 0) ||
        !WinHttpReceiveResponse(hRequest, nullptr)) {
      WinHttpCloseHandle(hRequest);
      WinHttpCloseHandle(hConnect);
      break;
    }
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status >= 300 && status < 400) {
      // Follow redirect: read Location header, re-parse host/path.
      DWORD locSize = 0;
      WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, nullptr,
                          &locSize, WINHTTP_NO_HEADER_INDEX);
      std::wstring location(locSize / sizeof(wchar_t), L'\0');
      WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                          location.data(), &locSize, WINHTTP_NO_HEADER_INDEX);
      location.resize(wcslen(location.c_str()));
      WinHttpCloseHandle(hRequest);
      WinHttpCloseHandle(hConnect);
      // Parse "https://host/path"
      const std::wstring https_prefix = L"https://";
      if (location.compare(0, https_prefix.size(), https_prefix) != 0) break;  // https only
      const size_t slash = location.find(L'/', https_prefix.size());
      if (slash == std::wstring::npos) break;
      host = location.substr(https_prefix.size(), slash - https_prefix.size());
      path = location.substr(slash);
      continue;
    }
    if (status != 200) {
      WinHttpCloseHandle(hRequest);
      WinHttpCloseHandle(hConnect);
      break;
    }
    std::ofstream out(tmp.c_str(), std::ios::binary | std::ios::trunc);
    std::vector<char> buf(1 << 15);
    DWORD total = 0;
    while (true) {
      DWORD read = 0;
      if (!WinHttpReadData(hRequest, buf.data(), static_cast<DWORD>(buf.size()), &read) ||
          read == 0) {
        break;
      }
      out.write(buf.data(), read);
      total += read;
    }
    out.close();
    ok = total > 0 && out.good();
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
  }
  WinHttpCloseHandle(hSession);
  if (!ok) {
    DeleteFileW(tmp.c_str());
    return false;
  }
  if (!MoveFileExW(tmp.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileW(tmp.c_str());
    return false;
  }
  return true;
}

bool OrtLandmarkProvider::EnsureModelFile_(const std::wstring& dir, const wchar_t* file_name,
                                           const char* sha256_hex) {
  const std::wstring dest = dir + L"\\" + file_name;
  if (GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES) {
    if (FilesEqualHash(dest, sha256_hex)) return true;  // already present & verified
    Log(kTag, "model %ls exists but SHA-256 mismatch — re-downloading", file_name);
  }
  Log(kTag, "downloading %ls from models-v1 release …", file_name);
  if (!DownloadFile_(file_name, dest)) {
    last_error_ = std::string("download gagal untuk ") + WideToUtf8(file_name) +
                  " (cek koneksi / rilis models-v1)";
    return false;
  }
  if (!FilesEqualHash(dest, sha256_hex)) {
    last_error_ = std::string("SHA-256 tidak cocok untuk ") + WideToUtf8(file_name) +
                  " — file dihapus, efek landmark dinonaktifkan";
    DeleteFileW(dest.c_str());
    return false;
  }
  return true;
}

bool OrtLandmarkProvider::LoadSessions_(const std::wstring& dir) {
  try {
    env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "KameraGeulis");
    Ort::SessionOptions opts;
    opts.SetIntraOpNumThreads(1);
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    det_ = std::make_unique<Ort::Session>(*env_, (dir + L"\\" + kDetFile).c_str(), opts);
    mesh_ = std::make_unique<Ort::Session>(*env_, (dir + L"\\" + kMeshFile).c_str(), opts);
    return true;
  } catch (const Ort::Exception& e) {
    last_error_ = std::string("ORT session error: ") + e.what();
    det_.reset();
    mesh_.reset();
    env_.reset();
    return false;
  }
}

bool OrtLandmarkProvider::EnsureModelsLoaded_() {
  if (load_attempted_) return available_;
  load_attempted_ = true;
  const std::wstring dir = ModelDir_();
  if (dir.empty()) {
    last_error_ = "KAMERAGEULIS_MODELS_DIR/LOCALAPPDATA tidak tersedia";
    Log(kTag, "%s", last_error_.c_str());
    available_ = false;
    return false;
  }
  if (!EnsureDirectoryExists(dir)) {
    last_error_ = "tidak bisa membuat folder model: " + WideToUtf8(dir);
    Log(kTag, "%s", last_error_.c_str());
    available_ = false;
    return false;
  }
  if (!EnsureModelFile_(dir, kDetFile, kDetSha256) ||
      !EnsureModelFile_(dir, kMeshFile, kMeshSha256)) {
    Log(kTag, "%s", last_error_.c_str());
    available_ = false;
    return false;
  }
  if (!LoadSessions_(dir)) {
    Log(kTag, "%s", last_error_.c_str());
    available_ = false;
    return false;
  }
  last_error_ = "ok";
  available_ = true;
  Log(kTag, "ORT CPU provider ready (models: %ls)", dir.c_str());
  return true;
}

namespace {
float SampleChannel(const Frame& f, float fx, float fy, int channel_bgr_or_rgb) {
  const int w = f.width, h = f.height;
  const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
  const float ax = fx - x0, ay = fy - y0;
  auto px = [&](int x, int y) -> float {
    if (x < 0 || y < 0 || x >= w || y >= h) return 0.0f;
    const uint8_t* p = f.pixels.data() + static_cast<size_t>(y) * f.stride_bytes + x * 4;
    const int c = channel_bgr_or_rgb;
    if (f.format == kFormatRGBA) return static_cast<float>(p[c]);
    // BGRA: R at [2], G at [1], B at [0]
    return static_cast<float>(p[c == 0 ? 2 : (c == 1 ? 1 : 0)]);
  };
  const float top = px(x0, y0) + (px(x0 + 1, y0) - px(x0, y0)) * ax;
  const float bot = px(x0, y0 + 1) + (px(x0 + 1, y0 + 1) - px(x0, y0 + 1)) * ax;
  return top + (bot - top) * ay;
}
}  // namespace

LandmarkSet OrtLandmarkProvider::Track(const Frame& frame) {
  const auto t0 = std::chrono::steady_clock::now();
  LandmarkSet out = TrackImpl_(frame);
  stage_.track_ms = std::chrono::duration<float, std::milli>(
                        std::chrono::steady_clock::now() - t0).count();
  return out;
}

bool OrtLandmarkProvider::GetStageTimings(StageTimings* out) const {
  if (!out) return false;
  *out = stage_;
  return true;
}

LandmarkSet OrtLandmarkProvider::TrackImpl_(const Frame& frame) {
  LandmarkSet out;
  out.ts_ms = frame.timestamp_ms;
  if (!frame.IsValid()) return out;
  stage_.detect_ms = 0.0f;
  stage_.mesh_ms = 0.0f;
  stage_.frames_since_detect = frames_since_detect_;
  if (!EnsureModelsLoaded_()) {
    if (!logged_failure_) {
      Log(kTag, "landmark provider unavailable: %s", last_error_.c_str());
      logged_failure_ = true;
    }
    return out;
  }

  const int src_w = frame.width, src_h = frame.height;

  // ---------- Cadence: detect only when lost / first frame / safety ----------
  const bool need_detection = !have_prev_roi_ || lost_streak_ >= 2 ||
                              frames_since_detect_ >= kSafetyRedetectFrames;
  AlignedRoi roi{};
  if (need_detection) {
    const auto det_start = std::chrono::steady_clock::now();
    const bool det_ok = DetectionStage_(frame, &roi, &out.face_score);
    stage_.detect_ms = std::chrono::duration<float, std::milli>(
                           std::chrono::steady_clock::now() - det_start).count();
    if (det_ok) {
      pending_roi_anchor_ = true;
    } else {
      ++lost_streak_;
      if (have_prev_roi_ && lost_streak_ < 2) {
        // Detector failed but we still hold a usable previous ROI: track instead.
        roi = TrackRoiFromPreviousLandmarks_(src_w, src_h);
        out.face_score = prev_face_score_;
      } else {
        have_prev_roi_ = false;
        return out;
      }
    }
  } else {
    roi = TrackRoiFromPreviousLandmarks_(src_w, src_h);
    out.face_score = prev_face_score_;
  }

  // ---------- Step 4: rotated square crop 192x192, [0,1] ----------
  const auto mesh_start = std::chrono::steady_clock::now();
  std::vector<float> mesh_input(static_cast<size_t>(192) * 192 * 3, 0.0f);
  const float ca = std::cos(roi.angle_rad), sa = std::sin(roi.angle_rad);
  for (int ty = 0; ty < 192; ++ty) {
    const float uy = ((ty + 0.5f) / 192.0f - 0.5f) * roi.side_px;
    for (int tx = 0; tx < 192; ++tx) {
      const float ux = ((tx + 0.5f) / 192.0f - 0.5f) * roi.side_px;
      const float fx = roi.cx_px + ca * ux - sa * uy;
      const float fy = roi.cy_px + sa * ux + ca * uy;
      float* dst = mesh_input.data() + (static_cast<size_t>(ty) * 192 + tx) * 3;
      for (int c = 0; c < 3; ++c) {
        dst[c] = SampleChannel(frame, fx, fy, c) / 255.0f;
      }
    }
  }

  // ---------- Step 5: mesh inference + face-flag gate ----------
  std::array<Vec2, kLandmarkCount> canvas_pts{};
  float face_flag = 0.0f;
  try {
    Ort::MemoryInfo memInfo =
        Ort::MemoryInfo::CreateCpu(OrtAllocatorType::OrtArenaAllocator, OrtMemType::OrtMemTypeDefault);
    const int64_t mesh_shape[4] = {1, 192, 192, 3};
    Ort::Value mesh_tensor = Ort::Value::CreateTensor<float>(memInfo, mesh_input.data(),
                                                              mesh_input.size(), mesh_shape, 4);
    const char* input_names[] = {"input_1"};
    const char* output_names[] = {"conv2d_21", "conv2d_31"};
    Ort::RunOptions run_opts;
    auto mesh_out = mesh_->Run(run_opts, input_names, &mesh_tensor, 1, output_names, 2);
    DecodeMeshTensor(mesh_out[0].GetTensorData<float>(), &canvas_pts);
    face_flag = ReadMeshFaceFlag(mesh_out[1].GetTensorData<float>());
  } catch (const Ort::Exception& e) {
    stage_.mesh_ms = std::chrono::duration<float, std::milli>(
                         std::chrono::steady_clock::now() - mesh_start).count();
    Log(kTag, "mesh inference failed: %s", e.what());
    out = LandmarkSet{};
    out.ts_ms = frame.timestamp_ms;
    NoteMeshOutcome_(out, roi, src_w, src_h);
    stage_.frames_since_detect = frames_since_detect_;
    return out;
  }
  stage_.mesh_ms = std::chrono::duration<float, std::milli>(
                       std::chrono::steady_clock::now() - mesh_start).count();
  if (face_flag <= 0.0f) {
    // ROI does not contain a face — count it; re-detect on the next frame.
    out = LandmarkSet{};
    out.ts_ms = frame.timestamp_ms;
    NoteMeshOutcome_(out, roi, src_w, src_h);
    stage_.frames_since_detect = frames_since_detect_;
    return out;
  }

  // ---------- Step 6: unmap to frame coordinates ----------
  for (size_t i = 0; i < kLandmarkCount; ++i) {
    out.pts[i] = MeshCanvasToFrame(canvas_pts[i], roi, src_w, src_h);
  }
  out.valid = true;
  NoteMeshOutcome_(out, roi, src_w, src_h);
  stage_.frames_since_detect = frames_since_detect_;
  return out;
}

bool OrtLandmarkProvider::DetectionStage_(const Frame& frame, AlignedRoi* roi_out,
                                          float* face_score_out) {
  const int src_w = frame.width, src_h = frame.height;
  const LetterboxInfo lb = ComputeLetterbox(src_w, src_h, 128);
  std::vector<float> det_input(static_cast<size_t>(128) * 128 * 3, 0.0f);
  for (int ty = 0; ty < 128; ++ty) {
    const float sy = ((ty + 0.5f) - lb.pad_y) / lb.scale - 0.5f;
    for (int tx = 0; tx < 128; ++tx) {
      const float sx = ((tx + 0.5f) - lb.pad_x) / lb.scale - 0.5f;
      float* dst = det_input.data() + (static_cast<size_t>(ty) * 128 + tx) * 3;
      for (int c = 0; c < 3; ++c) {
        dst[c] = (SampleChannel(frame, sx, sy, c) / 255.0f) * 2.0f - 1.0f;
      }
    }
  }

  const auto anchors = GenerateFaceAnchors();
  std::vector<float> regressors;
  std::vector<float> classificators;
  try {
    Ort::MemoryInfo memInfo =
        Ort::MemoryInfo::CreateCpu(OrtAllocatorType::OrtArenaAllocator, OrtMemType::OrtMemTypeDefault);
    const int64_t det_shape[4] = {1, 128, 128, 3};
    Ort::Value det_tensor = Ort::Value::CreateTensor<float>(memInfo, det_input.data(),
                                                             det_input.size(), det_shape, 4);
    const char* input_names[] = {"input"};
    const char* output_names[] = {"regressors", "classificators"};
    Ort::RunOptions run_opts;
    auto det_out = det_->Run(run_opts, input_names, &det_tensor, 1, output_names, 2);
    const float* reg = det_out[0].GetTensorData<float>();
    const float* cls = det_out[1].GetTensorData<float>();
    regressors.assign(reg, reg + 896 * 16);
    classificators.assign(cls, cls + 896);
  } catch (const Ort::Exception& e) {
    Log(kTag, "detector inference failed: %s", e.what());
    return false;
  }

  // ---------- decode + NMS ----------
  const std::vector<Detection> dets =
      DecodeDetections(regressors.data(), classificators.data(), anchors, 0.5f, 0.3f);
  if (dets.empty()) return false;  // no face in frame
  const Detection& det = dets[0];
  *face_score_out = det.score;

  // ---------- map the detection back to frame pixels ----------
  const Vec2 box_center_norm =
      lb.MapToFrame({det.cx * 128.0f, det.cy * 128.0f});
  const float box_w_px = det.w * 128.0f / lb.scale;
  const float box_h_px = det.h * 128.0f / lb.scale;
  const float box_cx_px = box_center_norm.x * src_w;
  const float box_cy_px = box_center_norm.y * src_h;
  const Vec2 kp0_norm = lb.MapToFrame({det.keypoints[0].x * 128.0f, det.keypoints[0].y * 128.0f});
  const Vec2 kp1_norm = lb.MapToFrame({det.keypoints[1].x * 128.0f, det.keypoints[1].y * 128.0f});
  const Vec2 kp0_px{kp0_norm.x * src_w, kp0_norm.y * src_h};
  const Vec2 kp1_px{kp1_norm.x * src_w, kp1_norm.y * src_h};
  *roi_out = ComputeAlignedRoi(box_cx_px, box_cy_px, box_w_px, box_h_px, kp0_px, kp1_px, 1.5f);
  return true;
}

AlignedRoi OrtLandmarkProvider::TrackRoiFromPreviousLandmarks_(int src_w, int src_h) const {
  Vec2 sum{};
  for (size_t i = 0; i < kLandmarkCount; ++i) sum = sum + prev_pts_[i];
  const float inv = 1.0f / static_cast<float>(kLandmarkCount);
  const Vec2 centroid{sum.x * inv, sum.y * inv};  // normalized [0,1]
  const Vec2 right_mid{(prev_pts_[33].x + prev_pts_[133].x) * 0.5f,
                       (prev_pts_[33].y + prev_pts_[133].y) * 0.5f};
  const Vec2 left_mid{(prev_pts_[362].x + prev_pts_[263].x) * 0.5f,
                      (prev_pts_[362].y + prev_pts_[263].y) * 0.5f};
  const float dx = (left_mid.x - right_mid.x) * static_cast<float>(src_w);
  const float dy = (left_mid.y - right_mid.y) * static_cast<float>(src_h);
  AlignedRoi roi = prev_roi_;  // side_px carried over
  roi.cx_px = (centroid.x + roi_center_offset_.x) * static_cast<float>(src_w);
  roi.cy_px = (centroid.y + roi_center_offset_.y) * static_cast<float>(src_h);
  roi.angle_rad = std::atan2(dy, dx);
  return roi;
}

void OrtLandmarkProvider::NoteMeshOutcome_(const LandmarkSet& result, const AlignedRoi& roi_used,
                                           int src_w, int src_h) {
  if (!result.valid) {
    ++lost_streak_;
    if (lost_streak_ >= 2) {
      have_prev_roi_ = false;
      pending_roi_anchor_ = false;
    }
    return;
  }
  lost_streak_ = 0;
  prev_pts_ = result.pts;
  prev_face_score_ = result.face_score;
  prev_roi_ = roi_used;
  have_prev_roi_ = true;
  if (pending_roi_anchor_) {
    Vec2 sum{};
    for (size_t i = 0; i < kLandmarkCount; ++i) sum = sum + prev_pts_[i];
    const float inv = 1.0f / static_cast<float>(kLandmarkCount);
    const Vec2 centroid{sum.x * inv, sum.y * inv};
    roi_center_offset_ = Vec2{roi_used.cx_px / static_cast<float>(src_w) - centroid.x,
                              roi_used.cy_px / static_cast<float>(src_h) - centroid.y};
    pending_roi_anchor_ = false;
    frames_since_detect_ = 0;
  } else {
    ++frames_since_detect_;
  }
}
}  // namespace kgcore
