# TESTING-WINDOWS.md — Menguji KameraGeulis (artifact CI) di Windows nyata

**Tanggal:** 2026-11-12 · **Artifact:** `kamerageulis-m2-windows-x64` · **Run terakhir (M2c, dengan smoke test):** [43192212350](https://github.com/hanlimzie-glitch/KameraGeulis/actions/runs/43192212350) · **Kedaluwarsa:** 2026-11-26

> Dokumen ini jujur: artifact **belum pernah dijalankan di Windows mana pun** sebelum kamu mengujinya.
> Semua yang di bawah "Checklist" adalah hal yang **perlu kamu verifikasi**, bukan jaminan yang sudah hijau.

## 1. Prasyarat

- Windows 10/11 **x64** (10 1809+ / 11).
- GPU + driver dengan dukungan **OpenGL 3.3+** (Intel/AMD/NVIDIA integrated pun umumnya cukup).
- Webcam (opsional — tanpa kamera, aplikasi menampilkan gradien sintetis).
- Koneksi internet **hanya untuk run pertama** (download model ~2,8 MB dari release `models-v1`).
- **Tidak perlu** install Visual Studio, VC++ redist, atau runtime apa pun — CRT sudah app-local.

## 2. Unduh & ekstrak

**Cara A — via browser (paling mudah):**
1. Buka <https://github.com/hanlimzie-glitch/KameraGeulis/actions/runs/43192212350>
2. Scroll ke bagian **Artifacts** di bawah → klik `kamerageulis-m2-windows-x64` → unduh zip.
3. Ekstrak zip ke folder mana pun (mis. `C:\KameraGeulis\`).

**Cara B — via gh CLI:**
```bat
gh run download 43192212350 -R hanlimzie-glitch/KameraGeulis -n kamerageulis-m2-windows-x64 -D C:\KameraGeulis
```

**Isi folder hasil ekstrak:**
`KameraGeulis.exe`, `gpupixel.dll`, `onnxruntime.dll`, `vcruntime140.dll`, `vcruntime140_1.dll`, `msvcp140.dll`, `THIRD_PARTY.md`, `README.md`, `STATUS.md`.

## 3. Run pertama — apa yang normal

1. **Launch pertama bisa "freeze"/blank beberapa detik** — jendela dibuat dulu, lalu pipeline mengunduh 2 model ONNX ke `%LOCALAPPDATA%\KameraGeulis\models` (`face_detection_short_range.onnx` 418.502 B + `face_mesh.onnx` 2.429.059 B, SHA-256 diverifikasi otomatis). Tunggu, jangan kill.
2. Bila internet mati/diblokir: download gagal → **efek landmark nonaktif** (pesan jujur di tab Info), aplikasi **tetap jalan** (skin smoothing/whitening tetap aktif — tidak butuh landmark).
3. Bila webcam gagal/dicabut/permission ditolak: muncul **gradien sintetis** + status "tidak ada kamera" — itu jalur fallback yang disengaja, bukan crash.
4. Log `kg::Log` menulis ke **stderr**. Karena ini GUI app, cara lihat log:
   ```bat
   cd C:\KameraGeulis
   KameraGeulis.exe 2>log.txt
   ```
   lalu buka `log.txt`.

## 4. Diagnosis dump force-close (artifact M2c)

Artifact M2 dulu force-close di PC kamu. Artifact ini (M2c) menambahkan logging + isolasi. Urutan:

1. `KameraGeulis.exe --selftest` → catat exit code (0 = sehat; selain itu lihat `docs/TROUBLESHOOTING.md` §4).
2. `KameraGeulis.exe --safe` → apakah window + UI muncul (gradien sintetis, tanpa kamera/ORT/GPUPixel)?
3. `KameraGeulis.exe` → normal.
4. Di semua kasus, log tersimpan di `%APPDATA%\KameraGeulis\log.txt` — kirim isinya. Kalau crash, file `crash-*.dmp` muncul di folder yang sama — kirim juga. Faulting module: Event Viewer → Application → Event ID 1000 (lihat TROUBLESHOOTING.md §1).

## 5. Checklist pengujian (urut)

| # | Aksi | Yang diharapkan | Hasil (isi) |
|---|------|-----------------|-------------|
| 1 | Klik dua kali `KameraGeulis.exe` | Jendela terbuka (1440×810), judul "KameraGeulis", tema gelap, tidak ada crash | |
| 2 | Kolom kiri | Preview besar; kolom kanan panel 6 tab: **Kulit / Wajah / Makeup / Preset / Kamera / Info** | |
| 3 | Tab **Kulit** | Slider **Haluskan Kulit** & **Cerahkan Kulit** (0–100%); geser → wajah di preview langsung halus/terang | |
| 4 | Tab **Wajah** / **Makeup** | Semua kontrol (Tiruskan Wajah, Perbesar Mata, Lipstik, Blusher, Foundation, Eyeliner, Eyeshadow, Brow Tint) tampil **disabled** dengan label **"Segera hadir"** — bukan hilang | |
| 5 | Tab **Preset** | Klik **Natural / Soft / Glam / ClearSkin** → slider di tab Kulit ikut berubah; label preset aktif hijau / "Kustom" bila slider digeser manual; tombol **Reset** | |
| 6 | **Tahan** tombol "Tahan untuk Bandingkan" | Preview berganti ke frame mentah selama ditahan; lepas → kembali ke efek | |
| 7 | Tab **Preset** → **Simpan** → ubah slider → **Muat** | Nilai kembali ke yang tersimpan; file di `%APPDATA%\KameraGeulis\settings.json` | |
| 8 | Tab **Kamera** | Daftar device (nama kamera kamu); resolusi & fps dari mode native; tombol **Terapkan** mengganti mode; **Segarkan** memuat ulang; bila kosong → fallback 1280×720/640×480 + peringatan | |
| 9 | Tab **Info** | Status landmark: hijau "Aktif" (bila model berhasil diunduh) atau oranye "Tidak aktif" + alasan; checkbox **Overlay debug** | |
| 10 | Tekan **F3** (atau checkbox di Info) | Titik **468** kuning + **bounding box** cyan + HUD (FPS, cap/det/mesh/trk/render ms, "sejak deteksi N") di atas preview | |
| 11 | Tutup dengan tombol X | Keluar bersih, tidak ada dialog error | |
| 12 | Jalankan ulang | Settings sebelumnya (slider, preset, bahasa, mirror) **dimuat otomatis** — log menulis `settings: loaded` | |
| 13 | Toggle bahasa ID ↔ EN (status bar) | Seluruh teks UI berganti bahasa seketika | |
| 14 | Centang **Mirror** | Preview terflip horizontal | |

## 5. Yang TIDAK akan bekerja (memang belum)

- **Efek landmark apa pun** (face slim, lipstick, …) — disabled `Segera hadir` (v1: pemetaan 468→GPUPixel-Mars tidak kredibel, docs/LANDMARKS.md §8).
- **Kamera virtual** (toggle disabled) — M3, butuh softcam + registrasi admin 1×.
- **Snapshot PNG** — tombol disabled.
- **Recording MP4** — belum diimplementasikan.

## 6. Bila sesuatu gagal — apa yang perlu dicatat/dikirim

1. Langkah checklist mana yang gagal (nomor).
2. Apakah `log.txt` (dari §3) ada? Kirim isinya.
3. Pesan error/crash dialog persis (screenshot).
4. Spesifikasi: GPU + versi driver, resolusi webcam, versi Windows.
5. Apakah folder `%LOCALAPPDATA%\KameraGeulis\models` berisi 2 file `.onnx`? Ukuran cocok?
6. Screenshot tab **Info** (status landmark) dan overlay F3 bila wajah terdeteksi.

## 7. Path yang dipakai aplikasi

- Settings: `%APPDATA%\KameraGeulis\settings.json`
- Model: `%LOCALAPPDATA%\KameraGeulis\models\` (bisa dioverride via env `KAMERAGEULIS_MODELS_DIR`)
- Log: stderr → alihkan ke file seperti §3.
