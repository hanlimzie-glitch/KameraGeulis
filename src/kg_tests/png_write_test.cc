#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>
#include "kgcore/png_write.h"

namespace fs = std::filesystem;

namespace {
std::vector<uint8_t> ReadAll(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                              std::istreambuf_iterator<char>());
}
uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
}  // namespace

TEST_CASE("png: crc32 matches the known zip vector") {
  // CRC32("123456789") = 0xCBF43926 (standard check vector).
  const char* s = "123456789";
  REQUIRE(kgcore::Crc32(reinterpret_cast<const uint8_t*>(s), 9) == 0xCBF43926u);
}

TEST_CASE("png: adler32 matches the known vector") {
  // Adler32("Wikipedia") = 0x11E60398.
  const char* s = "Wikipedia";
  REQUIRE(kgcore::Adler32(reinterpret_cast<const uint8_t*>(s), 9) == 0x11E60398u);
}

TEST_CASE("png: written file has valid signature, IHDR dims and IEND") {
  const fs::path out =
      fs::temp_directory_path() / "kg_png_write_test.png";
  // 3x2 RGBA image, recognizable pixels.
  uint8_t img[3 * 2 * 4];
  for (int i = 0; i < 3 * 2; ++i) {
    img[i * 4 + 0] = uint8_t(i * 10);
    img[i * 4 + 1] = uint8_t(i * 20);
    img[i * 4 + 2] = uint8_t(i * 30);
    img[i * 4 + 3] = 255;
  }
  REQUIRE(kgcore::WritePngRgba(out.string().c_str(), img, 3, 2));

  const auto bytes = ReadAll(out);
  REQUIRE(bytes.size() > 8 + 25 + 12);
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  REQUIRE(std::memcmp(bytes.data(), sig, 8) == 0);

  // IHDR: length 13, type IHDR, big-endian width/height.
  REQUIRE(Be32(&bytes[8]) == 13);
  REQUIRE(std::memcmp(&bytes[12], "IHDR", 4) == 0);
  REQUIRE(Be32(&bytes[16]) == 3);   // width
  REQUIRE(Be32(&bytes[20]) == 2);   // height
  REQUIRE(bytes[24] == 8);          // bit depth
  REQUIRE(bytes[25] == 6);          // color type RGBA
  // IHDR CRC validates over type+data.
  const uint32_t crc = kgcore::Crc32(&bytes[12], 4 + 13);
  REQUIRE(Be32(&bytes[29]) == crc);

  // File ends with IEND chunk (len 0, type IEND, CRC).
  const size_t n = bytes.size();
  REQUIRE(std::memcmp(&bytes[n - 8], "IEND", 4) == 0);
  REQUIRE(Be32(&bytes[n - 12]) == 0);  // IEND length
  const uint32_t iend_crc = kgcore::Crc32(&bytes[n - 8], 4);
  REQUIRE(Be32(&bytes[n - 4]) == iend_crc);

  // IDAT present between IHDR and IEND.
  REQUIRE(std::memcmp(&bytes[8 + 4 + 4 + 13 + 4 + 4], "IDAT", 4) == 0);

  std::error_code ec;
  fs::remove(out, ec);
}

TEST_CASE("png: RGB8 & Gray8 terkompresi DEFLATE jauh lebih kecil dari raw") {
  const int w = 320, h = 240;
  std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4, 255);
  std::vector<uint8_t> gray(static_cast<size_t>(w) * h, 0);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const size_t i = static_cast<size_t>(y) * w + x;
      rgba[i * 4 + 0] = static_cast<uint8_t>((x / 8) * 6);
      rgba[i * 4 + 1] = static_cast<uint8_t>((y / 8) * 8);
      rgba[i * 4 + 2] = 128;
      gray[i] = static_cast<uint8_t>((x + y) % 64);
    }
  }
  const fs::path p_rgb = fs::temp_directory_path() / "kg_png_rgb_test.png";
  const fs::path p_gray = fs::temp_directory_path() / "kg_png_gray_test.png";
  REQUIRE(kgcore::WritePngRgbFromRgba(p_rgb.string().c_str(), rgba.data(), w, h));
  REQUIRE(kgcore::WritePngGray(p_gray.string().c_str(), gray.data(), w, h));

  const auto b_rgb = ReadAll(p_rgb);
  const auto b_gray = ReadAll(p_gray);
  REQUIRE(b_rgb[25] == 2);   // color type RGB8
  REQUIRE(b_gray[25] == 0);  // color type Gray8
  REQUIRE(b_rgb.size() < static_cast<size_t>(w) * h * 3 / 4);
  REQUIRE(b_gray.size() < static_cast<size_t>(w) * h / 4);

  std::error_code ec;
  fs::remove(p_rgb, ec);
  fs::remove(p_gray, ec);
}
