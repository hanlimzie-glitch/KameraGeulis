// M2k — uji matematika layer mask (Linux): urutan union→pengecualian→ambang,
// gerbang warna tajam, kompensasi motion UV seg, laporan diff F5 (per rentang
// & per kelas), dan sifat luar-mask byte-identik pada komposisi referensi.
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "kgcore/mask_layers.h"
#include "kgcore/skin_mask.h"

using namespace kgcore;

TEST_CASE("mask_layers: gerbang warna — dalam radius=1, luar radius*(1+span)=0") {
  ColorGate g;
  g.cb0 = 128; g.cr0 = 140; g.radius = 25; g.span = 0.35f;
  REQUIRE(GateValue(g, 128, 140) == 1.0f);
  REQUIRE(GateValue(g, 128 + 25, 140) == 1.0f);
  float mid = GateValue(g, 128 + 25 + 25 * 0.35f * 0.5f, 140);
  REQUIRE((mid > 0.2f && mid < 0.8f));
  REQUIRE(GateValue(g, 128 + 25 * 1.35f + 0.5f, 140) == 0.0f);
  REQUIRE(GateValue(g, 128 + 30, 140 + 30) == 0.0f);
}

TEST_CASE("mask_layers: union dulu, exclusion sesudahnya, lalu hard threshold") {
  const int mw = 8, mh = 8;
  std::vector<float> face(mw * mh, 0.0f), seg_p(mw * mh, 0.0f),
      seg_g(mw * mh, 1.0f), excl(mw * mh, 0.0f), gate(mw * mh, 1.0f);
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 4; ++x) face[y * mw + x] = 1.0f;
  for (int y = 4; y < 8; ++y)
    for (int x = 4; x < 8; ++x) seg_p[y * mw + x] = 1.0f;
  for (int y = 0; y < 2; ++y)
    for (int x = 0; x < 2; ++x) excl[y * mw + x] = 1.0f;
  excl[5 * mw + 5] = 1.0f;
  ComposeParams prm;
  std::vector<uint8_t> mask;
  ComposeMaskCpu(face.data(), seg_p.data(), seg_g.data(), excl.data(),
                 gate.data(), mw, mh, prm, &mask);
  REQUIRE(mask[0] == 0);
  REQUIRE(mask[2 * mw + 2] == 255);
  REQUIRE(mask[5 * mw + 4] == 191);
  REQUIRE(mask[5 * mw + 5] == 0);
  REQUIRE(mask[6 * mw + 6] == 191);
  REQUIRE(mask[7 * mw] == 0);
  REQUIRE(mask[3 * mw + 7] == 0);
}

TEST_CASE("mask_layers: nilai di bawah hard threshold dipaksa nol persis") {
  const int mw = 4, mh = 4;
  std::vector<float> face(mw * mh, 0.015f);
  ComposeParams prm;
  std::vector<uint8_t> mask;
  ComposeMaskCpu(face.data(), nullptr, nullptr, nullptr, nullptr, mw, mh, prm,
                 &mask);
  for (auto v : mask) REQUIRE(v == 0);
  face[0] = 0.5f;
  ComposeMaskCpu(face.data(), nullptr, nullptr, nullptr, nullptr, mw, mh, prm,
                 &mask);
  REQUIRE(mask[0] == 128);
  REQUIRE(mask[1] == 0);
}

TEST_CASE("mask_layers: UV shift seg mengompensasi gerak wajah") {
  Letterbox lb;
  lb.scale = 0.125f; lb.pad_x = 8; lb.pad_y = 8;
  lb.src_w = 1920; lb.src_h = 1080;
  float du, dv;
  SegUvShift(lb, 40.0f, 0.0f, &du, &dv);
  REQUIRE(std::fabs(du - (40.0f * 0.125f) / 256.0f) < 1e-6f);
  REQUIRE(std::fabs(dv) < 1e-9f);
  SegUvShift(lb, 0.0f, -30.0f, &du, &dv);
  REQUIRE(std::fabs(dv - (-30.0f * 0.125f) / 256.0f) < 1e-6f);
}

TEST_CASE("mask_layers: F5 diff dilaporkan per rentang dan kelas") {
  const int w = 8, h = 8, mw = 8, mh = 8;
  std::vector<uint8_t> raw(w * h * 4, 100), out(w * h * 4, 140),
      mask(mw * mh, 0), cls(w * h, 3);
  for (int i = 0; i < mw * mh / 2; ++i) mask[i] = 255;
  F5DiffReport rep;
  ComputeF5DiffReport(raw.data(), out.data(), mask.data(), mw, mh, w, h, 1.0f,
                      cls.data(), &rep);
  REQUIRE(rep.per_range[0].n == 32);
  REQUIRE(rep.per_range[0].sum > 0);
  REQUIRE(rep.per_range[4].n == 32);
  REQUIRE(rep.outside_n == 32);
  REQUIRE(std::fabs(rep.outside_mean - 40.0) < 1e-6);
  REQUIRE(rep.per_class[3].n == 64);
  REQUIRE(rep.per_class[0].n == 0);
}

TEST_CASE("mask_layers: diff amplifikasi untuk diff PNG") {
  const int w = 4, h = 2;
  std::vector<uint8_t> raw(w * h * 4, 50), out(w * h * 4, 60), rgba;
  WriteDiffAmplified(raw.data(), out.data(), w, h, 8.0f, &rgba);
  REQUIRE(rgba.size() == static_cast<size_t>(w) * h * 4);
  REQUIRE(rgba[0] == 80);
  REQUIRE(rgba[3] == 255);

  // M2l: diff grayscale downsampled 1920x1080 -> 960x540
  std::vector<uint8_t> r2(1920 * 1080 * 4, 40), o2(1920 * 1080 * 4, 45), gray;
  WriteDiffAmplifiedGray(r2.data(), o2.data(), 1920, 1080, 8.0f, 960, 540, &gray);
  REQUIRE(gray.size() == 960u * 540u);
  REQUIRE(gray[0] == 40);
}

TEST_CASE("mask_layers: di luar <8/255 hasil byte-identik raw; mix di dalam linear") {
  const int w = 4, h = 2;
  std::vector<uint8_t> raw(w * h * 4), beauty(w * h * 4), mask(w * h, 0), out(w * h * 4);
  for (int i = 0; i < w * h; ++i) {
    raw[i * 4 + 0] = raw[i * 4 + 1] = raw[i * 4 + 2] = 100;
    raw[i * 4 + 3] = 255;
    beauty[i * 4 + 0] = beauty[i * 4 + 1] = beauty[i * 4 + 2] = 200;
    beauty[i * 4 + 3] = 255;
  }
  mask[0] = 7;   // kurang dari 8/255: harus passthrough persis
  mask[1] = 128; // blend tengah
  mask[2] = 255; // blend penuh
  BeautyComposeConfig cfg;
  cfg.smooth_amount = 0.0f;
  cfg.chroma_retain = 1.0f;
  ComposeBeautyRaw(raw.data(), beauty.data(), mask.data(), w, h, w, h, 1.0f,
                   cfg, out.data());
  for (int c = 0; c < 4; ++c) REQUIRE(out[c] == raw[c]);
  REQUIRE((out[4] >= 149 && out[4] <= 151));
  REQUIRE((out[8] >= 199 && out[8] <= 201));
  for (int i = 3; i < w * h; ++i)
    for (int c = 0; c < 3; ++c) REQUIRE(out[i * 4 + c] == raw[i * 4 + c]);
}
