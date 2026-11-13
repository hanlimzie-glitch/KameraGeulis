#include "kgcore/png_write.h"
#include <cstring>
#include <fstream>
#include <vector>

namespace kgcore {
namespace {
struct CrcTable {
  uint32_t t[256];
  CrcTable() {
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      t[n] = c;
    }
  }
};

void PutBe32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(static_cast<uint8_t>(x >> 24));
  v.push_back(static_cast<uint8_t>(x >> 16));
  v.push_back(static_cast<uint8_t>(x >> 8));
  v.push_back(static_cast<uint8_t>(x));
}

void WriteChunk(std::ofstream& f, const char type[4], const uint8_t* data,
                size_t len) {
  uint8_t head[8];
  head[0] = static_cast<uint8_t>(len >> 24);
  head[1] = static_cast<uint8_t>(len >> 16);
  head[2] = static_cast<uint8_t>(len >> 8);
  head[3] = static_cast<uint8_t>(len);
  std::memcpy(head + 4, type, 4);
  f.write(reinterpret_cast<const char*>(head), 8);
  if (len) f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
  // CRC over type + data
  static const CrcTable tab;
  uint32_t crc = 0xFFFFFFFFu;
  for (int i = 0; i < 4; ++i) crc = tab.t[(crc ^ static_cast<uint8_t>(type[i])) & 0xFF] ^ (crc >> 8);
  for (size_t i = 0; i < len; ++i) crc = tab.t[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  crc ^= 0xFFFFFFFFu;
  uint8_t c[4] = {static_cast<uint8_t>(crc >> 24), static_cast<uint8_t>(crc >> 16),
                  static_cast<uint8_t>(crc >> 8), static_cast<uint8_t>(crc)};
  f.write(reinterpret_cast<const char*>(c), 4);
}
}  // namespace

uint32_t Crc32(const uint8_t* data, size_t len, uint32_t seed) {
  static const CrcTable tab;
  uint32_t crc = ~seed;
  for (size_t i = 0; i < len; ++i) crc = tab.t[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

uint32_t Adler32(const uint8_t* data, size_t len) {
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < len; ++i) {
    a = (a + data[i]) % 65521u;
    b = (b + a) % 65521u;
  }
  return (b << 16) | a;
}

bool WritePngRgba(const char* path, const uint8_t* rgba, int width, int height) {
  if (!path || !rgba || width <= 0 || height <= 0) return false;
  const size_t row = static_cast<size_t>(width) * 4;

  // Raw scanlines with filter byte 0 (None) per row.
  std::vector<uint8_t> raw;
  raw.reserve((row + 1) * static_cast<size_t>(height));
  for (int y = 0; y < height; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgba + static_cast<size_t>(y) * row,
               rgba + static_cast<size_t>(y + 1) * row);
  }

  // zlib stream: 0x78 0x01 header + stored deflate blocks + adler32.
  std::vector<uint8_t> idat;
  idat.push_back(0x78);
  idat.push_back(0x01);
  size_t off = 0;
  while (off < raw.size()) {
    const size_t n = raw.size() - off > 0xFFFF ? 0xFFFF : raw.size() - off;
    const bool last = off + n >= raw.size();
    idat.push_back(last ? 0x01 : 0x00);  // BFINAL, BTYPE=00 (stored)
    idat.push_back(static_cast<uint8_t>(n & 0xFF));
    idat.push_back(static_cast<uint8_t>(n >> 8));
    const uint16_t nlen = static_cast<uint16_t>(~n);
    idat.push_back(static_cast<uint8_t>(nlen & 0xFF));
    idat.push_back(static_cast<uint8_t>(nlen >> 8));
    idat.insert(idat.end(), raw.begin() + static_cast<std::ptrdiff_t>(off),
                raw.begin() + static_cast<std::ptrdiff_t>(off + n));
    off += n;
  }
  const uint32_t adler = Adler32(raw.data(), raw.size());
  PutBe32(idat, adler);

  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  f.write(reinterpret_cast<const char*>(sig), 8);

  uint8_t ihdr[13];
  ihdr[0] = static_cast<uint8_t>(width >> 24);
  ihdr[1] = static_cast<uint8_t>(width >> 16);
  ihdr[2] = static_cast<uint8_t>(width >> 8);
  ihdr[3] = static_cast<uint8_t>(width);
  ihdr[4] = static_cast<uint8_t>(height >> 24);
  ihdr[5] = static_cast<uint8_t>(height >> 16);
  ihdr[6] = static_cast<uint8_t>(height >> 8);
  ihdr[7] = static_cast<uint8_t>(height);
  ihdr[8] = 8;   // bit depth
  ihdr[9] = 6;   // color type RGBA
  ihdr[10] = 0;  // compression
  ihdr[11] = 0;  // filter
  ihdr[12] = 0;  // interlace
  WriteChunk(f, "IHDR", ihdr, 13);
  WriteChunk(f, "IDAT", idat.data(), idat.size());
  WriteChunk(f, "IEND", nullptr, 0);
  f.close();
  return static_cast<bool>(f);
}
}  // namespace kgcore
