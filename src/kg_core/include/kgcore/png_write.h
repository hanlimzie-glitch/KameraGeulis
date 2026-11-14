#pragma once
#include <cstddef>
#include <cstdint>

namespace kgcore {
// Minimal, dependency-free PNG writer (RGBA8, no compression - zlib stored
// blocks). Valid PNG: any viewer opens it. Used by the F5 debug dump.
// `rgba` is tightly packed top-down (stride = width*4), alpha included.
// Returns false on I/O failure or invalid arguments.
bool WritePngRgba(const char* path, const uint8_t* rgba, int width, int height);

// CRC32 (PNG/zip polynomial) over data - exposed for tests.
uint32_t Crc32(const uint8_t* data, size_t len, uint32_t seed = 0);
// Adler-32 over data - exposed for tests.
uint32_t Adler32(const uint8_t* data, size_t len);
}  // namespace kgcore
