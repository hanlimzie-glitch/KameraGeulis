# TROUBLESHOOTING.md — Diagnosis KameraGeulis di Windows nyata

> Dibuat untuk mempercepat diagnosis crash di mesin pengguna.
> Artifact M2c **belum diverifikasi di Windows nyata mana pun** — dokumen ini adalah alat ukur, bukan jaminan.

## 0. Ringkasan cepat

| Jalur | Perintah | Hasil |
|---|---|---|
| Run normal | `KameraGeulis.exe` | Jendela + kamera (atau gradien sintetis bila kamera gagal) |
| Mode aman | `KameraGeulis.exe --safe` | Jendela + UI saja — **tanpa** kamera, **tanpa** ORT, **tanpa** GPUPixel (menguji apakah crash berasal dari window/UI murni) |
| Smoke test | `KameraGeulis.exe --selftest` | Tidak ada window; memuat DLL + ORT + kedua model (dari `KAMERAGEULIS_MODELS_DIR` atau `<folder-exe>\models`), 1 inferensi per model, exit code 0 = PASS |

## 1. Mengumpulkan data untuk laporan bug

Kumpulkan **empat** hal ini:

1. **Log** — selalu ditulis ke `%APPDATA%\KameraGeulis\log.txt` (append; ditulis per baris + di-flush langsung, jadi walaupun crash, baris terakhir tetap tersimpan). Cara membukanya: `Win+R` → ketik `%APPDATA%\KameraGeulis` → buka `log.txt`. Atau dari PowerShell:
   ```powershell
   notepad "$env:APPDATA\KameraGeulis\log.txt"
   ```
2. **Dump crash** — bila app crash, file `crash-YYYYMMDD-HHMMSS.dmp` muncul di folder yang sama (`%APPDATA%\KameraGeulis\`). Dibuat otomatis oleh crash handler (SetUnhandledExceptionFilter + MiniDumpWriteDump). Ukurannya bisa beberapa MB — lampirkan bila < 20 MB.
3. **Faulting module** — modul yang menyebabkan crash, dari Event Viewer:
   - `Win+R` → `eventvwr.msc` → **Windows Logs → Application** → cari entri **Application Error** (Event ID **1000**) waktu crash → catat **Faulting module name** dan **Faulting module version**.
   - Atau PowerShell:
     ```powershell
     Get-WinEvent -FilterHashtable @{LogName='Application'; Id=1000} -MaxEvents 3 |
       Format-List TimeCreated, Message
     ```
4. **Screenshot** — jendela crash/messagebox yang muncul, dan (bila sempat terbuka) tab **Info**.

## 2. Menjalankan aplikasi dengan log di foreground

GUI app tidak punya konsol; pakai redirect supaya stderr juga masuk ke file:
```bat
cd C:\path\ke\KameraGeulis
KameraGeulis.exe 2>log-stderr.txt
```
Catatan: `%APPDATA%\KameraGeulis\log.txt` tetap yang paling lengkap (ber-timestamp per baris).

## 3. Menafsirkan log startup

Baris penting dan maknanya:

```
[startup] windows: Windows 10.0.19045 (build 19045)   ← versi Windows (dari RtlGetVersion)
[startup] cmdline: ...                                 ← argumen (--safe? --selftest?)
[startup] exe_dir: C:\path\...\                        ← folder DLL dicari
[startup] crash handler terpasang ...                  ← sebelum init apa pun
[startup] SetDllDirectory(C:\path\...\) -> OK          ← pencarian DLL dialihkan ke folder app
[startup] onnxruntime: dimuat dari C:\path\...\onnxruntime.dll   ← HARUS folder app, BUKAN C:\Windows\System32
[startup] onnxruntime: OrtGetApiBase OK — version string '1.31.1'
[startup] gpupixel: dimuat dari C:\path\...\gpupixel.dll
[gl] VENDOR/RENDERER/VERSION/GLSL: ...                 ← GPU & driver asli yang menjawab
[app] GL context: OpenGL 3.3 core — BERHASIL/GAGAL ... ← dicoba 3 tahap; lihat log detail
[app] pipeline GPUPixel init: OK / GAGAL ...           ← GAGAL = preview raw, app tetap hidup
[app] MF capture start: ... -> OK / GAGAL ...          ← gagal = gradien sintetis
```

**Kalau onnxruntime dimuat dari `C:\Windows\System32\onnxruntime.dll`** (bukan folder app), itu bug — laporkan; seharusnya preload app-local + `SetDllDirectory` mencegah hal ini.

## 4. Exit code proses

| Kode | Arti |
|---|---|
| 0 | Normal / selftest PASS |
| 2 | selftest: preload DLL gagal (file hilang/terhalang/antivirus) |
| 3 | selftest: `OrtGetApiBase`/`GetApi` gagal — onnxruntime rusak/tidak cocok |
| 4 | selftest: model ONNX hilang/ukuran/SHA-256 tidak cocok |
| 5 | selftest: sesi ORT gagal dibuat (model korup/tidak kompatibel) |
| 6 | selftest: inferensi gagal / output NaN / shape meleset |
| 10 | APPDATA tidak tersedia / folder log tidak bisa dibuat |
| 11 | Preload DLL runtime gagal (fatal, normal mode) |
| 12 | `glfwInit()` gagal (driver/OpenGL rusak total) |
| 13 | Semua percobaan GL context gagal (3.3 core → 3.3 any → 4.6 core) |
| 14 | glad gagal memuat fungsi GL (driver/konteks tidak menyediakan) |

## 5. "Faulting module" umum dan apa artinya

| Module | Kemungkinan besar |
|---|---|
| `KameraGeulis.exe` | Bug aplikasi — kirim log + dump + langkah repro |
| `onnxruntime.dll` / `onnxruntime_providers_shared.dll` | ORT load/run gagal — cek baris `onnxruntime:` di log; cek apakah path-nya folder app |
| `gpupixel.dll` | Shaders gagal diverifikasi / driver GL di bawah standar — cek `[gl]` baris |
| `glfw3.dll` | Window/context creation — cek baris `[app] GL context:` |
| `dwmapi.dll` / `user32.dll` | Umumnya *korban*, bukan penyebab — lihat exception code di Event Viewer (0xC0000005 = access violation dsb.) |
| `nvwgf2umx.dll` / `atio6axx.dll` / `igdumdim64.dll` | Driver GPU crash — perbarui driver; coba `--safe` (kalau `--safe` juga crash → bukan GPU) |

## 6. Memisahkan komponen penyebab crash (gunakan berurutan)

1. `KameraGeulis.exe --selftest` → DLL + ORT + model OK? (exit 0)
2. `KameraGeulis.exe --safe` → window + UI hidup? (tanpa kamera/ORT/GPUPixel)
3. `KameraGeulis.exe` → normal
4. Jika (1) gagal → masalah DLL/ORT (kode 2–6); jika (2) gagal → masalah window/UI/GL driver; jika (3) gagal tapi (2) OK → masalah kamera/MF atau pipeline GPUPixel.

## 7. Catatan jujur

- `--selftest` **tidak menguji GL/GPUPixel nyata** (butuh GPU). Keluar 0 ≠ "semua baik-baik saja", hanya "DLL+ORT+model OK".
- Crash handler menangkap **unhandled exception** dan **std::terminate** — bukan GFlags/fast-fail murni (mis. `int3` yang difang ≠ 0xC0000005 tetap tercatat di Event Viewer).
- Dump berisi memory process (mungkin termasuk isi frame) — perlakukan sebagai data pribadi.
