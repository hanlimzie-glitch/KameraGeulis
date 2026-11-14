#pragma once
#include <cstddef>
#include <cstdint>

namespace kgcore {

// M2f — tangga fallback pembuatan IMFSourceReader + guard readback GPUPixel.
// Logika murni (diuji di Linux dengan pabrik palsu); pemakaian Windows ada di
// kg_capture/camera_capture.cc dan kg_pipeline/beauty_pipeline.cc.
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

// Urutan default tangga: (a) -> (b) -> (c).
inline const int* DefaultReaderLadder(size_t* count) {
  static const int kOrder[] = {kReaderFull, kReaderBasic, kReaderPlain};
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
}  // namespace kgcore
