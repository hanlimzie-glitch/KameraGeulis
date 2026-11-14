#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>
#include "kgcore/reader_fallback.h"

using kgcore::DefaultReaderLadder;
using kgcore::kReaderBasic;
using kgcore::kReaderFull;
using kgcore::kReaderPlain;
using kgcore::RunReaderFallbackLadder;
using kgcore::SinkReadbackAllowed;

namespace {
struct AttemptRec {
  int level;
  int64_t hr;
  bool ok;
};

// Pabrik palsu: gagal pada tingkat di `fail_levels`, sukses di lainnya
// (hr sukses = 0; hr gagal = fail_hr).
struct FakeFactory {
  std::vector<int> fail_levels;
  int64_t fail_hr = 0x80070057;
  std::vector<AttemptRec> recs;

  bool Attempt(int level, int64_t* hr) {
    for (int f : fail_levels) {
      if (f == level) {
        *hr = fail_hr;
        recs.push_back({level, fail_hr, false});
        return false;
      }
    }
    *hr = 0;
    recs.push_back({level, 0, true});
    return true;
  }
  void OnAttempt(int level, int64_t hr, bool ok) { recs.push_back({level, hr, ok}); }
};
}  // namespace

TEST_CASE("ladder: gagal di tingkat penuh -> memakai basic (bukti PC pengguna)") {
  FakeFactory f;
  f.fail_levels = {kReaderFull};
  int64_t out_hr = -1;
  size_t n = 0;
  const int* levels = DefaultReaderLadder(&n);
  int used = -1;
  int log_count = 0;
  used = RunReaderFallbackLadder(
      levels, n,
      [&](int level, int64_t* hr) { return f.Attempt(level, hr); },
      [&](int, int64_t, bool) { ++log_count; }, &out_hr);
  REQUIRE(used == kReaderBasic);
  REQUIRE(out_hr == 0);  // hr sukses percobaan terakhir yang dipakai
  REQUIRE(log_count == 2);
  REQUIRE(f.recs.size() == 2);
  REQUIRE(f.recs[0].level == kReaderFull);
  REQUIRE(f.recs[0].ok == false);
  REQUIRE(f.recs[0].hr == 0x80070057);
  REQUIRE(f.recs[1].level == kReaderBasic);
  REQUIRE(f.recs[1].ok == true);
}

TEST_CASE("ladder: gagal penuh+basic -> memakai polos (tingkat c)") {
  FakeFactory f;
  f.fail_levels = {kReaderFull, kReaderBasic};
  int64_t out_hr = -1;
  size_t n = 0;
  const int* levels = DefaultReaderLadder(&n);
  const int used = RunReaderFallbackLadder(
      levels, n,
      [&](int level, int64_t* hr) { return f.Attempt(level, hr); },
      [](int, int64_t, bool) {}, &out_hr);
  REQUIRE(used == kReaderPlain);
  REQUIRE(f.recs.size() == 3);
  REQUIRE(f.recs[2].level == kReaderPlain);
  REQUIRE(f.recs[2].ok == true);
}

TEST_CASE("ladder: semua tingkat gagal -> -1 + hr terakhir") {
  FakeFactory f;
  f.fail_levels = {kReaderFull, kReaderBasic, kReaderPlain};
  f.fail_hr = 0x80070005;
  int64_t out_hr = 0;
  size_t n = 0;
  const int* levels = DefaultReaderLadder(&n);
  const int used = RunReaderFallbackLadder(
      levels, n,
      [&](int level, int64_t* hr) { return f.Attempt(level, hr); },
      [](int, int64_t, bool) {}, &out_hr);
  REQUIRE(used == -1);
  REQUIRE(out_hr == 0x80070005);  // hr percobaan TERAKHIR
  REQUIRE(f.recs.size() == 3);    // tiap tingkat dicoba persis sekali
}

TEST_CASE("ladder: tingkat penuh langsung sukses -> hanya satu percobaan") {
  FakeFactory f;  // fail_levels kosong = semua sukses
  int64_t out_hr = -1;
  size_t n = 0;
  const int* levels = DefaultReaderLadder(&n);
  const int used = RunReaderFallbackLadder(
      levels, n,
      [&](int level, int64_t* hr) { return f.Attempt(level, hr); },
      [](int, int64_t, bool) {}, &out_hr);
  REQUIRE(used == kReaderFull);
  REQUIRE(out_hr == 0);
  REQUIRE(f.recs.size() == 1);
}

TEST_CASE("ladder: --capture-plain hanya mencoba tingkat polos") {
  FakeFactory f;
  f.fail_levels = {kReaderBasic};  // irrelevant: tidak pernah dicoba
  f.fail_hr = 0x80070057;
  int64_t out_hr = 0;
  const int plain_only[] = {kReaderPlain};
  const int used = RunReaderFallbackLadder(
      plain_only, 1,
      [&](int level, int64_t* hr) { return f.Attempt(level, hr); },
      [](int, int64_t, bool) {}, &out_hr);
  REQUIRE(used == kReaderPlain);
  REQUIRE(f.recs.size() == 1);
  REQUIRE(f.recs[0].level == kReaderPlain);

  // Gagal -> -1, tanpa mencoba tingkat lain.
  FakeFactory g;
  g.fail_levels = {kReaderPlain};
  g.fail_hr = 0x80070057;
  const int used2 = RunReaderFallbackLadder(
      plain_only, 1,
      [&](int level, int64_t* hr) { return g.Attempt(level, hr); },
      [](int, int64_t, bool) {}, &out_hr);
  REQUIRE(used2 == -1);
  REQUIRE(out_hr == 0x80070057);
  REQUIRE(g.recs.size() == 1);
}

TEST_CASE("ladder: urutan default persis (a) penuh, (b) basic, (c) polos") {
  size_t n = 0;
  const int* levels = DefaultReaderLadder(&n);
  REQUIRE(n == 3);
  REQUIRE(levels[0] == kReaderFull);
  REQUIRE(levels[1] == kReaderBasic);
  REQUIRE(levels[2] == kReaderPlain);
}

TEST_CASE("readback guard: hanya setelah output ada + ukuran sink valid") {
  // Belum pernah frame (kasus F5 yang crash di PC pengguna).
  REQUIRE_FALSE(SinkReadbackAllowed(0, 0, 0));
  // Frame diproses tapi sink belum dirender (ukuran 0).
  REQUIRE_FALSE(SinkReadbackAllowed(0, 0, 1));
  REQUIRE_FALSE(SinkReadbackAllowed(1280, 0, 1));
  REQUIRE_FALSE(SinkReadbackAllowed(0, 720, 1));
  // Ukuran valid tapi belum pernah menghasilkan output.
  REQUIRE_FALSE(SinkReadbackAllowed(1280, 720, 0));
  // Kedua syarat terpenuhi -> readback diizinkan.
  REQUIRE(SinkReadbackAllowed(1280, 720, 1));
  REQUIRE(SinkReadbackAllowed(1280, 720, 100));
}
