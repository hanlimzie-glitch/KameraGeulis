#include "seg_provider.h"

#include <onnxruntime_cxx_api.h>

#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "kgcore/log.h"
#include "windows_paths.h"

namespace kgcore {
namespace {
constexpr char kTag[] = "seg";

// File ONNX hasil konversi CI (tf2onnx opset 17) — TIDAK pernah di-commit;
// diunduh dari rilis `models-v1` pada pemakaian pertama.
// Sumber: TFLITE resmi
// https://storage.googleapis.com/mediapipe-models/image_segmenter/
//   selfie_multiclass_256x256/float32/1/selfie_multiclass_256x256.tflite
// SHA-256 TFLITE (diverifikasi di CI terhadap 4 sumber independen):
//   c6748b1253a99067ef71f7e26ca71096cd449baefa8f101900ea23016507e0e0
// SHA-256 ONNX diisi SETELAH konversi pertama CI (lihat STATUS.md M2j) —
// selama placeholder, file diterima dengan PERINGATAN jujur di log.
constexpr wchar_t kSegFile[] = L"selfie_multiclass_256x256.onnx";
constexpr char kSegOnnxSha256[] = "TODO_ONNX_SHA256_SET_AFTER_CI";
constexpr wchar_t kDownloadBase[] = L"github.com";
constexpr wchar_t kDownloadPathPrefix[] =
    L"/hanlimzie-glitch/KameraGeulis/releases/download/models-v1/";
constexpr wchar_t kUserAgent[] =
    L"KameraGeulis/0.2 (+https://github.com/hanlimzie-glitch/KameraGeulis)";

// Cadangan thread: target 10-15 Hz => jangan inferensi lebih cepat dari ini.
constexpr int kMinPeriodMs = 66;  // ~15 Hz

std::string WideToUtf8(const std::wstring& s) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                    nullptr, 0, nullptr, nullptr);
  std::string out(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n,
                      nullptr, nullptr);
  return out;
}

std::string Sha256HexOfFile(const std::wstring& path, bool* ok) {
  *ok = false;
  BCRYPT_ALG_HANDLE hAlg = nullptr;
  if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) !=
      0) {
    return {};
  }
  DWORD objLen = 0, cb = 0;
  BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH,
                    reinterpret_cast<PUCHAR>(&objLen), sizeof(objLen), &cb, 0);
  std::vector<UCHAR> hash_object(objLen);
  BCRYPT_HASH_HANDLE hHash = nullptr;
  if (BCryptCreateHash(hAlg, &hHash, hash_object.data(), objLen, nullptr, 0,
                       0) != 0) {
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return {};
  }
  std::ifstream in(path.c_str(), std::ios::binary);
  std::vector<uint8_t> buf(1 << 16);
  bool read_ok = true;
  while (in) {
    in.read(reinterpret_cast<char*>(buf.data()), buf.size());
    const std::streamsize n = in.gcount();
    if (n > 0 && BCryptHashData(hHash, buf.data(), static_cast<ULONG>(n), 0) !=
                     0) {
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

bool PinIsPlaceholder() {
  return std::strncmp(kSegOnnxSha256, "TODO", 4) == 0;
}

bool EnsureDirectoryExists(const std::wstring& dir) {
  size_t pos = 0;
  while (pos != std::wstring::npos) {
    pos = dir.find_first_of(L"\\/", pos);
    const std::wstring part =
        (pos == std::wstring::npos) ? dir : dir.substr(0, pos);
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
}  // namespace

SegProvider::SegProvider() = default;

SegProvider::~SegProvider() { Stop(); }

void SegProvider::Start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) return;
  stop_.store(false);
  th_ = std::thread([this] { ThreadMain_(); });
}

void SegProvider::Stop() {
  if (!running_.load()) return;
  stop_.store(true);
  cv_.notify_all();
  if (th_.joinable()) th_.join();
  running_.store(false);
}

std::string SegProvider::LastError() const {
  std::lock_guard<std::mutex> lk(err_mu_);
  return last_error_;
}

void SegProvider::SubmitFrame(const uint8_t* pixels, int w, int h,
                              int stride_bytes, bool bgra,
                              uint64_t frame_serial) {
  if (dead_.load()) return;  // gagal permanen: hemat, pemanggil pakai wajah
  if (!pixels || w <= 0 || h <= 0) return;
  LetterboxInfo lb = ComputeSegLetterbox(w, h, kSegInputSize);
  std::vector<float> input(static_cast<size_t>(kSegInputSize) *
                           kSegInputSize * 3);
  LetterboxToInput(pixels, w, h, stride_bytes, bgra, lb, input.data(),
                   kSegInputSize);
  std::lock_guard<std::mutex> lk(mu_);
  if (slot_pending_) dropped_.fetch_add(1);  // frame lama tertimpa
  slot_input_ = std::move(input);
  slot_lb_ = lb;
  slot_serial_ = frame_serial;
  slot_pending_ = true;
  cv_.notify_one();
}

bool SegProvider::GetResult(std::vector<float>* probs, LetterboxInfo* lb,
                            uint64_t* frame_serial, double* infer_ms) const {
  std::lock_guard<std::mutex> lk(mu_);
  if (!res_valid_) return false;
  if (probs) *probs = res_probs_;
  if (lb) *lb = res_lb_;
  if (frame_serial) *frame_serial = res_serial_;
  if (infer_ms) *infer_ms = res_ms_;
  return true;
}

void SegProvider::ThreadMain_() {
  // Muat model DI THREAD (unduh pertama kali tidak memblokir UI/render).
  const bool ok = EnsureModelLoaded_();
  {
    std::lock_guard<std::mutex> lk(err_mu_);
    if (ok) {
      last_error_ = "ok";
    } else if (last_error_.empty() || last_error_ == "belum dimuat") {
      last_error_ = "gagal memuat model segmentasi";
    }
  }
  available_.store(ok);

  Ort::Session* session = ok ? session_.get() : nullptr;
  Ort::MemoryInfo mem =
      Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

  std::vector<float> input;
  LetterboxInfo lb{};
  uint64_t serial = 0;
  while (true) {
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [&] { return stop_.load() || slot_pending_; });
      if (stop_.load()) break;
      input = std::move(slot_input_);
      lb = slot_lb_;
      serial = slot_serial_;
      slot_pending_ = false;
    }

    if (!session) continue;  // model gagal: buang frame (fallback muka saja)

    const auto t0 = std::chrono::steady_clock::now();
    try {
      constexpr size_t kIn = static_cast<size_t>(kSegInputSize) *
                            kSegInputSize * 3;
      constexpr size_t kOut = static_cast<size_t>(kSegInputSize) *
                              kSegInputSize * kSegClasses;
      std::array<int64_t, 4> in_shape{1, kSegInputSize, kSegInputSize, 3};
      Ort::Value in_tensor = Ort::Value::CreateTensor<float>(
          mem, input.data(), kIn, in_shape.data(), 4);

      // Nama input/output diambil dinamis (tf2onnx bisa berganti nama).
      Ort::AllocatorWithDefaultOptions alloc;
      auto in_name = session->GetInputNameAllocated(0, alloc);
      auto out_name = session->GetOutputNameAllocated(0, alloc);
      const char* in_names[] = {in_name.get()};
      const char* out_names[] = {out_name.get()};
      Ort::RunOptions run_opts;
      auto out = session->Run(run_opts, in_names, &in_tensor, 1, out_names, 1);
      float* out_data = out.front().GetTensorMutableData<float>();
      const auto out_info = out.front().GetTensorTypeAndShapeInfo();
      const size_t out_count = out_info.GetElementCount();
      if (out_count != kOut) {
        std::lock_guard<std::mutex> lk(err_mu_);
        last_error_ = "bentuk keluaran seg tak terduga (" +
                      std::to_string(out_count) + " != 65536)";
        available_.store(false);
        dead_.store(true);
        session = nullptr;
        continue;
      }
      std::vector<float> probs(out_data, out_data + kOut);
      // Softmax DI LUAR MODEL bila keluarannya logit (deteksi otomatis).
      SegActivate(probs.data(),
                  static_cast<int>(kOut / kSegClasses));
      const auto t1 = std::chrono::steady_clock::now();
      const double ms =
          std::chrono::duration<double, std::milli>(t1 - t0).count();
      {
        std::lock_guard<std::mutex> lk(mu_);
        res_probs_ = std::move(probs);
        res_lb_ = lb;
        res_serial_ = serial;
        res_ms_ = ms;
        res_valid_ = true;
      }
      last_ms_.store(ms);
      inferred_.fetch_add(1);
      // Cadangi kecepatan ke ~15 Hz (jangan membanjiri CPU).
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - t0)
                               .count();
      if (elapsed < kMinPeriodMs) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(kMinPeriodMs - elapsed));
      }
    } catch (const Ort::Exception& e) {
      std::lock_guard<std::mutex> lk(err_mu_);
      last_error_ = std::string("inferensi seg gagal: ") + e.what();
      Log(kTag, "%s (fallback: mask wajah saja)", last_error_.c_str());
      session = nullptr;  // hentikan percobaan lanjutan (jujur, tanpa crash)
      available_.store(false);
      dead_.store(true);
    }
  }
}

bool SegProvider::EnsureModelLoaded_() {
  try {
    const std::wstring env = KgEnvModelsDirW();
    const std::wstring exe = KgExeModelsDirW();
    const std::wstring local = KgLocalModelsDirW();
    auto present_verified = [](const std::wstring& dir) {
      if (dir.empty()) return false;
      const std::wstring p = dir + L"\\" + kSegFile;
      if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;
      if (PinIsPlaceholder()) return true;  // belum dipin -> terima + nanti warn
      return FilesEqualHash(p, kSegOnnxSha256);
    };
    std::wstring dir;
    if (present_verified(env)) dir = env;
    else if (present_verified(exe)) dir = exe;
    else if (present_verified(local)) dir = local;
    else dir = !env.empty() ? env : local;  // target unduh

    if (dir.empty()) {
      std::lock_guard<std::mutex> lk(err_mu_);
      last_error_ = "folder model tidak tersedia";
      dead_.store(true);
      return false;
    }
    if (!EnsureDirectoryExists(dir)) {
      std::lock_guard<std::mutex> lk(err_mu_);
      last_error_ = "tidak bisa membuat folder model: " + WideToUtf8(dir);
      dead_.store(true);
      return false;
    }
    if (!EnsureModelFile_(dir, kSegFile, kSegOnnxSha256)) {
      dead_.store(true);  // last_error_ sudah diisi
      return false;
    }
    env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "KameraGeulisSeg");
    Ort::SessionOptions opts;
    opts.SetIntraOpNumThreads(1);
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    session_ = std::make_unique<Ort::Session>(*env_,
                                              (dir + L"\\" + kSegFile).c_str(),
                                              opts);
    Log(kTag, "ORT seg siap (%ls)", dir.c_str());
    return true;
  } catch (const Ort::Exception& e) {
    std::lock_guard<std::mutex> lk(err_mu_);
    last_error_ = std::string("ORT session seg error: ") + e.what();
    session_.reset();
    env_.reset();
    dead_.store(true);
    return false;
  }
}

bool SegProvider::EnsureModelFile_(const std::wstring& dir,
                                   const wchar_t* file_name,
                                   const char* sha256_hex) {
  const std::wstring dest = dir + L"\\" + file_name;
  const bool pinned = !PinIsPlaceholder();
  if (GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES) {
    if (!pinned) {
      bool ok = false;
      const std::string actual = Sha256HexOfFile(dest, &ok);
      if (ok) {
        Log(kTag,
            "seg ONNX ada; SHA-256 (belum dipin - catat untuk STATUS): %s",
            actual.c_str());
      }
      return true;
    }
    if (FilesEqualHash(dest, sha256_hex)) return true;
    Log(kTag, "seg %ls ada tapi SHA-256 mismatch — unduh ulang", file_name);
  }
  Log(kTag, "mengunduh %ls dari rilis models-v1 …", file_name);
  if (!DownloadFile_(file_name, dest)) {
    std::lock_guard<std::mutex> lk(err_mu_);
    last_error_ = std::string("unduh gagal untuk ") + WideToUtf8(file_name) +
                  " (cek koneksi / rilis models-v1)";
    dead_.store(true);
    return false;
  }
  if (pinned && !FilesEqualHash(dest, sha256_hex)) {
    std::lock_guard<std::mutex> lk(err_mu_);
    bool ok = false;
    const std::string actual = Sha256HexOfFile(dest, &ok);
    last_error_ = "SHA-256 ONNX seg tidak cocok (dihitung: " +
                  (ok ? actual : std::string("?")) + ")";
    DeleteFileW(dest.c_str());
    dead_.store(true);
    return false;
  }
  if (!pinned) {
    bool ok = false;
    const std::string actual = Sha256HexOfFile(dest, &ok);
    Log(kTag,
        "seg ONNX terunduh; SHA-256 (BELUM DIPIN — perbarui kSegOnnxSha256): %s",
        ok ? actual.c_str() : "?");
  }
  return true;
}

bool SegProvider::DownloadFile_(const wchar_t* host_path,
                                const std::wstring& dest) {
  const std::wstring tmp = dest + L".tmp";
  HINTERNET hSession =
      WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hSession) return false;
  std::wstring host = kDownloadBase;
  std::wstring path = std::wstring(kDownloadPathPrefix) + host_path;
  bool ok = false;
  for (int redirect = 0; redirect < 6 && !ok; ++redirect) {
    HINTERNET hConnect =
        WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) break;
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) {
      WinHttpCloseHandle(hConnect);
      break;
    }
    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hRequest, nullptr)) {
      WinHttpCloseHandle(hRequest);
      WinHttpCloseHandle(hConnect);
      break;
    }
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(hRequest,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                        WINHTTP_NO_HEADER_INDEX);
    if (status >= 300 && status < 400) {
      DWORD locSize = 0;
      WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION,
                          WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &locSize,
                          WINHTTP_NO_HEADER_INDEX);
      std::wstring location(locSize / sizeof(wchar_t), L'\0');
      WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION,
                          WINHTTP_HEADER_NAME_BY_INDEX, location.data(),
                          &locSize, WINHTTP_NO_HEADER_INDEX);
      location.resize(wcslen(location.c_str()));
      WinHttpCloseHandle(hRequest);
      WinHttpCloseHandle(hConnect);
      const std::wstring https_prefix = L"https://";
      if (location.compare(0, https_prefix.size(), https_prefix) != 0) break;
      const size_t slash = location.find(L'/', https_prefix.size());
      if (slash == std::wstring::npos) break;
      host = location.substr(https_prefix.size(),
                             slash - https_prefix.size());
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
      if (!WinHttpReadData(hRequest, buf.data(), static_cast<DWORD>(buf.size()),
                           &read) ||
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

}  // namespace kgcore
