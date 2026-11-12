# LANDMARKS.md — Landmark wajah di KameraGeulis (M1b)

**Tanggal:** 2026-11-12 · **Status:** implementasi C++ M1b mendarat (kg_core: anchor/decode/ROI/mesh decode + OneEuro teruji hijau di Linux — 8319 assertions; provider ORT Windows); pemetaan 468→GPUPixel-Mars **ditolak** (§8); efek landmark v1 tidak di-wire; models-v1 belum berisi aset.
Bahasa dokumen: Indonesia (konsisten dengan dokumen lain). English key terms dipertahankan.

Dokumen ini **menggantikan** keputusan M0 ("tidak pakai model wajah di v1") di `docs/RESEARCH.md` §1/§2 dan catatan di `STATUS.md`. Riset baru (di bawah) menemukan jalur yang jelas-jelas berlisensi Apache-2.0 **dan** bisa dipin dengan SHA-256.

---

## 1. TL;DR keputusan

| Keputusan | Nilai |
|---|---|
| Provider landmark v1 (Windows) | **ONNX Runtime (MIT)** + 2 model ONNX hasil konversi dari model MediaPipe Apache-2.0 |
| Model deteksi | `face_detection_short_range.onnx` (BlazeFace short-range, dari `face_detection_short_range.tflite`) |
| Model mesh | `face_mesh.onnx` (468-point FaceMesh "facecontours", dari npm `@mediapipe/face_mesh`) |
| Distribusi model | **GitHub Release `models-v1`** di repo ini (asset download URL stabil), diunduh saat build/first-run oleh CMake/script dengan **embedded SHA-256**; model **tidak pernah** di-commit |
| Landmark count | **468** (bukan 478 — lihat §6 keterbatasan) |
| Runtime | ONNX Runtime win-x64 (CPU EP), v1.31.1+ |
| Lisensi semua komponen | Apache-2.0 (model), MIT (ORT), MIT (onnxruntime GitHub) — atribusi di `THIRD_PARTY.md` |

Alasan singkat: ini satu-satunya jalur yang **(a)** lisensinya jelas komersial-aman (Apache-2.0), **(b)** sumbernya bisa diverifikasi dengan SHA-256 yang diterbitkan Google, **(c)** tidak butuh Bazel/TF di mesin user/CI, **(d)** model kecil (≈2.8 MB total).

---

## 2. Rantai provenance & checksum (terverifikasi sendiri, 2026-11-12)

Sumber kebenaran checksum: `third_party/external_files.bzl` repo `google-ai-edge/mediapipe` (master, diambil via GitHub API `Accept: application/vnd.github.raw`, 2026-11-12).

### 2.1 Detektor — verified ✓

| Item | SHA-256 |
|---|---|
| `face_detection_short_range.tflite` resmi (http_file `com_google_mediapipe_modules_face_detection_face_detection_short_range_tflite`) | `3bc182eb9f33925d9e58b5c8d59308a760f4adea8f282370e428c51212c26633` |
| `face_detection_short_range.tflite` di dalam npm `@mediapipe/face_detection@0.4.1646425229` (diekstrak dari tarball) | **sama persis** — `cmp` identik |
| carved record 2 dari `face_mesh_solution_packed_assets.data` (npm `@mediapipe/face_mesh`) | **sama persis** — `cmp` identik |

→ Rantai: Google menerbitkan SHA-256 → kita cocokkan dengan file di npm registry (kanal distribusi yang berfungsi) → **provenance detector terbukti byte-identik**.

### 2.2 FaceMesh (468, facecontours) — npm provenance ✓

| Item | SHA-256 |
|---|---|
| `@mediapipe/face_mesh@0.4.1633559619` tarball (`/-/face_mesh-0.4.1633559619.tgz`) | `a65c9a7509560f29e574c026dc6a667d2154617eb3ba0c287db02350de0c116f` |
| `face_mesh_solution_packed_assets.data` di dalamnya | `dbe5905c582c0462cdaea17e7e6ecea92edaa8cccc515c5e2e7291f2cbb5fb99` |
| `facemesh_iris_faceflag_blaze` record @ offset 0 (panjang 2495952) | `883b7411747bac657c30c462d305d312e9dec6adbf8b85e2f5d8d722fca9455d` — **byte-identik dengan `face_landmark_with_attention.tflite` resmi** (`external_files.bzl` baris 223-226, sama dengan `modules/face_landmark/face_landmark_with_attention.tflite`) ✓ |
| `face_landmark.tflite` record @ offset 2495952, berhenti di 3737848 (**panjang 1241896**, dipakai v1) | `c603fa6149219a3e9487dc9abd7a0c24474c77263273d24868378cdf40aa26d1` — model lawas "facemesh_facecontours_faceflag_blaze"; **tidak ada di `external_files.bzl` main branch saat ini** (hanya ada varian iris/attention dan testdata). Provenance: paket npm Apache-2.0 (model card paket: Apache-2.0). |
| `geometry_pipeline_metadata_landmarks.binarypb` record @ offset 3737848, berhenti di 3757224 (panjang 19376) | `bdbcda96dfcb7da883da124aaa2c55dee49770d934f0fcc71747f8c21bdc75b4` — tidak dipakai v1 (metadata pipa GPU), diverifikasi hanya sebagai penguat rantai |
| `face_detection_short_range.tflite` di dalam packed assets @ offset 3757224 (panjang 229032) | `3bc182eb9f33925d9e58b5c8d59308a760f4adea8f282370e428c51212c26633` — **identik dengan paket `@mediapipe/face_detection`** → dua sumber npm sepakat ✓ |

Koreksi M1c (2026-11-12): dokumen versi awal menulis record mesh sebagai "panjang 1261272" dengan SHA `2b2f6274…` — itu sebenarnya `face_landmark.tflite` **ditambah 19376 B binarypb di belakangnya** (end offset keliru menunjuk 3757224). tf2onnx mengabaikan byte trailing sehingga konversi tetap berhasil; record yang benar adalah `c603fa61…` di atas. Pipeline CI (`scripts/models/prepare_sources.py`) sekarang memotong tepat sesuai offset dari metadata loader paket dan memverifikasi semua digest di atas — tidak ada lagi pencarakan manual.

### 2.3 Model ONNX hasil konversi (yang di-download build)

Keduanya dikonversi dari `.tflite` di atas secara **otomatis di GitHub Actions** (workflow `Models`, `.github/workflows/models.yml`) oleh `scripts/models/prepare_sources.py` + `tf2onnx==1.17.0` / `tensorflow==2.17.1` / opset 15 — tanpa modifikasi bobot/graf (hanya Transpose/Cast/Const/Reshape fusion dari optimizer tf2onnx). Verifikasi I/O via onnxruntime CPU: nama & bentuk input/output dicek ketat (inference test lulus di run 43179962364). Digest di bawah adalah **keluaran CI** (bukan dari mesin dev) dan dipin di `ort_landmark_provider.cc`:

Model di-upload ke release **`models-v1`** (workflow create-if-missing; asset published 2026-11-12 — `face_detection_short_range.onnx` 418.502 B + `face_mesh.onnx` 2.429.059 B). Download sisi klien terverifikasi SHA-256 di `ort_landmark_provider.cc` — pin tidak cocok → file dihapus → efek landmark nonaktif dengan pesan jujur.

| File ONNX | Sumber .tflite | SHA-256 ONNX (pin di `ort_landmark_provider.cc`) |
|---|---|---|
| `face_detection_short_range.onnx` | npm face_detection (sha tflite `3bc1…` ✓) | `82ac962b24dfd78a5b6a3331e8c4091e377d699c6e4fdaca158fba03b5af8e3d` |
| `face_mesh.onnx` | carve `face_landmark.tflite` `c603fa61…` ✓ | `ac72f85b77368a2ee1d065257674cb25099699d98376685af549df50d6a40337` |

I/O ONNX (diverifikasi dengan onnxruntime 1.31.1):
- `face_detection_short_range.onnx`: input `input` `[1,128,128,3]` f32, range `[-1,1]` (letterbox, keep-aspect, zero border). Output `regressors` `[1,896,16]` (x,y,w,h + 6 keypoints ×2, ternormalisasi ke 128-px tensor), output `classificators` `[1,896,1]` (logit, apply sigmoid).
- `face_mesh.onnx`: input `input_1` `[1,192,192,3]` f32 range `[0,1]`. Output `conv2d_21` `[1,1,1,1404]` = 468 titik × (x,y,z) (skala: piksel relatif tensor 192×192, dibagi 192 saat denormalisasi), output `conv2d_31` `[1,1,1,1]` = face flag (score kehadiran wajah, pakai untuk gerbang "di lokasi ROI ada wajah").

Pipeline lengkap (letterbox → anchor decode (896 anchor, stride 8/16/16/16, min_scale 0.1484375, max_scale 0.75, fixed anchor size, aspect 1.0, interpolated aspect 1.0, `reverse_output_order`, sigmoid score, NMS IoU 0.3) → eye-aligned ROI (`DetectionsToRects(start_kp=0, end_kp=1, target 0°)` + `RectTransformation` scale 1.5 square_long) → crop rotasi → mesh → inverse transform ke koordinat frame) **sudah diimplementasikan dan diverifikasi numerik** memakai Python + onnxruntime 1.31.1 pada `skimage.data.astronaut()` (512×512, skimage; NASA public domain):

- deteksi: 1 wajah, score **0.921**, box ≈ (179,82)–(274,177) (sesuai wajah pada gambar);
- faceflag mesh: **31.45** (sangat tinggi = wajah di ROI);
- overlay titik 468 pada gambar **menempel persis di kontur wajah** (mata, hidung, bibir, rahang) — lihat `/tmp/mp/astronaut_overlay.png` (artefak lokal, tidak di-commit);
- koordinat landmark terdenormalisasi kembali ke frame asli dengan benar.

Catatan: implementasi final adalah C++ (di `kg_core/landmark/`) dengan matematika identik; file Python `/tmp/mp/pipe.py` adalah referensi executable dan **bukan** bagian dari repo.

---

## 3. Opsi yang dievaluasi & DITOLAK (dengan alasan)

### Option A — MediaPipe Tasks C API (`libmediapipe.dll`) + `face_landmarker.task`
- `libmediapipe.dll` ada di dalam wheel `mediapipe-1.2.3-py3-none-win_amd64.whl` (PyPI, Apache-2.0, 25.7 MB wheel; dll SHA-256 `b5a7c1f2162996f88f947f9fba4930a04dcbced33eece033386064c37602a3de`) export 638 simbol termasuk `MpFaceLandmarkerCreate/DetectForVideo/DetectImage/Close` (modul `mediapipe/tasks/c/vision/face_landmarker/*.h`, Apache-2.0). API C bersih, dukungan LIVE_STREAM + callback.
- **DITOLAK sebagai jalur utama** karena model `face_landmarker.task`:
  1. Tidak ada SHA-256 publik yang stabil untuk varian produksi (`storage.googleapis.com/mediapipe-models/face_landmarker/...float16/latest/...`) — hanya `?generation=` floating URL; constraint kita "download-with-embedded-sha256" **tidak bisa dipenuhi**.
  2. `external_files.bzl` *memang* memaku `face_landmarker.task` **testdata** (`7cf2bbf1…`, http_file `…tasks_testdata_vision_face_landmarker_task`) dan `face_landmarker_v2.task`, tetapi itu aset pengujian MediaPipe, bukan artefak rilis resmi untuk produk; selain itu **URL `storage.googleapis.com` diblokir dari sandbox ini** (curl exit 35) dan tidak ada jaminan keterjangkauan dari CI.
  3. Lisensi file `.task` produksi (model card menyebut Apache-2.0) — OK, tapi tanpa checksum publik kita tidak bisa menyematkan pin yang bisa diverifikasi → melanggar constraint M1b.
- Catatan: kalau nanti URL+SHA produksi diterbitkan Google, Option A layak ditinjau kembali (478 landmark + blendshapes, dukungan GPU delegate).

### Option C — TFLite-C + carve `.tflite` dari npm
- Carve berhasil dan byte-identik (§2.1, §2.2) — tapi **tidak ada binary TFLite-C resmi untuk Windows**: rilis terbaru `tensorflow/tensorflow` (v2.21.0 / v2.22.0-rc0, diperiksa 2026-11-12) **tidak menyertakan aset rilis apa pun** (`assets: []`), dan TensorFlow C library memang tidak pernah dirilis untuk Windows. Membangun TFLite dari source (Bazel) terlalu berat/rapuh untuk CI pengguna. **DITOLAK.**

### Option D — OpenCV Zoo YuNet (`face_detection_yunet_2023mar.onnx`, MIT) — deteksi saja
- Lisensi OK (MIT), tapi hanya **deteksi kotak wajah**, bukan 468 landmark. Efek face slim/big eyes/makeup butuh titik landmark mesh → tidak cukup. **DITOLAK sebagai provider landmark** (tetap boleh sebagai fallback detektor masa depan; tidak dipakai v1).

### Option B″ — build-time carve dari npm tarball + runtime `.tflite`
- Membutuhkan interpreter TFLite di Windows Runtime (dll) → sama dengan blocker Option C. **DITOLAK.** (Carve *memang* dipakai untuk memperoleh model §2.3, tapi dilakukan sekarang, sekali, oleh maintainer — bukan di mesin user.)

### Ditolak sebelumnya (M0) tetap berlaku
- dlib 68-pt (300-W non-komersial), PFLD-106 (lisensi tidak jelas), Mars-Face Windows (binary tidak ada), contrib face models (lisensi tidak jelas).
- `face_landmark_with_attention` → ONNX **gagal**: tf2onnx melaporkan op kustom MediaPipe yang tidak ter-map (`TFL_TransformLandmarks` ×5, `TFL_Landmarks2TransformMatrix` ×3, `TFL_TransformTensorBilinear` ×3 — custom op "faceflag/iris GPU transform"). File `facemesh_iris.onnx` yang sempat "tersimpan" **tidak bisa diload** di ONNX Runtime (`InvalidGraph … No Op registered for TFL_Landmarks2TransformMatrix`) — dicatat jujur, jangan dipakai.

---

## 4. Rencana integrasi (M1b, setelah M1 hijau)

1. **CMake/`FetchContent`-style download** (`cmake/models.cmake` + `scripts/download_models.cmake`): build/first-run mengunduh 2 file ONNX dari `https://github.com/hanlimzie-glitch/KameraGeulis/releases/download/models-v1/{face_detection_short_range.onnx,face_mesh.onnx}`, verifikasi SHA-256 (nilai §2.3), cache di `build/_models/` (atau `%LOCALAPPDATA%/KameraGeulis/models` untuk first-run). Gagal download/verifikasi → efek landmark auto-disabled + pesan jujur di UI & log (tidak pernah crash). Release GitHub `models-v1` sudah dibuat (catatan rilis berisi lisensi/provenance) — **asset belum di-upload dari sandbox ini** (`uploads.github.com` unreachable; lihat §5). Perintah upload (jalankan di mesin yang bisa mengakses GitHub):
   ```
   gh release upload models-v1 face_detection_short_range.onnx face_mesh.onnx
   ```
   (kedua file SHA-256 tercantum di §2.3; jangan ubah nama file.)
2. **ONNX Runtime**: FetchContent/`find_package` — pin `onnxruntime` v1.31.1 (2026-11 latest; zip win-x64 `onnxruntime-win-x64-1.31.1.zip`, 82,929,944 B, SHA-256 `5869e26bb25314375f2d5ef1877e3ea06b1543d6c710e6e4a1ec2f7177ab4b2c` dari GitHub release digest; Linux tgz `c849e36a…`). Link `onnxruntime.lib`/`onnxruntime.dll` (CPU EP). Jika DirectML EP ikut terbundel di zip — biarkan opsional, tidak wajib.
3. **`kg_core` (unit-tested di Linux)**:
   - `landmark/face_anchors.*` — generator 896 anchor (grid merged-same-stride, offset 0.5, formula dari `ssd_anchors_calculator.cc`), test: jumlah anchor == 896, anchor ke-0 = (0.5/16, 0.5/16), range skala [0.1484375, 0.75].
   - `landmark/face_detection_decode.*` — decode `regressors/classificators` + sigmoid + NMS (IoU 0.3, weighted) + letterbox unmap; test dengan tensor sintetik.
   - `landmark/roi_align.*` — eye-aligned ROI (rotasi via kp0→kp1) + square crop 1.5× + inverse transform landmark; test round-trip.
   - `landmark/one_euro.*` — OneEuroFilter per titik (minCutoff/β default 1.0/0.007? — akan dikalibrasi; expose setting), test konvergensi + lag.
   - `landmark/landmark_set.*` — `LandmarkSet{ std::array<Vec2,468>; bool valid; float confidence; int64_t ts_ms; }` — menggantikan `std::array<Vec2,111>` dari desain M0 (hapus referensi Mars).
4. **`kg_pipeline`**: `OrtLandmarkProvider` (Windows; Linux: compile-time stub + unit test decode saja) — menjalankan detektor tiap frame (atau tiap N frame + tracking), mesh pada ROI, `faceflag > threshold` sebagai gerbang kehadiran, OneEuro smoothing, fade-out 300 ms saat wajah hilang.
5. **UI**: indikator status di panel ("Landmark: aktif (ORT CPU)" / "model tidak tersedia — efek landmark nonaktif"), jujur sesuai desain M0.
6. **`THIRD_PARTY.md`**: tambah entri — MediaPipe models (Apache-2.0, via npm tarball + carve + tf2onnx; URL, SHA-256, model card links), ONNX Runtime (MIT), tf2onnx (MIT, hanya dipakai maintainer).

### Constraint yang dipenuhi
- Model **tidak di-commit** ✓ (hanya SHA-256 di CMake/docs).
- Download-with-embedded-checksum ✓ (build/first-run; gagal → disabled jujur).
- Lisensi jelas: Apache-2.0/MIT ✓; tidak ada komponen komersial-terlarang ✓.
- THIRD_PARTY.md mencatat sumber+lisensi ✓ (akan diupdate di M1b).

---

## 5. Yang BELUM diverifikasi / pekerjaan maintainer

1. ~~Upload 2 ONNX ke release `models-v1`~~ — **SELESAI (M1c, 2026-11-12):** workflow `Models` mengunggah kedua ONNX ke release `models-v1` (run 43179962364 hijau; I/O diverifikasi onnxruntime di CI). Download first-run dari mesin bersih belum diverifikasi (tidak ada Windows+koneksi bebas di lingkungan pengembang) — first-run di mesin user bisa gagal dengan pesan jujur (efek landmark nonaktif) bila `github.com` tidak terjangkau; `KAMERAGEULIS_MODELS_DIR` bisa menunjuk folder model lokal.
2. **Reachability `github.com/.../releases/download/...` dari runner Windows CI / mesin bersih** — belum dites end-to-end (asset sekarang sudah ada; download 2,8 MB dari sandbox tidak mungkin). Pertama-tama akan terjawab saat M2 artifact diuji di Windows nyata.
3. **`face_mesh.onnx` (468, facecontours) tidak punya SHA-256 terbitan Google** — checksum record berasal dari tarball npm `a65c…` yang kita unduh; kita terbitkan checksum hasil carve. Risiko residual: mirror npm yang dikompromikan akan gagal di checksum tarball/record (berlapis: tgz → .data → record). Dapat diterima untuk proyek ini; didokumentasikan jujur.
4. **Kualitas mesh 468** vs 478 (iris/attention): v1 **tidak punya** 10 titik iris & refinement bibir/mata dari with-Attention. Untuk efek v1 (slim, big eyes, lipstick, blush, chin, nose) 468 cukup; iris-tracking **bukan** bagian v1. Peningkatan masa depan: konversi with_attention ke runtime yang mendukung custom op (MediaPipe GPU delegate / TFLite) atau ONNX op kustom — didokumentasikan di `docs/ROADMAP.md` (dibuat nanti).
5. **Performa CPU** belum diukur di hardware nyata (estimasi: detektor ~1–3 ms, mesh ~2–5 ms @ 720p CPU; BELUM DIVERIFIKASI).
6. **Linux/macOS**: provider Mars-Face (GPUPixel) lama masih opsional; jalur utama (ORT) cross-platform — keputusan final saat M1b selesai.

---

## 6. Ringkasan spesifikasi teknis (untuk implementasi C++)

**Anchors (896):** strides `[8,16,16,16]`; `scale_i = min_scale + (max_scale - min_scale)·i/(num_strides−1)` dengan `min_scale=0.1484375`, `max_scale=0.75` → `scales = [0.1484375, 0.3489583, 0.5494792, 0.75]`. Layer di-merge per stride yang sama; untuk setiap layer di grup, anchor = `scales[i]` dan `sqrt(scales[i]·scales[i+1])` (layer terakhir: `sqrt(scales[i]·1.0)`), `w=h=1` dalam unit cell (`fixed_anchor_size`), aspect 1.0, center `(x+0.5)/fmap`. Jumlah: stride 8 → 16×16×2 = 512; stride 16 → 8×8×6 = 384; **total 896**. Diverifikasi: generator Python menghasilkan persis 896 anchor dan deteksi benar. **Referensi otoritatif:** `ssd_anchors_calculator.cc` + `face_detection.pbtxt` (google-ai-edge/mediapipe `master`, diambil 2026-11-12) — port C++ harus identik; unit test membandingkan terhadap tabel golden yang dibekukan di test.

**Decode:** `cx = raw[0]/128*anchor.w + anchor.cx` (idem cy,w,h; keypoints sama), sigmoid(score), threshold 0.5, NMS IoU 0.3 (WEIGHTED: skor hasil weighted NMS — detail di `tflite_tensors_to_detections_calculator.cc`; untuk 1 wajah non-maximum sufficiently distinct).

**ROI→crop:** rotasi = `atan2(kp1.y-kp0.y, kp1.x-kp0.x)` (kp = index 0,1 pada output keypoint detektor = mata kiri/kanan menurut MediaPipe: rotation_vector_start_keypoint_index=0, end=1), kotak square `max(w,h)*1.5` berpusat di box center, crop dirotasi → 192×192; landmark tensor → bagi 192 → un-rotate → tambah center → koordinat frame.

**Face flag:** tensor `conv2d_31` — gunakan `> 0` sebagai "ada wajah di ROI" (MediaPipe memakai threshold 0.5 setelah sigmoid untuk flag bertipe logit? — faceflag di model lawas adalah skor kehadiran mentah; di pipeline Python, nilai 31.45 untuk wajah valid — ambang final ditentukan saat M1b dengan beberapa sampel; aman: gerbang = NMS score deteksi + flag > 0).

Semua detail numerik di atas **sudah dijalankan dan menghasilkan overlay landmark yang benar** di Python reference — risiko implementasi hanyalah pada porting, yang diuji unit test (anchor golden, decode sintetik, round-trip ROI, OneEuro konvergensi) di Linux CI sebelum merge.

---

## 7. Dampak pada dokumen lain

- `docs/RESEARCH.md` §2 (paragraf face_landmarker.task) — dinyatakan superseded oleh dokumen ini.
- `STATUS.md` — baris "Efek landmark nonaktif di Windows" diubah setelah M1b mendarat (default → aktif dengan ORT CPU).
- `THIRD_PARTY.md` — tambah ONNX Runtime (MIT) + catatan model (§4.6).
- `docs/ARCHITECTURE.md` §5 — `LandmarkSet` jadi 468 titik; provider baru `OrtLandmarkProvider`; `NullLandmarkProvider` tetap ada sebagai fallback saat model tidak tersedia.
- `docs/ROADMAP.md` (dibuat saat M5) — "478/iris via custom op" + "Option A revisit jika checksum produksi diterbitkan".

---

## 8. Temuan M1b (2026-11-12): pemetaan 468 → 106/111 titik Mars TIDAK kredibel — efek GPUPixel tidak di-wire di v1

Saat implementasi, pertanyaan arsitektural "468 titik MP → 106/111 titik Mars (FaceReshape/Lipstick/Blusher)" dianalisis dengan kedua canonical model yang tersedia:

**Metode:** `canonical_face_model.obj` (MediaPipe, Apache-2.0; 468 vertex 3D, diunduh via GitHub raw 2026-11-12, dipakai hanya sebagai referensi offline — tidak di-commit/di-bundle) vs `FaceTextureCoordinates()` GPUPixel `face_makeup_filter.cc` (111 titik uv 2D; indeks untuk `FaceReshapeFilter` mengacu `face_reshape_filter.cc`).

**Hasil numerik:**

1. Kedua canonical model ber*proporsi berbeda*: model MP menempatkan mata di y_norm ≈ 0.30–0.34 dan alis ≈ 0.18; kanonikal Mars menempatkan alis di v ≈ 0.33, mata di v ≈ 0.39–0.42, bibir di v ≈ 0.58–0.65, dagu v ≈ 0.71 — dan oval Mars (indeks 0–32) hanya busur pipi→dagu (v 0.384–0.710), bukan seluruh wajah. Tidak ada transformasi affine global yang menyejajarkan keduanya secara serentak.
2. Fit affine anisotropik pada busur oval (32/33 titik, traversal sama) konvergen dengan residual mean 0.010, max 0.018 — **hanya untuk region busur bawah wajah**. Region jidat/alis/mata melenceng sistematis ±0.08–0.12.
3. Hungarian 1:1 global di atas transform oval menghasilkan penugasan alis/mata ke titik anatomi yang salah (indeks mata Mars → cheekbone/under-eye MP; jarak 0.01–0.04 tetapi beda struktur), termasuk indeks MP duplikat pada kandidat NN. Untuk warp yang sensitif posisi (enlargeEye berpusat di titik 74/77), kesalahan sebesar itu = efek kasat salah.
4. SDK Mars (MNN, `mars-face-kit`) tertutup; tidak ada pemetaan 468→106/111 referensi di tree GPUPixel; demo hanya passthrough output Mars. Verifikasi visual butuh wajah nyata + GPU — tidak tersedia di sandbox/CI.

**Keputusan:** di v1, `FaceReshapeFilter`, `LipstickFilter`, `BlusherFilter` **tidak di-wire**. Efek landmark tetap **nonaktif**; UI menyatakannya jujur (`landmark.inactive` + tooltip "Belum tersedia di v1"). Rencana lanjutan (evaluasi terpisah): renderer custom berbasis MP (mesh bibir dari kontur MP 61…291, big-eye dari sudut mata MP 33/133/263/362, slim dari oval MP) — murni geometri MP, tanpa kanonikal Mars; atau rilis models-v1 + verifikasi di mesin Windows nyata dengan foto wajah terlebih dahulu.

**Dampak pada kontrak `SetFaceLandmarks` (terverifikasi dari source GPUPixel v1.3.1):**
- `FaceReshapeFilter::SetFaceLandmarks` → uniform `facePoints[106*2]` (212 float); ukuran vektor lain akan mengirim jumlah tak valid ke `glUniform1fv`.
- `FaceMakeupFilter::SetFaceLandmarks` → nilai input diduga [0,1] (dikalikan 2−1 ke NDC; dipakai sebagai vertex attribute, bukan uniform). Arah y (gl_Position.y tanpa flip) tidak bisa diverifikasi tanpa runtime — satu alasan lagi untuk tidak mem-wire sekarang.
- `LipstickFilter`/`BlusherFilter` memerlukan aset `res/mouth.png`/`blusher.png` + `SetTextureBounds` — tergantung `Util::GetResourcePath()`.

**Status implementasi M1b:** kg_core (face_anchors/face_detection_decode/roi_align/face_mesh_decode/OneEuro) teruji hijau di Linux; `OrtLandmarkProvider` (Windows, ORT CPU) diimplementasikan — download first-run dari rilis `models-v1` (masih kosong per 2026-11-12) dengan SHA-256 pin; gagal → efek nonaktif + pesan jujur (tidak crash). Wiring UI efek: tidak ada (sengaja). Lihat `STATUS.md`.
