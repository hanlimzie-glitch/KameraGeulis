# REDISTRIBUTABLES.md — DLL yang wajib ikut di distribusi (M1c)

**Status:** ditetapkan 2026-11-12 (M1c). Belum diverifikasi di Windows nyata — tidak ada jaminan runtime sampai diuji di mesin bersih.

## Keputusan CRT

**Semua target dibangun `/MD` (dynamic CRT) — bukan `/MT`.** Alasannya:

1. **ONNX Runtime (onnxruntime.dll)** adalah binary prebuilt yang selalu `/MD` dan membutuhkan `vcruntime140.dll`, `vcruntime140_1.dll`, `msvcp140.dll`. `/MT` di target kita **tidak** menghilangkan kebutuhan ini.
2. **GPUPixel** membangun DLL-nya dengan `MultiThreadedDLL` (lihat `src/CMakeLists.txt` upstream); memaksakannya `/MT` tidak didukung upstream dan tidak memberi apa-apa.
3. Deployment **app-local** dari 3 DLL tersebut eksplisit diizinkan oleh Microsoft ("Distributing Visual C++ Runtime" / VS license: daftar redistributable termasuk `vcruntime140.dll`, `vcruntime140_1.dll`, `msvcp140.dll`) dan **tidak butuh install / admin**.

Konsekuensi: paket `kamerageulis-*-windows-x64` (artifact CI) bersifat mandiri — tidak butuh "Visual C++ Redistributable" terpasang di mesin user.

## DLL yang dibundel di artifact CI

| DLL | Sumber | Lisensi | Kewajiban |
|---|---|---|---|
| `KameraGeulis.exe` | build repo ini | (tanpa LICENSE — keputusan owner, lihat STATUS.md) | — |
| `gpupixel.dll` | GPUPixel `v1.3.1` (FetchContent, build dari source) | Apache-2.0 (© 2021 PixPark) | Pertahankan notice (THIRD_PARTY.md; tidak dimodifikasi) |
| `onnxruntime.dll` | ONNX Runtime `v1.31.1` win-x64 CPU (zip, SHA-256 `5869e26b…`) | MIT | atribusi |
| `onnxruntime_providers_shared.dll` | idem (jika ada di zip; biasanya ~13 KB) | MIT | atribusi |
| `vcruntime140.dll` | VS 2022 redist (`VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT`) | VS redist license (app-local OK) | tidak dimodifikasi |
| `vcruntime140_1.dll` | idem | idem | idem |
| `msvcp140.dll` | idem | idem | idem |
| `THIRD_PARTY.md`, `README.md`, `STATUS.md` | repo | — | atribusi |

**Dibundel sejak M2d (artifact):** `models\face_detection_short_range.onnx` + `models\face_mesh.onnx` (~2,8 MB, Apache-2.0, dari release `models-v1` yang diverifikasi CI; SHA-256 pin di kode; diunduh `gh release download` saat packaging) dan `res\*.png` (7 file, Apache-2.0, dari source GPUPixel ref yang di-pin) — tanpa keduanya app crash (lihat `docs/GPUPIXEL.md`). First-run TIDAK butuh internet.

**Tidak** dibundel (sengaja): `softcam.dll` (M3, butuh registrasi admin + build softcam terpisah), `mars-face*` (tidak ada di Windows). `gpupixel.pdb` + `gpupixel.map` **ikut artifact** untuk symbolisasi dump (bukan untuk runtime).

## Yang belum diverifikasi

- Ekstraksi artifact → jalankan di **Windows bersih** tanpa VC++ redist terpasang — belum pernah diuji (tidak ada Windows di lingkungan pengembang). Sampai uji nyata ada, klaim "jalan di mesin bersih" tidak boleh dibuat.
- GPU/webcam runtime (capture, GL, ORT perf) — belum diverifikasi. Lihat STATUS.md.
