#pragma once
#include <cstdio>
#include <cstdarg>

namespace kgcore {
// Minimal process-wide logger. M5 replaces this with a file+level logger;
// call sites stay the same.
inline void Log(const char* tag, const char* fmt, ...) {
  std::fprintf(stderr, "[%s] ", tag);
  va_list args;
  va_start(args, fmt);
  std::vfprintf(stderr, fmt, args);
  va_end(args);
  std::fprintf(stderr, "\n");
}
}  // namespace kgcore
