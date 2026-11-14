# STATUS.md — KameraGeulis (jujur, diperbarui per milestone)

Terakhir diperbarui: **2026-11-14 (M2g — objek IMFMediaSource BARU per tingkat tangga (bug MF_E_SHUTDOWN di log-4), urutan baru basic→polos→penuh, log device/ActivateObject/tingkat dipakai, UI kode awal+akhir + nama simbolis + petunjuk sibuk hanya untuk kode yang benar)**
Legenda status:
- ✅ selesai & diverifikasi di lingkungan yang bisa kami akses
- 🔨 sedang/selesai dikerjakan, **belum** diverifikasi di Windows nyata
- ⛔ diblokir fakta/lisensi/aksi di luar sandbox kami
- ⬜ belum dimulai

| # | Fitur | Implemented | Compiled in CI (Windows) | Tested on real Windows | Known issues |
|---|---|---|---|---|---|
| M0 | Riset + capability check + arsitektur | ✅ | ✅ capability workflow hijau (vswhere/cmake/git) | n/a | Log Actions tidak bisa dibaca dari sandbox (lihat di bawah) |
| M1 | CMake + CI build window + GPUPixel + camera preview | 🔨 | ✅ (run 43166017897) | ⬜ | Runtime GL/MF BELUM diverifikasi (tidak ada GPU/webcam di CI/sandbox); artifact M1: `kamerageulis-m1-windows-x64` (581 KB) |
| M1b | Pipeline landmark (ORT CPU, 468 pt) + kg_core decode/ROI/OneEuro | 🔨 | ✅ (run 43174282611) | ⬜ | Windows runtime belum diverifikasi di mesin nyata; GPUPixel landmark filters **tidak di-wire** (pemetaan 468→Mars tidak kredibel, docs/LANDMARKS.md §8); kg_core hijau di Linux (45 TC / 8319 assertions) |
| M2 | UI ImGui lengkap + slider skin live + debug overlay | ✅ (dikode) | ✅ (run 43183762692) | ⬜ (crash: force-close saat startup) | belum diverifikasi di Windows nyata; snapshot PNG belum |
| M2c | Stabilitas startup: log file flush/baris, crash handler + dump + MessageBox, preload DLL app-local + OrtGetApiBase, GL fallback 3 tahap, `--safe`, `--selftest` + CI smoke test | ✅ | ✅ (run 43192212350 — termasuk step smoke test `--selftest` hijau) | ⬜ | belum diverifikasi di PC pengguna; penyebab force-close belum diketahui — setelah M2c, kegagalan tercatat di log.txt/dump, bukan keluar diam-diam |
| M2d | Crash startup gpupixel.dll 0x3968a (resource `res/` hilang → null lookup → AV di BeautyFaceUnitFilter::DoRender); models ikut artifact; urutan pencarian model disatukan (env → exe\\models → LOCALAPPDATA); PDB/MAP + symbolizer RVA di CI; safe-mode sepenuhnya inert (objek pipeline tidak dibuat; tab Kamera disabled; Terapkan/apply dimatikan); FPS render vs kamera dipisah label; vsync checkbox; resource hilang → efek GPU nonaktif + pesan UI (bukan crash) | ✅ (dikode) | ✅ (run terakhir hijau) | ⬜ | belum diverifikasi di PC pengguna — fix terbukti di level source + CI (artifact wajib berisi res/ & models/, `--selftest` hijau dari dist) |
| M2e | Diagnosis preview hitam: konversi BGRA→RGBA (alpha 255) sebelum GPUPixel (akar masalah: upload BGRA gpupixel no-op di Windows), F4 frame mentah, F5 dump PNG, log diagnosis 1 detik, negosiasi mode kamera (advanced processing + read-back + reinit saat ukuran berubah), teks status frame | ✅ (dikode) | ⬜ (CI dijalankan setelah push) | ⬜ | logika konversi/stride/flip/PNG **teruji di Linux**; preview hitam/putih, orientasi UV, F4/F5, dan scaling kamera **belum bisa diverifikasi tanpa GPU + webcam** (lihat "Batasan" + bagian M2e) |
| M2f | Regresi M2e: tangga fallback atribut source reader (penuh→basic→polos, HRESULT per percobaan, --capture-plain), status kamera jujur ("OK" hanya setelah frame pertama), retry backoff 1-8 dtk + tombol "Coba lagi" + petunjuk, guard readback GPUPixel (crash F5 0xC0000005 sebelum frame pertama), overlay F4 "Tidak ada frame kamera" | ✅ (dikode) | ⬜ (CI dijalankan setelah push) | ⬜ | logika tangga+guard teruji di Linux (7 TC baru); E_INVALIDARG 0x80070057 pengguna TIDAK terdokumentasi di docs MS (diperiksa: kedua atribut sah secara terpisah; kombinasi ditolak runtime di PC-nya) → tangga turun ke tingkat yang terbukti; regresi BGRA tetap teratasi (belum terbukti karena frame belum pernah masuk) |
| M2g | Bug log-4: kegagalan membunuh IMFMediaSource lalu tingkat berikut = MF_E_SHUTDOWN → **objek source BARU per tingkat** (ActivateObject ulang), urutan tangga baru **basic→polos→penuh (paling akhir)**, log jumlah/nama device + hr ActivateObject per tingkat + tingkat dipakai, UI **kode awal+akhir** dengan nama simbolis, petunjuk "dipakai aplikasi lain" **hanya** E_ACCESSDENIED/ERROR_SHARING_VIOLATION, audit situs reuse MF lain | ✅ (dikode) | ⬜ (CI dijalankan setelah push) | ⬜ | tiruan objek mati meniru log-4 persis (E_INVALIDARG lalu SHUTDOWN×2); test pola baru gagal di kode lama & lulus di kode baru; docs MS: default-nya reader me-Shutdown sumber saat di-release → mekanisme bug terkonfirmasi dokumentasi |
| M3 | Virtual camera (softcam) | ⬜ | ⬜ | ⬜ | butuh admin 1×; belum diuji OBS/Discord |
| M4 | Makeup/reshape kustom + preset + settings | ⬜ | ⬜ | ⬜ | **Efek landmark nonaktif di Windows (default)** — Mars-Face Windows tidak tersedia publik |
| M5 | Perf pass, README, installer notes | ⬜ | ⬜ | ⬜ | — |
| — | Skin smoothing / whitening (global) | ✅ | ✅ | ⬜ | terhubung live (per-frame SetBlurAlpha/SetWhite); dirancang tanpa landmark |
| — | Face slim / big eyes / lipstick / blush (built-in) | ⬜ | ⬜ | ⬜ | butuh Mars-Face; Windows: disabled (pesan jujur di UI) |
| — | Chin, nose, eye brighten, brow tint, eyeshadow, eyeliner, foundation, lip finish | ⬜ | ⬜ | ⬜ | idem |
| — | Settings JSON persist | ✅ | ✅ | ⬜ | auto-load `%APPDATA%\KameraGeulis\settings.json`; tombol Simpan/Muat; round-trip+clamp diuji di kg_tests |
| — | Snapshot PNG | ⬜ | ⬜ | ⬜ | — |
| — | Mirror toggle | ✅ | ✅ | ⬜ | CPU-mirror di preview (flip horizontal); round-trip buffer mirror diuji di kg_core standar |
| — | Preset Natural/Soft/Glam/ClearSkin | ✅ | ✅ | ⬜ | nilai preset teruji di kg_tests (Natural 0.25/0.10, Soft 0.45/0.20, Glam 0.65/0.32, ClearSkin 0.55/0.26); slider manual → preset "Kustom" |
| — | Recording (MP4) | ⬜ stretch | ⬜ | ⬜ | kemungkinan dibatalkan; akan dilaporkan jujur |
| — | FPS/GPU stats overlay | ✅ (F3) | ✅ | ⬜ | debug overlay: 468 titik + bbox + HUD (FPS, capture/detect/mesh/track/render ms, frame sejak deteksi) |
| — | Hotkeys | 🟡 sebagian | ⬜ (M2e menyusul) | ⬜ | F3 = overlay debug; F4 = toggle kamera mentah (M2e); F5 = dump PNG debug (M2e) |
| — | MFCreateVirtualCamera (Win11 only) | ⬜ stretch | ⬜ | ⬜ | didokumentasikan sbg opsi masa depan |

## Batasan lingkungan (aktual, per 2026-11-12)

- **GPU di CI Windows tidak ada.** "Compiled in CI" ≠ "berjalan mulus". Verifikasi runtime GL tidak mungkin di workflow ini.
- **Log Actions tidak bisa diunduh dari sandbox ini** (`results-receiver.actions.githubusercontent.com` diblokir proxy). Yang bisa dibaca: status job/step (via `gh api .../jobs`) dan anotasi `::error::` yang sengaja diemit workflow sebagai pengganti log mentah.
- **Tidak bisa apt-install paket X11/GL** di sandbox (deb.debian.org diblokir) → verifikasi GPU lokal di Linux **tidak mungkin**; hanya kompilasi unit test logika murni yang jalan lokal.
- **Tidak ada webcam** di sandbox → capture tidak pernah di-test di sandbox; hanya di `windows-latest` (dan itupun headless, tanpa kamera).

## Keputusan jujur yang sudah diambil

1. Windows default: **tanpa Mars-Face** (sesuai upstream GPUPixel). Efek landmark dijadwalkan aktif via **ONNX Runtime + model hasil konversi Apache-2.0 (468 titik)** — rencana, provenance & checksum di `docs/LANDMARKS.md`; sampai M1b mendarat, efek landmark tetap **disabled** dengan pesan jujur di UI. Alasan historis: `docs/RESEARCH.md` §1 & §6.
2. Tidak ada model wajah di-bundling di Windows; model Mars-Face hanya ada di build Linux/macOS via GPUPixel dan lisensinya belum dikonfirmasi upstream (issue #253) → tidak untuk rilis komersial sebelum klarifikasi.
3. Kamera virtual = softcam (DirectShow) + **registrasi admin 1×**; nama device dipatch jadi `KameraGeulis Camera`. Windows 10: hanya DShow consumers yang melihatnya; Win11 MF-only apps tidak.
4. Setiap milestone CI meng-upload artifact (`kamerageulis-mN-windows-x64`; M1b: `kamerageulis-m1b-windows-x64`). Log mentah Actions tidak bisa diunduh dari sandbox — error MSVC dibaca via anotasi `::error::` (workflow memancarkannya; lihat `.github/workflows/build.yml` langkah Build).
5. Detil fix CI selalu dianotasikan di commit message fix (analisis + akar masalah + perbaikan).

## M2g — detail (2026-11-14)

**Masalah (log-4.txt pengguna):** tiap siklus retry: tingkat 2 (penuh)
`hr=0x80070057` (E_INVALIDARG) → tingkat 1 (basic) `hr=0xC00D3E85` → tingkat 0
(polos) `hr=0xC00D3E85` = **MF_E_SHUTDOWN**. Dugaan user: percobaan pertama yang
gagal mematikan IMFMediaSource, lalu tingkat berikutnya memakai objek yang sama
yang sudah mati → konfigurasi M2d yang terbukti tak pernah jalan.

**Verifikasi di kode:** benar - tangga M2f memakai SATU `source` untuk ketiga
tingkat tanpa Shutdown di antaranya; `MFCreateSourceReaderFromMediaSource` yang
gagal melepas reader internalnya, dan menurut dokumentasi Microsoft
(`MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN`): *"By default, when the
application releases the source reader, the source reader shuts down the media
source"* → sumber ikut mati (MF_E_SHUTDOWN pada pemakaian ulang).

**Perubahan (M2g):**
1. **Objek source baru per tingkat** (`camera_capture.cc`): `activate` disimpan
   lintas tingkat; tiap tingkat: Shutdown+release sisa objek gagal →
   `ActivateObject` baru (hr dicetak per tingkat) → baru coba reader. Tidak ada
   objek MF yang dipakai ulang setelah kegagalan.
2. **Urutan tangga baru** (`DefaultReaderLadder`): **basic (M2d) → polos →
   penuh (usaha terakhir)** - konfigurasi terbukti dicoba pertama pada objek
   segar. `--capture-plain` tetap hanya polos.
3. **Log perangkat vs reader terpisah:** `device: N ditemukan, memakai index I
   "NAMA"`, `ActivateObject (objek baru, tingkat X): hr=...`,
   `reader percobaan tingkat X ...`, `MF capture: tingkat dipakai = X (...)`.
4. **UI:** baris merah `Kamera gagal: 0xTERAKHIR (NAMA), awal 0xAWAL (NAMA)
   (mencoba lagi...)`; nama simbolis via `MfErrorName()` (E_INVALIDARG, E_FAIL,
   E_ACCESSDENIED, ERROR_SHARING_VIOLATION, ERROR_NOT_FOUND, MF_E_SHUTDOWN);
   petunjuk "dipakai aplikasi lain atau akses diblokir" **hanya** untuk
   E_ACCESSDENIED (izin diblokir) dan ERROR_SHARING_VIOLATION (dipakai proses
   lain) - MF tidak mendokumentasikan HRESULT khusus "kamera dipakai aplikasi
   lain". Untuk E_INVALIDARG/MF_E_SHUTDOWN tampil baris netral "Lihat log…".
5. **Uji Linux (68 TC / 8666 asersi):** pabrik tiruan objek yang MATI setelah
   kegagalan - pola lama (urutan lama + reuse) mereproduksi log-4 persis
   (E_INVALIDARG, SHUTDOWN, SHUTDOWN → -1); pola baru (urutan M2g + objek
   segar) → basic sukses 1 percobaan (test ini GAGAL bila dijalankan terhadap
   kode lama); + tes `MfErrorName`/`WantsCameraBusyHint`.
6. **Audit reuse MF lain:** Coba lagi/apply = `Stop()`+`Start()` (siklus segar
   penuh) OK; `EnumerateModes` = source+reader sendiri, gagal → Shutdown OK;
   reinit perubahan ukuran = hanya pipeline GPUPixel, MF tak disentuh OK.
   Satu-satunya situs reuse = tangga (diperbaiki).

**Belum terverifikasi (jujur):** perilaku Windows asli (aktivasi ulang per
tingkat) belum diuji di PC pengguna - hanya tiruan Linux; E_INVALIDARG penuh
masih bisa muncul di tingkat TERAKHIR (memang usaha terakhir).

## M2f — detail (2026-11-14)

**Bukti PC pengguna (log-3.txt):** (1) `MFCreateSourceReaderFromMediaSource failed (0x80070057)` — M2e menambahkan `MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING` bersama `MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING` → regresi (M2d dengan flag basic saja menghasilkan 329/512 frame). (2) F5 tanpa frame pernah masuk → `SinkRawData::GetRgbaBuffer()` me-run `glReadPixels` pada framebuffer yang belum pernah dibuat → AV 0xC0000005 di gpupixel.dll (RVA 0x153a6, `mov edx,[rcx+0x2c]` rcx=0). (3) F4 tanpa frame = diam.

**Dokumentasi Microsoft yang diperiksa (repo MicrosoftDocs/win32, bukan tebakan):** `mf-source-reader-enable-video-processing.md` (UINT32; bila TRUE: D3D_MANAGER & DISABLE_CONVERTERS harus FALSE), `mf-source-reader-enable-advanced-video-processing.md` (BOOL/UINT32; bila TRUE: DISABLE_CONVERTERS harus FALSE; "similar to ... ENABLE_VIDEO_PROCESSING, but offers ... advantages"), `source-reader-attributes.md` (kedua atribut sah untuk `MFCreateSourceReaderFromMediaSource`). Kedua syarat selalu terpenuhi (kita tidak pernah set DISABLE_CONVERTERS/D3D_MANAGER). **Docs TIDAK melarang kombinasi keduanya** — penolakan 0x80070057 adalah perilaku runtime yang tidak terdokumentasi di PC pengguna → solusinya tangga fallback dengan HRESULT dicatat per percobaan.

**Fix:** (1) `kgcore/reader_fallback.h` — logika tangga (a) penuh → (b) basic → (c) polos, diuji Linux dengan pabrik palsu yang gagal di tingkat tertentu; camera_capture memakainya + mencoba ulang dengan backoff 1→2→4→8 dtk; `--capture-plain` = langsung tingkat (c). (2) Log jujur: baris awal hanya "thread dijalankan (menunggu reader + frame pertama)"; `MF capture start: OK - frame pertama diterima ...` dicetak worker SETELAH reader jadi + frame pertama; kegagalan mencetak `MF capture start: GAGAL (<tahap>) hr=0x... - percobaan N, mencoba lagi dalam X ms`. (3) UI: baris merah "Kamera gagal: 0x... (mencoba lagi...)" (tab Kamera + status bar), petunjuk "Kamera mungkin dipakai aplikasi lain atau akses kamera diblokir", tombol "Coba lagi" (Stop+Start segar). (4) Guard readback: `BeautyPipeline::OutputRGBA` menolak (log "tidak ada frame keluaran") selama `SinkReadbackAllowed(sink_w, sink_h, frames_produced_)` false — gpupixel tidak disentuh sama sekali sebelum frame pertama; F5 mencatat "tidak ada frame keluaran" dan lanjut. (5) F4 tanpa frame: overlay/teks "Tidak ada frame kamera".

**Teruji di Linux:** 7 test case baru (tangga: gagal penuh→basic; gagal penuh+basic→polos; semua gagal→-1+hr terakhir; sukses tingkat pertama; --capture-plain; urutan default; guard readback 7 kasus) — kg_tests 64 TC / 8642 assertions hijau.

**TIDAK bisa diverifikasi tanpa webcam + GPU (jujur):** apakah tingkat (b)/(c) benar-benar membuka kamera di PC pengguna (harusnya ya — itu konfigurasi M2d yang terbukti), apakah 0x80070057 memang hilang di kombinasi lain, perilaku backoff + tombol Coba lagi di UI nyata, apakah F5 aman (guard diuji logika, bukan terhadap gpupixel.dll), apakah "OK menyusul frame pertama" muncul di log PC, dan status BGRA→RGBA M2e (tetap dipertahankan, TETAP belum terbukti karena frame belum pernah masuk).

## M2e — detail (2026-11-12)

**Akar masalah preview hitam (terbukti di source, bukan tebakan):** `SourceRawData::GenerateTextureWithPixels` (gpupixel pin `f33abfa`): cabang `GPUPIXEL_FRAME_TYPE_BGRA` membungkus satu-satunya `glTexImage2D` dengan `#if defined(GPUPIXEL_IOS) || defined(GPUPIXEL_MAC)` → **di Windows upload BGRA = NO-OP**, tekstur tidak pernah terisi → seluruh rantai memproses tekstur kosong → readback hitam. Kamera MF memberi `kFormatBGRA` (RGB32) → selalu kena jalur ini. Cabang RGBA tidak dijaga platform → bekerja di semua OS. Masalah sekunder: byte alpha frame RGB32 sering 0 → preview terlihat "hilang" di panel gelap.

**Fix (tanpa menambal gpupixel):** (1) `BeautyPipeline::ProcessFrame` mengonversi BGRA→RGBA + memaksa alpha=255 ke buffer sendiri (`kgcore::ConvertToRgbaTopDown`) sebelum `ProcessData` dengan tipe RGBA. (2) `OutputRGBA()` menyalin readback sink + memaksa alpha 255. (3) upload preview memaksa alpha 255. (4) UV preview diseragamkan `(0,0)-(1,1)` (semua sumber kita top-down). (5) Teks "Menunggu frame kamera..." hanya sampai frame tampil pertama; sesudahnya baris status "N frame kamera diterima, M diproses" (overlay pada gambar / teks inline bila belum ada gambar). (6) F4 = upload frame capture langsung (tanpa GPUPixel) — memisahkan masalah capture/upload dari pipeline; kotak centang di tab Info. (7) F5 = simpan `raw_*.png` + `out_*.png` ke `%APPDATA%\KameraGeulis\debug\` (penulis PNG minimal tanpa dependensi, CRC/adler teruji). (8) Log `diag` tiap 1 detik: MF recv/ukuran/stride/subtype/fps/luma/alpha-tengah, frame masuk/keluar/err pipeline, upload out/raw, `glGetError` terakhir+total setelah tiap upload, id+ukuran tekstur, pointer konteks GL aplikasi; catatan konteks startup menjelaskan gpupixel membaca frame ke CPU lewat konteks kerjanya sendiri (`GetRgbaBuffer` = jalur resmi, id tekstur gpupixel tidak dipakai di konteks aplikasi; PBO menyusul/M3-diblokir). (9) Kamera: flag `MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING` (scaling agar 1280x720@30 bisa diterima), baca-balik tipe aktual (ukuran/stride-bertanda/subtype/fps) dicatat di log + tab Info; stride negatif (bottom-up) di-flip saat copy, tidak lagi di-clamp; ukuran frame berubah di tengah jalan → pipeline di-Shutdown + di-Init ulang dengan ukuran baru.

**Teruji di Linux (kg_tests, hijau):** konversi BGRA/RGB32→RGBA alpha=255, stride berpady, flip bottom-up→top-down, statistik luma/alpha (termasuk baris center pada sumber bottom-up), penulis PNG (signature, IHDR w/h, CRC IHDR/IEND, keberadaan IDAT) + vektor CRC32/Adler32 standar.

**TIDAK bisa diverifikasi tanpa GPU + webcam di PC pengguna (jujur):** apakah preview kamera tampil (hitam→terang), orientasi gambar (tegak/terbalik — konvensi UV baru berdasarkan analisis source gpupixel, bukan pengamatan), F4 menampilkan frame mentah dengan warna/benar, F5 menghasilkan PNG yang bisa dibuka, scaling 1280x720@30 diterima kamera (atau jatuh ke mode native), perilaku stride negatif di perangkat nyata, jumlah `glGetError`=0 di GPU nyata, dan reinit pipeline saat ukuran berubah. **Jangan mengklaim preview berfungsi sebelum pengguna menguji.**

## M2d — detail (2026-11-12)

**Akar masalah (terbukti di source):** dump pengguna: AV READ 0x0 di `gpupixel.dll` RVA 0x3968a, deterministik lintas build. `beauty_face_unit_filter.cc` (GPUPixel v1.3.1): `gray_image_ = SourceImage::Create(res/lookup_*.png)` — tanpa folder `res/`, `SourceImage::Create` return `nullptr` (assert di-strip di Release) → `Init()` tetap `return true` → `DoRender()` mengikat GL_TEXTURE5 → `gray_image_->GetFramebuffer()` di atas null → AV. Fungsi berisi 0x3968a dilaporkan CI via PDB/MAP (`scripts/resolve-pdb-rva.ps1`).

**Fix:** (1) CI menyalin `src/res/*.png` dari ref GPUPixel yang di-pin ke `dist\res\` (tidak di-commit ke repo); app memanggil `GPUPixel::SetResourcePath(exe_dir)` sebelum filter dibuat; log `[pipeline] resource res/<file>: OK (N B)` per file. (2) Pre-verify 4 lookup wajib sebelum pipeline dibuat — hilang/ukuran nol → init gagal → efek GPU nonaktif (preview tetap jalan) + pesan merah di status bar, bukan crash. (3) Cek null `BeautyFaceFilter::Create` + source_/sink_. (4) gpupixel dibangun `/Zi` + `/MAP`; CI resolve RVA crash terhadap `gpupixel.map`. (5) Langkah CI "Verify artifact contents" menggagalkan build bila ada file wajib yang hilang (exe, 4 DLL, 7 PNG res, 2 model, THIRD_PARTY/README/STATUS, pdb+map). (6) `models/*.onnx` ikut artifact; urutan pencarian disatukan via `kg_pipeline/windows_paths.h` (env → `exe\models` → LOCALAPPDATA) di app dan selftest. (7) `--safe` sepenuhnya inert (objek pipeline tidak dibuat; tab Kamera disabled; apply kamera diblokir); label FPS render/kamera dipisah; checkbox VSync; glif non-ASCII dibuang dari UI.

**Fungsi di RVA 0x3968a (CI, terverifikasi):** `?DoRender@BeautyFaceUnitFilter@gpupixel@@UEAA_N_N@Z` — mulai 0x39030 (Init 0x39CD0); crash di tengah badan DoRender. Bukti level fungsi untuk hipotesis resource-hilang. Catatan: `/DEBUG` mengubah default `/OPT` linker (REF/ICF) dan menggeser layout — dipaksa eksplisit `/OPT:REF /OPT:ICF` agar peta cocok dengan dll yang crash.

**Belum diverifikasi:** crash-hilang-di-PC (butuh PC pengguna menjalankan artifact M2d).

## M2c — detail (2026-11-12)

**Latar:** artifact M2 langsung *force-close* saat dibuka di PC pengguna — tanpa pesan apa pun. M2c bukan perbaikan tebakan, melainkan (a) agar crash berikutnya **tercatat** (log + dump + faulting module + MessageBox), dan (b) **mengisolasi** tahap yang gagal lewat `--selftest` (DLL/ORT/model), `--safe` (window/UI tanpa kamera/ORT/GPUPixel), dan normal.

**Artifact terbaru:** `kamerageulis-m2-windows-x64` — **CI hijau, run [43192212350](https://github.com/hanlimzie-glitch/KameraGeulis/actions/runs/43192212350)** (Configure → Build → Test → Package → **Smoke test** → Upload ✅; 7.510.560 B; kedaluwarsa 2026-11-26).

**Yang diverifikasi di CI (bukan di Windows nyata):**
- `--selftest` **exit 0** di runner windows-latest dari folder artifact yang dipaketkan (`dist/`, bukan build dir): preload `onnxruntime.dll` + `gpupixel.dll` dari folder app-local (path modul aktual dicatat di log), `OrtGetApiBase()->GetVersionString()` jalan, kedua model `models-v1` diunduh (`gh release download`) lalu dimuat ORT CPU dengan verifikasi SHA-256 + ukuran, satu inferensi per model dengan output shape sesuai pin (BlazeFace `regressors[1,896,16]`, `classificators[1,896]`; FaceMesh `conv2d_21[1404]`, `conv2d_31[1]`), semua finite. Model dihapus dari `dist/` sebelum upload artifact (tidak ikut).
- Log `%APPDATA%\KameraGeulis\log.txt` ber-timestamp per baris, di-flush tiap baris — di runner terbukti berisi jejak ini (dibaca di CI step).
- Crash handler: **belum pernah terpicu di CI** (tidak ada crash); sintaks/struktur benar, perilakunya belum diuji di kasus nyata (dump validitas, isi MessageBox).
- Linux: `kg_core` tests hijau (log.h baru backward-compatible).

**Yang BELUM diverifikasi (jujur):**
- App normal (`KameraGeulis.exe`) tetap **belum pernah berhasil dibuka** di Windows nyata mana pun — penyebab force-close awal **belum diketahui**. M2c hanya menjamin penyebabnya kini tercatat + bisa diisolasi.
- GL context 3 tahap + log `GL_VERSION/RENDERER` — belum pernah jalan di mesin nyata.
- `--safe` dan `--selftest` di PC pengguna — belum dicoba; ini langkah diagnosis berikutnya.
- GPUPixel init di mesin nyata; MF capture; UI.
- GPU/VRAM: `--selftest` tidak menyentuh GL sama sekali.

**Untuk pengembang/pemilik (urut):**
```bat
KameraGeulis.exe --selftest   :: exit 0 = DLL+ORT+model OK; kode lain → docs/TROUBLESHOOTING.md §4
KameraGeulis.exe --safe       :: window+UI hidup? tanpa kamera/ORT/GPUPixel
KameraGeulis.exe              :: normal
```
Kirim `%APPDATA%\KameraGeulis\log.txt` + `crash-*.dmp` (bila ada) + faulting module dari Event Viewer (TROUBLESHOOTING.md §1).

**Catatan proses CI:** 1 run merah (43191736110 — `glfwGetError` signature GLFW 3.3 salah + `RunSelftest` tidak dideklarasikan; diperbaiki a7e3721), lalu hijau penuh termasuk smoke test. Ini sekaligus membuktikan step smoke test bekerja (kalau `--selftest` gagal exit, job merah).

## M2 — detail (2026-11-12)

**Artifact:** `kamerageulis-m2-windows-x64` — **CI hijau penuh** pada run [43183762692](https://github.com/hanlimzie-glitch/KameraGeulis/actions/runs/43183762692) (Configure → Build → Test → Package → Upload ✅, 7.488.666 byte) dan di-rebuild identik untuk commit docs terakhir (run [43184275535](https://github.com/hanlimzie-glitch/KameraGeulis/actions/runs/43184275535), 7.491.609 byte). Kedaluwarsa: **2026-11-26**. Utamakan mengunduh dari run terakhir (43184507751); perintah di bawah memakai run terakhir.

**Isi artifact** (diverifikasi dari langkah Package/Upload CI, bukan unduhan sandbox — unduhan dari sandbox gagal di blob storage; isi persis sesuai `dist/` di workflow): `KameraGeulis.exe`, `gpupixel.dll`, `onnxruntime.dll`, CRT app-local `vcruntime140.dll` + `vcruntime140_1.dll` + `msvcp140.dll` (VS binary terdistribusi ulang, diizinkan app-local, tanpa admin — lihat [docs/REDISTRIBUTABLES.md](docs/REDISTRIBUTABLES.md)), `THIRD_PARTY.md`, `README.md`, `STATUS.md`.

**Cara mengunduh artifact** (pilih salah satu):

```bat
:: via gh CLI (perlu gh auth login)
gh run download 43184507751 -R hanlimzie-glitch/KameraGeulis -n kamerageulis-m2-windows-x64 -D kamerageulis-m2
```

atau buka https://github.com/hanlimzie-glitch/KameraGeulis/actions/runs/43184507751 → bagian **Artifacts** → `kamerageulis-m2-windows-x64` → ekstrak → jalankan `KameraGeulis.exe`. Tidak perlu installer; CRT sudah app-local. (Run 43183762692 dan 43184275535 juga masih tersedia dengan isi yang sama.)

**Yang diimplementasikan (M2):**
- Preview besar + panel 6 tab (**Kulit | Wajah | Makeup | Preset | Kamera | Info**), tema gelap.
- **Slider Haluskan Kulit & Cerahkan Kulit (0–100%) aktif** dan di-apply live tiap frame (`BeautyFaceFilter::SetBlurAlpha` / `SetWhite` via `BeautyPipeline::SetBeautyStrength`).
- Preset **Natural / Soft / Glam / ClearSkin** (nilai di `src/kg_core/include/kgcore/presets.h`, diuji di `kg_tests`); slider manual → preset "**Kustom**"; tombol **Reset** (default pabrik); **Simpan/Muat** `settings.json` (`%APPDATA%\KameraGeulis\settings.json`, load otomatis saat startup).
- **Tahan tombol "Tahan untuk Bandingkan"** → preview raw (tanpa efek) selama ditahan.
- **Semua kontrol landmark** (Tiruskan Wajah, Perbesar Mata, Lipstik, Blusher, Foundation, Eyeliner, Eyeshadow, Brow Tint) dirender **disabled** berlabel **"Segera hadir"** (bukan disembunyikan) — alur UI final untuk M4.
- **Picker kamera**: daftar device Media Foundation + resolusi/fps dari mode native kamera (fallback 1280×720 / 640×480 + peringatan bila mode tidak terdeteksi); tombol **Terapkan** / **Segarkan**.
- **Debug overlay (F3** atau checkbox di tab Info)**: menggambar 468 titik landmark + bounding box + HUD (FPS, waktu capture/detect/mesh/track/render dalam ms, frame sejak deteksi terakhir) di atas preview — untuk verifikasi visual akurasi landmark.
- Seluruh string lewat tabel ID (default) + EN; toggle radio di status bar.
- Virtual camera (softcam) tetap **disabled** — M3.

**BELUM diverifikasi di Windows nyata (jujur):**
- Artifact belum pernah dijalankan di Windows mana pun. Tidak ada GPU, webcam, atau display di CI (`windows-latest`, headless) — "compile+test hijau" ≠ "berjalan mulus".
- Runtime GL (OpenGL 3.3+), Media Foundation capture, dan ONNX Runtime di mesin nyata belum diuji.
- **Tampilan UI** (font, tata letak, skala DPI) belum pernah dilihat manusia.
- Kamera virtual (M3) belum ada sama sekali.
- Bila `github.com` tidak terjangkau dari mesin user, download model `models-v1` saat first-run gagal → efek landmark nonaktif (pesan jujur) — aplikasi tetap berjalan.
- Artifact expired 2026-11-26; untuk rilis permanen perlu GitHub Release (di luar scope M2).

**CI notes:** 2 run merah sebelum hijau — (1) `EnumerateDevices` bentrok dengan helper bebas (C2660/C2440, run 43181651089) → helper di-rename `EnumerateDevicesRaw` (dd7c738); (2) konstanta `MF_E_NO_MORE_TYPES` tidak terdeklarasi di toolchain CI (run 43182162374) → enumerasi berhenti pada `FAILED(hr)` apa pun; (3) `C2027: undefined type gpupixel::BeautyFaceFilter` di `main.cc` (run 43182519015) → parameter beauty live dipindah ke `BeautyPipeline::SetBeautyStrength`; (4) Build+Test sudah hijau di run 43182772993 tapi Package gagal — penyebab tidak terbaca; langkah redist kini diagnostik `::error::` + memilih folder CRT terlengkap (dabacc4); (5) run 43183762692 **hijau penuh** termasuk upload artifact.

**Catatan:** `kg_app` hanya di-build di Windows (`KG_BUILD_APP=ON`); di Linux app tidak dikonfigurasi — logika murni (settings, presets, strings, decode) yang diuji di Linux (`kg_tests` hijau, termasuk di CI run di atas).

## LICENSE — keputusan pemilik (owner decision)

- **Repository ini sengaja TIDAK berisi file `LICENSE`.** Penentuan lisensi adalah keputusan **pemilik repository**, bukan keputusan agen. Sampai pemilik menentukan, jangan distribusi publik / rilis binary dengan klaim lisensi tertentu.
- GPUPixel dipakai di bawah **Apache-2.0**; notice/attribution institusi dipertahankan utuh di [THIRD_PARTY.md](THIRD_PARTY.md) (dan dicopy ke artifact).
- Dependensi lain (GLFW, Dear ImGui, Catch2, onnxruntime, model MediaPipe→ONNX) telah didokumentasikan lisensi + provenance-nya di THIRD_PARTY.md; model tidak di-commit ke repo (diunduh first-run dari release `models-v1` dengan SHA-256 pin, atau dari `KAMERAGEULIS_MODELS_DIR` untuk CI/dev).
- Sebelum rilis publik: (a) pilih lisensi, (b) tambahkan file LICENSE, (c) update README/badge, (d) audit ulang THIRD_PARTY.md. Ini ditandai sebagai **keputusan pemilik** di sini agar tidak hilang.

## M1b — detail (2026-11-12)

- **kg_core** (Linux-tested, hijau): `face_anchors` (896 anchor BlazeFace short-range, faithful ke `SsdAnchorsCalculator` + `face_detection_short_range.pbtxt`), `face_detection_decode` (sigmoid, weighted NMS IoU 0.3, letterbox keep-aspect BORDER_ZERO), `roi_align` (eye-aligned crop 1.5×, round-trip teruji), `face_mesh_decode` (conv2d_21 → 468 titik [0,1]; `conv2d_31` face flag), OneEuro/LandmarkSet/FlattenLandmarks (sudah ada).
- **OrtLandmarkProvider** (Windows, `kg_pipeline`): BlazeFace 128² → decode → ROI mata-sejajar → FaceMesh 468 → gate faceflag → OneEuro + fade via FaceTracker. Download first-run dari rilis GitHub `models-v1` (SHA-256 tertanam di `ort_landmark_provider.cc`, dihasilkan CI: det `82ac962b…`, mesh `ac72f85b…`); env override `KAMERAGEULIS_MODELS_DIR` untuk CI/dev; gagal → efek nonaktif + pesan jujur (tidak pernah crash).
- **Efek landmark v1 TIDAK di-wire** (FaceReshape/Lipstick/Blusher GPUPixel): pemetaan 468→106/111 titik Mars tidak bisa diandalkan (proporsi canonical berbeda; residual busur OK 0.010 tetapi alis/mata salah anatomi; docs/LANDMARKS.md §8). UI menyatakannya jujur.
- **THIRD_PARTY.md**: model ONNX tetap Apache-2.0 via konversi tf2onnx; tidak di-commit. `canonical_face_model.obj` hanya referensi analisis offline (tidak di-commit).
- **CI**: run 43174282611 hijau (Linux: kg_core tests; Windows: configure mengunduh ONNX Runtime 1.31.1 win-x64 dengan pin SHA-256, build+link sukses termasuk `onnxruntime.dll`, ctest hijau, artifact `kamerageulis-m1b-windows-x64` 6,8 MB). Catatan: run sebelumnya (43173665412) Linux Test exit 8 tanpa ::error::; Test step kini meng-annotasi output ctest; kegagalan itu tidak berulang dan tidak terdiagnosis (jeda infra?) — dicatat jujur.
- **Belum terverifikasi**: provider di Windows nyata (belum pernah jalan), performa CPU, kualitas ROI/rotasi pada wajah nyata, download first-run dari `models-v1` di mesin bersih (release sudah berisi aset sejak 2026-11-12 — workflow Models run 43179962364), artifact vcam (belum ada).
