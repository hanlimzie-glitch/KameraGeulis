#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>
#include "kgcore/virtual_camera.h"

namespace {

// Buat gambar RGBA top-down dengan piksel unik tiap sudut:
// (0,0)=Merah, (W-1,0)=Hijau, (0,H-1)=Biru, (W-1,H-1)=Putih.
std::vector<uint8_t> MakeCornerRgba(int w, int h) {
  std::vector<uint8_t> buf(static_cast<size_t>(w) * h * 4, 0);
  auto set = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    size_t idx = (static_cast<size_t>(y) * w + x) * 4;
    buf[idx + 0] = r;
    buf[idx + 1] = g;
    buf[idx + 2] = b;
    buf[idx + 3] = 255;
  };
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      set(x, y, 40, 80, 120);
    }
  }
  set(0, 0, 250, 10, 20);
  set(w - 1, 0, 10, 240, 30);
  set(0, h - 1, 20, 30, 245);
  set(w - 1, h - 1, 255, 255, 255);
  return buf;
}

}  // namespace

TEST_CASE("M3: konversi RGBA -> BGR24 top-down menjaga orientasi, warna, dan mirror") {
  const int w = 4, h = 4;
  const auto src = MakeCornerRgba(w, h);
  std::vector<uint8_t> bgr;

  SECTION("1:1 tanpa flip dan tanpa mirror") {
    REQUIRE(kgcore::ConvertRgbaToVcamFormat(
        src.data(), w, h, w * 4, w, h, kgcore::VcamPixelFormat::kBgr24TopDown,
        /*flip_vertical=*/false, /*mirror_horizontal=*/false, &bgr));
    REQUIRE(bgr.size() == static_cast<size_t>(w * h * 3));
    // (0,0) RGBA (250,10,20) -> BGR (20,10,250)
    REQUIRE(bgr[0] == 20);
    REQUIRE(bgr[1] == 10);
    REQUIRE(bgr[2] == 250);
    // (w-1,0) RGBA (10,240,30) -> BGR (30,240,10)
    const size_t tr = static_cast<size_t>(w - 1) * 3;
    REQUIRE(bgr[tr + 0] == 30);
    REQUIRE(bgr[tr + 1] == 240);
    REQUIRE(bgr[tr + 2] == 10);
    // (0,h-1) RGBA (20,30,245) -> BGR (245,30,20)
    const size_t bl = static_cast<size_t>(h - 1) * w * 3;
    REQUIRE(bgr[bl + 0] == 245);
    REQUIRE(bgr[bl + 1] == 30);
    REQUIRE(bgr[bl + 2] == 20);
  }

  SECTION("flip_vertical=true membalik baris atas dan bawah") {
    REQUIRE(kgcore::ConvertRgbaToVcamFormat(
        src.data(), w, h, w * 4, w, h, kgcore::VcamPixelFormat::kBgr24TopDown,
        /*flip_vertical=*/true, /*mirror_horizontal=*/false, &bgr));
    // Baris 0 sekarang berasal dari (0, h-1) = (20, 30, 245) -> BGR (245, 30, 20)
    REQUIRE(bgr[0] == 245);
    REQUIRE(bgr[1] == 30);
    REQUIRE(bgr[2] == 20);
    // Baris h-1 sekarang berasal dari (0, 0) = (250, 10, 20) -> BGR (20, 10, 250)
    const size_t bl = static_cast<size_t>(h - 1) * w * 3;
    REQUIRE(bgr[bl + 0] == 20);
    REQUIRE(bgr[bl + 1] == 10);
    REQUIRE(bgr[bl + 2] == 250);
  }

  SECTION("mirror_horizontal=true membalik kiri dan kanan") {
    REQUIRE(kgcore::ConvertRgbaToVcamFormat(
        src.data(), w, h, w * 4, w, h, kgcore::VcamPixelFormat::kBgr24TopDown,
        /*flip_vertical=*/false, /*mirror_horizontal=*/true, &bgr));
    // (0,0) menjadi (w-1,0) = (10,240,30) -> BGR (30,240,10)
    REQUIRE(bgr[0] == 30);
    REQUIRE(bgr[1] == 240);
    REQUIRE(bgr[2] == 10);
    // (w-1,0) menjadi (0,0) = (250,10,20) -> BGR (20,10,250)
    const size_t tr = static_cast<size_t>(w - 1) * 3;
    REQUIRE(bgr[tr + 0] == 20);
    REQUIRE(bgr[tr + 1] == 10);
    REQUIRE(bgr[tr + 2] == 250);
  }
}

TEST_CASE("M3: konversi BGRA32 & NV12 dan letterbox resolusi berbeda") {
  // Sumber potret 4x8 (mis. kamera diputar 90 derajat) -> target lanskap 16x8.
  std::vector<uint8_t> portrait(4 * 8 * 4, 0);
  for (size_t i = 0; i < portrait.size(); i += 4) {
    portrait[i + 0] = 200;
    portrait[i + 1] = 150;
    portrait[i + 2] = 100;
    portrait[i + 3] = 255;
  }
  const auto box = kgcore::ComputeLetterboxRect(4, 8, 16, 8);
  REQUIRE(box.w == 4);
  REQUIRE(box.h == 8);
  REQUIRE(box.x == 6);
  REQUIRE(box.y == 0);

  std::vector<uint8_t> bgra;
  REQUIRE(kgcore::ConvertRgbaToVcamFormat(
      portrait.data(), 4, 8, 4 * 4, 16, 8,
      kgcore::VcamPixelFormat::kBgra32TopDown, false, false, &bgra));
  REQUIRE(bgra.size() == 16 * 8 * 4);
  // Area pillarbox kiri (x=0) hitam alpha 255
  REQUIRE(bgra[0] == 0);
  REQUIRE(bgra[1] == 0);
  REQUIRE(bgra[2] == 0);
  REQUIRE(bgra[3] == 255);
  // Area tengah (x=7) terisi piksel sumber (BGR = 100, 150, 200)
  const size_t mid = (static_cast<size_t>(4) * 16 + 7) * 4;
  REQUIRE(bgra[mid + 0] == 100);
  REQUIRE(bgra[mid + 1] == 150);
  REQUIRE(bgra[mid + 2] == 200);
  REQUIRE(bgra[mid + 3] == 255);

  std::vector<uint8_t> nv12;
  REQUIRE(kgcore::ConvertRgbaToVcamFormat(
      portrait.data(), 4, 8, 4 * 4, 16, 8,
      kgcore::VcamPixelFormat::kNv12TopDown, false, false, &nv12));
  REQUIRE(nv12.size() == 16 * 8 * 3 / 2);
  // Y di luar kotak = 16 (hitam BT.601), Y di dalam kotak > 100
  REQUIRE(nv12[0] == 16);
  REQUIRE(nv12[4 * 16 + 7] > 100);
}

TEST_CASE("M3: VcamFramePacer menjaga FPS konstan (ulang saat ~20fps, drop saat >30fps, PTS monoton)") {
  kgcore::VcamFramePacer pacer(30);
  REQUIRE(pacer.FrameIntervalUs() == 33333);

  std::vector<uint8_t> fr1(4 * 4 * 4, 11);
  std::vector<uint8_t> fr2(4 * 4 * 4, 22);
  std::vector<uint8_t> fr3(4 * 4 * 4, 33);

  // Kamera fisik mengirim frame pertama pada t=0, lalu tidak ada frame baru selama 100 ms (~10-20 fps).
  REQUIRE_FALSE(pacer.SubmitFrame(fr1.data(), 4, 4, 0));

  std::vector<uint8_t> out;
  int ow = 0, oh = 0;
  int64_t pts = -1, cap_ts = -1;
  bool repeat = true;

  // Tick 1: t=0 -> frame asli #1
  REQUIRE(pacer.PollFrame(0, &out, &ow, &oh, &pts, &cap_ts, &repeat));
  REQUIRE_FALSE(repeat);
  REQUIRE(out[0] == 11);
  REQUIRE(pts == 0);

  // Terlalu cepat (t=10 ms) -> belum waktunya
  REQUIRE_FALSE(pacer.PollFrame(10000, &out, &ow, &oh, &pts, &cap_ts, &repeat));

  // Tick 2: t=33.4 ms -> belum ada frame kamera baru -> ulang frame #1
  const int64_t prev_pts = pts;
  REQUIRE(pacer.PollFrame(33400, &out, &ow, &oh, &pts, &cap_ts, &repeat));
  REQUIRE(repeat);
  REQUIRE(out[0] == 11);
  REQUIRE(pts > prev_pts);

  // Tick 3: t=67 ms -> ulang lagi
  const int64_t prev_pts2 = pts;
  REQUIRE(pacer.PollFrame(67000, &out, &ow, &oh, &pts, &cap_ts, &repeat));
  REQUIRE(repeat);
  REQUIRE(pts > prev_pts2);
  REQUIRE(pacer.FramesSent() == 3);
  REQUIRE(pacer.FramesRepeated() == 2);

  // Sumber mengirim 2 frame cepat sebelum tick berikutnya -> drop-oldest
  REQUIRE_FALSE(pacer.SubmitFrame(fr2.data(), 4, 4, 80000));
  REQUIRE(pacer.SubmitFrame(fr3.data(), 4, 4, 90000));  // menimpa fr2
  REQUIRE(pacer.FramesDropped() == 1);

  REQUIRE(pacer.PollFrame(101000, &out, &ow, &oh, &pts, &cap_ts, &repeat));
  REQUIRE_FALSE(repeat);
  REQUIRE(out[0] == 33);
  REQUIRE(cap_ts == 90000);
}

TEST_CASE("M3: logika status kamera virtual & normalisasi konfigurasi") {
  const auto s0 = kgcore::ComputeVcamStatus(false, false, -1, false);
  REQUIRE(s0.state == kgcore::VcamDriverState::kNotRegistered);
  REQUIRE(std::string(s0.status_key) == "vcam.status_not_registered");

  const auto s1 = kgcore::ComputeVcamStatus(true, false, 0, false);
  REQUIRE(s1.state == kgcore::VcamDriverState::kRegisteredIdle);
  REQUIRE(std::string(s1.status_key) == "vcam.status_registered_idle");

  const auto s2 = kgcore::ComputeVcamStatus(true, true, 1, false);
  REQUIRE(s2.state == kgcore::VcamDriverState::kStreaming);
  REQUIRE(std::string(s2.status_key) == "vcam.status_streaming");
  REQUIRE(s2.consumer_count == 1);

  kgcore::VcamConfig cfg{1283, 719, 120, false};
  kgcore::NormalizeVcamConfig(&cfg);
  REQUIRE(cfg.width == 1280);
  REQUIRE(cfg.height == 716);
  REQUIRE(cfg.fps == 60);
  REQUIRE_FALSE(cfg.mirror);
}
