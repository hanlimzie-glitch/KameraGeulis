# STATUS.md — KameraGeulis (jujur, diperbarui per milestone)

Terakhir diperbarui: **2026-11-12 (M2 hijau di CI — run 43183762692, artifact `kamerageulis-m2-windows-x64` (~7,1 MB); M2 belum diverifikasi di Windows nyata)**
Legenda status:
- ✅ selesai & diverifikasi di lingkungan yang bisa kami akses
- 🔨 sedang/selesai dikerjakan, **belum** diverifikasi di Windows nyata
- ⛔ diblokir fakta/lisensi/aksi di luar sandbox kami
- ⬜ belum dimulai

| # | Fitur | Implemented | Compiled in CI (Windows) | Tested on real Windows | Known issues |
|---|---|---|---|---|---|
| M0 | Riset + capability check + arsitektur | ✅ | ✅ capability workflow hijau (vswhere/cmake/git) | n/a | Log Actions tidak bisa dibaca dari sandbox (lihat di bawah) |
| M1 | CMake + CI build window + GPUPixel + camera preview | 🔨 | ✅ (run 43166017897) | ⬜ | Runtime GL/MF BELUM diverifikasi (tidak ada GPU/webcam di CI/sandbox); artifact M1: `kamerageulis-m1-windows-x64` (581 KB) |
| M1b | Pipeline landmark (ORT CPU, 468 pt) + kg_core decode/ROI/OneEuro | 🔨 | ✅ (run 43174282611) | ⬜ | Windows runtime belum diverifikasi di mesin nyata; GPUPixel landmark filters **tidak di-wire** (pemetaan 468→Mars tidak kredibel, docs/LANDMARKS.md §8); kg_core hijau di Linux (45 TC / 8319 assertions) |
| M2 | UI ImGui lengkap + slider skin live + debug overlay | ✅ (dikode) | ✅ (run 43183762692) | ⬜ | belum diverifikasi di Windows nyata (GPU/webcam/capture headless di CI); snapshot PNG belum |
| M3 | Virtual camera (softcam) | ⬜ | ⬜ | ⬜ | butuh admin 1×; belum diuji OBS/Discord |
| M4 | Makeup/reshape kustom + preset + settings | ⬜ | ⬜ | ⬜ | **Efek landmark nonaktif di Windows (default)** — Mars-Face Windows tidak tersedia publik |
| M5 | Perf pass, README, installer notes | ⬜ | ⬜ | ⬜ | — |
| — | Skin smoothing / whitening (global) | ✅ | ✅ | ⬜ | terhubung live (per-frame SetBlurAlpha/SetWhite); dirancang tanpa landmark |
| — | Face slim / big eyes / lipstick / blush (built-in) | ⬜ | ⬜ | ⬜ | butuh Mars-Face; Windows: disabled (pesan jujur di UI) |
| — | Chin, nose, eye brighten, brow tint, eyeshadow, eyeliner, foundation, lip finish | ⬜ | ⬜ | ⬜ | idem |
| — | Settings JSON persist | ✅ | ✅ | ⬜ | auto-load `%APPDATA%\KameraGeulis\settings.json`; tombol Simpan/Muat; round-trip+clamp diuji di kg_tests |
| — | Snapshot PNG | ⬜ | ⬜ | ⬜ | — |
| — | Mirror toggle | ✅ | ✅ | ⬜ | CPU-mirror di preview (flip horizontal); round-trip buffer mirror diuji di kg_core standar |
| — | Preset Natural/Soft/Glam/ClearSkin | ✅ | ✅ | ⬜ | nilai preset teruji di kg_tests (Natural 0.25/0.10, Soft 0.45/0.20, Glam 0.65/0.32, ClearSkin 0.55/0.26); slider manual → preset "Kustom" |
| — | Recording (MP4) | ⬜ stretch | ⬜ | ⬜ | kemungkinan dibatalkan; akan dilaporkan jujur |
| — | FPS/GPU stats overlay | ✅ (F3) | ✅ | ⬜ | debug overlay: 468 titik + bbox + HUD (FPS, capture/detect/mesh/track/render ms, frame sejak deteksi) |
| — | Hotkeys | 🟡 sebagian | ✅ | ⬜ | F3 = toggle debug overlay; hotkey lain belum |
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

## M2 — detail (2026-11-12)

**Artifact:** `kamerageulis-m2-windows-x64` — **CI hijau, run [43183762692](https://github.com/hanlimzie-glitch/KameraGeulis/actions/runs/43183762692)** (Linux unit tests ✅ 37 detik; Windows x64 Release: Configure → Build → Test → Package → Upload ✅). Ukuran ~7,1 MB (7.488.666 byte). Kedaluwarsa artifact: **2026-11-26**.

**Isi artifact** (diverifikasi dari langkah Package/Upload CI, bukan unduhan sandbox — unduhan dari sandbox gagal di blob storage; isi persis sesuai `dist/` di workflow): `KameraGeulis.exe`, `gpupixel.dll`, `onnxruntime.dll`, CRT app-local `vcruntime140.dll` + `vcruntime140_1.dll` + `msvcp140.dll` (VS binary terdistribusi ulang, diizinkan app-local, tanpa admin — lihat [docs/REDISTRIBUTABLES.md](docs/REDISTRIBUTABLES.md)), `THIRD_PARTY.md`, `README.md`, `STATUS.md`.

**Cara mengunduh artifact** (pilih salah satu):

```bat
:: via gh CLI (perlu gh auth login)
gh run download 43183762692 -R hanlimzie-glitch/KameraGeulis -n kamerageulis-m2-windows-x64 -D kamerageulis-m2
```

atau buka https://github.com/hanlimzie-glitch/KameraGeulis/actions/runs/43183762692 → bagian **Artifacts** → `kamerageulis-m2-windows-x64` → ekstrak → jalankan `KameraGeulis.exe`. Tidak perlu installer; CRT sudah app-local.

**Yang diimplementasikan (M2):**
- Preview besar + panel 6 tab (**Kulit | Wajah | Makeup | Preset | Kamera | Info**), tema gelap.
- **Slider Haluskan Kulit & Cerahkan Kulit (0–100%) aktif** dan di-apply live tiap frame (`BeautyFaceFilter::SetBlurAlpha` / `SetWhite` via `BeautyPipeline::SetBeautyStrength`).
- Preset **Natural / Soft / Glam / ClearSkin** (nilai di `src/kg_core/include/kgcore/presets.h`, diuji di `kg_tests`); slider manual → preset "**Kustom**"; tombol **Reset** (default pabrik); **Simpan/Muat** `settings.json` (`%APPDATA%\KameraGeulis\settings.json`, load otomatis saat startup).
- **Tahan tombol "Tahan untuk Bandingkan"** → preview raw (tanpa efek) selama ditahan.
- **Semua kontrol landmark** (Tiruskan Wajah, Perbesar Mata, Lipstik, Blusher, Foundation, Eyeliner, Eyeshadow, Brow Tint) dirender **disabled** berlabel **"Segera hadir"** (bukan disembunyikan) — alur UI final untuk M4.
- **Picker kamera**: daftar device Media Foundation + resolusi/fps dari mode native kamera (fallback 1280×720 / 640×480 + peringatan bila mode tidak terdeteksi); tombol **Terapkan** / **Segarkan**.
- **Debug overlay (F3** atau checkbox di tab Info)**: menggambar 468 titik landmark + bounding box + HUD (FPS, waktu capture/detect/mesh/track/render dalam ms, frame sejak deteksi terakhir) di atas preview — untuk verifikasi visual akurasi landmark.
- Seluruh string lewat tabel ID (default) + EN; toggle radio di status bar.
- Virtual camera (softcam) tetap **disabled** — M3.

**BELUM diverifikasi di Windows nyata (jujur):**
- Artifact belum pernah dijalankan di Windows mana pun. Tidak ada GPU, webcam, atau display di CI (`windows-latest`, headless) — "compile+test hijau" ≠ "berjalan mulus".
- Runtime GL (OpenGL 3.3+), Media Foundation capture, dan ONNX Runtime di mesin nyata belum diuji.
- **Tampilan UI** (font, tata letak, skala DPI) belum pernah dilihat manusia.
- Kamera virtual (M3) belum ada sama sekali.
- Bila `github.com` tidak terjangkau dari mesin user, download model `models-v1` saat first-run gagal → efek landmark nonaktif (pesan jujur) — aplikasi tetap berjalan.
- Artifact expired 2026-11-26; untuk rilis permanen perlu GitHub Release (di luar scope M2).

**CI notes:** 2 run merah sebelum hijau — (1) `EnumerateDevices` bentrok dengan helper bebas (C2660/C2440, run 43181651089) → helper di-rename `EnumerateDevicesRaw` (dd7c738); (2) konstanta `MF_E_NO_MORE_TYPES` tidak terdeklarasi di toolchain CI (run 43182162374) → enumerasi berhenti pada `FAILED(hr)` apa pun; (3) `C2027: undefined type gpupixel::BeautyFaceFilter` di `main.cc` (run 43182519015) → parameter beauty live dipindah ke `BeautyPipeline::SetBeautyStrength`; (4) Build+Test sudah hijau di run 43182772993 tapi Package gagal — penyebab tidak terbaca; langkah redist kini diagnostik `::error::` + memilih folder CRT terlengkap (dabacc4); (5) run 43183762692 **hijau penuh** termasuk upload artifact.

**Catatan:** `kg_app` hanya di-build di Windows (`KG_BUILD_APP=ON`); di Linux app tidak dikonfigurasi — logika murni (settings, presets, strings, decode) yang diuji di Linux (`kg_tests` hijau, termasuk di CI run di atas).

## LICENSE — keputusan pemilik (owner decision)

- **Repository ini sengaja TIDAK berisi file `LICENSE`.** Penentuan lisensi adalah keputusan **pemilik repository**, bukan keputusan agen. Sampai pemilik menentukan, jangan distribusi publik / rilis binary dengan klaim lisensi tertentu.
- GPUPixel dipakai di bawah **Apache-2.0**; notice/attribution institusi dipertahankan utuh di [THIRD_PARTY.md](THIRD_PARTY.md) (dan dicopy ke artifact).
- Dependensi lain (GLFW, Dear ImGui, Catch2, onnxruntime, model MediaPipe→ONNX) telah didokumentasikan lisensi + provenance-nya di THIRD_PARTY.md; model tidak di-commit ke repo (diunduh first-run dari release `models-v1` dengan SHA-256 pin, atau dari `KAMERAGEULIS_MODELS_DIR` untuk CI/dev).
- Sebelum rilis publik: (a) pilih lisensi, (b) tambahkan file LICENSE, (c) update README/badge, (d) audit ulang THIRD_PARTY.md. Ini ditandai sebagai **keputusan pemilik** di sini agar tidak hilang.

## M1b — detail (2026-11-12)

- **kg_core** (Linux-tested, hijau): `face_anchors` (896 anchor BlazeFace short-range, faithful ke `SsdAnchorsCalculator` + `face_detection_short_range.pbtxt`), `face_detection_decode` (sigmoid, weighted NMS IoU 0.3, letterbox keep-aspect BORDER_ZERO), `roi_align` (eye-aligned crop 1.5×, round-trip teruji), `face_mesh_decode` (conv2d_21 → 468 titik [0,1]; `conv2d_31` face flag), OneEuro/LandmarkSet/FlattenLandmarks (sudah ada).
- **OrtLandmarkProvider** (Windows, `kg_pipeline`): BlazeFace 128² → decode → ROI mata-sejajar → FaceMesh 468 → gate faceflag → OneEuro + fade via FaceTracker. Download first-run dari rilis GitHub `models-v1` (SHA-256 tertanam di `ort_landmark_provider.cc`, dihasilkan CI: det `82ac962b…`, mesh `ac72f85b…`); env override `KAMERAGEULIS_MODELS_DIR` untuk CI/dev; gagal → efek nonaktif + pesan jujur (tidak pernah crash).
- **Efek landmark v1 TIDAK di-wire** (FaceReshape/Lipstick/Blusher GPUPixel): pemetaan 468→106/111 titik Mars tidak bisa diandalkan (proporsi canonical berbeda; residual busur OK 0.010 tetapi alis/mata salah anatomi; docs/LANDMARKS.md §8). UI menyatakannya jujur.
- **THIRD_PARTY.md**: model ONNX tetap Apache-2.0 via konversi tf2onnx; tidak di-commit. `canonical_face_model.obj` hanya referensi analisis offline (tidak di-commit).
- **CI**: run 43174282611 hijau (Linux: kg_core tests; Windows: configure mengunduh ONNX Runtime 1.31.1 win-x64 dengan pin SHA-256, build+link sukses termasuk `onnxruntime.dll`, ctest hijau, artifact `kamerageulis-m1b-windows-x64` 6,8 MB). Catatan: run sebelumnya (43173665412) Linux Test exit 8 tanpa ::error::; Test step kini meng-annotasi output ctest; kegagalan itu tidak berulang dan tidak terdiagnosis (jeda infra?) — dicatat jujur.
- **Belum terverifikasi**: provider di Windows nyata (belum pernah jalan), performa CPU, kualitas ROI/rotasi pada wajah nyata, download first-run dari `models-v1` di mesin bersih (release sudah berisi aset sejak 2026-11-12 — workflow Models run 43179962364), artifact vcam (belum ada).
