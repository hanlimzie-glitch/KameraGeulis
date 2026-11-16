#pragma once
// M2k — topologi wajah resmi MediaPipe FaceMesh (Apache-2.0, lihat
// THIRD_PARTY.md). Data edge/segitiga di-generate dari
// face_mesh_connections.py oleh scripts/topology/gen_face_topology.py.
// Dipakai rasterisasi area kulit (triangulasi) + kontur pengecualian
// (mata/alisis/bibir/lubang hidung) — lihat mask_gen.h (GPU) dan uji Linux
// kg_tests (urutan loop, validitas triangulasi).
#include <cstdint>
#include <utility>
#include <vector>

namespace kgcore {

// Ellipse pengecualian lubang hidung (koordinat piksel frame, y ke bawah).
struct ExclusionEllipse {
  float cx = 0, cy = 0;
  float rx = 0, ry = 0;
};

// Bentuk-bentuk pengecualian dari landmark posisi (xy: 468*2, y ke bawah).
// Loop tertutup = urutan indeks landmark (digambar sebagai poligon berpusat
// centroid pada rasterisasi). Urutan dijamin tertutup dan tidak berpotongan
// diri untuk data FaceMesh valid.
struct ExclusionShapes {
  std::vector<std::vector<int>> loops;   // mata R, mata L, alis R, alis L, bibir
  std::vector<ExclusionEllipse> ellipses;  // lubang hidung R, L
};

// M2m: basis koordinat wajah ternormalisasi (asal = tengah garis mata,
// sumbu u sejajar garis mata ke kiri subjek, sumbu v tegak lurus ke bawah,
// satuan = jarak antar-mata / IOD). false bila mata belum valid.
struct FaceEyeBasis {
  float eye_mid_x = 0.0f, eye_mid_y = 0.0f;
  float ux = 1.0f, uy = 0.0f;
  float vx = 0.0f, vy = 1.0f;
  float iod = 0.0f;
  bool valid = false;
};
FaceEyeBasis ComputeFaceEyeBasis(const float* xy);
bool ToFaceNormalizedCoords(const FaceEyeBasis& basis, float x, float y,
                            float* norm_x, float* norm_y);

// Konstanta geometri lubang hidung kanonik MediaPipe (dalam satuan IOD):
// pusat sekitar (±0.15, +0.64) * IOD, radius inti 0.035 * IOD.
inline constexpr float kCanonicalNostrilNormX = 0.15f;
inline constexpr float kCanonicalNostrilNormY = 0.64f;
inline constexpr float kNostrilRadiusIodScale = 0.035f;

// Satu loop per bentuk: [right_eye, left_eye, right_brow, left_brow, lips_outer].
// Urutan loops[] tetap di atas; bila loop gagal dibentuk, loop tersebut dilewati
// (kosong) supaya rasterisasi tetap aman.
ExclusionShapes BuildExclusionShapes(const float* xy);

// Triangulasi area wajah (852 segitiga source-order resmi, indeks landmark
// <468; exclusions mata/mulut diterapkan sesudah union).
struct TessTriangle {
  uint16_t a, b, c;
};
const TessTriangle* TessTriangles();  // statis, tidak dimiliki pemanggil
int TessTriangleCount();

// Utility tertutup-dari-edge: semua vertex derajat 2 (mata, bibir luar) →
// satu siklus. Bila gagal (bukan siklus tunggal) mengembalikan kosong.
std::vector<int> OrderLoopFromEdges(const std::pair<uint16_t, uint16_t>* edges,
                                    int n);

// Raster referensi CPU untuk tes sintetis; produksi memakai rasterisasi GPU
// mask_gen. xy_px = landmark [468*2] dalam piksel frame top-down.
void RasterizeFaceMaskCpu(const float* xy_px, int fw, int fh, int mw, int mh,
                          std::vector<uint8_t>* mask);
void RasterizeExclusionMaskCpu(const float* xy_px, int fw, int fh, int mw,
                               int mh, std::vector<uint8_t>* exclusion);

}  // namespace kgcore
