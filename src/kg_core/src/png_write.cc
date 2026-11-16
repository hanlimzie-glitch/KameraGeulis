#include "kgcore/png_write.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
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
  static const CrcTable tab;
  uint32_t crc = 0xFFFFFFFFu;
  for (int i = 0; i < 4; ++i) crc = tab.t[(crc ^ static_cast<uint8_t>(type[i])) & 0xFF] ^ (crc >> 8);
  for (size_t i = 0; i < len; ++i) crc = tab.t[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  crc ^= 0xFFFFFFFFu;
  uint8_t c[4] = {static_cast<uint8_t>(crc >> 24), static_cast<uint8_t>(crc >> 16),
                  static_cast<uint8_t>(crc >> 8), static_cast<uint8_t>(crc)};
  f.write(reinterpret_cast<const char*>(c), 4);
}

struct BitWriter {
  std::vector<uint8_t> out;
  uint8_t cur = 0;
  int bits = 0;

  void WriteLsb(uint32_t v, int n) {
    for (int i = 0; i < n; ++i) {
      cur |= static_cast<uint8_t>(((v >> i) & 1u) << bits);
      if (++bits == 8) {
        out.push_back(cur);
        cur = 0;
        bits = 0;
      }
    }
  }
  void WriteMsb(uint32_t v, int n) {
    for (int i = n - 1; i >= 0; --i) {
      cur |= static_cast<uint8_t>(((v >> i) & 1u) << bits);
      if (++bits == 8) {
        out.push_back(cur);
        cur = 0;
        bits = 0;
      }
    }
  }
  void FlushByte() {
    if (bits > 0) {
      out.push_back(cur);
      cur = 0;
      bits = 0;
    }
  }
};

void WriteFixedLitLen(BitWriter& bw, int sym) {
  if (sym <= 143) {
    bw.WriteMsb(static_cast<uint32_t>(0x30 + sym), 8);
  } else if (sym <= 255) {
    bw.WriteMsb(static_cast<uint32_t>(0x190 + (sym - 144)), 9);
  } else if (sym <= 279) {
    bw.WriteMsb(static_cast<uint32_t>(sym - 256), 7);
  } else {
    bw.WriteMsb(static_cast<uint32_t>(0xC0 + (sym - 280)), 8);
  }
}

void EncodeLength(BitWriter& bw, int len) {
  static const int kBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,
                                15, 17, 19, 23, 27, 31, 35, 43, 51,  59,
                                67, 83, 99, 115, 131, 163, 195, 227, 258};
  static const int kExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
                                 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
                                 4, 4, 4, 4, 5, 5, 5, 5, 0};
  int idx = 28;
  for (int i = 0; i < 28; ++i) {
    if (len < kBase[i + 1]) {
      idx = i;
      break;
    }
  }
  WriteFixedLitLen(bw, 257 + idx);
  if (kExtra[idx] > 0) {
    bw.WriteLsb(static_cast<uint32_t>(len - kBase[idx]), kExtra[idx]);
  }
}

void EncodeDist(BitWriter& bw, int dist) {
  int dcode = 0;
  int extra_bits = 0;
  int base = 1;
  if (dist <= 4) {
    dcode = dist - 1;
    base = dist;
  } else {
    for (int dc = 4; dc < 30; ++dc) {
      const int k = dc / 2 - 1;
      const int b = 1 + ((2 + (dc & 1)) << k);
      const int span = 1 << k;
      if (dist < b + span) {
        dcode = dc;
        extra_bits = k;
        base = b;
        break;
      }
    }
  }
  bw.WriteMsb(static_cast<uint32_t>(dcode), 5);
  if (extra_bits > 0) {
    bw.WriteLsb(static_cast<uint32_t>(dist - base), extra_bits);
  }
}

// Kompresi zlib dengan DEFLATE LZ77 + Fixed Huffman (BTYPE=01).
std::vector<uint8_t> CompressZlibFixed(const uint8_t* raw, size_t n) {
  BitWriter bw;
  bw.out.reserve(n / 2 + 64);
  bw.out.push_back(0x78);  // CMF
  bw.out.push_back(0x9C);  // FLG (check mod 31 == 0)

  bw.WriteLsb(1, 1);  // BFINAL = 1
  bw.WriteLsb(1, 2);  // BTYPE = 01 (Fixed Huffman)

  constexpr int kHashBits = 15;
  constexpr int kHashSize = 1 << kHashBits;
  constexpr int kWindow = 32768;
  constexpr int kMaxChain = 12;
  std::vector<int> head(kHashSize, -1);
  std::vector<int> prev(n, -1);

  auto hash3 = [&](size_t p) -> uint32_t {
    uint32_t h = (static_cast<uint32_t>(raw[p]) << 10) ^
                 (static_cast<uint32_t>(raw[p + 1]) << 5) ^
                 static_cast<uint32_t>(raw[p + 2]);
    return (h * 2654435761u) >> (32 - kHashBits);
  };

  size_t pos = 0;
  while (pos < n) {
    int best_len = 0;
    int best_dist = 0;
    if (pos + 2 < n) {
      const uint32_t h = hash3(pos);
      int cand = head[h];
      prev[pos] = cand;
      head[h] = static_cast<int>(pos);

      int steps = 0;
      const int min_pos = pos > kWindow ? static_cast<int>(pos - kWindow) : 0;
      const int max_len = static_cast<int>(std::min<size_t>(258, n - pos));
      while (cand >= min_pos && steps++ < kMaxChain) {
        if (raw[cand + best_len] == raw[pos + best_len]) {
          int l = 0;
          while (l < max_len && raw[cand + l] == raw[pos + l]) ++l;
          if (l >= 3 && l > best_len) {
            best_len = l;
            best_dist = static_cast<int>(pos - cand);
            if (best_len >= 64) break;
          }
        }
        cand = prev[cand];
      }
    }

    if (best_len >= 3) {
      EncodeLength(bw, best_len);
      EncodeDist(bw, best_dist);
      // Masukkan posisi yang dilewati ke tabel hash untuk rasio kompresi lebih baik
      const size_t end = std::min(pos + static_cast<size_t>(best_len),
                                  n > 2 ? n - 2 : 0);
      for (size_t k = pos + 1; k < end; ++k) {
        const uint32_t h = hash3(k);
        prev[k] = head[h];
        head[h] = static_cast<int>(k);
      }
      pos += static_cast<size_t>(best_len);
    } else {
      WriteFixedLitLen(bw, raw[pos]);
      ++pos;
    }
  }

  WriteFixedLitLen(bw, 256);  // End of block
  bw.FlushByte();
  PutBe32(bw.out, Adler32(raw, n));
  return std::move(bw.out);
}

bool WritePngInternal(const char* path, int width, int height, int color_type,
                      int bpp, const uint8_t* pixels) {
  if (!path || !pixels || width <= 0 || height <= 0) return false;
  const size_t row = static_cast<size_t>(width) * static_cast<size_t>(bpp);

  // Scanline dengan filter PNG Sub (1) untuk menurunkan entropi piksel bertetangga.
  std::vector<uint8_t> raw((row + 1) * static_cast<size_t>(height));
  for (int y = 0; y < height; ++y) {
    uint8_t* dst = raw.data() + static_cast<size_t>(y) * (row + 1);
    const uint8_t* src = pixels + static_cast<size_t>(y) * row;
    dst[0] = 1;  // Sub filter
    for (size_t x = 0; x < row; ++x) {
      const uint8_t left = x >= static_cast<size_t>(bpp) ? src[x - bpp] : 0;
      dst[1 + x] = static_cast<uint8_t>(src[x] - left);
    }
  }

  const std::vector<uint8_t> idat = CompressZlibFixed(raw.data(), raw.size());

  std::ofstream f(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
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
  ihdr[8] = 8;
  ihdr[9] = static_cast<uint8_t>(color_type);
  ihdr[10] = 0;
  ihdr[11] = 0;
  ihdr[12] = 0;
  WriteChunk(f, "IHDR", ihdr, 13);
  WriteChunk(f, "IDAT", idat.data(), idat.size());
  WriteChunk(f, "IEND", nullptr, 0);
  f.close();
  return static_cast<bool>(f);
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
  return WritePngInternal(path, width, height, 6, 4, rgba);
}

bool WritePngRgbFromRgba(const char* path, const uint8_t* rgba, int width,
                         int height) {
  if (!path || !rgba || width <= 0 || height <= 0) return false;
  const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height);
  std::vector<uint8_t> rgb(n * 3);
  for (size_t i = 0; i < n; ++i) {
    rgb[i * 3 + 0] = rgba[i * 4 + 0];
    rgb[i * 3 + 1] = rgba[i * 4 + 1];
    rgb[i * 3 + 2] = rgba[i * 4 + 2];
  }
  return WritePngInternal(path, width, height, 2, 3, rgb.data());
}

bool WritePngGray(const char* path, const uint8_t* gray, int width, int height) {
  return WritePngInternal(path, width, height, 0, 1, gray);
}
}  // namespace kgcore
