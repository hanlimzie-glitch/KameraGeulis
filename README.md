# KameraGeulis

**Kamera beauty real-time untuk Windows 10/11 — webcam masuk → efek GPU → preview langsung + kamera virtual untuk OBS/Discord/Zoom.**
*A real-time GPU beauty camera for Windows 10/11 — webcam in → GPU filters → live preview + virtual camera for OBS/Discord/Zoom.*

[![CI](https://github.com/hanlimzie-glitch/KameraGeulis/actions/workflows/build.yml/badge.svg)](https://github.com/hanlimzie-glitch/KameraGeulis/actions/workflows/build.yml)

> **Status:** tahap awal pengembangan (M0 selesai — riset & arsitektur; M1 build menyusul). Lihat [STATUS.md](STATUS.md) untuk tabel fitur yang jujur dan [BUILD.md](BUILD.md) untuk langkah build.

## Bahasa Indonesia

KameraGeulis adalah aplikasi desktop native (C++17/OpenGL) yang mempercantik video webcam secara real-time dengan engine [GPUPixel](https://github.com/pixpark/gpupixel) (Apache-2.0), menampilkannya di jendela preview, dan mengalirkan hasilnya sebagai kamera virtual ("KameraGeulis Camera") yang bisa dipakai di OBS, Discord, Zoom, dll.

- **Efek built-in GPUPixel** (slider 0–100): skin smoothing, whitening, face slimming, big eyes, lipstick, blush.
- **Efek kustom** (landmark SDF, toggle + slider): chin reshape, nose slim, eye brighten, eyebrow tint, eyeshadow, eyeliner, foundation/even skin tone, lip color matte/gloss.
- **Preset 1-tap:** Natural, Soft, Glam, Clear Skin. **Hold-to-compare** before/after. **Snapshot PNG.** Settings tersimpan ke JSON.
- **UI:** Dear ImGui + GLFW + OpenGL, tab Face/Skin/Makeup/Presets, tema gelap, Bahasa Indonesia (default) + English.

### Catatan penting (kejujuran)

- CMake build GPUPixel **wajib Release x64** di Windows; MSVC 2019+ (2022 direkomendasikan). 
- **Efek landmark** dirancang aktif di semua platform via ONNX Runtime + model Apache-2.0 hasil konversi (468 titik) — detail & checksum di [docs/LANDMARKS.md](docs/LANDMARKS.md). Sampai pipeline M1b mendarat, status efek landmark di Windows: **disabled** (lihat [STATUS.md](STATUS.md)). Skin smoothing & whitening tetap berfungsi penuh.
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
- One-tap presets, hold-to-compare, PNG snapshot, JSON settings.
- Dear ImGui + GLFW + OpenGL dark UI, tabs Face/Skin/Makeup/Presets, Indonesian (default) + English strings.

> **Honesty notes:** Windows x64 **Release only**; MSVC required. Landmark-dependent effects are **being wired up via ONNX Runtime + Apache-2.0-derived models** (468 pts) — until that lands they stay disabled; skin smoothing/whitening always work. Virtual camera needs a **one-time admin registration** (softcam/DirectShow). See [docs/LANDMARKS.md](docs/LANDMARKS.md) and [STATUS.md](STATUS.md).

## Struktur repo

```
CMakeLists.txt        # root project (FetchContent pins)
src/                  # app source
  core/               # cross-platform logic (unit-tested, no GL/X11)
  capture/            # Media Foundation capture (Windows)
  pipeline/           # GPUPixel wrapper + custom filters
  vcam/               # softcam sender wrapper
  ui/                 # ImGui panel, strings (ID/EN), presets
docs/                 # RESEARCH.md, ARCHITECTURE.md
replica/              # replica-skill recon outputs
.github/workflows/    # CI
BUILD.md, STATUS.md, THIRD_PARTY.md
```

## Lisensi

Kode KameraGeulis: MIT (lihat LICENSE). GPUPixel Apache-2.0, softcam MIT, ImGui MIT, nlohmann/json MIT — detail & atribusi: [THIRD_PARTY.md](THIRD_PARTY.md).
