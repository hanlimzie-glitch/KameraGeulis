# Recon — Pola UX panel beauty camera (public behavior only)

Metode: replica-skill (clean-room). Tidak ada kode/aset/teks brand apa pun yang disalin;
yang diamati hanyalah *pola fungsional publik* dari produk beauty camera populer
(TikTok beauty, Prism Lens / SNOW-class apps, plugin beauty OBS seperti WidsMob/XSplit VCam,
ManyCam) di sumber publik (help center, artikel tutorial, docs OBS). Perilaku persis tiap
aplikasi bisa berubah; ini pola umum, bukan spesifikasi terverifikasi terhadap satu aplikasi.

Sumber publik yang dibaca:
- TikTok beauty mode tutorials (taller/slider 0–100, ikon custom untuk face shape/nose/eye — lihat fastercapital.com & filmora.wondershare.com; TikTok help center)
- OBS Video Capture Device (DirectShow) docs: https://obsproject.com/kb/ (capture device menampilkan device virtual)
- Media Foundation / softcam docs (virtual cam Windows) — lihat RESEARCH.md

## Pola yang diambil

1. **Slider per efek 0–100** — konvergen di semua produk. Default konservatif (≈30).
2. **Tab Face / Skin / Makeup / Presets** — mental model: bentuk wajah, kulit, riasan, preset.
3. **Preset 1-tap** (Natural/Soft/Glam) — satu ketukan menerapkan tabel nilai; bukan satu-satunya jalan, slider tetap tersedia.
4. **Hold-to-compare** (tahan tombol/👁) — tampilkan frame asli selama ditahan; lepas untuk kembali.
5. **Toggle per efek** — makeup bisa di-enable/disable tanpa mengubah slider (zero re-konfigurasi).
6. **Global reset** + **per-effect reset** (ikon ↺ kecil per baris).
7. **Mirror preview** (selfie mode default ON) + **capture device selector** + **resolusi/fps selector**.
8. **Snapshot PNG** (ikon kamera) — satu ketukan, disimpan ke folder snapshots/.
9. **Before/after A/B** tidak permanen — tidak ada mode "split screen" wajib; hold-compare cukup.
10. **Indikator status wajah** (wajah hilang → efek fade, bukan pop) — pola dari app AR stabil.
11. ** Virtual camera on/off** — toggle besar di panel utama, dengan status koneksi.
12. **Tooltips singkat** per kontrol; istilah produk tidak diterjemahkan secara harfiah, natural ID/EN.

## Yang TIDAK diambil

- Aset, nama, logo, teks pemasaran, atau palet warna brand apa pun.
- Preset bermerek (mis. nama lookup/look dari satu produk).
- Fitur sosial/cloud dari app konsumen (feed, sharing, akun).

## Keluaran

`replica/features.csv` — matriks fitur → must/should/could → milestone target.
Parity diukur pada M5 dengan parity.py dari replica-skill.
