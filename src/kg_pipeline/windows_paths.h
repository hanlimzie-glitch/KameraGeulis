// Shared Windows-only path helpers (kg_pipeline PUBLIC — used by both the
// landmark provider and the app selftest so the model search order is
// IDENTICAL everywhere).
//
// Model search order (ort_landmark_provider.cc + selftest.cc):
//   1) KAMERAGEULIS_MODELS_DIR env var (explicit override; authoritative —
//      the app downloads into it if files are missing and it is writable)
//   2) <exe_dir>\models (shipped inside the CI artifact since M2d — read-only)
//   3) %LOCALAPPDATA%\KameraGeulis\models (download cache; the app downloads
//      here when nothing usable is found next to the exe)
#pragma once
#include <string>
#include <windows.h>

namespace kgcore {

inline std::wstring KgExeDirW() {
  wchar_t buf[MAX_PATH]{};
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return {};
  std::wstring path(buf);
  const size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return {};
  return path.substr(0, slash + 1);
}

inline std::wstring KgEnvModelsDirW() {
  wchar_t buf[MAX_PATH]{};
  const DWORD n = GetEnvironmentVariableW(L"KAMERAGEULIS_MODELS_DIR", buf, MAX_PATH);
  if (n > 0 && n < MAX_PATH) return std::wstring(buf);
  return {};
}

inline std::wstring KgExeModelsDirW() {
  const std::wstring exe = KgExeDirW();
  if (exe.empty()) return {};
  return exe + L"models";
}

inline std::wstring KgLocalModelsDirW() {
  wchar_t buf[MAX_PATH]{};
  const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return {};
  return std::wstring(buf) + L"\\KameraGeulis\\models";
}

}  // namespace kgcore
