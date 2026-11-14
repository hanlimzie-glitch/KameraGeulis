#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <vector>
#include "kgcore/reader_fallback.h"

using kgcore::DefaultReaderLadder;
using kgcore::kReaderBasic;
using kgcore::kReaderFull;
using kgcore::kReaderPlain;
using kgcore::MfErrorName;
using kgcore::RunReaderFallbackLadder;
using kgcore::SinkReadbackAllowed;
using kgcore::WantsCameraBusyHint;

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

// ---- M2g: tiruan objek IMFMediaSource yang MENINGGAL ----
// Meniru perilaku MF di PC pengguna (log-4.txt): pembuatan reader penuh
// gagal E_INVALIDARG, dan objek source yang sama lalu selalu membalas
// MF_E_SHUTDOWN - "by default ... the source reader shuts down the media
// source" (docs MS MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN).
constexpr int64_t kHrEInvalidArg = static_cast<int64_t>(0x80070057);
constexpr int64_t kHrShutdown = static_cast<int64_t>(0xC00D3E85);

// `source` = status objek: 0 = belum ada, 1 = hidup, kHrShutdown = mati.
// `fresh_per_level` = KODE BARU (objek segar per tingkat); false = kode lama
// (satu objek dipakai lintas tingkat).
// Return: 1 sukses, 0 gagal-lanjut, -1 gagal-berhenti.
int DeadSourceFactory(int64_t* source, bool fresh_per_level, int level,
                      int64_t* lhr) {
  if (fresh_per_level) {
    *source = 1;  // objek BARU tiap tingkat (aktivasi ulang)
  }
  if (*source == kHrShutdown) {
    *lhr = kHrShutdown;  // objek lama sudah mati - inilah yang terjadi di PC
    return 0;            // (kode lama: ladder lanjut ke tingkat berikut)
  }
  if (level == kReaderFull) {
    // Kombinasi atribut ditolak PC (E_INVALIDARG) DAN mematikan objeknya.
    *lhr = kHrEInvalidArg;
    *source = kHrShutdown;
    return 0;
  }
  *lhr = 0;
  return 1;
}
}  // namespace

// ---------------------------------------------------------------------------
// Tangga fallback - urutan M2g (basic dulu, penuh terakhir).
// ---------------------------------------------------------------------------

TEST_CASE("ladder: gagal basic -> mencoba polos, lalu penuh (usaha terakhir)") {
  FakeFactory f;
  f.fail_levels = {kReaderBasic};
  int64_t out_hr = -1;
  size_t n = 0;
  const int* levels = DefaultReaderLadder(&n);
  const int used = RunReaderFallbackLadder(
      levels, n,
      [&](int level, int64_t* hr) { return f.Attempt(level, hr); },
      [](int, int64_t, bool) {}, &out_hr);
  REQUIRE(used == kReaderPlain);
  REQUIRE(out_hr == 0);
  REQUIRE(f.recs.size() == 2);
  REQUIRE(f.recs[0].level == kReaderBasic);
  REQUIRE(f.recs[0].ok == false);
  REQUIRE(f.recs[1].level == kReaderPlain);
  REQUIRE(f.recs[1].ok == true);
}

TEST_CASE("ladder: gagal basic+polos -> memakai penuh sebagai terakhir") {
  FakeFactory f;
  f.fail_levels = {kReaderBasic, kReaderPlain};
  int64_t out_hr = -1;
  size_t n = 0;
  const int* levels = DefaultReaderLadder(&n);
  const int used = RunReaderFallbackLadder(
      levels, n,
      [&](int level, int64_t* hr) { return f.Attempt(level, hr); },
      [](int, int64_t, bool) {}, &out_hr);
  REQUIRE(used == kReaderFull);
  REQUIRE(f.recs.size() == 3);
  REQUIRE(f.recs[2].level == kReaderFull);
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

TEST_CASE("ladder: tingkat pertama (basic) langsung sukses -> satu percobaan") {
  FakeFactory f;  // fail_levels kosong = semua sukses
  int64_t out_hr = -1;
  size_t n = 0;
  const int* levels = DefaultReaderLadder(&n);
  const int used = RunReaderFallbackLadder(
      levels, n,
      [&](int level, int64_t* hr) { return f.Attempt(level, hr); },
      [](int, int64_t, bool) {}, &out_hr);
  REQUIRE(used == kReaderBasic);  // urutan M2g: basic duluan (konfigurasi M2d)
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

TEST_CASE("ladder: urutan default M2g - basic, polos, penuh (paling akhir)") {
  size_t n = 0;
  const int* levels = DefaultReaderLadder(&n);
  REQUIRE(n == 3);
  REQUIRE(levels[0] == kReaderBasic);  // konfigurasi M2d yang terbukti di PC
  REQUIRE(levels[1] == kReaderPlain);
  REQUIRE(levels[2] == kReaderFull);   // usaha terakhir (kombinasi = 0x80070057)
}

// ---------------------------------------------------------------------------
// M2g requirement 5: objek yang MENINGGAL setelah kegagalan.
// ---------------------------------------------------------------------------

TEST_CASE("M2g: pola LAMA (objek dipakai ulang) mereproduksi log-4 PC") {
  // Kode lama = urutan lama {penuh, basic, polos} + satu objek source untuk
  // semua tingkat -> persis urutan hr di log-4.txt pengguna:
  //   tingkat 2 (penuh)  0x80070057
  //   tingkat 1 (basic)  0xC00D3E85
  //   tingkat 0 (polos)  0xC00D3E85
  const int old_order[] = {kReaderFull, kReaderBasic, kReaderPlain};
  int64_t source = 1;  // objek hidup
  std::vector<AttemptRec> recs;
  int64_t out_hr = 0;
  const int used = RunReaderFallbackLadder(
      old_order, 3,
      [&](int level, int64_t* hr) {
        return DeadSourceFactory(&source, /*fresh_per_level=*/false, level, hr);
      },
      [&](int level, int64_t hr, bool ok) { recs.push_back({level, hr, ok}); },
      &out_hr);
  REQUIRE(used == -1);  // pola lama GAGAL TOTAL - inilah yang terjadi di PC
  REQUIRE(recs.size() == 3);
  REQUIRE(recs[0].level == kReaderFull);
  REQUIRE(recs[0].hr == kHrEInvalidArg);
  REQUIRE(recs[1].level == kReaderBasic);
  REQUIRE(recs[1].hr == kHrShutdown);  // objek sudah mati - basic tak pernah
  REQUIRE(recs[2].level == kReaderPlain);
  REQUIRE(recs[2].hr == kHrShutdown);  // dicoba di objek yang sama
  REQUIRE(out_hr == kHrShutdown);
}

TEST_CASE("M2g: pola BARU (objek segar per tingkat) -> basic langsung sukses") {
  // Kode baru = urutan default M2g + activate objek BARU tiap tingkat:
  // tingkat basic (konfigurasi M2d) langsung jalan, tanpa E_INVALIDARG,
  // tanpa MF_E_SHUTDOWN. Test INI gagal bila dijalankan terhadap kode lama
  // (urutan lama mencoba penuh dulu -> dua percobaan + hr salah).
  int64_t source = 0;  // belum ada objek
  int attempts = 0;
  int64_t out_hr = -1;
  size_t n = 0;
  const int* levels = DefaultReaderLadder(&n);
  const int used = RunReaderFallbackLadder(
      levels, n,
      [&](int level, int64_t* hr) {
        ++attempts;
        return DeadSourceFactory(&source, /*fresh_per_level=*/true, level, hr);
      },
      [](int, int64_t, bool) {}, &out_hr);
  REQUIRE(used == kReaderBasic);
  REQUIRE(attempts == 1);  // kode lama: 2+ percobaan (penuh dulu, gagal)
  REQUIRE(out_hr == 0);
  REQUIRE(source == 1);  // objek tetap hidup setelah sukses
}

// ---------------------------------------------------------------------------
// M2g requirement 4: nama simbolis + petunjuk sibuk hanya untuk kode yang
// memang bermakna "dipakai aplikasi lain / akses diblokir".
// ---------------------------------------------------------------------------

TEST_CASE("M2g: MfErrorName memberi nama hanya untuk kode yang dikenal") {
  REQUIRE(MfErrorName(0x80070057) == std::string("E_INVALIDARG"));
  REQUIRE(MfErrorName(0x80004005) == std::string("E_FAIL"));
  REQUIRE(MfErrorName(0x80070005) == std::string("E_ACCESSDENIED"));
  REQUIRE(MfErrorName(0x80070020) == std::string("ERROR_SHARING_VIOLATION"));
  REQUIRE(MfErrorName(0x80070490) == std::string("ERROR_NOT_FOUND"));
  REQUIRE(MfErrorName(0xC00D3E85) == std::string("MF_E_SHUTDOWN"));
  REQUIRE(std::string(MfErrorName(0x12345678)).empty());  // tak dikenal
}

TEST_CASE("M2g: petunjuk sibuk HANYA untuk E_ACCESSDENIED/SHARING_VIOLATION") {
  REQUIRE(WantsCameraBusyHint(0x80070005));  // E_ACCESSDENIED: izin diblokir
  REQUIRE(WantsCameraBusyHint(0x80070020));  // ERROR_SHARING_VIOLATION: dipakai
  // BUKAN untuk kegagahan konfigurasi/khidup-mati objek:
  REQUIRE_FALSE(WantsCameraBusyHint(0x80070057));  // E_INVALIDARG (log-4 tingkat 2)
  REQUIRE_FALSE(WantsCameraBusyHint(0xC00D3E85));  // MF_E_SHUTDOWN (log-4 tingkat 1/0)
  REQUIRE_FALSE(WantsCameraBusyHint(0x80004005));  // E_FAIL
  REQUIRE_FALSE(WantsCameraBusyHint(0x80070490));  // ERROR_NOT_FOUND
}

// ---------------------------------------------------------------------------
// M2f guard readback (dipertahankan M2g).
// ---------------------------------------------------------------------------

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
