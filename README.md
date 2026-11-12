# KameraGeulis

**Kamera beauty real-time untuk Windows 10/11 — webcam masuk → efek GPU → preview langsung + kamera virtual untuk OBS/Discord/Zoom.**
*A real-time GPU beauty camera for Windows 10/11 — webcam in → GPU filters → live preview + virtual camera for OBS/Discord/Zoom.*

[![CI](https://github.com/hanlimzie-glitch/KameraGeulis/actions/workflows/build.yml/badge.svg)](https://github.com/hanlimzie-glitch/KameraGeulis/actions/workflows/build.yml)

> **Status:** M2 (UI ImGui lengkap: preview besar, panel 6 tab — Kulit/Wajah/Makeup/Preset/Kamera/Info, slider smoothing & whitening terhubung live, preset Natural/Soft/Glam/ClearSkin, picker kamera MF + resolusi/fps, debug overlay F3 (468 titik + bbox + timing per stage), settings JSON otomatis, ID/EN) — **CI hijau** (run 43192212350 — termasuk smoke test `--selftest`; artifact `kamerageulis-m2-windows-x64`, ~7,5 MB). Belum diverifikasi di Windows nyata. **Efek landmark tetap nonaktif** (pemetaan 468→Mars tidak kredibel; lihat [docs/LANDMARKS.md](docs/LANDMARKS.md) §8). Lihat [STATUS.md](STATUS.md) untuk tabel fitur yang jujur dan [BUILD.md](BUILD.md) untuk langkah build + cara mengunduh artifact.

## Bahasa Indonesia

KameraGeulis adalah aplikasi desktop native (C++17/OpenGL) yang mempercantik video webcam secara real-time dengan engine [GPUPixel](https://github.com/pixpark/gpupixel) (Apache-2.0), menampilkannya di jendela preview, dan mengalirkan hasilnya sebagai kamera virtual ("KameraGeulis Camera") yang bisa dipakai di OBS, Discord, Zoom, dll.

- **Efek built-in GPUPixel**: skin smoothing & whitening **sudah terhubung** ke slider (0–100); face slimming / big eyes / lipstick / blush **disabled** (`Segera hadir`) — butuh landmark.
- **Efek kustom** (M4): belum diimplementasikan; semua kontrolnya di UI tampil **disabled** berlabel `Segera hadir` agar alur UI final.
- **Preset 1-tap**: Natural, Soft, Glam, Clear Skin. **Hold-to-compare** before/after (tahan tombol). Snapshot PNG (belum — tombol disabled). Settings tersimpan ke JSON (`%APPDATA%\KameraGeulis\settings.json`, auto-load).
- **UI:** Dear ImGui + GLFW + OpenGL, tab Face/Skin/Makeup/Presets, tema gelap, Bahasa Indonesia (default) + English.

### Catatan penting (kejujuran)

- CMake build GPUPixel **wajib Release x64** di Windows; MSVC 2019+ (2022 direkomendasikan). 
- **Efek landmark** (face slim, big eyes, lipstick, …): pipeline deteksi+mesh **sudah diimplementasikan** (ONNX Runtime CPU, 468 titik, model Apache-2.0 hasil konversi; rilis `models-v1` **sudah berisi kedua aset** sejak 2026-11-12 — diupload workflow `Models`, I/O diverifikasi di CI), tetapi pemetaan 468→filter GPUPixel (106/111 titik Mars) terbukti tidak kredibel — jadi v1 **tidak mem-wiring** efek landmark; UI menyatakannya jujur **Segera hadir / disabled**. Skin smoothing & whitening dirancang tanpa landmark dan **aktif**.
- **Kamera virtual memerlukan registrasi 1× dengan admin** (softcam, DirectShow) dan hanya terlihat oleh aplikasi berbasis DirectShow. Windows 11 punya API MFCreateVirtualCamera (didokumentasikan sebagai opsi masa depan).

### Build singkat

```bat
git clone https://github.com/hanlimzie-glitch/KameraGeulis.git
cd KameraGeulis
scripts\build_windows.bat     :: lihat BUILD.md untuk detail
```

## English

KameraGeulis is a native desktop app (C++17/OpenGL) that beautifies your webcam in real time using [GPUPixel](https://github.com/pixpark/gpupixel) (Apache-2.0): it shows a live preview window and streams the result as a virtual camera ("KameraGeulis Camera") usable in OBS/Discord/Zoom.

- Built-in GPUPixel effects on 0–100 sliders: skin smoothing, whitening, face slimming, big eyes, lipstick, blush.
- Custom landmark-SDF effects: chin reshape, nose slim, eye brighten, eyebrow tint, eyeshadow, eyeliner, foundation/even skin tone, lip color with matte/gloss finish.
- One-tap presets, hold-to-compare, PNG snapshot, JSON settings (M2+).
- Dear ImGui + GLFW + OpenGL dark UI, tabs Face/Skin/Makeup/Presets, Indonesian (default) + English strings.

> **Honesty notes:** Windows x64 **Release only**; MSVC required. Landmark detection pipeline (ONNX Runtime CPU, 468 pts) is implemented and `models-v1` ships the ONNX assets (this project's GitHub Actions CI, run 43179962364), but **landmark-driven effects are disabled in v1** — the 468→GPUPixel-Mars mapping proved unreliable; see [docs/LANDMARKS.md](docs/LANDMARKS.md). Preview is untested on real Windows so far; v1 has never been run with a physical GPU/webcam. Virtual camera needs a **one-time admin registration** (softcam/DirectShow). See [STATUS.md](STATUS.md).

## Struktur repo

```
CMakeLists.txt        # root project (FetchContent/download pins)
cmake/                # Dependencies.cmake
src/
  kg_core/            # cross-platform logic (unit-tested; one euro, anchors, decode, ROI)
  kg_tests/           # Catch2 unit tests (run on Linux + Windows)
  kg_capture/         # Media Foundation capture (Windows)
  kg_pipeline/        # GPUPixel wrapper + OrtLandmarkProvider (Windows)
  kg_vcam/            # softcam sender wrapper (runtime LoadLibrary)
  kg_app/             # app entry, ImGui panel, strings
docs/                 # RESEARCH.md, ARCHITECTURE.md, LANDMARKS.md
replica/              # replica-skill recon outputs
.github/workflows/    # CI
BUILD.md, STATUS.md, THIRD_PARTY.md
```

## Lisensi

Kode KameraGeulis: MIT (lihat LICENSE). GPUPixel Apache-2.0, softcam MIT, ImGui MIT, nlohmann/json MIT — detail & atribusi: [THIRD_PARTY.md](THIRD_PARTY.md).
