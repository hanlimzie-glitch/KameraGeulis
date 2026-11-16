#pragma once
#include <cstddef>
#include <cstdint>

namespace kgcore {
// Minimal, dependency-free PNG writer dengan filter Sub + DEFLATE LZ77 +
// Fixed Huffman (kompresi tinggi tanpa zlib eksternal) untuk dump F5 agar
// ukuran file kecil. Semua path ditafsirkan UTF-8.
bool WritePngRgba(const char* path, const uint8_t* rgba, int width, int height);
bool WritePngRgbFromRgba(const char* path, const uint8_t* rgba, int width,
                         int height);
bool WritePngGray(const char* path, const uint8_t* gray, int width, int height);

// CRC32 (PNG/zip polynomial) over data - exposed for tests.
uint32_t Crc32(const uint8_t* data, size_t len, uint32_t seed = 0);
// Adler-32 over data - exposed for tests.
uint32_t Adler32(const uint8_t* data, size_t len);
}  // namespace kgcore
