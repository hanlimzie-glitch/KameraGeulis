#include "kgcore/face_mesh_topology.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <utility>

#include "kgcore/face_mesh_topology_data.h"

namespace kgcore {
using topology::Edge;

namespace {
// Indeks bibir LUAR (20 titik) — sisa indeks kLipsEdges adalah bibir dalam.
// Divalidasi uji: edge dengan kedua ujung di set ini membentuk siklus tertutup
// tunggal berjumlah 20.
constexpr uint16_t kLipOuter[20] = {61, 146, 91, 181, 84,  17,  314, 405, 321, 375,
                                   291, 185, 40,  39,  37,  0,   267, 269, 270, 409};
bool InLipOuter(uint16_t v) {
  for (uint16_t k : kLipOuter)
    if (k == v) return true;
  return false;
}

std::vector<int> OrderLoopGeneric(const Edge* edges, int n) {
  std::map<uint16_t, std::vector<uint16_t>> adj;
  for (int i = 0; i < n; ++i) {
    adj[edges[i].a].push_back(edges[i].b);
    adj[edges[i].b].push_back(edges[i].a);
  }
  // Semua vertex harus derajat 2 (siklus tunggal).
  for (const auto& kv : adj)
    if (kv.second.size() != 2) return {};
  if (adj.empty()) return {};
  std::vector<int> loop;
  uint16_t start = adj.begin()->first;
  uint16_t prev = start, cur = adj[start][0];
  loop.push_back(start);
  while (cur != start) {
    loop.push_back(cur);
    const auto& nb = adj[cur];
    uint16_t nxt = (nb[0] == prev) ? nb[1] : nb[0];
    prev = cur;
    cur = nxt;
    if (loop.size() > 512) return {};  // jaga-jaga dari siklus salah
  }
  if (loop.size() != adj.size()) return {};  // bukan satu siklus tunggal
  return loop;
}

// Pisahkan edge-set menjadi rantai terbuka (vertex derajat 1/2).
std::vector<std::vector<int>> ChainsOf(const Edge* edges, int n) {
  std::map<uint16_t, std::vector<uint16_t>> adj;
  for (int i = 0; i < n; ++i) {
    adj[edges[i].a].push_back(edges[i].b);
    adj[edges[i].b].push_back(edges[i].a);
  }
  std::vector<std::vector<int>> chains;
  std::map<uint16_t, bool> used;
  auto deg = [&](uint16_t v) { return adj[v].size(); };
  // Mulai dari ujung (derajat 1); bila semua derajat 2 → siklus (OrderLoop).
  std::vector<uint16_t> starts;
  for (const auto& kv : adj)
    if (deg(kv.first) == 1) starts.push_back(kv.first);
  if (starts.empty()) {
    auto one = OrderLoopGeneric(edges, n);
    if (!one.empty()) chains.push_back(std::move(one));
    return chains;
  }
  for (uint16_t s : starts) {
    if (used[s]) continue;
    std::vector<int> ch;
    uint16_t prev = s, cur = adj[s][0];
    ch.push_back(s);
    used[s] = true;
    while (!used[cur]) {
      used[cur] = true;
      ch.push_back(cur);
      const auto& nb = adj[cur];
      // Sampai di ujung rantai kedua (derajat 1): akhiri, jangan akses nb[1].
      if (nb.size() == 1) break;
      if (nb.size() != 2) {
        ch.clear();
        break;
      }
      uint16_t nxt = (nb[0] == prev) ? nb[1] : nb[0];
      prev = cur;
      cur = nxt;
      if (ch.size() > 64) break;
    }
    chains.push_back(std::move(ch));
  }
  return chains;
}
}  // namespace

std::vector<int> OrderLoopFromEdges(const std::pair<uint16_t, uint16_t>* edges,
                                    int n) {
  std::vector<Edge> tmp(n);
  for (int i = 0; i < n; ++i) tmp[i] = {edges[i].first, edges[i].second};
  return OrderLoopGeneric(tmp.data(), n);
}

const TessTriangle* TessTriangles() {
  static const std::vector<TessTriangle> tris = [] {
    std::vector<TessTriangle> v;
    v.reserve(topology::kTessTris_N);
    for (int i = 0; i < topology::kTessTris_N; ++i) {
      const auto& t = topology::kTessTris[i];
      v.push_back({t.a, t.b, t.c});
    }
    return v;
  }();
  return tris.data();
}
int TessTriangleCount() { return topology::kTessTris_N; }

namespace {
// Merge rantai berkoordinat (jarak ujung minimum) — untuk alis.
std::vector<int> MergeChainsXY(const std::vector<std::vector<int>>& chains,
                               const float* xy) {
  if (chains.empty()) return {};
  if (chains.size() == 1) return chains[0];
  if (chains.size() != 2) return {};
  const auto& A = chains[0];
  const auto& B = chains[1];
  auto d2 = [&](int ia, int ib) {
    float dx = xy[ia * 2] - xy[ib * 2];
    float dy = xy[ia * 2 + 1] - xy[ib * 2 + 1];
    return dx * dx + dy * dy;
  };
  // Dua orientasi penutupan: A+B vs A+rev(B); pilih total sambungan terkecil.
  double direct = d2(A.back(), B.front()) + d2(A.front(), B.back());
  double cross = d2(A.back(), B.back()) + d2(A.front(), B.front());
  std::vector<int> out = A;
  if (direct <= cross) {
    out.push_back(B.front());
    for (size_t i = 1; i < B.size(); ++i) out.push_back(B[i]);
  } else {
    out.push_back(B.back());
    for (int i = static_cast<int>(B.size()) - 2; i >= 0; --i)
      out.push_back(B[i]);
  }
  return out;
}
}  // namespace

namespace {
bool PointInTriangle(float px, float py, float ax, float ay, float bx, float by,
                     float cx, float cy) {
  const float d1 = (px - bx) * (ay - by) - (ax - bx) * (py - by);
  const float d2 = (px - cx) * (by - cy) - (bx - cx) * (py - cy);
  const float d3 = (px - ax) * (cy - ay) - (cx - ax) * (py - ay);
  const bool neg = d1 < 0 || d2 < 0 || d3 < 0;
  const bool pos = d1 > 0 || d2 > 0 || d3 > 0;
  return !(neg && pos);
}
bool PointInLoop(float px, float py, const float* xy,
                 const std::vector<int>& loop) {
  bool inside = false;
  for (size_t i = 0, j = loop.size() - 1; i < loop.size(); j = i++) {
    const int a = loop[i], b = loop[j];
    const float ax = xy[a * 2], ay = xy[a * 2 + 1];
    const float bx = xy[b * 2], by = xy[b * 2 + 1];
    if (((ay > py) != (by > py)) &&
        (px < (bx - ax) * (py - ay) / (by - ay + 1e-12f) + ax))
      inside = !inside;
  }
  return inside;
}
}  // namespace

void RasterizeFaceMaskCpu(const float* xy_px, int fw, int fh, int mw, int mh,
                          std::vector<uint8_t>* mask) {
  if (!xy_px || !mask || fw <= 0 || fh <= 0 || mw <= 0 || mh <= 0) return;
  mask->assign(static_cast<size_t>(mw) * mh, 0);
  const TessTriangle* tris = TessTriangles();
  for (int t = 0; t < TessTriangleCount(); ++t) {
    const int ids[3] = {tris[t].a, tris[t].b, tris[t].c};
    float x[3], y[3];
    for (int k = 0; k < 3; ++k) { x[k] = xy_px[ids[k] * 2]; y[k] = xy_px[ids[k] * 2 + 1]; }
    const int x0 = std::clamp(static_cast<int>(std::floor(std::min({x[0], x[1], x[2]}) * mw / fw)), 0, mw - 1);
    const int x1 = std::clamp(static_cast<int>(std::ceil(std::max({x[0], x[1], x[2]}) * mw / fw)), 0, mw - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(std::min({y[0], y[1], y[2]}) * mh / fh)), 0, mh - 1);
    const int y1 = std::clamp(static_cast<int>(std::ceil(std::max({y[0], y[1], y[2]}) * mh / fh)), 0, mh - 1);
    for (int py = y0; py <= y1; ++py) for (int px = x0; px <= x1; ++px) {
      const float fx = (px + 0.5f) * fw / mw;
      const float fy = (py + 0.5f) * fh / mh;
      if (PointInTriangle(fx, fy, x[0], y[0], x[1], y[1], x[2], y[2]))
        (*mask)[static_cast<size_t>(py) * mw + px] = 255;
    }
  }
}

void RasterizeExclusionMaskCpu(const float* xy_px, int fw, int fh, int mw,
                               int mh, std::vector<uint8_t>* exclusion) {
  if (!xy_px || !exclusion || fw <= 0 || fh <= 0 || mw <= 0 || mh <= 0) return;
  exclusion->assign(static_cast<size_t>(mw) * mh, 0);
  const ExclusionShapes shapes = BuildExclusionShapes(xy_px);
  for (const auto& loop : shapes.loops) {
    if (loop.size() < 3) continue;
    float minx = fw, maxx = 0, miny = fh, maxy = 0;
    for (int id : loop) { minx = std::min(minx, xy_px[id * 2]); maxx = std::max(maxx, xy_px[id * 2]);
                          miny = std::min(miny, xy_px[id * 2 + 1]); maxy = std::max(maxy, xy_px[id * 2 + 1]); }
    int x0 = std::clamp(static_cast<int>(std::floor(minx * mw / fw)), 0, mw - 1);
    int x1 = std::clamp(static_cast<int>(std::ceil(maxx * mw / fw)), 0, mw - 1);
    int y0 = std::clamp(static_cast<int>(std::floor(miny * mh / fh)), 0, mh - 1);
    int y1 = std::clamp(static_cast<int>(std::ceil(maxy * mh / fh)), 0, mh - 1);
    for (int py = y0; py <= y1; ++py) for (int px = x0; px <= x1; ++px) {
      const float fx = (px + 0.5f) * fw / mw, fy = (py + 0.5f) * fh / mh;
      if (PointInLoop(fx, fy, xy_px, loop))
        (*exclusion)[static_cast<size_t>(py) * mw + px] = 255;
    }
  }
  for (const auto& e : shapes.ellipses) {
    const float rx_eff = std::max(e.rx, 0.6f * static_cast<float>(fw) / mw);
    const float ry_eff = std::max(e.ry, 0.6f * static_cast<float>(fh) / mh);
    const int x0 = std::clamp(static_cast<int>(std::floor((e.cx - rx_eff) * mw / fw)), 0, mw - 1);
    const int x1 = std::clamp(static_cast<int>(std::ceil((e.cx + rx_eff) * mw / fw)), 0, mw - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor((e.cy - ry_eff) * mh / fh)), 0, mh - 1);
    const int y1 = std::clamp(static_cast<int>(std::ceil((e.cy + ry_eff) * mh / fh)), 0, mh - 1);
    for (int py = y0; py <= y1; ++py) for (int px = x0; px <= x1; ++px) {
      const float fx = (px + 0.5f) * fw / mw, fy = (py + 0.5f) * fh / mh;
      const float dx = (fx - e.cx) / std::max(rx_eff, 1e-3f);
      const float dy = (fy - e.cy) / std::max(ry_eff, 1e-3f);
      if (dx * dx + dy * dy <= 1.0f)
        (*exclusion)[static_cast<size_t>(py) * mw + px] = 255;
    }
  }
}

FaceEyeBasis ComputeFaceEyeBasis(const float* xy) {
  FaceEyeBasis b;
  if (!xy) return b;
  auto pt = [&](int idx) { return std::pair<float, float>{xy[idx * 2], xy[idx * 2 + 1]}; };
  auto edge_centroid = [&](const Edge* edges, int n) {
    double sx = 0.0, sy = 0.0;
    int cnt = 0;
    auto accum = [&](int id) {
      const float x = xy[id * 2], y = xy[id * 2 + 1];
      if (std::fabs(x) + std::fabs(y) > 1e-4f) {
        sx += x;
        sy += y;
        ++cnt;
      }
    };
    for (int i = 0; i < n; ++i) {
      accum(edges[i].a);
      accum(edges[i].b);
    }
    return std::pair<float, float>{cnt ? static_cast<float>(sx / cnt) : 0.0f,
                                   cnt ? static_cast<float>(sy / cnt) : 0.0f};
  };

  const auto p33 = pt(33), p133 = pt(133), p362 = pt(362), p263 = pt(263);
  const float d_outer = std::hypot(p263.first - p33.first, p263.second - p33.second);
  const float d_inner = std::hypot(p362.first - p133.first, p362.second - p133.second);
  std::pair<float, float> right_c, left_c;
  if (d_outer > 2.0f && d_inner > 0.5f && d_inner < d_outer * 0.9f) {
    right_c = {0.5f * (p33.first + p133.first), 0.5f * (p33.second + p133.second)};
    left_c = {0.5f * (p362.first + p263.first), 0.5f * (p362.second + p263.second)};
  } else {
    right_c = edge_centroid(topology::kRightEyeEdges, topology::kRightEyeEdges_N);
    left_c = edge_centroid(topology::kLeftEyeEdges, topology::kLeftEyeEdges_N);
  }
  const float dx = left_c.first - right_c.first;
  const float dy = left_c.second - right_c.second;
  const float iod = std::hypot(dx, dy);
  if (iod <= 1e-3f) return b;
  b.eye_mid_x = 0.5f * (right_c.first + left_c.first);
  b.eye_mid_y = 0.5f * (right_c.second + left_c.second);
  b.ux = dx / iod;
  b.uy = dy / iod;
  b.vx = -b.uy;
  b.vy = b.ux;
  // Pastikan sumbu v mengarah ke bawah wajah (ke arah mulut/bibir) meski cermin.
  const auto lip_c = edge_centroid(topology::kLipsEdges, topology::kLipsEdges_N);
  const float dot_lip =
      (lip_c.first - b.eye_mid_x) * b.vx + (lip_c.second - b.eye_mid_y) * b.vy;
  if (dot_lip < -1.0f) {
    b.vx = -b.vx;
    b.vy = -b.vy;
  }
  b.iod = iod;
  b.valid = true;
  return b;
}

bool ToFaceNormalizedCoords(const FaceEyeBasis& basis, float x, float y,
                            float* norm_x, float* norm_y) {
  if (!basis.valid || basis.iod <= 1e-3f) return false;
  const float dx = x - basis.eye_mid_x;
  const float dy = y - basis.eye_mid_y;
  if (norm_x) *norm_x = (dx * basis.ux + dy * basis.uy) / basis.iod;
  if (norm_y) *norm_y = (dx * basis.vx + dy * basis.vy) / basis.iod;
  return true;
}

ExclusionShapes BuildExclusionShapes(const float* xy) {
  ExclusionShapes out;
  // Mata: siklus tertutup resmi (16 titik) — iris tak tersedia di model 468,
  // cincin mata menutup seluruh bukaan mata.
  out.loops.push_back(OrderLoopGeneric(topology::kRightEyeEdges,
                                       topology::kRightEyeEdges_N));
  out.loops.push_back(OrderLoopGeneric(topology::kLeftEyeEdges,
                                       topology::kLeftEyeEdges_N));
  // Alis: dua rantai (baris atas + bawah) digabung jarak ujung minimum.
  {
    auto ch = ChainsOf(topology::kRightBrowEdges, topology::kRightBrowEdges_N);
    out.loops.push_back(MergeChainsXY(ch, xy));
    ch = ChainsOf(topology::kLeftBrowEdges, topology::kLeftBrowEdges_N);
    out.loops.push_back(MergeChainsXY(ch, xy));
  }
  // Bibir luar: edge resmi yang kedua ujungnya di set bibir luar → siklus 20.
  {
    std::vector<Edge> outer;
    for (int i = 0; i < topology::kLipsEdges_N; ++i) {
      const Edge& e = topology::kLipsEdges[i];
      if (InLipOuter(e.a) && InLipOuter(e.b)) outer.push_back(e);
    }
    out.loops.push_back(
        OrderLoopGeneric(outer.data(), static_cast<int>(outer.size())));
  }
  // M2m: lubang hidung diturunkan dari titik-titik pembuka lubang hidung
  // resmi di kTessTris ({20,60,99,242} kanan, {250,290,328,462} kiri — bukan
  // sayap hidung 48/278 yang terlalu luar/tinggi), dengan fallback ke model
  // kanonik MediaPipe (±0.15, +0.64) * IOD dalam basis mata bila landmark
  // lubang belum terisi; radius inti ~ 0.035 * IOD.
  {
    const FaceEyeBasis basis = ComputeFaceEyeBasis(xy);
    constexpr int kRightNostrilRing[4] = {20, 60, 99, 242};
    constexpr int kLeftNostrilRing[4] = {250, 290, 328, 462};
    auto try_nostril_center = [&](const int idx[4], float sign_x,
                                  float* cx_out, float* cy_out) {
      if (basis.valid) {
        double sx = 0.0, sy = 0.0;
        for (int i = 0; i < 4; ++i) {
          sx += xy[idx[i] * 2];
          sy += xy[idx[i] * 2 + 1];
        }
        const float cx = static_cast<float>(sx * 0.25);
        const float cy = static_cast<float>(sy * 0.25);
        float nx = 0.0f, ny = 0.0f;
        float max_spread = 0.0f;
        for (int i = 0; i < 4; ++i) {
          max_spread = std::max(
              max_spread, std::hypot(xy[idx[i] * 2] - cx, xy[idx[i] * 2 + 1] - cy));
        }
        if (ToFaceNormalizedCoords(basis, cx, cy, &nx, &ny) &&
            nx * sign_x >= 0.06f && nx * sign_x <= 0.24f &&
            ny >= 0.48f && ny <= 0.80f && max_spread <= 0.16f * basis.iod) {
          *cx_out = cx;
          *cy_out = cy;
          return;
        }
        *cx_out = basis.eye_mid_x +
                  (sign_x * kCanonicalNostrilNormX * basis.ux +
                   kCanonicalNostrilNormY * basis.vx) *
                      basis.iod;
        *cy_out = basis.eye_mid_y +
                  (sign_x * kCanonicalNostrilNormX * basis.uy +
                   kCanonicalNostrilNormY * basis.vy) *
                      basis.iod;
        return;
      }
      // Fallback darurat bila basis mata tak valid (seharusnya tidak terjadi).
      *cx_out = xy[idx[0] * 2];
      *cy_out = xy[idx[0] * 2 + 1];
    };

    const float r = basis.valid ? std::max(1.5f, kNostrilRadiusIodScale * basis.iod)
                                : 1.5f;
    ExclusionEllipse er, el;
    try_nostril_center(kRightNostrilRing, -1.0f, &er.cx, &er.cy);
    er.rx = r;
    er.ry = r;
    try_nostril_center(kLeftNostrilRing, +1.0f, &el.cx, &el.cy);
    el.rx = r;
    el.ry = r;
    out.ellipses.push_back(er);
    out.ellipses.push_back(el);
  }
  return out;
}

namespace {
inline float PixelLumaRgba(const uint8_t* p) {
  return 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
}

void FindDarkBlobInNostrilRoi(const ExclusionEllipse& anchor,
                              const ExclusionEllipse* other_anchor,
                              const uint8_t* rgba, int fw, int fh, int stride,
                              float skin_luma, float iod_px,
                              NostrilRoiInfo* info) {
  *info = NostrilRoiInfo{};
  info->anchor_cx = anchor.cx;
  info->anchor_cy = anchor.cy;
  info->dark_cx = anchor.cx;
  info->dark_cy = anchor.cy;
  info->excl_cx = anchor.cx;
  info->excl_cy = anchor.cy;
  const float roi_r =
      std::max(8.0f, (iod_px > 1.0f ? 0.14f * iod_px : anchor.rx * 4.0f));
  info->roi_radius_px = roi_r;
  if (!rgba || fw <= 0 || fh <= 0 || stride < fw * 4 || skin_luma < 20.0f) {
    return;
  }
  const float dark_thresh = 0.58f * skin_luma;
  const int x0 = std::max(0, static_cast<int>(std::floor(anchor.cx - roi_r)));
  const int x1 = std::min(fw - 1, static_cast<int>(std::ceil(anchor.cx + roi_r)));
  const int y0 = std::max(0, static_cast<int>(std::floor(anchor.cy - roi_r)));
  const int y1 = std::min(fh - 1, static_cast<int>(std::ceil(anchor.cy + roi_r)));
  const float r2 = roi_r * roi_r;

  float min_y = 1e9f;
  int min_px = -1, min_py = -1;
  int dark_n = 0;
  for (int y = y0; y <= y1; ++y) {
    const uint8_t* row = rgba + static_cast<size_t>(y) * stride;
    for (int x = x0; x <= x1; ++x) {
      const float dx = (x + 0.5f) - anchor.cx;
      const float dy = (y + 0.5f) - anchor.cy;
      const float d2 = dx * dx + dy * dy;
      if (d2 > r2) continue;
      if (other_anchor) {
        const float odx = (x + 0.5f) - other_anchor->cx;
        const float ody = (y + 0.5f) - other_anchor->cy;
        if (odx * odx + ody * ody < d2) continue;
      }
      const float lum = PixelLumaRgba(row + static_cast<size_t>(x) * 4);
      if (lum < min_y) {
        min_y = lum;
        min_px = x;
        min_py = y;
      }
      if (lum < dark_thresh) ++dark_n;
    }
  }
  const int min_dark_pixels = (fw >= 640) ? 4 : 1;
  if (min_px < 0 || dark_n < min_dark_pixels || min_y >= dark_thresh) {
    return;
  }

  // Hitung centroid berbobot kuadrat di sekitar piksel tergelap agar inti
  // lubang hidung tetap terkunci meski tepi ROI menyentuh bayangan sayap.
  const float core_r =
      std::min(roi_r, std::max(5.0f, iod_px > 1.0f ? 0.065f * iod_px : anchor.rx * 2.0f));
  const float core_r2 = core_r * core_r;
  const int cx0 = std::max(x0, static_cast<int>(std::floor(min_px + 0.5f - core_r)));
  const int cx1 = std::min(x1, static_cast<int>(std::ceil(min_px + 0.5f + core_r)));
  const int cy0 = std::max(y0, static_cast<int>(std::floor(min_py + 0.5f - core_r)));
  const int cy1 = std::min(y1, static_cast<int>(std::ceil(min_py + 0.5f + core_r)));
  double wx = 0.0, wy = 0.0, wsum = 0.0;
  for (int y = cy0; y <= cy1; ++y) {
    const uint8_t* row = rgba + static_cast<size_t>(y) * stride;
    for (int x = cx0; x <= cx1; ++x) {
      const float dx = (x + 0.5f) - (min_px + 0.5f);
      const float dy = (y + 0.5f) - (min_py + 0.5f);
      if (dx * dx + dy * dy > core_r2) continue;
      const float lum = PixelLumaRgba(row + static_cast<size_t>(x) * 4);
      if (lum >= dark_thresh) continue;
      const float diff = dark_thresh - lum;
      const double w = static_cast<double>(diff) * diff;
      wx += static_cast<double>(x + 0.5f) * w;
      wy += static_cast<double>(y + 0.5f) * w;
      wsum += w;
    }
  }
  if (wsum > 0.0) {
    info->dark_cx = static_cast<float>(wx / wsum);
    info->dark_cy = static_cast<float>(wy / wsum);
    info->found_dark_blob = true;
  }
}
}  // namespace

void RefineNostrilEllipsesFromLuma(const ExclusionShapes& base_shapes,
                                  const uint8_t* rgba, int fw, int fh,
                                  int stride, float skin_luma, float iod_px,
                                  ExclusionShapes* out_shapes,
                                  NostrilRoiInfo out_info[2]) {
  if (!out_shapes) return;
  *out_shapes = base_shapes;
  if (base_shapes.ellipses.empty()) return;
  for (size_t k = 0; k < base_shapes.ellipses.size() && k < 2; ++k) {
    const ExclusionEllipse& anchor = base_shapes.ellipses[k];
    const ExclusionEllipse* other =
        (base_shapes.ellipses.size() >= 2) ? &base_shapes.ellipses[1 - k] : nullptr;
    NostrilRoiInfo info;
    FindDarkBlobInNostrilRoi(anchor, other, rgba, fw, fh, stride, skin_luma,
                             iod_px, &info);
    if (info.found_dark_blob) {
      out_shapes->ellipses[k].cx = info.dark_cx;
      out_shapes->ellipses[k].cy = info.dark_cy;
    }
    info.excl_cx = out_shapes->ellipses[k].cx;
    info.excl_cy = out_shapes->ellipses[k].cy;
    info.offset_px =
        std::hypot(info.excl_cx - info.dark_cx, info.excl_cy - info.dark_cy);
    if (out_info) out_info[k] = info;
  }
}

void MeasureNostrilDarkBlobOffset(const ExclusionShapes& final_shapes,
                                  const uint8_t* rgba, int fw, int fh,
                                  int stride, float skin_luma, float iod_px,
                                  NostrilRoiInfo out_info[2]) {
  if (!out_info) return;
  for (size_t k = 0; k < 2; ++k) {
    out_info[k] = NostrilRoiInfo{};
    if (k >= final_shapes.ellipses.size()) continue;
    const ExclusionEllipse& anchor = final_shapes.ellipses[k];
    const ExclusionEllipse* other =
        (final_shapes.ellipses.size() >= 2) ? &final_shapes.ellipses[1 - k] : nullptr;
    FindDarkBlobInNostrilRoi(anchor, other, rgba, fw, fh, stride, skin_luma,
                             iod_px, &out_info[k]);
    out_info[k].excl_cx = anchor.cx;
    out_info[k].excl_cy = anchor.cy;
    out_info[k].offset_px =
        std::hypot(anchor.cx - out_info[k].dark_cx,
                   anchor.cy - out_info[k].dark_cy);
  }
}

}  // namespace kgcore
