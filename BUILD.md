# BUILD.md — membangun KameraGeulis

**Peringatan:** seluruh milestone sejauh ini diverifikasi hanya sampai "berhasil dikompilasi+link di CI Windows x64 (Release) headless". Biner **belum pernah dijalankan di mesin Windows nyata dengan GPU/webcam**. Lihat [STATUS.md](STATUS.md).

## Prasyarat (Windows)

- Windows 10/11 x64
- Visual Studio 2022 (workload "Desktop development with C++"), Windows 10/11 SDK
- CMake ≥ 3.21 (VS bundled atau installer terpisah)
- Git
- Koneksi internet saat configure (FetchContent: GPUPixel, imgui, json, Catch2) + download ONNX Runtime win-x64 1.31.1 (82,929,944 B, SHA-256 `5869e26bb25314375f2d5ef1877e3ea06b1543d6c710e6e4a1ec2f7177ab4b2c`) — gagal download = configure gagal dengan pesan jelas.

## Build

```bat
git clone https://github.com/hanlimzie-glitch/KameraGeulis.git
cd KameraGeulis
cmake -S . -B build -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
ctest --test-dir build -C Release --output-on-failure
```

Hasil: `build\Release\KameraGeulis.exe` (subsistem GUI). `onnxruntime.dll` (dan `onnxruntime_providers_shared.dll` bila ada) otomatis disalin ke folder yang sama via POST_BUILD. Runtime GPUPixel (`gpupixel.dll`, `glfw3.dll`) ikut di `build\Release\` melalui build GPUPixel shared.

## Model landmark (opsional, first-run)

Efek landmark memakai dua model ONNX (Apache-2.0, Google MediaPipe, hasil konversi tf2onnx), **tidak dibundel di repo**. Saat aplikasi pertama kali berjalan, `OrtLandmarkProvider` mengunduh dari rilis GitHub `models-v1` ke `%LOCALAPPDATA%\KameraGeulis\models` dan memverifikasi SHA-256 yang tertanam di kode:

- `face_detection_short_range.onnx` — sha256 `613ee7148e56c9d904ba672a7c6065f8c0179ea3c2cc3c451e9ddc6970ab536c`
- `face_mesh.onnx` — sha256 `5497a6c18f18c37ebf38b63b31e98a3f97ed1a7aedf86a882f4627e97a875e8d`

**Status 2026-11-12: rilis `models-v1` belum berisi aset** (upload `gh release upload models-v1 face_detection_short_range.onnx face_mesh.onnx` harus dijalankan maintainer di mesin yang bisa akses GitHub; sandbox tidak bisa). Sampai itu terjadi, download gagal → efek landmark nonaktif dengan pesan jujur di UI/log; aplikasi tetap jalan normal.

Untuk CI/dev tanpa internet: arahkan `KAMENAGEULIS_MODELS_DIR` ke folder yang berisi kedua file di atas (harus lolos SHA-256, tidak diunduh ulang).

## Struktur build

- `kg_core` — logika murni (anchor, decode, ROI, OneEuro), zero GL/OS; unit test jalan di Linux & Windows (`kg_tests`).
- `kg_pipeline` (Windows) — wrapper GPUPixel + `OrtLandmarkProvider` (ONNX Runtime CPU; BlazeFace 128² → eye-aligned ROI → FaceMesh 468² → OneEuro + fade).
- `kg_app` — entry `wWinMain`, ImGui panel.

## Troubleshooting

- **Configure gagal di download ONNX Runtime**: cek koneksi; hapus `build\_deps\onnxruntime-win-x64-1.31.1.zip` lalu configure ulang (file yang korup tidak akan lolos SHA-256).
- **GPUPixel gagal**: hanya didukung Release (bukan Debug) + x64.
- **Efek landmark "model tidak tersedia"**: lihat `%LOCALAPPDATA%\KameraGeulis\models` (ada `face_detection_short_range.onnx`/`face_mesh.onnx`?); jalankan dari konsol/log untuk melihat alasan gagal (`KAMENAGEULIS_MODELS_DIR` mem-bypass download).
- **Kamera virtual**: butuh `softcam.dll` (dibangun terpisah, M3/M5) + registrasi admin 1× — belum termasuk artifact M1/M1b.
