# THIRD_PARTY.md — Dependensi pihak ketiga

Semua dependensi di-pin ke versi/commit persis. Tidak ada yang divendor sebagai source di repo ini (kecuali `stb`-like helper kecil yang kita tulis sendiri); semuanya di-fetch saat CMake configure/build via FetchContent (atau ExternalProject untuk softcam).

| Komponen | Versi / ref | Lisensi | Keperluan di app | Cara ambil |
|---|---|---|---|---|
| [GPUPixel](https://github.com/pixpark/gpupixel) | tag `v1.3.1` = commit `f33abfa5a802e0dfb04b98f9240a4f24b9d0eb96` | **Apache-2.0** | Engine filter GPU, SourceRawData/SinkRawData, BeautyFaceFilter, FaceReshapeFilter, Lipstick/Blusher, FaceDetector (Linux/macOS saja) | FetchContent GIT_REPOSITORY |
| [Dear ImGui](https://github.com/ocornut/imgui) | `v1.91.9` | MIT | UI (GLFW+OpenGL3 backends dibundel di repo imgui) | FetchContent GIT_REPOSITORY |
| [nlohmann/json](https://github.com/nlohmann/json) | `v3.11.3` | MIT | Settings persist (`%APPDATA%/KameraGeulis/settings.json`) | FetchContent GIT_REPOSITORY |
| [Catch2](https://github.com/catchorg/Catch2) | `v3.7.1` | BSL-1.0 | Unit tests (build-time saja, tidak didistribusikan) | FetchContent GIT_REPOSITORY |
| [softcam](https://github.com/tshino/softcam) | commit `d1acde4617782f87a3929d7a7528fa3e524ec56b` (2026-11-11, repo tanpa tag) | MIT | Virtual camera DirectShow (x64 + Win32), filter name dipatch → `"KameraGeulis Camera"` | ExternalProject (msbuild `softcam.sln`) |
| GPUPixel bundled: GLFW 3.3.10 | (di tree gpupixel) | zlib/libpng (GLFW license) | Window & GL context (target `glfw` dari build GPUPixel) | via GPUPixel |
| GPUPixel bundled: glad | (di tree gpupixel) | MIT | OpenGL loader | via GPUPixel |
| GPUPixel bundled: libyuv | (di tree gpupixel) | BSD-3-Clause | YUV conversion (kebutuhan internal gpupixel) | via GPUPixel |
| GPUPixel bundled: stb_image | (di tree gpupixel) | Public domain / MIT | SourceImage PNG (mouth/blusher) | via GPUPixel |
| GPUPixel resources: `mouth.png`, `blusher.png`, `lookup_*.png` | (di tree gpupixel) | Apache-2.0 (bagian dari gpupixel) | Tekstur makeup built-in | via GPUPixel install |
| GPUPixel bundled: mars-face-kit + models | v0.0.2, `face_det/face_align.mars_model` (~1.25 MB) | **TIDAK JELAS** — GPUPixel Apache-2.0, tetapi lisensi eksplisit binary/model belum dikonfirmasi (upstream issue #253 open) | Face detection **hanya Linux/macOS**; di Windows `GPUPIXEL_ENABLE_FACE_DETECTOR=OFF` | via GPUPixel (Linux only) |

## Atribusi kode/shader yang diadaptasi

- **OpenMakeupSDK** (https://github.com/ehsanwwe/OpenMakeupSDK) — **MIT** © ehsanwwe.
  Tidak ada kode TypeScript/GLSL yang disalin. Yang diadaptasi sebagai *inspirasi desain* di shader kita:
  konsep lip-gloss spekular (reflect + gradient ramp), mask prosedural berbasis SDF untuk eyeliner/eyeshadow/blush,
  dan pola temporal smoothing landmark (diimplementasikan ulang sebagai OneEuroFilter).
  Komentar `// Inspired by OpenMakeupSDK (MIT) — concepts only, see THIRD_PARTY.md` ada di setiap file terkait.
- **GPUPixel** (Apache-2.0) — filter bawaan dipakai apa adanya via API publik; tidak ada modifikasi source.
- **replica-skill** (https://github.com/Jakeschincariol/replica-skill, MIT © Jake Schincariol) — metodologi recon→architect→build→test;
  `replica/recon.md` + `replica/features.csv` adalah hasil kerjanya. Tidak ada kode skill yang ditanam ke app.
- **Softcam** (MIT © tshino) — patch hanya 1 string (`FILTER_NAME`) via langkah build; seluruh logika shared-memory & DShow filter dipakai apa adanya.

## Yang SENGAJA TIDAK dibundel / tidak dipakai

- **MediaPipe models / runtime** (.task/.tflite) — model card sah (Apache-2.0) tetapi TFLite C++ runtime tidak tersedia di vcpkg & Bazel terlalu berat untuk CI ini; dicatat sebagai pekerjaan masa depan.
- **dlib shape_predictor_68** — model card menyatakan batasan komersial (iBUG 300-W). ❌
- **Model PFLD-106 publik** — tidak ada lisensi jelas. ❌
- **Mars-Face Windows** — binary tidak tersedia publik. ❌
- **OpenCV/YuNet** — YuNet ONNX MIT, tetapi total build OpenCV hanya untuk 5 titik landmark tidak sebanding; ditunda.

## Kewajiban lisensi saat distribusi

1. Sertakan `LICENSE` (Apache-2.0 GPUPixel + MIT notice lain + teks lengkap tiap lisensi) di folder distribusi/`licenses/`.
2. Pertahankan notice hak cipta GPUPixel (`Copyright © 2021 PixPark`) pada file bawaan; GPUPixel tidak mensyaratkan rename.
3. **Jangan** redistribusi binary Mars-Face/models di rilis komersial sebelum klarifikasi upstream (issue #253). Di Windows tidak relevan (tidak dibundel).
4. `softcam.dll` teregistrasi di mesin user — sertakan `THIRD_PARTY.md`/`licenses/softcam-LICENSE.txt` di installer + teks atribusi di dialog "About".
