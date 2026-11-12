# ARCHITECTURE.md — KameraGeulis

**Status:** desain M0, diperbarui M1c (§5.1 — detector-skip/ROI tracking) dan M2 (debug overlay via StageTimings). Kode implementasi ada di `src/`. Tanggal 2026-11-12.

## 1. Gambaran besar

```
┌──────────────┐   ┌─────────────────────────────────────────────────────┐
│ Webcam       │   │ GPUPixel GPU pipeline (1 OpenGL context, GLFW)       │
│ (Media       │──▶│ SourceRawData → [FaceTracker] → BeautyFaceFilter    │
│  Foundation) │   │   → FaceReshapeFilter* → custom makeup filters       │
└──────────────┘   │   → SinkRawData (preview) └→ (virtual cam: RGBA)     │
                   └──────────────┬───────────────────────────────────────┘
                                  │ RGBA frames (GPU→CPU readback, bounded)
                                  ▼
                   ┌───────────────────────────────┐
                   │ Preview window (OpenGL blit   │
                   │ + Dear ImGui side panel)      │
                   └───────────────────────────────┘
                                  │
                                  ▼
                   ┌───────────────────────────────┐
                   │ Virtual Camera sink (softcam) │
                   │ scCreateCamera/scSendFrame    │
                   └───────────────────────────────┘
```

\* butuh landmark; nonaktif otomatis bila provider tidak tersedia.

## 2. Prinsip

1. **Satu proses, tiga thread** — capture (MF) / render+GPU / (virtual camera consume async di thread sama dgn render, kirim setelah readback). Antrian frame bounded (kapasitas 2), **drop-oldest**, tidak pernah menaikkan latency demi kelengkapan frame.
2. **GPU-only untuk efek.** Semua filter di GPU (GPUPixel). CPU hanya: capture convert, landmark smoothing (murah), readback untuk virtual cam + preview upload.
3. **Landmark abstraction.** Semua filter landmark menerima `LandmarkSet` (468 normalized pts + `valid` flag + `confidence`), bukan sumber konkret → kompatibel dengan provider ONNX Runtime (M1b, semua platform) dan provider lain di masa depan. Landmark smoothing (OneEuro) diterapkan di satu tempat (sebelum dibagikan ke filter), bukan per filter. Detail pipeline deteksi/ROI/mesh: `docs/LANDMARKS.md`.
4. **Face lost ≠ crash.** Jika wajah hilang > N ms: envelope gain landmark → 0 (fade-out mulus 300 ms), filter landmark dinonaktifkan, preview tetap 30/60 fps, UI menampilkan indikator "Wajah tidak terdeteksi".
5. **Zero pop.** Semua parameter efek ramping (lerp) saat berubah — baik dari slider maupun dari preset (hold-compare tidak memicu lompatan).
6. **Kejujuran fitur.** Setiap efek punya status `DONE / PARTIAL / DISABLED_ON_THIS_BUILD` — ditampilkan jujur di STATUS.md, bukan stub tersembunyi.

## 3. Modul (CMake targets)

| Target | Isi | Platform |
|---|---|---|
| `kg_core` | `landmark/` (LandmarkSet, OneEuro, smoothing, ROI/SDF helpers), `settings/` (JSON persist via nlohmann/json), `strings/` (ID/EN table), `util/` (log file, time, bounded queue template) | Semua (unit-tested di Linux) |
| `kg_capture` | `CameraCapture` — Media Foundation `IMFSourceReader`, capture thread, bounded queue | Windows |
| `kg_pipeline` | GPUPixel wrapper: build chain, apply `EffectParams`, readback RGBA | Windows (GL); di Linux: stub kosong + unit-testable helpers |
| `kg_vcam` | Softcam sender wrapper (`vcam/SoftcamSender.*`) + installer detection (registry) | Windows |
| `kg_app` | `main.cc`, GLFW window, Dear ImGui panel (tabs Face/Skin/Makeup/Presets), hold-to-compare, theming | Windows |
| `kg_tests` | ctest — pure logic (landmark math, smoothing, queue, settings round-trip, string table, preset math) | Semua |

Dependensi (FetchContent, pinned): GPUPixel `v1.3.1` (Apache-2.0, `GPUPIXEL_ENABLE_FACE_DETECTOR=OFF` di Windows), Dear ImGui `v1.91.9` (MIT), nlohmann/json `v3.11.3` (MIT), Catch2 `v3.7.1` (BSL-1.0, test-only). **softcam TIDAK di-link** — `kg_vcam` memuat `softcam.dll` saat runtime via `LoadLibrary` (app tetap jalan bila DLL/registrasi tidak ada); build DLL softcam (dengan patch `FILTER_NAME` → `"KameraGeulis Camera"`) dilakukan di milestone packaging (M3/M5), bukan dependensi build-time. libyuv/glad/glfw berasal dari tree GPUPixel (reuse target `glfw`, `glad`).

## 4. Data flow per frame (hot path)

1. `CameraCapture` thread: MF sample (RGB32, stride pad) → convert → `Frame{rgba, w, h, t}` → `BoundedQueue::pushDropOldest`.
2. Render loop (GUI thread, vsync-off, target 30/60): pop latest frame (skip bila kosong — jangan stall).
3. `SourceRawData::ProcessData(...)` → pipeline `DoRender`:
   - `FaceTracker::Track(frame)` → smoothed `LandmarkSet` (atau valid=false).
   - `BeautyFaceFilter` (smoothing/whitening, selalu aktif bila intensitas>0).
   - `FaceReshapeFilter` (slim/eyes) bila provider valid.
   - Custom filters (custom effects, masing-masing toggle + slider).
   - `SinkRawData` → RGBA buffer.
4. Preview: upload RGBA → texture preview → draw ImGui blit + panel.
5. VCam: bila enabled → `scSendFrame` (copy RGBA; softcam sendiri handle stride), non-blocking (drop bila consumer lambat).

Budget (720p30): capture convert ~2-4 ms, GL 5-10 ms, readback 3-6 ms, UI 1-2 ms → ~15-25 ms/frame tipikal. 1080p30 stretch.

## 5. Landmark provider

```cpp
struct LandmarkSet { std::array<Vec2,468> pts; bool valid; float confidence; int64_t ts_ms; };
class ILandmarkProvider { public: virtual ~ILandmarkProvider(); virtual bool Available() const; virtual LandmarkSet Track(const Frame&) = 0; };
```

- `OrtLandmarkProvider` (M1b, semua platform dengan model tersedia): ONNX Runtime CPU — BlazeFace short-range (deteksi) + FaceMesh 468 (mesh); detail & checksum di `docs/LANDMARKS.md`. Gerbang `valid` = skor NMS deteksi + faceflag mesh.
- `MarsFaceLandmarkProvider` (opsional, `GPUPIXEL_ENABLE_FACE_DETECTOR=ON`, Linux/macOS): panggil `FaceDetector::Detect`, bungkus + smooth. Tidak dipakai di Windows.
- `NullLandmarkProvider` (fallback bila model tidak tersedia/download gagal): `Available()==false`, semua efek landmark UI disabled dengan tooltip jujur.
- `SyntheticLandmarkProvider` (test/demo `--selftest`): wajah prosedural statis untuk CI logs/screenshot.

### 5.1 Cadence detektor wajah (M1c — temuan & keputusan)

**Temuan (sebelum M1c):** `OrtLandmarkProvider::Track` menjalankan BlazeFace **setiap frame** — letterbox 128² + inference + decode 896 anchor + NMS — disusul mesh pada ROI. Itu biaya CPU terbesar pipeline (detektor ≈ mesh), padahal posisi wajah antar-frame hanya bergeser sedikit.

**Keputusan (diimplementasikan di `ort_landmark_provider.{h,cc}`):** detektor dijalankan **hanya saat wajah hilang** — bukan pada cadence tetap:

| Momen | Perilaku |
|---|---|
| Frame pertama / `have_prev_roi_ == false` | deteksi penuh (letterbox → infer → decode → NMS) |
| Mesh faceflag ≤ 0 atau inference mesh gagal — 1× berturut-turut | tracking masih dibolehkan 1 frame (fallback ke ROI sebelumnya) |
| **2 frame invalid berturut-turut** ("lost streak") | ROI dibatalkan (`have_prev_roi_ = false`) → deteksi lagi di frame berikutnya |
| Tracking normal | mesh di-crop dari **ROI yang diwariskan dari frame sebelumnya** |
| Safety | re-deteksi setelah **300 frame** tanpa deteksi (~10 dtk @ 30 fps) untuk mengikat drift halus yang tidak memicu faceflag |
| Wajah tidak ada di frame | `have_prev_roi_ == false` → detektor tetap jalan tiap frame (mode pencarian) — memang diinginkan |

**ROI tracking dari frame sebelumnya** (bukan detektor): center ROI diikat ke **centroid landmark 468 frame sebelumnya** ditambah offset konstan yang dikunci saat deteksi (`roi_center_offset_`, sehingga posisi ROI transisi dari mode deteksi → tracking **tidak melompat**); sudut ROI di-refresh tiap frame dari sumbu mata (indeks MP kanan 33/133, kiri 362/263) dalam ruang piksel; sisi ROI (1,5× ukuran terbesar kotak deteksi) dipertahankan sampai deteksi berikutnya. Landmark disimpan **mentah** (sebelum smoothing OneEuro) agar anchor tidak dipengaruhi filter.

**Telemetri:** `frames_since_detect_` diekspos via `StageTimings` (`GetStageTimings`) → overlay debug (M2) menampilkannya bersama `detect_ms` / `mesh_ms` / `track_ms`.

**Keterbatasan (jujur):**
1. Tidak ada model gerak (Kalman/optical flow) — ROI mengikuti dengan **lag 1 frame** (titik pusat dari frame lalu); pada 30 fps itu ≤ ~33 ms gerakan.
2. Skala ROI tidak berubah di antara deteksi — wajah yang mendekat/menjauh ekstrem dapat memicu faceflag → 2 strike → deteksi ulang (mekanisme yang sama).
3. Rotasi kepala besar dapat memutus sumbu mata sesaat; sama, ditangani lost-streak + re-deteksi.
4. **Belum diverifikasi di Windows nyata** (tidak ada GPU/webcam di CI/sandbox) — perilaku flag/edge case hanya teruji melalui unit test matematika (ROI round-trip, anchor, decode), bukan pada wajah asli.

## 6. Efek (status di STATUS.md)

**Built-in GPUPixel (slider 0–100):** skin smoothing, whitening, face slim, big eyes, lipstick, blush.
**Custom (toggle + slider, landmark SDF):** chin shorten/lengthen (warp vertikal daerah dagu), nose slim (warp horizontal daerah hidung), eye brighten (iris whitening lokal), eyebrow tint (SDF pita alis), eyeshadow (SDF kelopak atas), eyeliner (SDF garis bulu mata atas), foundation/even skin tone (flatten high-pass + tint), lip color + finish matte/gloss (SDF region bibir; gloss = spekular reflect + ramp; matte = diffuse flat), blush (built-in bila Mars ada; custom SDF fallback opsional).

## 7. Preset & settings

- Preset: Natural / Soft / Glam / Clear Skin / No Makeup — tabel `effect_id → intensity` di C++ (`presets.*`), hold-compare = tampilkan frame bypass (simpan snapshot params, render raw).
- Settings JSON (`%APPDATA%/KameraGeulis/settings.json`): camera device, resolution, fps, mirror, all sliders/toggles, vcam on/off, locale, first-run flags. Load saat start, save saat exit + debounced saat berubah. Round-trip teruji di unit test.
- Mirror: toggle — capture tidak dibalik; preview & vcam dibalik via uniform flip (satu flag di shader blit + capture no flip) → konsisten & murah.

## 8. Virtual camera (softcam)

- Build: ExternalProject → `softcam.sln` (x64 + Win32) → `softcam.dll` x64/Win32 + `softcam.lib` (sender import) + `softcam_installer.exe`.
- Patch saat build: `FILTER_NAME` "DirectShow Softcam" → **"KameraGeulis Camera"** (sed via patch step, terdokumentasi).
- Register: tombol "Install Virtual Camera (admin)" → `ShellExecuteEx("runas", softcam_installer.exe, dllPath)`; Unregister serupa dengan `--unregister`? (cek sumber installer softcam: menerima arg path DLL; unregister via bat yang sama dengan flag berbeda — diverifikasi saat M3 dari source).
- Sinkron: paket frame RGBA 720p/1080p → shared memory softcam (copy, ~4 MB/frame @1080p RGBA? RGB24 di softcam 3 byte/px → 1920×1080×3 ≈ 6.2 MB @30fps ≈ 187 MB/s — OK untuk shared memory lokal).
- Toggle UI "Virtual Camera" + indikator "Terhubung/Terputus" via `scIsConnected`.

## 9. Threads & sinkronisasi

- `CameraCapture` thread → `moodycamel`-less hand-rolled `BoundedQueue<Frame>` (mutex+cv, drop-oldest).
- `FaceTracker` di render thread (murah setelah Mars/syntesis; readback 1×/frame untuk dipakai filter & smoother).
- `SinkRawData` buffer dipinjam render thread; copy untuk vcam dilakukan sinkron di thread yang sama (hindari GL lintas-thread; GPUPixel single-context).
- Settings save: background thread, debounced 500 ms (atomic flag + mutex untuk snapshot).

## 10. Testing strategy

- `kg_tests` (Catch2, semua platform): bounded queue drop-oldest, OneEuro convergence & jitter reduction, LandmarkSet validity, ROI/SDF math, preset application (offset dari default), settings JSON round-trip (save→load→equal), string table ID/EN completeness (keys equal), effect-param clamping.
- Linux CI: build `kg_core` + `kg_tests` saja (tanpa X11/GL) → `ctest`.
- Windows CI: full build Release x64 + run `kg_tests` + `KameraGeulis.exe --selftest` (no GL: config/presets/strings; exit 0).
- Tidak ada GPU di CI → runtime render tidak diverifikasi CI; RISK dilaporkan di STATUS.md.
