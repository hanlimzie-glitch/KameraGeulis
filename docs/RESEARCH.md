# RESEARCH.md — Hasil Riset (M0)

**Tanggal:** 2026-11-12 · **Diverifikasi oleh:** inspeksi langsung terhadap repo sumber (shallow clone) + GitHub API + docs resmi.
Semua yang tertulis di sini diverifikasi; yang tidak bisa diverifikasi diberi label **BELUM DIVERIFIKASI**.

---

## 1. GPUPixel — engine inti

- **Repo:** https://github.com/pixpark/gpupixel (shallow clone @ `main` = `ef552bf`, 2026-09-26)
- **Lisensi: APACHE-2.0** — BUKAN MIT. File `LICENSE` di repo adalah teks Apache License 2.0 standar. Konflik yang disebutkan di brief ("MIT vs Apache-2.0") terjawab: **Apache-2.0**. ✅ Terverifikasi.
- **Rilis terbaru:** `v1.3.1` (2025-06-27), tag commit `f33abfa5a802e0dfb04b98f9240a4f24b9d0eb96`. ✅ Terverifikasi via GitHub API. Yang akan kita pin via CMake FetchContent.
- **Versi di CMakeLists:** project version `1.3.0` (meski rilis v1.3.1) — kosmetik, tidak masalah.
- **C++ standard:** C++17, CMake ≥ 3.16 (dikatakan ≥ 3.10 di docs, perlu 3.16 sesungguhnya), Windows **hanya Release x64** — `Windows platform only supports Release build mode` dan `x86_32 is not supported`. ✅ Terverifikasi dari `CMakeLists.txt`.
- **Toolchain Windows:** docs menyebut **MSVC ≥ 2017 + cmake**, contoh dengan `vcvars64.bat` / Windows Terminal x64 Native Tools. **Tidak ada sebutan MinGW64 di docs/README/README_CN** — perintah MinGW hanya asumsi dari brief; secara fakta **MinGW tidak didokumentasikan** dan tidak akan dipakai. Build resmi Windows lewat `script\build_windows.bat` dengan `ilammy/msvc-dev-cmd@v1` di GitHub Actions-nya. ✅ Terverifikasi (MSVC works; MinGW tidak dikonfirmasi).
- **Face detector:** Mars-Face (`mars-face-kit` v0.0.2, vendor `PixPark`). ✅ Terverifikasi dari `third_party/mars-face-kit/include/mars_vision/mars_defines.h`:
  - Landmark: **111 titik 2D ternormalisasi [0,1]** (GPUPixel hanya memakai ~106 titik pertama, lihat `uniform float facePoints[106 * 2]` di `face_reshape_filter.cc`).
  - Deteksi: **1 wajah** (`num_faces = 1`), `RunningMode::VIDEO`, `min_face_presence_confidence = 0.5`.
  - Model: `face_det.mars_model` (808 KB) + `face_align.mars_model` (472 KB). Total ≈ **1.25 MB**.
  - **KRITIS — TIDAK ADA binary Windows di repo.** Hanya `android/arm64-v8a`, `android/armeabi-v7a`, `ios`, `macos`, `linux` (`libmars-face-kit.so`). Windows TIDAK didistribusikan sebagai source maupun binary publik.
  - Issue upstream yang relevan:
    - #216 "mars-face-kit 没有发现PC端可用的库是否可以添加一下" (user meminta binary PC; tidak ada resolusi; komentar: "win端没有…那不是要换别的人脸算法") — ditutup tersembunyi.
      https://github.com/pixpark/gpupixel/issues/216
    - #181 "BUG: mars-face-kit no source code" (masih open; ada laporan crash `std::bad_alloc`).
      https://github.com/pixpark/gpupixel/issues/181
    - #253 "Licensing clarification for bundled Mars Face Kit binaries and models" (**masih open, belum dijawab maintainer**) — lisensi binary/model Mars-Face **tidak eksplisit**. Jangan redistribusi komersial sampai klarifikasi.
      https://github.com/pixpark/gpupixel/issues/253
  - `script/build_windows.bat` resmi memakai **`-DGPUPIXEL_ENABLE_FACE_DETECTOR=OFF`**. CI resmi GPUPixel untuk Windows (`.github/workflows/build.yml`, job `build-windows-cl`) juga build dengan face detector **OFF**. ✅ Terverifikasi.
- **Konsekuensi arsitektur (fakta → keputusan):** di Windows, semua efek yang butuh landmark GPUPixel (face slim, big eyes, lipstick, blush berbasis 106/111 titik) **tidak bisa dipakai dari sumber publik** dengan lisensi jelas. GPUPixel tetap dipakai sebagai engine filter/GPU pipeline (BeautyFaceFilter/whitening bersifat global, tidak butuh landmark) di semua platform. Fitur berbasis landmark dirancang di belakang interface `LandmarkProvider`; di Windows (build default) provider = "tidak tersedia" (UI memberi pesan jujur + log), di Linux/macOS provider = GPUPixel Mars-Face (Linux: bisa diverifikasi penuh, termasuk deteksi wajah dari file gambar — tetapi BUKAN Windows; Windows tetap "BELUM DIVERIFIKASI" untuk efek landmark). Detail di `docs/ARCHITECTURE.md`.
- **Custom filter API:** subclass `gpupixel::Filter` (diekspor `GPUPIXEL_API`) atau `Filter::CreateWithFragmentShaderString(...)`. Pola resmi (dari `face_reshape_filter.cc`): override `DoRender`, `GetGlProgram()->SetUniformValue("nama", ...)`, `SetFaceLandmarks(std::vector<float>)`. Shader desktop = GLSL 1.30 (`#version 130` / `varying`/`texture2D`), macOS = `#version 330 core`. ✅ Terverifikasi.
- **Pipeline GPUPixel:** `SourceRawData` (input piksel CPU) → `Filter` chain → `SinkRawData` (RGBA) / `SinkRawYuv` (I420). `GPUPixel::SetResourcePath()` menunjuk ke root resource (folder `models/` untuk detektor; `mouth.png`, `blusher.png`, `lookup_*.png` dipasang ke `models/` dan `res/` oleh `src/CMakeLists.txt`). ✅ Terverifikasi.
- **Filter built-in yang akan dipakai (tanpa landmark vs butuh landmark):**
  | Filter | Butuh landmark? | Catatan |
  |---|---|---|
  | `BeautyFaceFilter` (smoothing+whitening) | ❌ | `SetBlurAlpha(0..1)`, `SetWhite(0..~0.5)`, `SetHighPassDelta`, `SetSharpen` |
  | `FaceReshapeFilter` (face slim, big eyes) | ✅ (Mars 106/111) | `SetFaceSlimLevel`, `SetEyeZoomLevel` |
  | `LipstickFilter` / `BlusherFilter` | ✅ | `FaceMakeupFilter` subclass; pakai `mouth.png` / `blusher.png` dari resource |
  | `Brightness/Contrast/Saturation/WhiteBalanceFilter` dll. | ❌ | tersedia untuk tuning look & preset |
- **Resource GPU yang ikut terinstal:** `mouth.png`, `blusher.png`, `lookup_light.png`, `lookup_skin.png`, `lookup_gray.png`, `lookup_origin.png`, `lookup_custom.png` (Apache-2.0, boleh dipakai, wajib atribusi).
- **ImGui bawaan GPUPixel:** vendored di `third_party/imgui` v1.91.9 (GLSL backend demo tersedia untuk OpenGL3+GLFW). Kita FetchContent pinned **v1.91.9** yang sama (MIT) agar konsisten.

## 2. OpenMakeupSDK — referensi teknik makeup (BUKAN untuk dilink / disalin)

- **Repo:** https://github.com/ehsanwwe/OpenMakeupSDK (shallow clone). **Lisensi: MIT** (file `LICENSE` ada, copyright ehsanwwe). ✅ Terverifikasi.
- Arsitektur: web/TypeScript/Three.js; MediaPipe FaceMesh **468/478 titik 3D**; makeup = overlay mesh + tekstur (matte/gloss/shimmer) + GLSL; reshape = morph targets GLB (`morphs.js`: noseSlim, jawAngle, browLift, dll.) + `faceWarpMaterial` (pull UV); smoothing landmark = `KalmanFilter2D` (r=0.005, q=0.002) + tracking MediaPipe.
- **Yang diambil (konsep, bukan kode):**
  1. **Lip gloss** = warna dasar + spekular `reflect(view, normal)` + sharpness/gradient mask + noise halus (dither) → kita implementasikan sebagai SDF lip-mask + spekular di fragment shader GPUPixel.
  2. **Eyeshadow / eyeliner / blush / foundation** = mask prosedural (SDF elips/pita) di region mata/alis/pipi/wajah + alpha blend dengan warna; bukan tekstur bitmap → lebih kecil, tanpa aset pihak ketiga.
  3. **Temporal smoothing landmark** = filter 2D (Kalman/OneEuro) — kita implementasikan `OneEuroFilter` (lebih simpel, standar industri) di `src/app/landmark/temporal_filter.*`, diuji unit test di Linux.
  4. **Blend shapes** untuk makeup-like makeup (lipPlump) → kita hanya ambil konsep "intensitas 0..1 per region".
- **Disclaimer atribusi wajib** ada di `THIRD_PARTY.md` dan komentar file shader ("Inspired by OpenMakeupSDK (MIT) — concepts only, no code copied").
> **⚠️ SUPERSEDED (M1b):** Keputusan "tidak pakai model wajah di v1" digantikan oleh `docs/LANDMARKS.md` (2026-11-12). Riset lanjutan menemukan jalur berlisensi jelas: model MediaPipe Apache-2.0 (BlazeFace/FaceMesh) dikonversi ke ONNX dan dijalankan via ONNX Runtime (MIT) — semua checksum diverifikasi; model tidak di-commit, diunduh saat build/first-run dengan SHA-256 pin. Catatan di bawah dipertahankan apa adanya sebagai audit trail.

- MediaPipe model-cards (face_landmarker.task) ternyata **Apache-2.0** (BlazeFace short-range + FaceMesh V2 + Blendshape, model card PDF resmi menyatakan "Apache License 2.0"), dan URL + SHA-256 disediakan repo MediaPipe (`third_party/external_files.bzl`). **Keputusan: TIDAK dipakai di v1** — menjalankan .tflite butuh runtime TF (build Bazel besar/ rapuh; vcpkg tidak punya tensorflow-lite; onnxruntime build-from-source terlalu berat untuk iterasi CI). Dicatat sebagai pekerjaan masa depan (`docs/ROADMAP.md`). ✅ Terverifikasi lisensi; ⛔ keputusan teknis menolak karena bobot/kompleksitas — bukan masalah lisensi.
- **Kandidat ditolak karena lisensi model TIDAK jelas/dilarang komersial:**
  - dlib `shape_predictor_68_face_landmarks.dat` — model card: "the license for the iBUG 300-W dataset excludes commercial use" (dlib.net, diverifikasi 2026-11-12). ❌
  - PFLD-106 weights di repo publik — tidak ada file LICENSE atau dataset 300W-derived. ❌
  - Mars-Face Windows — tidak ada binary publik sama sekali. ❌

## 3. Kamera virtual

| Opsi | Status | Admin? | Kompatibilitas | Keputusan |
|---|---|---|---|---|
| **softcam** (`tshino/softcam`, MIT, aktif 2026-11-11, commit `d1acde4617782f87a3929d7a7528fa3e524ec56b`) | Terverifikasi source+CI-nya | **Ya** (reg selama 1×, tulis registry via UAC shell) | DirectShow capture: OBS, Discord, Zoom (umumnya ya; **BELUM DIVERIFIKASI** di mesin nyata) | ✅ **Dipilih** |
| `MFCreateVirtualCamera` (Win11 22000+) | Docs resmi ada (Microsoft Learn), sample `microsoft/Windows-Camera/Samples/VirtualCamera` | Biasanya tidak (per-user FrameServer) | Consumers yang berbasis Media Foundation; Win10 TIDAK bisa | ⏳ Masa depan / opsional Win11-only |
| Kernel driver virtualcam | Butuh WHQL/signature, tidak masuk akal untuk hobby project | Ya + driver signing | Universal | ❌ |

- Softcam: filter name bawaan `"DirectShow Softcam"`, shared-memory + named mutex untuk streaming `scSendFrame(RGBA/BGR)`. x64 DLL hanya terlihat oleh aplikasi 64-bit; perlu build Win32 juga untuk aplikasi 32-bit. **Kita patch string nama** menjadi `"KameraGeulis Camera"` saat build (FetchContent + `target_compile_options` override define atau patch kecil via `PATCH_COMMAND`) — dicatat di docs & THIRD_PARTY.
- Softcam mensyaratkan **registrasi satu kali per mesin** (`softcam_installer.exe` dengan UAC) — harus dikomunikasikan ke user di UI + README (bukan bug).
- Catatan: tanpa registrasi, toggle "Virtual Camera" di UI harus memberi pesan jelas + tombol "Register dulu (admin)".

## 4. Capture kamera (Windows)

- **Windows Media Foundation** (`IMFSourceReader`, `MFEnumDeviceSources`) — dipilih. Link: `mfplat`, `mfreadwrite`, `mfuuid`, `ole32`, `avrt` (optional).
- Sinkronisasi: capture thread → bounded queue (drop-oldest, kapasitas 2) → GL/render thread → preview + virtual cam sink. Target < ~60 ms glass-to-glass.
- RGB32 dari MF butuh stride handling (MF bisa padded) → konversi ke RGBA/BGRA tepat sekali per frame di capture thread.
- **BELUM DIVERIFIKASI** di hardware nyata (tidak ada webcam di sandbox CI) — diberi label di STATUS.md.

## 5. UI

- **Dear ImGui v1.91.9 + GLFW 3.3.10 (dari tree GPUPixel) + glad** — konsisten dengan stack GPUPixel; OpenGL context dibagikan dengan GPUPixel (GPUPixel render offscreen → texture → kita blit ke preview quad ImGui). ✅ Terverifikasi API dari demo desktop GPUPixel (`demo/desktop/app.cc`).
- Fonts: default ImGui font + opsional TTF (font bawaan OS, tidak dibundel — hindari masalah lisensi).
- String table: Indonesian (default) + English, di `resources/strings.{id,en}.json`-like header — tidak butuh JSON untuk string (cukup header C++ + map) — keputusan: **string table di C++** (`src/app/ui/strings.*`), tanpa file eksternal → tidak bisa hilang saat distribusi.

## 6. Ringkasan temuan yang bertentangan dengan brief

| Brief bilang | Fakta (terverifikasi) |
|---|---|
| "license GPUPixel: MIT vs Apache-2.0 conflict" | **Apache-2.0** (LICENSE file) |
| "toolchain docs mention CMake + MinGW64" | Docs hanya dokumentasikan **MSVC ≥ 2017, x64 Release**. MinGW tidak disebut di mana pun. |
| "Mars-Face as of v1.3.0-beta" | Benar — Mars-Face sejak v1.3.0-beta; terbaru rilis v1.3.1 (2025-06-27). |
| "windows builds on windows-latest CI" | Bisa **dengan face detector OFF** (sesuai upstream) — MSVC Release OK. |
| (implisit) "106 landmarks" | 111 titik mentah; GPUPixel pakai 106 pertama di shader reshape. |
| OpenMakeupSDK MIT | ✅ Benar. Tidak dilink; hanya referensi konsep, atribusi di THIRD_PARTY.md. |

## 7. Batasan yang harus jujur diakui sejak awal

1. Windows build default: **tanpa face detector** → efek landmark nonaktif di UI (dengan pesan), skin smoothing/whitening tetap jalan. (Mars-Face Windows tidak tersedia publik; model lain berlisensi tidak jelas atau dilarang komersial.)
2. GPU CI Windows tidak punya GPU nyata (GL tidak bisa diverifikasi runtime di CI) — "terkompilasi di CI" ≠ "berjalan mulus di GPU kamu".
3. Softcam butuh **admin 1×** untuk registrasi; tanpa itu fitur virtual camera tidak aktif (pesan jelas di UI).
4. Zoom/Discord/OBS: **tidak bisa diverifikasi** di sandbox — akan diverifikasi user di mesin nyata; dokumentasi menyatakan apa yang belum terbukti.

## 8. UX pattern study (ringkas, public behavior only)

Lihat `replica/recon.md` dan `replica/features.csv` — pola dari panel beauty TikTok/Prism Lens/plugin beauty OBS (slider intensitas, preset 1-tap, hold-to-compare, tab Face/Skin/Makeup/Presets) dijadikan acuan desain panel kita tanpa menyalin aset/teks/brand mereka.
