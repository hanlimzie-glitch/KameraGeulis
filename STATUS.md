# STATUS.md — KameraGeulis (jujur, diperbarui per milestone)

Terakhir diperbarui: **2026-11-12 (M1 selesai — artifact `kamerageulis-m1-windows-x64`; M1b hijau di CI — run 43174282611, artifact `kamerageulis-m1b-windows-x64` (6,8 MB, berisi onnxruntime.dll))**
Legenda status:
- ✅ selesai & diverifikasi di lingkungan yang bisa kami akses
- 🔨 sedang/selesai dikerjakan, **belum** diverifikasi di Windows nyata
- ⛔ diblokir fakta/lisensi/aksi di luar sandbox kami
- ⬜ belum dimulai

| # | Fitur | Implemented | Compiled in CI (Windows) | Tested on real Windows | Known issues |
|---|---|---|---|---|---|
| M0 | Riset + capability check + arsitektur | ✅ | ✅ capability workflow hijau (vswhere/cmake/git) | n/a | Log Actions tidak bisa dibaca dari sandbox (lihat di bawah) |
| M1 | CMake + CI build window + GPUPixel + camera preview | 🔨 | ✅ (run 43166017897) | ⬜ | Runtime GL/MF BELUM diverifikasi (tidak ada GPU/webcam di CI/sandbox); artifact M1: `kamerageulis-m1-windows-x64` (581 KB) |
| M1b | Pipeline landmark (ORT CPU, 468 pt) + kg_core decode/ROI/OneEuro | 🔨 | ✅ (run 43174282611) | ⬜ | Windows runtime belum diverifikasi; models-v1 belum berisi aset → efek landmark nonaktif; GPUPixel landmark filters **tidak di-wire** (pemetaan 468→Mars tidak kredibel, docs/LANDMARKS.md §8); kg_core hijau di Linux (45 TC / 8319 assertions) |
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

1. Windows default: **tanpa Mars-Face** (sesuai upstream GPUPixel). Efek landmark dijadwalkan aktif via **ONNX Runtime + model hasil konversi Apache-2.0 (468 titik)** — rencana, provenance & checksum di `docs/LANDMARKS.md`; sampai M1b mendarat, efek landmark tetap **disabled** dengan pesan jujur di UI. Alasan historis: `docs/RESEARCH.md` §1 & §6.
2. Tidak ada model wajah di-bundling di Windows; model Mars-Face hanya ada di build Linux/macOS via GPUPixel dan lisensinya belum dikonfirmasi upstream (issue #253) → tidak untuk rilis komersial sebelum klarifikasi.
3. Kamera virtual = softcam (DirectShow) + **registrasi admin 1×**; nama device dipatch jadi `KameraGeulis Camera`. Windows 10: hanya DShow consumers yang melihatnya; Win11 MF-only apps tidak.
4. Setiap milestone CI meng-upload artifact (`kamerageulis-mN-windows-x64`; M1b: `kamerageulis-m1b-windows-x64`). Log mentah Actions tidak bisa diunduh dari sandbox — error MSVC dibaca via anotasi `::error::` (workflow memancarkannya; lihat `.github/workflows/build.yml` langkah Build).
5. Detil fix CI selalu dianotasikan di commit message fix (analisis + akar masalah + perbaikan).

## M1b — detail (2026-11-12)

- **kg_core** (Linux-tested, hijau): `face_anchors` (896 anchor BlazeFace short-range, faithful ke `SsdAnchorsCalculator` + `face_detection_short_range.pbtxt`), `face_detection_decode` (sigmoid, weighted NMS IoU 0.3, letterbox keep-aspect BORDER_ZERO), `roi_align` (eye-aligned crop 1.5×, round-trip teruji), `face_mesh_decode` (conv2d_21 → 468 titik [0,1]; `conv2d_31` face flag), OneEuro/LandmarkSet/FlattenLandmarks (sudah ada).
- **OrtLandmarkProvider** (Windows, `kg_pipeline`): BlazeFace 128² → decode → ROI mata-sejajar → FaceMesh 468 → gate faceflag → OneEuro + fade via FaceTracker. Download first-run dari rilis GitHub `models-v1` (SHA-256 tertanam: det `613ee7…`, mesh `5497a6…`); env override `KAMERAGEULIS_MODELS_DIR` untuk CI/dev; gagal → efek nonaktif + pesan jujur (tidak pernah crash).
- **Efek landmark v1 TIDAK di-wire** (FaceReshape/Lipstick/Blusher GPUPixel): pemetaan 468→106/111 titik Mars tidak bisa diandalkan (proporsi canonical berbeda; residual busur OK 0.010 tetapi alis/mata salah anatomi; docs/LANDMARKS.md §8). UI menyatakannya jujur.
- **THIRD_PARTY.md**: model ONNX tetap Apache-2.0 via konversi tf2onnx; tidak di-commit. `canonical_face_model.obj` hanya referensi analisis offline (tidak di-commit).
- **CI**: run 43174282611 hijau (Linux: kg_core tests; Windows: configure mengunduh ONNX Runtime 1.31.1 win-x64 dengan pin SHA-256, build+link sukses termasuk `onnxruntime.dll`, ctest hijau, artifact `kamerageulis-m1b-windows-x64` 6,8 MB). Catatan: run sebelumnya (43173665412) Linux Test exit 8 tanpa ::error::; Test step kini meng-annotasi output ctest; kegagalan itu tidak berulang dan tidak terdiagnosis (jeda infra?) — dicatat jujur.
- **Belum terverifikasi**: provider di Windows nyata (belum pernah jalan), performa CPU, kualitas ROI/rotasi pada wajah nyata, models-v1 download dari release (masih 404), artifact vcam (belum ada).
