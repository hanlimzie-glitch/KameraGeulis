# TESTING-WINDOWS.md — Menguji KameraGeulis (artifact CI) di Windows nyata

**Tanggal:** 2026-11-14 · **Artifact:** `kamerageulis-m2-windows-x64` · **Run terakhir (M2f — regresi reader + guard F5, lihat §9):** [43551626084](https://github.com/hanlimzie-glitch/KameraGeulis/actions/runs/43551626084) · **Kedaluwarsa:** 2026-11-28

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
1. Buka <https://github.com/hanlimzie-glitch/KameraGeulis/actions/runs/43431139047>
2. Scroll ke bagian **Artifacts** di bawah → klik `kamerageulis-m2-windows-x64` → unduh zip.
3. Ekstrak zip ke folder mana pun (mis. `C:\KameraGeulis\`).

**Cara B — via gh CLI:**
```bat
gh run download 43431139047 -R hanlimzie-glitch/KameraGeulis -n kamerageulis-m2-windows-x64 -D C:\KameraGeulis
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
| 8 | Tab **Kamera** | Daftar device (nama kamera kamu); resolusi & fps dari mode native; tombol **Terapkan** mengganti mode; **Segarkan** memuat ulang; bila kosong → fallback 1280×720/640×480 + peringatan. Di `--safe`, tab Kamera ter-nonaktif (label: kamera dinonaktifkan di mode aman) | |
| 9 | Tab **Info** | Status landmark: hijau "Aktif" (bila model berhasil diunduh) atau oranye "Tidak aktif" + alasan; checkbox **Overlay debug** | |
| 10 | Tekan **F3** (atau checkbox di Info) | Titik **468** kuning + **bounding box** cyan + HUD (FPS, cap/det/mesh/trk/render ms, "sejak deteksi N") di atas preview | |
| 11 | Tutup dengan tombol X | Keluar bersih, tidak ada dialog error | |
| 12 | Jalankan ulang | Settings sebelumnya (slider, preset, bahasa, mirror) **dimuat otomatis** — log menulis `settings: loaded` | |
| 13 | Toggle bahasa ID ↔ EN (status bar) | Seluruh teks UI berganti bahasa seketika | |
| 14 | Centang **Mirror** | Preview terflip horizontal | |

## 4a. Artifact M2d — isi folder (untuk verifikasi cepat di PC pengguna)

Folder artifact hasil CI WAJIB berisi (langkah "Verify artifact contents" di CI menggagalkan build bila tidak):
- `KameraGeulis.exe`, `gpupixel.dll`, `gpupixel.pdb`, `gpupixel.map` (simbol crash; tidak dipakai runtime)
- `onnxruntime.dll`, `vcruntime140.dll`, `vcruntime140_1.dll`, `msvcp140.dll` (app-local CRT, tanpa admin)
- `res\*.png` — 7 file: `lookup_gray.png`, `lookup_origin.png`, `lookup_skin.png`, `lookup_light.png` (WAJIB — tanpa ini frame pertama crash 0x3968a; lihat docs/GPUPIXEL.md) + `lookup_custom.png`, `blusher.png`, `mouth.png`
- `models\face_detection_short_range.onnx`, `models\face_mesh.onnx` (hash SHA-256 diverifikasi saat load; urutan pencarian `KAMERAGEULIS_MODELS_DIR` → `<exe>\models` → `%LOCALAPPDATA%\KameraGeulis\models`)
- `THIRD_PARTY.md`, `README.md`, `STATUS.md`

Setelah unzip ke D:\Project Camera (atau folder pilihanmu), cukup jalankan `KameraGeulis.exe`. First-run **tidak** butuh internet.

## 5. Yang TIDAK akan bekerja (memang belum)


- **Efek landmark apa pun** (face slim, lipstick, …) — disabled `Segera hadir` (v1: pemetaan 468→GPUPixel-Mars tidak kredibel, docs/LANDMARKS.md §8).
- **Kamera virtual** (toggle disabled) — M3, butuh softcam + registrasi admin 1×.
- **Snapshot PNG (tombol "Snapshot" di Info)** — masih disabled; yang ada sekarang adalah **F5 dump debug** (lihat §8), bukan fitur snapshot pengguna.
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
- Model: `<exe>\models\` (dibundel di artifact) → fallback `%LOCALAPPDATA%\KameraGeulis\models\` → override env `KAMERAGEULIS_MODELS_DIR` (dipakai aplikasi DAN `--selftest` dengan urutan sama)
- Log: stderr → alihkan ke file seperti §3.
- Dump debug F5: `%APPDATA%\KameraGeulis\debug\raw_*.png` + `out_*.png` (M2e).

## 8. Artifact M2e — uji singkat F4/F5 + apa yang dikirim balik

> **Jujur:** preview kamera (hitam/terang), orientasi gambar, F4, F5, dan
> penerimaan mode 1280×720@30 **belum pernah diverifikasi di Windows nyata** —
> logika konversi/PNG sudah teruji di Linux, tetapi GPU/webcam hanya ada di
> PC kamu. Hasil ujianmu di bawah adalah kebenaran final.

**Langkah uji singkat (urut, ~2 menit):**

1. Jalankan `KameraGeulis.exe` (kamera aktif, bukan `--safe`).
   - **UJI UTAMA:** apakah preview menampilkan gambar kamera (wajah/ruangan),
     bukan hitam kosong? Baris overlay bawah harus berubah dari
     "Menunggu frame kamera..." menjadi "N frame kamera diterima, M diproses".
2. Tekan **F4** (atau centang **"Tampilkan kamera mentah (tanpa GPUPixel)"**
   di tab Info): preview = frame mentah tanpa efek, warna normal, tegak.
   Tekan **F4** lagi untuk kembali ke jalur GPUPixel (bandingkan keduanya —
   keduanya harus menampilkan gambar; bila salah satu terbalik/aneh, catat).
3. Tekan **F5** (atau tombol **"Simpan frame debug PNG (F5)"** di tab Info):
   cek folder `%APPDATA%\KameraGeulis\debug\` → ada `raw_*.png` + `out_*.png`.
   Buka keduanya: `raw_` = frame kamera mentah, `out_` = keluaran pipeline.
4. Lihat tab **Info**: baris **"Kamera: WxH @ fps, stride, subtype, luma, alpha"**
   (mode aktual hasil baca-balik MF) + log `diag` tiap detik.

**File yang dikirim balik:**

1. `log.txt` (lengkap — harus memuat baris `capture started (aktual)`,
   `diag: MF: ...`, `F4:`, `F5: ... -> path`, dan `glGetError` bila ada).
2. Kedua PNG dari `%APPDATA%\KameraGeulis\debug\` (`raw_*.png` + `out_*.png`).
3. Screenshot jendela aplikasi (preview + tab Info) dan, bila ada yang aneh,
   screenshot khusus mode F4.
4. Bila crash: `crash-*.dmp` + pesan Event Viewer ID 1000 (§4).

## 9. Artifact M2f — uji regresi kamera + F5 (setelah log-3.txt)

> Regresi yang kamu temukan: (1) reader `E_INVALIDARG 0x80070057` (atribut
> M2e), (2) crash F5 `0xC0000005` sebelum frame pertama. Artifact M2f memakai
> tangga fallback + guard readback. Langkah singkat:

1. **Normal:** jalankan `KameraGeulis.exe` — harap log memuat, berurutan:
   `MF capture start: ... -> thread dijalankan (menunggu reader + frame pertama)`,
   `reader percobaan tingkat 2 (penuh...): hr=0x80070057 GAGAL` (atau OK),
   `reader percobaan tingkat 1 (basic...): hr=0x00000000 OK`,
   `MF capture start: OK - frame pertama diterima ...` — lalu preview hidup.
   Bila gagal: baris merah **"Kamera gagal: 0x... (mencoba lagi...)"** di tab
   Kamera + status bar, petunjuk pemakaian aplikasi lain, tombol **Coba lagi**,
   dan ulang otomatis ber-jeda 1→2→4→8 detik.
2. **F4** (kamera jalan): preview mentah; bila kamera belum/bisa mati:
   overlay **"Tidak ada frame kamera"** (tidak diam lagi).
3. **F5** tanpa frame: log `F5: tidak ada frame keluaran ... - dilewati`,
   **tidak crash**; dengan frame: PNG `raw_*` + `out_*` seperti M2e.
4. **`KameraGeulis.exe --capture-plain`** (bila perlu): log menunjukkan
   `flags: ... --capture-plain=1` dan hanya tingkat `(c) polos (tanpa atribut)`
   yang dicoba.

**Kirim balik:** `log.txt` lengkap (termasuk baris `reader percobaan tingkat`
dan `MF capture start:`), screenshot tab Kamera bila gagal, PNG F5 bila ada.
