# THIRD_PARTY.md — Dependensi pihak ketiga

Semua dependensi di-pin ke versi/commit persis. Tidak ada yang divendor sebagai source di repo ini (kecuali `stb`-like helper kecil yang kita tulis sendiri); semuanya di-fetch saat CMake configure/build via FetchContent (atau ExternalProject untuk softcam).

| Komponen | Versi / ref | Lisensi | Keperluan di app | Cara ambil |
|---|---|---|---|---|
| [GPUPixel](https://github.com/pixpark/gpupixel) | tag `v1.3.1` = commit `f33abfa5a802e0dfb04b98f9240a4f24b9d0eb96` | **Apache-2.0** | Engine filter GPU, SourceRawData/SinkRawData, BeautyFaceFilter, FaceReshapeFilter, Lipstick/Blusher, FaceDetector (Linux/macOS saja) | FetchContent GIT_REPOSITORY |
| [Dear ImGui](https://github.com/ocornut/imgui) | `v1.91.9` | MIT | UI (GLFW+OpenGL3 backends dibundel di repo imgui) | FetchContent GIT_REPOSITORY |
| [nlohmann/json](https://github.com/nlohmann/json) | `v3.11.3` | MIT | Settings persist (`%APPDATA%/KameraGeulis/settings.json`) | FetchContent GIT_REPOSITORY |
| [Catch2](https://github.com/catchorg/Catch2) | `v3.7.1` | BSL-1.0 | Unit tests (build-time saja, tidak didistribusikan) | FetchContent GIT_REPOSITORY |
| [softcam](https://github.com/tshino/softcam) | commit `d1acde4617782f87a3929d7a7528fa3e524ec56b` (2026-11-11, repo tanpa tag) | MIT | Virtual camera DirectShow (x64 + Win32), filter name dipatch → `"KameraGeulis Camera"` | ExternalProject (msbuild `softcam.sln`) |
| [ONNX Runtime](https://github.com/microsoft/onnxruntime) | `v1.31.1` (2026-11 latest) — win-x64 CPU zip (82,929,944 B, SHA-256 `5869e26bb25314375f2d5ef1877e3ea06b1543d6c710e6e4a1ec2f7177ab4b2c`); Linux tgz digest `c849e36a1ef3e22d4ff217b9d1ce4a3cf5b9467b975610913ae959ad5b16bdc9` | MIT | Inference landmark wajah (mulai M1b): detektor BlazeFace + FaceMesh 468 | FetchContent / download-with-SHA256 (build) |
| MediaPipe-derived ONNX models (`face_detection_short_range.onnx`, `face_mesh.onnx`) — **bukan file dari repo ini**, dilampirkan di GitHub Release `models-v1` | `face_detection_short_range.onnx` SHA-256 `82ac962b24dfd78a5b6a3331e8c4091e377d699c6e4fdaca158fba03b5af8e3d`; `face_mesh.onnx` SHA-256 `ac72f85b77368a2ee1d065257674cb25099699d98376685af549df50d6a40337` | Model: **Apache-2.0** (BlazeFace short-range & FaceMesh — MediaPipe model cards; detektor byte-identik dengan `external_files.bzl` `3bc182eb…`). Konversi otomatis di CI (workflow `Models`): tf2onnx (MIT) **1.17.0** / tensorflow 2.17.1, opset **15**, tanpa modifikasi bobot; I/O diverifikasi onnxruntime di CI (run 43179962364). | Deteksi wajah + 468 titik landmark (mulai M1b) | SHA-256 pin di kode; diverifikasi saat load; **di-bundle di artifact sejak M2d** (`dist\models\`); fallback: unduh dari release `models-v1` (first-run) bila bundle rusak; tidak pernah di-commit |
| GPUPixel bundled: GLFW 3.3.10 | (di tree gpupixel) | zlib/libpng (GLFW license) | Window & GL context (target `glfw` dari build GPUPixel) | via GPUPixel |
| GPUPixel bundled: glad | (di tree gpupixel) | MIT | OpenGL loader | via GPUPixel |
| GPUPixel bundled: libyuv | (di tree gpupixel) | BSD-3-Clause | YUV conversion (kebutuhan internal gpupixel) | via GPUPixel |
| GPUPixel bundled: stb_image | (di tree gpupixel) | Public domain / MIT | SourceImage PNG (mouth/blusher) | via GPUPixel |
| `canonical_face_model.obj` (MediaPipe face_geometry) — **referensi offline saja** untuk analisis pemetaan landmark (docs/LANDMARKS.md §8) | commit `master` 2026-11-12 (raw.githubusercontent.com) | **Apache-2.0** © Google LLC | Tidak dipakai di runtime; tidak di-commit/dibundel | diunduh sekali oleh maintainer (analisis) |
| GPUPixel resources: `mouth.png`, `blusher.png`, `lookup_*.png` | (di tree gpupixel) | Apache-2.0 (bagian dari gpupixel) | Tekstur makeup built-in | via GPUPixel install |
| GPUPixel resources `res/*.png` di-bundle apa adanya di artifact (`dist\res\`) | salinan file di `src/res/` pada ref yang di-pin; tidak dimodifikasi | Apache-2.0 | Lookup texture BeautyFaceUnitFilter (lookup_gray/origin/skin/light) + makeup (blusher/mouth) | Disalin **di CI** saat packaging; tidak di-commit ke repo; wajib ada saat runtime (docs/GPUPIXEL.md) |
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

- **MediaPipe .task / TFLite runtime langsung** — ditolak (tidak ada TFLite-C Windows resmi; .task tidak punya SHA-256 publik yang stabil; storage.googleapis.com tidak terjangkau dari sandbox). Yang dipakai: model **ONNX hasil konversi** dari sumber Apache-2.0 dengan checksum pin (lihat tabel + `docs/LANDMARKS.md`).
- **dlib shape_predictor_68** — model card menyatakan batasan komersial (iBUG 300-W). ❌
- **Model PFLD-106 publik** — tidak ada lisensi jelas. ❌
- **Mars-Face Windows** — binary tidak tersedia publik. ❌
- **OpenCV/YuNet** — YuNet ONNX MIT, hanya deteksi kotak (bukan 468 landmark); tidak dipakai v1.

## Kewajiban lisensi saat distribusi

1. Sertakan `LICENSE` (Apache-2.0 GPUPixel + MIT notice lain + teks lengkap tiap lisensi) di folder distribusi/`licenses/`.
2. Pertahankan notice hak cipta GPUPixel (`Copyright © 2021 PixPark`) pada file bawaan; GPUPixel tidak mensyaratkan rename.
3. **Jangan** redistribusi binary Mars-Face/models di rilis komersial sebelum klarifikasi upstream (issue #253). Di Windows tidak relevan (tidak dibundel).
4. `softcam.dll` teregistrasi di mesin user — sertakan `THIRD_PARTY.md`/`licenses/softcam-LICENSE.txt` di installer + teks atribusi di dialog "About".
5. Sertakan atribusi model: "Face detection & mesh models derived from MediaPipe (Apache-2.0), © Google LLC — converted with tf2onnx; see THIRD_PARTY.md" di About/installer + teks lengkap Apache-2.0 di `licenses/`.
