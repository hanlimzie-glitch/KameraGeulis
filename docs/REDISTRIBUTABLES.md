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

**Tidak** dibundel (sengaja): `softcam.dll` (M3, butuh registrasi admin + build softcam terpisah), model `.onnx` (diunduh first-run dari release `models-v1` atau dipakai dari `KAMERAGEULIS_MODELS_DIR`), `mars-face*` (tidak ada di Windows).

## Yang belum diverifikasi

- Ekstraksi artifact → jalankan di **Windows bersih** tanpa VC++ redist terpasang — belum pernah diuji (tidak ada Windows di lingkungan pengembang). Sampai uji nyata ada, klaim "jalan di mesin bersih" tidak boleh dibuat.
- GPU/webcam runtime (capture, GL, ORT perf) — belum diverifikasi. Lihat STATUS.md.
