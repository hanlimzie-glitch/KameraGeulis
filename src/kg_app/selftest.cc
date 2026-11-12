// KameraGeulis — headless smoke test (--selftest).
// No window, no GL, no MessageBox. Loads the two runtime DLLs from the app
// folder, verifies the ORT API, loads both models from KAMERAGEULIS_MODELS_DIR
// (or <exe_dir>\models), runs one detector + one mesh inference on a
// deterministic synthetic input, checks output shapes/finiteness, and exits:
//   0  pass
//   2  DLL preload failed (app-local Dll is missing/broken/shadowed)
//   3  ORT API mismatch/unusable
//   4  model missing / size / SHA-256 mismatch
//   5  ORT session creation failed
//   6  inference failed or output shape/NaN mismatch
// Everything is logged to the log file AND stderr (`2> log.txt` still works).
#include <windows.h>
#include <bcrypt.h>

#include <onnxruntime_cxx_api.h>
#include <onnxruntime_c_api.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "kgcore/log.h"
#include "startup.h"

namespace kgcore {
namespace {

// Model provenance (docs/LANDMARKS.md §2.3) — must match the app provider pins.
constexpr wchar_t kDetFile[] = L"face_detection_short_range.onnx";
constexpr wchar_t kMeshFile[] = L"face_mesh.onnx";
constexpr char kDetSha256[] =
    "82ac962b24dfd78a5b6a3331e8c4091e377d699c6e4fdaca158fba03b5af8e3d";
constexpr char kMeshSha256[] =
    "ac72f85b77368a2ee1d065257674cb25099699d98376685af549df50d6a40337";
constexpr uint64_t kDetBytes = 418502;
constexpr uint64_t kMeshBytes = 2429059;

std::wstring ModelDir_(const StartupPaths& paths) {
  wchar_t override_dir[MAX_PATH]{};
  const DWORD n = GetEnvironmentVariableW(L"KAMERAGEULIS_MODELS_DIR", override_dir, MAX_PATH);
  if (n > 0 && n < MAX_PATH) return std::wstring(override_dir);
  return paths.exe_dir + L"models";
}

std::string Sha256HexOfFile(const std::wstring& path, bool* ok) {
  *ok = false;
  BCRYPT_ALG_HANDLE hAlg = nullptr;
  if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
    return {};
  }
  DWORD objLen = 0, cb = 0;
  BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objLen), sizeof(objLen),
                    &cb, 0);
  std::vector<UCHAR> hash_object(objLen);
  BCRYPT_HASH_HANDLE hHash = nullptr;
  if (BCryptCreateHash(hAlg, &hHash, hash_object.data(), objLen, nullptr, 0, 0) != 0) {
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return {};
  }
  std::ifstream in(path, std::ios::binary);
  std::vector<uint8_t> buf(1 << 16);
  bool read_ok = true;
  while (in) {
    in.read(reinterpret_cast<char*>(buf.data()), buf.size());
    const std::streamsize n2 = in.gcount();
    if (n2 > 0 &&
        BCryptHashData(hHash, buf.data(), static_cast<ULONG>(n2), 0) != 0) {
      read_ok = false;
      break;
    }
  }
  BYTE hash[32]{};
  const bool fin_ok = (BCryptFinishHash(hHash, hash, sizeof(hash), 0) == 0);
  BCryptDestroyHash(hHash);
  BCryptCloseAlgorithmProvider(hAlg, 0);
  if (!read_ok || !fin_ok) return {};
  char hex[65]{};
  for (int i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02x", hash[i]);
  *ok = true;
  return std::string(hex, 64);
}

bool CheckModelFile_(const std::wstring& path, const char* expect_sha256, uint64_t expect_bytes,
                     const wchar_t* label) {
  const DWORD attrs = GetFileAttributesW(path.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES) {
    kgcore::Log("selftest", "model %ls TIDAK ADA: %ls", label, path.c_str());
    return false;
  }
  LARGE_INTEGER size{};
  HANDLE h =
      CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE || !GetFileSizeEx(h, &size)) {
    kgcore::Log("selftest", "model %ls tidak bisa dibaca: %ls", label, path.c_str());
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    return false;
  }
  CloseHandle(h);
  kgcore::Log("selftest", "model %ls: %lld bytes (diharapkan %llu)", label, size.QuadPart,
              expect_bytes);
  if (static_cast<uint64_t>(size.QuadPart) != expect_bytes) {
    kgcore::Log("selftest", "model %ls: ukuran tidak cocok", label);
    return false;
  }
  bool ok = false;
  const std::string sha = Sha256HexOfFile(path, &ok);
  if (!ok) {
    kgcore::Log("selftest", "model %ls: SHA-256 gagal dihitung", label);
    return false;
  }
  kgcore::Log("selftest", "model %ls: sha256 %s (%s)", label, sha.c_str(),
              sha == expect_sha256 ? "cocok" : "TIDAK COCOK");
  return sha == expect_sha256;
}

template <typename T>
bool AllFinite_(const T* data, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (!std::isfinite(static_cast<float>(data[i]))) return false;
  }
  return true;
}

}  // namespace

int RunSelftest(const StartupPaths& paths) {
  kgcore::Log("selftest", "=== KameraGeulis selftest ===");

  // [1] DLLs (app-local; not System32). PreloadRuntimeDlls logs both paths.
  const DllPreloadResult pre = PreloadRuntimeDlls(paths);
  if (!pre.all_ok) {
    // detail is wide; log via stderr too (already logged inside).
    std::wstring d = pre.detail;
    std::string du;
    const int n = WideCharToMultiByte(CP_UTF8, 0, d.c_str(), (int)d.size(), nullptr, 0, nullptr, nullptr);
    du.resize(static_cast<size_t>(n));
    WideCharToMultiByte(CP_UTF8, 0, d.c_str(), (int)d.size(), du.data(), n, nullptr, nullptr);
    kgcore::Log("selftest", "GAGAL [dll-preload]: %s", du.c_str());
    kgcore::Log("selftest", "=== SELFTEST FAIL (2) ===");
    EndFileLog();
    return 2;
  }
  kgcore::Log("selftest", "DLL OK: onnxruntime=%s gpupixel=(dimuat, tidak diinisialisasi GL)",
              pre.ort_version.c_str());

  // [2] ORT API is usable: OrtGetApiBase must match pins (1.31.x).
  const OrtApiBase* base = OrtGetApiBase();
  const OrtApi* api = base ? base->GetApi(ORT_API_VERSION) : nullptr;
  if (!base || !api) {
    kgcore::Log("selftest", "GAGAL [ort-api]: OrtGetApiBase/GetApi(ORT_API_VERSION=%u) gagal",
                ORT_API_VERSION);
    kgcore::Log("selftest", "=== SELFTEST FAIL (3) ===");
    EndFileLog();
    return 3;
  }
  kgcore::Log("selftest", "OrtGetApiBase OK — version '%s', ORT_API_VERSION %u",
              base->GetVersionString() ? base->GetVersionString() : "?", ORT_API_VERSION);

  // [3] Models present + verified (CI downloads them into <exe>\models;
  //     KAMERAGEULIS_MODELS_DIR overrides).
  const std::wstring dir = ModelDir_(paths);
  kgcore::Log("selftest", "direktori model: %ls", dir.c_str());
  const std::wstring det_path = dir + L"\\" + kDetFile;
  const std::wstring mesh_path = dir + L"\\" + kMeshFile;
  if (!CheckModelFile_(det_path, kDetSha256, kDetBytes, kDetFile) ||
      !CheckModelFile_(mesh_path, kMeshSha256, kMeshBytes, kMeshFile)) {
    kgcore::Log("selftest", "=== SELFTEST FAIL (4) ===");
    EndFileLog();
    return 4;
  }

  // [4] ORT environment + sessions (CPU provider).
  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "kg_selftest");
  Ort::SessionOptions opts;
  opts.SetIntraOpNumThreads(1);
  opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
  std::unique_ptr<Ort::Session> det, mesh;
  try {
    det = std::make_unique<Ort::Session>(env, det_path.c_str(), opts);
    mesh = std::make_unique<Ort::Session>(env, mesh_path.c_str(), opts);
  } catch (const Ort::Exception& e) {
    kgcore::Log("selftest", "GAGAL [ort-session]: %s", e.what());
    kgcore::Log("selftest", "=== SELFTEST FAIL (5) ===");
    EndFileLog();
    return 5;
  }
  kgcore::Log("selftest", "ORT sessions OK (det: %zu inputs, mesh: %zu inputs)",
              det->GetInputCount(), mesh->GetInputCount());

  // [5] One inference per model with a deterministic synthetic image.
  const Ort::MemoryInfo mem =
      Ort::MemoryInfo::CreateCpu(OrtAllocatorType::OrtArenaAllocator, OrtMemType::OrtMemTypeDefault);
  // Deterministic gradient pattern (non-trivial values; exact content unimportant).
  auto fill = [](std::vector<float>* v) {
    for (size_t i = 0; i < v->size(); ++i) {
      const size_t x = i % 192, y = (i / 192) % 192, c = i % 3;
      (*v)[i] = (static_cast<float>((x * 7 + y * 13 + c * 3) % 251) / 250.0f) * 2.0f - 1.0f;
    }
  };
  try {
    // ---- detector: [1,128,128,3] -> regressors[1,896,16], classificators[1,896,1]
    std::vector<float> det_input(static_cast<size_t>(1) * 128 * 128 * 3);
    fill(&det_input);
    const int64_t det_shape[4] = {1, 128, 128, 3};
    Ort::Value det_tensor = Ort::Value::CreateTensor<float>(
        mem, det_input.data(), det_input.size(), det_shape, 4);
    const char* det_in[] = {"input"};
    const char* det_out[] = {"regressors", "classificators"};
    Ort::RunOptions run_opts;
    auto det_out_values = det->Run(run_opts, det_in, &det_tensor, 1, det_out, 2);
    const size_t reg_n = det_out_values[0].GetTensorTypeAndShapeInfo().GetElementCount();
    const size_t cls_n = det_out_values[1].GetTensorTypeAndShapeInfo().GetElementCount();
    kgcore::Log("selftest", "det: regressors numel=%zu classificators numel=%zu", reg_n, cls_n);
    if (reg_n != 896 * 16 || cls_n != 896) {
      kgcore::Log("selftest", "GAGAL [det-shape]: diharapkan 896x16 & 896, dapat %zu & %zu",
                  reg_n, cls_n);
      kgcore::Log("selftest", "=== SELFTEST FAIL (6) ===");
      EndFileLog();
      return 6;
    }
    const float* reg = det_out_values[0].GetTensorData<float>();
    const float* cls = det_out_values[1].GetTensorData<float>();
    if (!AllFinite_(reg, reg_n) || !AllFinite_(cls, cls_n)) {
      kgcore::Log("selftest", "GAGAL [det-finite]: output mengandung NaN/Inf");
      kgcore::Log("selftest", "=== SELFTEST FAIL (6) ===");
      EndFileLog();
      return 6;
    }

    // ---- mesh: [1,192,192,3] -> conv2d_21[1,1,1,1404], conv2d_31[1,1,1,1]
    std::vector<float> mesh_input(static_cast<size_t>(1) * 192 * 192 * 3);
    fill(&mesh_input);
    const int64_t mesh_shape[4] = {1, 192, 192, 3};
    Ort::Value mesh_tensor = Ort::Value::CreateTensor<float>(
        mem, mesh_input.data(), mesh_input.size(), mesh_shape, 4);
    const char* mesh_in[] = {"input_1"};
    const char* mesh_out[] = {"conv2d_21", "conv2d_31"};
    auto mesh_out_values = mesh->Run(run_opts, mesh_in, &mesh_tensor, 1, mesh_out, 2);
    const size_t mesh0_n = mesh_out_values[0].GetTensorTypeAndShapeInfo().GetElementCount();
    const size_t mesh1_n = mesh_out_values[1].GetTensorTypeAndShapeInfo().GetElementCount();
    kgcore::Log("selftest", "mesh: conv2d_21 numel=%zu conv2d_31 numel=%zu", mesh0_n, mesh1_n);
    if (mesh0_n != 1404 || mesh1_n != 1) {
      kgcore::Log("selftest", "GAGAL [mesh-shape]: diharapkan 1404 & 1, dapat %zu & %zu",
                  mesh0_n, mesh1_n);
      kgcore::Log("selftest", "=== SELFTEST FAIL (6) ===");
      EndFileLog();
      return 6;
    }
    const float* mesh0 = mesh_out_values[0].GetTensorData<float>();
    const float* mesh1 = mesh_out_values[1].GetTensorData<float>();
    kgcore::Log("selftest", "mesh: conv2d_31 value=%.6f", mesh1[0]);
    if (!AllFinite_(mesh0, mesh0_n) || !std::isfinite(mesh1[0])) {
      kgcore::Log("selftest", "GAGAL [mesh-finite]: output mengandung NaN/Inf");
      kgcore::Log("selftest", "=== SELFTEST FAIL (6) ===");
      EndFileLog();
      return 6;
    }
  } catch (const Ort::Exception& e) {
    kgcore::Log("selftest", "GAGAL [inference]: %s", e.what());
    kgcore::Log("selftest", "=== SELFTEST FAIL (6) ===");
    EndFileLog();
    return 6;
  }

  // [6] GPUPixel cannot be initialised without a GL context — preloaded only.
  kgcore::Log("selftest", "GPUPixel: DLL ter-preload; inisialisasi GL/GPUPixel TIDAK diuji di sini "
                           "(butuh GPU + konteks OpenGL — belum ada di lingkungan dev/CI)");
  kgcore::Log("selftest", "=== SELFTEST PASS ===");
  EndFileLog();
  return 0;
}

}  // namespace kgcore
