#pragma once
#include <cstddef>
#include <cstdint>

namespace kgcore {

// M2f/M2g — tangga fallback pembuatan IMFSourceReader + guard readback
// GPUPixel + nama HRESULT untuk UI. Logika murni (diuji di Linux dengan
// pabrik palsu); pemakaian Windows ada di kg_capture/camera_capture.cc dan
// kg_pipeline/beauty_pipeline.cc.
//
// Dokumentasi Microsoft (repo MicrosoftDocs/win32, diakses 2026-11-14):
//  - mf-source-reader-enable-video-processing.md:
//    UINT32, nonzero = enable. Bila TRUE: MF_SOURCE_READER_D3D_MANAGER dan
//    MF_READWRITE_DISABLE_CONVERTERS harus FALSE.
//  - mf-source-reader-enable-advanced-video-processing.md:
//    BOOL (UINT32). Bila TRUE: MF_READWRITE_DISABLE_CONVERTERS harus FALSE;
//    "similar to MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, but offers the
//    following advantages" (konversi lebih luas, MFT lokal, GPU/XVP).
//  - source-reader-attributes.md: kedua atribut sah untuk
//    MFCreateSourceReaderFromMediaSource.
// Dokumen TIDAK melarang memakai keduanya; MF_READWRITE_DISABLE_CONVERTERS
// tidak pernah kita set (default FALSE = selalu memenuhi syarat). Namun di PC
// pengguna kombinasi (a) menghasilkan E_INVALIDARG (0x80070057) — bukti empiris
// -> turun tingkat dengan HRESULT tiap percobaan dicatat (bukan tebakan).

// Tingkat atribut, dari paling lengkap ke paling polos:
constexpr int kReaderPlain = 0;  // (c) tanpa atribut
constexpr int kReaderBasic = 1;  // (b) hanya ENABLE_VIDEO_PROCESSING (terbukti di PC pengguna, M2d)
constexpr int kReaderFull = 2;   // (a) ENABLE_VIDEO_PROCESSING + ENABLE_ADVANCED_VIDEO_PROCESSING

// Urutan default tangga (M2g, diubah setelah bukti PC log-4.txt):
//   (b) basic dulu - konfigurasi M2d yang TERBUKTI jalan di PC pengguna;
//   (c) polos - fallback aman;
//   (a) penuh - usaha terakhir saja (kombinasinya ditolak 0x80070057 di PC itu).
// Alasan urutan lama (a) dulu berbahaya: pembuatan reader yang GAGAL masih
// me-release reader internal-nya, dan "by default, when the application
// releases the source reader, the source reader shuts down the media source"
// (docs MS: MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN) -> sumber
// mati -> tingkat berikutnya di objek yang sama = MF_E_SHUTDOWN.
inline const int* DefaultReaderLadder(size_t* count) {
  static const int kOrder[] = {kReaderBasic, kReaderPlain, kReaderFull};
  *count = 3;
  return kOrder;
}

// Menjalankan attempt() untuk tiap tingkat berurutan sampai ada yang sukses.
//   attempt(level, &hr) -> true bila berhasil; WAJIB mengisi *hr
//                          (HRESULT, 0..0xFFFFFFFF, untuk log bila gagal).
//   on_attempt(level, hr, ok) -> callback log per percobaan.
// Mengembalikan tingkat yang dipakai, atau -1 bila semua gagal
// (*out_hr = hr percobaan terakhir; nullptr = abaikan).
template <typename AttemptFn, typename LogFn>
int RunReaderFallbackLadder(const int* levels, size_t count, AttemptFn&& attempt,
                            LogFn&& on_attempt, int64_t* out_hr) {
  int64_t last_hr = 0;
  for (size_t i = 0; i < count; ++i) {
    int64_t hr = 0;
    const bool ok = attempt(levels[i], &hr);
    last_hr = hr;
    on_attempt(levels[i], hr, ok);
    if (ok) {
      if (out_hr) *out_hr = hr;
      return levels[i];
    }
  }
  if (out_hr) *out_hr = last_hr;
  return -1;
}

// Guard readback GPUPixel (M2f): SinkRawData::GetRgbaBuffer() me-run
// glReadPixels di framebuffer yang baru dibuat pada Render() pertama — sebelum
// itu framebuffer_ null dan readback crash (0xC0000005, bukti pengguna F5).
// width_/height_ sink terisi bersamaan dengan pembuatan framebuffer, jadi
// aman hanya bila ukuran sink valid DAN pipeline sudah menghasilkan output.
inline bool SinkReadbackAllowed(int sink_w, int sink_h, uint64_t frames_produced) {
  return sink_w > 0 && sink_h > 0 && frames_produced > 0;
}

// Nama tingkat untuk log (ASCII).
inline const char* ReaderLevelName(int level) {
  switch (level) {
    case kReaderFull: return "penuh (basic+advanced)";
    case kReaderBasic: return "basic (ENABLE_VIDEO_PROCESSING)";
    case kReaderPlain: return "polos (tanpa atribut)";
    default: return "tidak dikenal";
  }
}

// Nama simbolis HRESULT yang kita kenal ("" = tidak dikenal). Hanya kode
// yang yakin artinya - jangan menebak nama untuk kode yang tidak pasti.
inline const char* MfErrorName(int64_t hr) {
  switch (static_cast<uint32_t>(hr)) {
    case 0x80070057u: return "E_INVALIDARG";
    case 0x80004005u: return "E_FAIL";
    case 0x80070005u: return "E_ACCESSDENIED";
    case 0x80070020u: return "ERROR_SHARING_VIOLATION";
    case 0x80070490u: return "ERROR_NOT_FOUND";
    case 0xC00D3E85u: return "MF_E_SHUTDOWN";
    default: return "";
  }
}

// Petunjuk "dipakai aplikasi lain atau akses kamera diblokir" HANYA untuk kode
// yang memang bermakna itu (M2g requirement 4):
//  - E_ACCESSDENIED (0x80070005): akses ditolak - mis. izin privasi kamera
//    Windows diblokir (dokumentasi Win32: "Access denied").
//  - ERROR_SHARING_VIOLATION (0x80070020): sumber daya dipakai proses lain
//    (dokumentasi Win32: sharing violation).
// MF tidak mendokumentasikan HRESULT khusus "kamera dipakai aplikasi lain";
// kode lain (E_INVALIDARG, MF_E_SHUTDOWN, ...) TIDAK menampilkan petunjuk ini.
inline bool WantsCameraBusyHint(int64_t hr) {
  const uint32_t c = static_cast<uint32_t>(hr);
  return c == 0x80070005u || c == 0x80070020u;
}
}  // namespace kgcore
