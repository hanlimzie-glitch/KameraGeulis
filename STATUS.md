# STATUS.md — KameraGeulis (jujur, diperbarui per milestone)

Terakhir diperbarui: **2026-11-12 (selesai M0)**
Legenda status:
- ✅ selesai & diverifikasi di lingkungan yang bisa kami akses
- 🔨 sedang/selesai dikerjakan, **belum** diverifikasi di Windows nyata
- ⛔ diblokir fakta/lisensi/aksi di luar sandbox kami
- ⬜ belum dimulai

| # | Fitur | Implemented | Compiled in CI (Windows) | Tested on real Windows | Known issues |
|---|---|---|---|---|---|
| M0 | Riset + capability check + arsitektur | ✅ | ✅ capability workflow hijau (vswhere/cmake/git) | n/a | Log Actions tidak bisa dibaca dari sandbox (lihat di bawah) |
| M1 | CMake + CI build window + GPUPixel + camera preview | ⬜ | ⬜ | ⬜ | — |
| M2 | Built-in beauty sliders + UI | ⬜ | ⬜ | ⬜ | — |
| M3 | Virtual camera (softcam) | ⬜ | ⬜ | ⬜ | butuh admin 1×; belum diuji OBS/Discord |
| M4 | Makeup/reshape kustom + preset + settings | ⬜ | ⬜ | ⬜ | **Efek landmark nonaktif di Windows (default)** — Mars-Face Windows tidak tersedia publik |
| M5 | Perf pass, README, installer notes | ⬜ | ⬜ | ⬜ | — |
| — | Skin smoothing / whitening (global) | ⬜ | ⬜ | ⬜ | dirancang tanpa landmark |
| — | Face slim / big eyes / lipstick / blush (built-in) | ⬜ | ⬜ | ⬜ | butuh Mars-Face; Windows: disabled (pesan jujur di UI) |
| — | Chin, nose, eye brighten, brow tint, eyeshadow, eyeliner, foundation, lip finish | ⬜ | ⬜ | ⬜ | idem |
| — | Settings JSON persist | ⬜ | ⬜ | ⬜ | — |
| — | Snapshot PNG | ⬜ | ⬜ | ⬜ | — |
| — | Mirror toggle | ⬜ | ⬜ | ⬜ | — |
| — | Preset Natural/Soft/Glam/ClearSkin | ⬜ | ⬜ | ⬜ | — |
| — | Recording (MP4) | ⬜ stretch | ⬜ | ⬜ | kemungkinan dibatalkan; akan dilaporkan jujur |
| — | FPS/GPU stats overlay | ⬜ stretch | ⬜ | ⬜ | — |
| — | Hotkeys | ⬜ stretch | ⬜ | ⬜ | — |
| — | MFCreateVirtualCamera (Win11 only) | ⬜ stretch | ⬜ | ⬜ | didokumentasikan sbg opsi masa depan |

## Batasan lingkungan (aktual, per 2026-11-12)

- **GPU di CI Windows tidak ada.** "Compiled in CI" ≠ "berjalan mulus". Verifikasi runtime GL tidak mungkin di workflow ini.
- **Log Actions tidak bisa diunduh dari sandbox ini** (`results-receiver.actions.githubusercontent.com` diblokir proxy). Yang bisa dibaca: status job/step (via `gh api .../jobs`) dan anotasi `::error::` yang sengaja diemit workflow sebagai pengganti log mentah.
- **Tidak bisa apt-install paket X11/GL** di sandbox (deb.debian.org diblokir) → verifikasi GPU lokal di Linux **tidak mungkin**; hanya kompilasi unit test logika murni yang jalan lokal.
- **Tidak ada webcam** di sandbox → capture tidak pernah di-test di sandbox; hanya di `windows-latest` (dan itupun headless, tanpa kamera).

## Keputusan jujur yang sudah diambil

1. Windows default: **tanpa face detector** (sesuai upstream GPUPixel). Semua efek landmark dimatikan dengan pesan jelas di UI + STATUS ini. Alasan lengkap: `docs/RESEARCH.md` §1 & §6.
2. Tidak ada model wajah di-bundling di Windows; model Mars-Face hanya ada di build Linux/macOS via GPUPixel dan lisensinya belum dikonfirmasi upstream (issue #253) → tidak untuk rilis komersial sebelum klarifikasi.
3. Kamera virtual = softcam (DirectShow) + **registrasi admin 1×**; nama device dipatch jadi `KameraGeulis Camera`. Windows 10: hanya DShow consumers yang melihatnya; Win11 MF-only apps tidak.
