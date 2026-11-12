# GPUPIXEL.md — RCA crash gpupixel.dll 0x3968a + aset yang ikut artifact

## Gejala (dump dari PC pengguna, Windows 11 build 26200, GTX 1660 SUPER, driver 617.42)

- Mode normal: log berhenti tepat setelah `MF capture start ... -> OK`;
  app langsung mati. Crash handler mencatat: **exception 0xC0000005 (AV read @ 0x0), faulting module `gpupixel.dll`, RVA 0x3968a** — **identik** antar-build (deterministik).
- Instruksi crash (disassembly dump): `mov rcx,[r14+0xd8]; mov rax,[rcx]; call [rax+0x40]` — panggilan virtual pada objek `this` **nullptr** — didahului `glActiveTexture(GL_TEXTURE5)` dan disusul `glBindTexture(GL_TEXTURE_2D, id)`.

## Verifikasi di source (pin `f33abfa5` = v1.3.1)

`src/filter/beauty_face_unit_filter.cc` → `BeautyFaceUnitFilter::Init()`:

```cpp
auto path = Util::GetResourcePath() / "res";
gray_image_     = SourceImage::Create((path / "lookup_gray.png").string());
original_image_ = SourceImage::Create((path / "lookup_origin.png").string());
skin_image_     = SourceImage::Create((path / "lookup_skin.png").string());
custom_image_   = SourceImage::Create((path / "lookup_light.png").string());
return true;                              // ← Init SELALU sukses
```

Rantai kegagalan:

1. `SourceImage::Create` mengembalikan `nullptr` bila file tidak ada (`fs::exists` gagal → `return nullptr`; `assert(false)` **ter-compile-out** di Release/NDEBUG).
2. `BeautyFaceUnitFilter::Init()` tetap `return true` → filter dibuat.
3. `DoRender()` (frame PERTAMA): `glActiveTexture(GL_TEXTURE5); glBindTexture(GL_TEXTURE_2D, gray_image_->GetFramebuffer()->GetTexture());` → virtual call (`GetFramebuffer`) pada `gray_image_ == nullptr` → **AV read @ 0x0** — pola persis `mov rcx,[r14+0xd8]; mov rax,[rcx]; call [rax+0x40]` setelah `glActiveTexture(GL_TEXTURE5)` + `glBindTexture`. **Hipotesis sumber-missing-resource TERKONFIRMASI** (sesuai pola instruksi + RVA statis; pembuktian fungsi persis via symbolizer di CI — lihat di bawah).

Pemetaan GL_TEXTURE: 5 = lookUpGray, 6 = lookUpOrigin, 7 = lookUpSkin, 0 = lookUpCustom.

## File resource GPUPixel yang harus ikut artifact

| File | Pemuat | Dipakai filter |
|---|---|---|
| `res/lookup_gray.png` | BeautyFaceUnitFilter::Init | rantai BeautyFaceFilter (dipakai) |
| `res/lookup_origin.png` | idem | idem |
| `res/lookup_skin.png` | idem | idem |
| `res/lookup_light.png` | idem (custom_image_ → lookUpCustom) | idem |
| `res/blusher.png` | BlusherFilter::Init | (belum di-wire; M4) |
| `res/mouth.png` | LipstickFilter::Init | (belum di-wire; M4) |
| `res/lookup_custom.png` | (tidak ada pemuat di source saat ini; ikut di-bundle apa adanya) | — |

Semua dari **repository GPUPixel** (`src/res/`), Apache-2.0 — lihat THIRD_PARTY.md. Tidak di-commit ke repo KameraGeulis; disalin **di CI** dari FetchContent source pada ref yang di-pin.

## API resource path (yang dipakai aplikasi)

- Public: `gpupixel::GPUPixel::SetResourcePath(const std::string& root)` (`include/gpupixel/gpupixel.h`) → diteruskan ke `Util::SetResourcePath(fs::path)` (`src/utils/util.cc`).
- Filter membaca `Util::GetResourcePath() / "res"`.
- Aplikasi memanggil `GPUPixel::SetResourcePath(exe_dir)` (trailing `\\`) → filter membuka `<exe_dir>res\lookup_*.png`. **Folder `res\` harus bersebelahan dengan exe.**

## Fix di aplikasi (M2d)

- `BeautyPipeline::Init` **mem-pre-verifikasi** keempat texture lookup (ada + ukuran tercatat di log `[pipeline] resource res/...`); bila ada yang hilang → `Init` gagal → efek GPU dimatikan, **preview = frame mentah**, dan `LastResourceError()` tampil di UI (`notice` merah) + log. Tidak ada lagi crash sunyi.
- CI: `gpupixel.dll` dibangun `/Zi /DEBUG /MAP` (PDB + MAP ikut artifact); step **Verify artifact contents** menggagalkan build bila `res/*.png`, `models/*.onnx`, DLL runtime, atau simbol tidak lengkap; step **Resolve crash RVA** menjalankan `scripts/resolve-pdb-rva.ps1` untuk memetakan RVA 0x3968a → simbol fungsi (bukti, bukan dugaan).
- Models ikut artifact (`dist\models\`, diverifikasi SHA-256 pin saat load) → first-run tidak butuh internet; urutan pencarian model disatukan antara aplikasi dan `--selftest` lewat `kg_pipeline/windows_paths.h` (env → `<exe>\models` → LOCALAPPDATA).

## Catatan jujur

- Perbaikan ini **belum** diverifikasi di PC pengguna — M2c/M2d artifact menampilkan preview hanya bila `res/` lengkap DAN driver GL menjawab. Laporan dari PC pengguna menentukan.
- `--safe` tetap tidak memuat GPUPixel/ORT sama sekali (objek pipeline tidak dibuat).
