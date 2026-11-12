#pragma once
// Minimal process-wide logger: stderr always a sink; optionally also a file
// (SetLogFile) that is flushed after EVERY line so a crash never loses the
// last messages. Cross-platform (kg_core is unit-tested on Linux).
// M5 replaces this with a proper file+level logger.
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace kgcore {

namespace detail {
struct LogState {
  std::mutex mu;
  std::FILE* file = nullptr;  // append mode; unbuffered writes
};
inline LogState& LogStateInstance() {
  static LogState s;
  return s;
}
#if defined(_WIN32)
inline std::wstring Utf8ToWide(const std::string& utf8) {
  if (utf8.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                                    nullptr, 0);
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), out.data(), n);
  return out;
}
#endif
}  // namespace detail

// Also write every Log() line to `path_utf8` (append; creates if missing).
// On failure stderr keeps working and the app continues (logging must never
// crash the app). Call as early as possible in startup.
inline void SetLogFile(const std::string& path_utf8) {
  auto& s = detail::LogStateInstance();
  std::lock_guard<std::mutex> lock(s.mu);
  if (s.file) {
    std::fclose(s.file);
    s.file = nullptr;
  }
#if defined(_WIN32)
  const std::wstring wpath = detail::Utf8ToWide(path_utf8);
  s.file = _wfopen(wpath.c_str(), L"ab");
#else
  s.file = std::fopen(path_utf8.c_str(), "ab");
#endif
  if (s.file) setvbuf(s.file, nullptr, _IONBF, 0);
}

// Flush + close the log file. Idempotent.
inline void CloseLogFile() {
  auto& s = detail::LogStateInstance();
  std::lock_guard<std::mutex> lock(s.mu);
  if (s.file) {
    std::fflush(s.file);
    std::fclose(s.file);
    s.file = nullptr;
  }
}

inline void Log(const char* tag, const char* fmt, ...) {
  auto& s = detail::LogStateInstance();
  std::lock_guard<std::mutex> lock(s.mu);

  std::fprintf(stderr, "[%s] ", tag);
  va_list args;
  va_start(args, fmt);
  std::vfprintf(stderr, fmt, args);
  va_end(args);
  std::fprintf(stderr, "\n");

  if (s.file) {
    std::time_t now = std::time(nullptr);
    std::tm tm_now{};
#if defined(_WIN32)
    localtime_s(&tm_now, &now);
#else
    localtime_r(&now, &tm_now);
#endif
    std::fprintf(s.file, "%02d:%02d:%02d [%s] ", tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec,
                 tag);
    va_list args2;
    va_start(args2, fmt);
    std::vfprintf(s.file, fmt, args2);
    va_end(args2);
    std::fprintf(s.file, "\n");
    std::fflush(s.file);  // every line flushed — survives a crash
  }
}

}  // namespace kgcore
