# VIRTUALCAM.md — Arsitektur, Matriks Kompatibilitas & Uji Manual Kamera Virtual (M3)

Dokumen ini menjelaskan arsitektur kamera virtual **KameraGeulis Camera** pada Windows 10 dan Windows 11, dasar pemilihan backend dari dokumentasi/issue resmi, jalur frame asinkron tanpa memblokir render, mekanisme registrasi, serta daftar uji manual pada aplikasi konsumen nyata.

---

## 1. Riset Backend & Matriks Kompatibilitas

Windows menyediakan dua API kamera virtual user-mode tanpa driver kernel:

1. **DirectShow Video Input Filter** (`CLSID_VideoInputDeviceCategory`, diimplementasikan melalui [`tshino/softcam`](https://github.com/tshino/softcam) berlisensi MIT).
2. **Windows Media Foundation Virtual Camera** (`MFCreateVirtualCamera` — diperkenalkan pada Windows 11 build 22000+).

### Perbandingan API

| Aspek | DirectShow (`softcam`) | Media Foundation (`MFCreateVirtualCamera`) |
|---|---|---|
| Dukungan OS | **Windows 10 & Windows 11** | Windows 11 build 22000+ saja (tidak ada di Windows 10) |
| Driver kernel | Tidak ada (COM in-proc DLL) | Tidak ada (COM media source / Frame Server) |
| Registrasi | 1× admin (`regsvr32`) | Registrasi COM dapat diakses layanan `FrameServer` (`Local Service` / `Local System`) |
| Bitness aplikasi konsumen | Mengikuti bitness proses: **kirim x64 + Win32 DLL** agar aplikasi 32-bit dan 64-bit melihat kamera | Disediakan oleh Windows Frame Server untuk konsumen MF/UWP |
| Batasan utama | Tidak muncul di aplikasi UWP/MF-only (mis. Windows Camera app) | Tidak jalan di Windows 10; konfigurasi COM + FrameServer lebih kompleks (lihat [`smourier/VCamSample`](https://github.com/smourier/VCamSample)) |

### Matriks Kompatibilitas Aplikasi (Berdasarkan Dokumentasi & Issue Resmi)

| Aplikasi Konsumen | DirectShow (`softcam`, x64 + Win32) | `MFCreateVirtualCamera` | Catatan & Sumber |
|---|---|---|---|
| **OBS Studio** (*Video Capture Device*) | **Didukung** | Didukung (Win11) | Kamera virtual bawaan OBS di Windows juga berbasis DirectShow; sumber *Video Capture Device* mengenumerasi `CLSID_VideoInputDeviceCategory` ([`obsproject/obs-studio#13439`](https://github.com/obsproject/obs-studio/issues/13439)). |
| **Discord Desktop** | **Didukung (wajib daftar x64 + Win32)** | Didukung (Win11) | Client desktop Discord di Windows berjalan sebagai proses 32-bit; `tshino` mengonfirmasi bahwa mendaftarkan `softcam.dll` **Win32** bersama **x64** membuat kamera terdeteksi di Discord ([`tshino/softcam#47`](https://github.com/tshino/softcam/issues/47)). |
| **Zoom Desktop** | **Didukung** | Didukung (Win11) | Zoom desktop mengenumerasi perangkat input video DirectShow (`ICreateDevEnum`). |
| **Google Chrome / Microsoft Edge** (`getUserMedia`) | **Didukung** | Didukung (Win11) | Chromium mengenumerasi kamera DirectShow pada Windows (`ICreateDevEnum`); pastikan izin privasi kamera Windows (*Let desktop apps access your camera*) aktif. |
| **Microsoft Teams** | **Classic & Web (Chrome/Edge): Didukung**; **New Teams (MSIX/UWP): Terbatas / bisa butuh MF** | Didukung (Win11) | Aplikasi Store/UWP/Win11 modern dapat mengutamakan atau mewajibkan Media Foundation Frame Server ([Microsoft Q&A #872883](https://learn.microsoft.com/en-us/answers/questions/872883/which-api-to-use-for-creating-virtual-webcam)). |
| **Windows Camera app / Pengaturan Win11 > Cameras** | **Tidak didukung** | Didukung (Win11) | Aplikasi UWP/Media Foundation-only tidak membaca `CLSID_VideoInputDeviceCategory` ([Microsoft Q&A #872883](https://learn.microsoft.com/en-us/answers/questions/872883/which-api-to-use-for-creating-virtual-webcam)). |

### Keputusan Arsitektur

- **Backend utama M3:** `softcam` (DirectShow, tanpa driver kernel, dipin ke commit `fa38d892bd931b761588f9b7641ba49f006953f0` yang menyertakan perbaikan watchdog disconnect PR #70 sehingga `scIsConnected()` kembali `false` saat aplikasi konsumen menutup kamera).
- **Nama perangkat:** `"KameraGeulis Camera"` (dipatch pada `FILTER_NAME`, `NamedMutexName`, dan `SharedMemoryName` saat build agar tidak bentrok dengan instalasi *DirectShow Softcam* lain).
- **Dual-architecture packaging:** Build CI mengompilasi `softcam.dll` **x64** dan **Win32** (linked `/MT` agar bebas ketergantungan VC++ Redistributable di dalam proses konsumen seperti `Discord.exe`/`obs64.exe`), dan skrip instalasi mendaftarkan keduanya.
- **Antarmuka abstraksi (`IVirtualCameraSink`):** Didefinisikan di `src/kg_core/include/kgcore/virtual_camera.h` sehingga backend kedua (`MFCreateVirtualCamera` untuk Windows 11 / New Teams / UWP) dapat ditambahkan di rilis mendatang tanpa mengubah pipeline render, PBO readback, atau frame pacer.

---

## 2. Jalur Frame Asinkron & Constant-FPS Pacing

```
[GPU Render Output]
  ├─ MaskBlender::DisplayTex(false)  <-- bersih: setelah mask kulit, efek, & rotasi; TANPA overlay F3/F6
  └─ atau PreviewTexture (un-mirrored)
         │
         ▼  (Render Thread — non-blocking)
[AsyncPboReadback: PBO[0] / PBO[1] + glFenceSync]
  ├─ glReadPixels(..., 0) -> DMA asinkron ke PBO tulis
  └─ glClientWaitSync(fence, 0, 0) -> jika selesai, glMapBufferRange + memcpy ke slot terbaru
         │
         ▼  (Drop-oldest single-slot handoff — tidak pernah menahan render thread)
[VcamService Worker Thread]
  ├─ VcamFramePacer (konstan 30 fps default; ulang frame terakhir bila kamera fisik ~20 fps)
  ├─ Resize aspect-preserving (letterbox hitam jika rasio/rotasi berbeda, mis. 90° potret -> 1280x720)
  ├─ Mirror horizontal OPSIONAL (default MATI — mirror hanya untuk preview)
  ├─ Konversi format RGBA -> BGR24 top-down (atau BGRA32/NV12 sesuai PreferredFormat() sink)
  └─ IVirtualCameraSink::SendFrame(..., timestamp_us monoton)
```

### Kebijakan Performa & Stabilitas

1. **Tidak ada `glReadPixels` sinkron di render thread:** PBO double-buffer + `glClientWaitSync(..., timeout=0)` hanya memanen frame yang sudah selesai ditransfer oleh DMA GPU; jika GPU masih sibuk, iterasi tersebut dilewati (`pbo_busy_skips`) tanpa menunggu.
2. **Mirror terpisah:** Preview boleh dimirror (`ui.mirror = true`), tetapi tekstur sumber PBO tetap tidak dimirror (`PreviewTexture` dan `MaskBlender::tex_out` keduanya un-mirrored; mirror preview diterapkan lewat UV `ImGui::Image`). Kamera virtual memakai flag terpisah `vcam_mirror` yang default-nya `false`.
3. **Hold-to-Compare aman:** Saat pengguna menahan tombol *Hold to Compare*, pengiriman PBO baru ditahan sejenak agar lawan bicara di Zoom/Discord tidak melihat kedipan frame mentah tanpa efek.
4. **Constant-FPS & Timestamp Monoton:** Kamera fisik yang hanya mengirim ~20 fps (mis. kondisi redup) tetap menghasilkan aliran ~30 fps ke konsumen virtual camera dengan mengulang frame terakhir dan memberi timestamp `pts_us` yang selalu naik.
5. **Drop-frame policy:** Jika thread worker belum sempat mengonsumsi frame baru sebelum frame GPU berikutnya masuk, slot frame lama langsung ditimpa (`frames_dropped++`) tanpa antrean menumpuk dan tanpa menunda thread render.

---

## 3. Registrasi, Izin & Status UI

- **Aplikasi utama berjalan tanpa hak Administrator.**
- Registrasi COM DirectShow dilakukan **1 kali** lewat `install_vcam.bat` (meminta UAC sekali, mendaftarkan `softcam.dll` x64 ke `System32\regsvr32.exe` dan `win32\softcam.dll` ke `SysWOW64\regsvr32.exe`).
- `uninstall_vcam.bat` membatalkan registrasi keduanya dan memastikan key registry CLSID `{AEF3B972-5FA5-4647-9571-358EB472BC9E}` bersih total (termasuk `WOW6432Node` dan `CLSID_VideoInputDeviceCategory`).
- **Status di Tab Kamera:**
  - `Belum terdaftar`: DLL tidak ditemukan atau CLSID belum terdaftar di registry; kotak centang nonaktif dan petunjuk jelas ke `install_vcam.bat` ditampilkan.
  - `Terdaftar (tidak mengirim)`: Driver siap di registry, namun tombol *Kamera Virtual* belum diaktifkan (atau sedang dibuka/berhenti).
  - `Mengirim frame`: Aktif mengirim frame pada resolusi/FPS terpilih, menampilkan status konsumen (`0 konsumen` atau `>= 1 konsumen terhubung`), FPS aktual, latensi konversi/kirim (ms), serta jumlah frame ulang/drop.

---

## 4. Daftar Uji Manual PC Nyata (Belum Terverifikasi di Sandbox/CI)

CI Windows hanya memverifikasi registrasi COM, enumerasi DirectShow (`ICreateDevEnum`), pengiriman frame sintetis, dan pembacaan kembali pixel DIB via pin DirectShow headless. **Pengujian pada aplikasi nyata berikut wajib dilakukan di PC Windows pengguna:**

### Persiapan
1. Ekstrak folder rilis Windows (`KameraGeulis.exe`, `softcam.dll`, `win32\softcam.dll`, `install_vcam.bat`, `uninstall_vcam.bat`, `models\`, `res\`).
2. Jalankan `install_vcam.bat` (setujui prompt UAC sekali).
3. Buka `KameraGeulis.exe` (tanpa admin), masuk ke tab **Kamera**:
   - Pastikan status terbaca `Terdaftar (tidak mengirim)`.
   - Centang **Kamera Virtual** (default `1280x720 @ 30`, *Cerminkan kamera virtual* = OFF).
   - Pastikan status berubah menjadi `Mengirim frame`, preview tetap lancar tanpa penurunan FPS berarti, dan `[vcam]` di `%APPDATA%\KameraGeulis\log.txt` mencatat FPS ~30 dan overhead ms/frame yang wajar.

### Aplikasi Konsumen yang Diuji
| Aplikasi | Langkah Uji | Kriteria Lulus & Hal yang Dicatat |
|---|---|---|
| **OBS Studio** | Tambah *Source* → *Video Capture Device* → pilih `KameraGeulis Camera` | Terdeteksi? Resolusi & FPS terbaca (`1280x720@30`)? Orientasi tidak terbalik/tidak mirror (teks di baju terbaca normal)? Latensi subjektif? Ada artefak/tearing? |
| **Discord Desktop** | *User Settings* → *Voice & Video* → *Camera* → pilih `KameraGeulis Camera` → *Test Video* | Terdeteksi (verifikasi Win32 DLL)? Resolusi/rasio benar? Status di KameraGeulis berubah menjadi konsumen terhubung? |
| **Zoom Desktop** | *Settings* → *Video* → pilih `KameraGeulis Camera` | Terdeteksi? Wajah mulus tanpa overlay F3/F6? Stabil saat pindah kamera virtual 1280x720 ↔ 1920x1080? |
| **Chrome (`getUserMedia`)** | Buka halaman uji webcam WebRTC (mis. `webrtc.github.io/samples/src/content/getusermedia/gum/` atau situs rapat WebRTC) → pilih `KameraGeulis Camera` | Terdeteksi di daftar kamera browser? `getSettings()` menampilkan `1280x720` @ ~30 fps? |

### Uji Tambahan & Bersih-Ulang
1. **Kamera fisik lebih lambat (~20 fps):** Pastikan output `KameraGeulis Camera` di OBS/browser tetap stabil di ~30 fps (`repeated` bertambah di log `[vcam]`, tidak macet).
2. **Rotasi kamera 90° / Hold-to-Compare / F3 / F6:** Nyalakan F3 & F6 di KameraGeulis — pastikan kotak/titik F3 dan lapisan merah F6 **TIDAK muncul** di OBS/Discord/Zoom.
3. **Uninstall bersih:** Tutup KameraGeulis & aplikasi konsumen, jalankan `uninstall_vcam.bat` (UAC), lalu periksa di OBS/Chrome bahwa `KameraGeulis Camera` sudah **hilang** dari daftar perangkat dan status di KameraGeulis kembali `Belum terdaftar`.
