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

struct CompactDarkBlob {
  float cx = 0.0f, cy = 0.0f;
  float u_off = 0.0f, v_off = 0.0f;
  float min_lum = 255.0f;
  float mean_lum = 255.0f;
  float rx = 0.0f, ry = 0.0f;
  int area = 0;
};

struct NoseTipSearchFrame {
  float tip_x = 0.0f, tip_y = 0.0f;
  float ux = 1.0f, uy = 0.0f;
  float vx = 0.0f, vy = 1.0f;
  float iod = 100.0f;
  float roi_r = 30.0f;
};

NoseTipSearchFrame BuildNoseTipSearchFrame(const ExclusionShapes& base_shapes,
                                           float iod_px, const float* xy_px,
                                           int fw, int fh) {
  NoseTipSearchFrame sf;
  const float ax0 = base_shapes.ellipses[0].cx;
  const float ay0 = base_shapes.ellipses[0].cy;
  const float ax1 = base_shapes.ellipses.size() >= 2 ? base_shapes.ellipses[1].cx : ax0;
  const float ay1 = base_shapes.ellipses.size() >= 2 ? base_shapes.ellipses[1].cy : ay0;
  const float mid_ax = 0.5f * (ax0 + ax1);
  const float mid_ay = 0.5f * (ay0 + ay1);

  if (xy_px) {
    const FaceEyeBasis basis = ComputeFaceEyeBasis(xy_px);
    if (basis.valid) {
      sf.ux = basis.ux;
      sf.uy = basis.uy;
      sf.vx = basis.vx;
      sf.vy = basis.vy;
      sf.iod = basis.iod;
    }
  }
  if (sf.iod <= 1.0f || !xy_px) {
    const float adx = ax1 - ax0;
    const float ady = ay1 - ay0;
    const float ad = std::hypot(adx, ady);
    if (ad > 2.0f) {
      sf.ux = adx / ad;
      sf.uy = ady / ad;
      sf.vx = -sf.uy;
      sf.vy = sf.ux;
      sf.iod = iod_px > 1.0f ? iod_px : (ad / 0.30f);
    } else {
      sf.iod = iod_px > 1.0f ? iod_px : std::max(40.0f, base_shapes.ellipses[0].rx * 28.0f);
    }
  }
  if (iod_px > 1.0f) sf.iod = iod_px;

  auto valid_tip_pt = [&](int idx, float* x, float* y) -> bool {
    if (!xy_px) return false;
    const float px = xy_px[idx * 2];
    const float py = xy_px[idx * 2 + 1];
    if (std::fabs(px) + std::fabs(py) <= 1e-4f) return false;
    if (px < 0.0f || py < 0.0f || px >= static_cast<float>(fw) ||
        py >= static_cast<float>(fh)) {
      return false;
    }
    if (std::hypot(px - mid_ax, py - mid_ay) > 0.55f * sf.iod) return false;
    *x = px;
    *y = py;
    return true;
  };
  float x1 = 0, y1 = 0, x4 = 0, y4 = 0;
  const bool ok1 = valid_tip_pt(1, &x1, &y1);
  const bool ok4 = valid_tip_pt(4, &x4, &y4);
  if (ok1 && ok4) {
    sf.tip_x = 0.5f * (x1 + x4);
    sf.tip_y = 0.5f * (y1 + y4);
  } else if (ok1) {
    sf.tip_x = x1;
    sf.tip_y = y1;
  } else if (ok4) {
    sf.tip_x = x4;
    sf.tip_y = y4;
  } else {
    sf.tip_x = mid_ax - 0.06f * sf.iod * sf.vx;
    sf.tip_y = mid_ay - 0.06f * sf.iod * sf.vy;
  }

  sf.roi_r = std::max(14.0f, 0.30f * sf.iod);
  return sf;
}

std::vector<CompactDarkBlob> DetectCompactNostrilBlobs(
    const NoseTipSearchFrame& sf, const uint8_t* rgba, int fw, int fh,
    int stride, float skin_luma) {
  std::vector<CompactDarkBlob> blobs;
  if (!rgba || fw <= 0 || fh <= 0 || stride < fw * 4 || skin_luma < 20.0f) {
    return blobs;
  }
  const float dark_thresh = 0.80f * skin_luma;
  const int x0 = std::max(0, static_cast<int>(std::floor(sf.tip_x - sf.roi_r)));
  const int x1 = std::min(fw - 1, static_cast<int>(std::ceil(sf.tip_x + sf.roi_r)));
  const int y0 = std::max(0, static_cast<int>(std::floor(sf.tip_y - sf.roi_r)));
  const int y1 = std::min(fh - 1, static_cast<int>(std::ceil(sf.tip_y + sf.roi_r)));
  const int gw = x1 - x0 + 1;
  const int gh = y1 - y0 + 1;
  if (gw <= 0 || gh <= 0) return blobs;

  const float r2 = sf.roi_r * sf.roi_r;
  const float min_v = -0.04f * sf.iod;
  const float max_v = 0.32f * sf.iod;
  const float max_abs_u = 0.30f * sf.iod;

  std::vector<float> lum_grid(static_cast<size_t>(gw) * gh, 255.0f);
  std::vector<uint8_t> cand_mask(static_cast<size_t>(gw) * gh, 0);
  int total_dark = 0;
  for (int y = y0; y <= y1; ++y) {
    const uint8_t* row = rgba + static_cast<size_t>(y) * stride;
    for (int x = x0; x <= x1; ++x) {
      const float fx = static_cast<float>(x) + 0.5f;
      const float fy = static_cast<float>(y) + 0.5f;
      const float dx = fx - sf.tip_x;
      const float dy = fy - sf.tip_y;
      if (dx * dx + dy * dy > r2) continue;
      const float u_off = dx * sf.ux + dy * sf.uy;
      const float v_off = dx * sf.vx + dy * sf.vy;
      if (v_off < min_v || v_off > max_v || std::fabs(u_off) > max_abs_u) {
        continue;
      }
      const float lum = PixelLumaRgba(row + static_cast<size_t>(x) * 4);
      const size_t gi = static_cast<size_t>(y - y0) * gw + (x - x0);
      lum_grid[gi] = lum;
      if (lum < dark_thresh) {
        cand_mask[gi] = 1;
        ++total_dark;
      }
    }
  }
  const int min_blob_px = (fw >= 640) ? 3 : 1;
  if (total_dark < min_blob_px) return blobs;

  const int max_blob_area =
      std::max(36, static_cast<int>(0.025f * sf.iod * sf.iod));
  const float max_span = std::max(8.0f, 0.17f * sf.iod);

  auto build_blob_from_pixels = [&](const std::vector<int>& pix) {
    if (static_cast<int>(pix.size()) < min_blob_px ||
        static_cast<int>(pix.size()) > max_blob_area) {
      return;
    }
    float umin = 1e9f, umax = -1e9f, vmin = 1e9f, vmax = -1e9f;
    float min_l = 255.0f;
    double lsum = 0.0;
    double wx = 0.0, wy = 0.0, wsum = 0.0;
    for (int gi : pix) {
      const int gx = gi % gw;
      const int gy = gi / gw;
      const float fx = static_cast<float>(x0 + gx) + 0.5f;
      const float fy = static_cast<float>(y0 + gy) + 0.5f;
      const float dx = fx - sf.tip_x;
      const float dy = fy - sf.tip_y;
      const float u_off = dx * sf.ux + dy * sf.uy;
      const float v_off = dx * sf.vx + dy * sf.vy;
      umin = std::min(umin, u_off);
      umax = std::max(umax, u_off);
      vmin = std::min(vmin, v_off);
      vmax = std::max(vmax, v_off);
      const float l = lum_grid[static_cast<size_t>(gi)];
      min_l = std::min(min_l, l);
      lsum += l;
      const float diff = std::max(0.5f, dark_thresh - l);
      const double w = static_cast<double>(diff) * diff;
      wx += static_cast<double>(fx) * w;
      wy += static_cast<double>(fy) * w;
      wsum += w;
    }
    const float span_u = std::max(1.0f, umax - umin + 1.0f);
    const float span_v = std::max(1.0f, vmax - vmin + 1.0f);
    if (span_u > max_span || span_v > max_span || wsum <= 0.0) return;

    CompactDarkBlob b;
    b.cx = static_cast<float>(wx / wsum);
    b.cy = static_cast<float>(wy / wsum);
    const float dx = b.cx - sf.tip_x;
    const float dy = b.cy - sf.tip_y;
    b.u_off = dx * sf.ux + dy * sf.uy;
    b.v_off = dx * sf.vx + dy * sf.vy;
    b.min_lum = min_l;
    b.mean_lum = static_cast<float>(lsum / static_cast<double>(pix.size()));
    const float r_eq = std::sqrt(static_cast<float>(pix.size()) / 3.14159265f);
    b.rx = std::clamp(std::max(r_eq, 0.5f * span_u), 2.0f, 0.08f * sf.iod);
    b.ry = std::clamp(std::max(r_eq, 0.5f * span_v), 2.0f, 0.08f * sf.iod);
    b.area = static_cast<int>(pix.size());
    blobs.push_back(b);
  };

  // Connected components (4-ketetanggaan); jika sebuah komponen melebar
  // (mis. kedua lubang tersambung oleh bayangan kolumela di 0.80x), pecah
  // lewat minima gelap lokal yang terpisah >= 0.08 * IOD.
  std::vector<uint8_t> visited(static_cast<size_t>(gw) * gh, 0);
  std::vector<int> q;
  for (int i = 0; i < gw * gh; ++i) {
    if (!cand_mask[i] || visited[i]) continue;
    q.clear();
    q.push_back(i);
    visited[i] = 1;
    size_t head = 0;
    float umin = 1e9f, umax = -1e9f;
    while (head < q.size()) {
      const int cur = q[head++];
      const int gx = cur % gw;
      const int gy = cur / gw;
      const float dx = (static_cast<float>(x0 + gx) + 0.5f) - sf.tip_x;
      const float dy = (static_cast<float>(y0 + gy) + 0.5f) - sf.tip_y;
      const float u_off = dx * sf.ux + dy * sf.uy;
      umin = std::min(umin, u_off);
      umax = std::max(umax, u_off);
      const int nb[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
      for (const auto& d : nb) {
        const int nx = gx + d[0];
        const int ny = gy + d[1];
        if (nx < 0 || ny < 0 || nx >= gw || ny >= gh) continue;
        const int ni = ny * gw + nx;
        if (cand_mask[ni] && !visited[ni]) {
          visited[ni] = 1;
          q.push_back(ni);
        }
      }
    }

    if (umax - umin <= max_span && static_cast<int>(q.size()) <= max_blob_area) {
      build_blob_from_pixels(q);
    } else if (static_cast<int>(q.size()) >= 2 * min_blob_px) {
      // Pecah komponen lebar: ambil dua benih minima paling gelap berjarak >= 0.08*IOD.
      std::vector<int> sorted_q = q;
      std::sort(sorted_q.begin(), sorted_q.end(), [&](int a, int b) {
        return lum_grid[a] < lum_grid[b];
      });
      std::vector<int> seeds;
      const float min_sep = std::max(5.0f, 0.08f * sf.iod);
      for (int idx : sorted_q) {
        const float px = static_cast<float>(x0 + (idx % gw)) + 0.5f;
        const float py = static_cast<float>(y0 + (idx / gw)) + 0.5f;
        bool far = true;
        for (int s : seeds) {
          const float sx = static_cast<float>(x0 + (s % gw)) + 0.5f;
          const float sy = static_cast<float>(y0 + (s / gw)) + 0.5f;
          if (std::hypot(px - sx, py - sy) < min_sep) {
            far = false;
            break;
          }
        }
        if (far) {
          seeds.push_back(idx);
          if (seeds.size() >= 4) break;
        }
      }
      const float core_r = std::max(4.0f, 0.075f * sf.iod);
      for (int s : seeds) {
        const float sx = static_cast<float>(x0 + (s % gw)) + 0.5f;
        const float sy = static_cast<float>(y0 + (s / gw)) + 0.5f;
        const float local_cut =
            std::min(dark_thresh, lum_grid[s] + 0.14f * skin_luma);
        std::vector<int> sub;
        for (int idx : q) {
          if (lum_grid[idx] > local_cut) continue;
          const float px = static_cast<float>(x0 + (idx % gw)) + 0.5f;
          const float py = static_cast<float>(y0 + (idx / gw)) + 0.5f;
          if (std::hypot(px - sx, py - sy) <= core_r) sub.push_back(idx);
        }
        build_blob_from_pixels(sub);
      }
    }
  }
  return blobs;
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
      std::max(14.0f, (iod_px > 1.0f ? 0.30f * iod_px : anchor.rx * 8.0f));
  info->roi_radius_px = roi_r;
  if (!rgba || fw <= 0 || fh <= 0 || stride < fw * 4 || skin_luma < 20.0f) {
    return;
  }
  const float dark_thresh = 0.80f * skin_luma;
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
  const int min_dark_pixels = (fw >= 640) ? 3 : 1;
  if (min_px < 0 || dark_n < min_dark_pixels || min_y >= dark_thresh) {
    return;
  }
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
    info->anchor_blob_offset_px =
        std::hypot(info->dark_cx - info->anchor_cx, info->dark_cy - info->anchor_cy);
  }
}
}  // namespace

void RefineNostrilEllipsesFromLuma(const ExclusionShapes& base_shapes,
                                  const uint8_t* rgba, int fw, int fh,
                                  int stride, float skin_luma, float iod_px,
                                  ExclusionShapes* out_shapes,
                                  NostrilRoiInfo out_info[2],
                                  const float* xy_px) {
  if (!out_shapes) return;
  *out_shapes = base_shapes;
  if (base_shapes.ellipses.empty()) return;

  const NoseTipSearchFrame sf =
      BuildNoseTipSearchFrame(base_shapes, iod_px, xy_px, fw, fh);
  const std::vector<CompactDarkBlob> blobs =
      DetectCompactNostrilBlobs(sf, rgba, fw, fh, stride, skin_luma);

  int best_r = -1, best_l = -1;
  double best_pair_score = 1e18;
  for (int i = 0; i < static_cast<int>(blobs.size()); ++i) {
    for (int j = 0; j < static_cast<int>(blobs.size()); ++j) {
      if (i == j) continue;
      const auto& br = blobs[i];
      const auto& bl = blobs[j];
      if (br.u_off >= bl.u_off) continue;  // kanan (-u) di kiri kiri (+u)
      const float du = bl.u_off - br.u_off;
      const float dv = std::fabs(bl.v_off - br.v_off);
      const float mid_u = 0.5f * (br.u_off + bl.u_off);
      if (du < 0.10f * sf.iod || du > 0.42f * sf.iod) continue;
      if (dv > 0.14f * sf.iod) continue;
      if (std::fabs(mid_u) > 0.22f * sf.iod) continue;
      if (std::min(br.area, bl.area) >= 3 &&
          std::max(br.area, bl.area) > 4.5f * std::min(br.area, bl.area)) {
        continue;
      }
      const double score =
          (br.min_lum + bl.min_lum) / std::max(20.0f, skin_luma) +
          2.0 * (dv / sf.iod) + 1.5 * std::fabs(du / sf.iod - 0.28f) +
          0.8 * (std::fabs(mid_u) / sf.iod);
      if (score < best_pair_score) {
        best_pair_score = score;
        best_r = i;
        best_l = j;
      }
    }
  }

  bool side_found[2] = {false, false};
  CompactDarkBlob chosen[2]{};
  if (best_r >= 0 && best_l >= 0) {
    side_found[0] = true;
    side_found[1] = true;
    chosen[0] = blobs[best_r];
    chosen[1] = blobs[best_l];
  } else if (!blobs.empty() && base_shapes.ellipses.size() >= 2) {
    // Jika hanya 1 blob cocok, pasangkan ke sisi jangkar terdekat.
    int best_k = -1, best_b = -1;
    float best_d = 1e9f;
    for (int k = 0; k < 2; ++k) {
      const auto& a = base_shapes.ellipses[k];
      for (int bi = 0; bi < static_cast<int>(blobs.size()); ++bi) {
        const float d = std::hypot(blobs[bi].cx - a.cx, blobs[bi].cy - a.cy);
        if (d <= 0.26f * sf.iod && d < best_d) {
          best_d = d;
          best_k = k;
          best_b = bi;
        }
      }
    }
    if (best_k >= 0) {
      side_found[best_k] = true;
      chosen[best_k] = blobs[best_b];
    }
  } else if (!blobs.empty() && base_shapes.ellipses.size() == 1) {
    side_found[0] = true;
    chosen[0] = blobs[0];
  }

  const int total_found =
      (side_found[0] ? 1 : 0) + (side_found[1] ? 1 : 0);
  constexpr float kNostrilDilationPx = 3.0f;
  for (size_t k = 0; k < base_shapes.ellipses.size() && k < 2; ++k) {
    const ExclusionEllipse& anchor = base_shapes.ellipses[k];
    NostrilRoiInfo info;
    info.anchor_cx = anchor.cx;
    info.anchor_cy = anchor.cy;
    info.roi_radius_px = sf.roi_r;
    info.search_cx = sf.tip_x;
    info.search_cy = sf.tip_y;
    info.found_count = total_found;
    info.found_dark_blob = side_found[k];
    if (side_found[k]) {
      out_shapes->ellipses[k].cx = chosen[k].cx;
      out_shapes->ellipses[k].cy = chosen[k].cy;
      out_shapes->ellipses[k].rx =
          std::max(anchor.rx, chosen[k].rx) + kNostrilDilationPx;
      out_shapes->ellipses[k].ry =
          std::max(anchor.ry, chosen[k].ry) + kNostrilDilationPx;
      info.dark_cx = chosen[k].cx;
      info.dark_cy = chosen[k].cy;
    } else {
      info.dark_cx = anchor.cx;
      info.dark_cy = anchor.cy;
    }
    info.excl_cx = out_shapes->ellipses[k].cx;
    info.excl_cy = out_shapes->ellipses[k].cy;
    info.offset_px =
        std::hypot(info.excl_cx - info.dark_cx, info.excl_cy - info.dark_cy);
    info.anchor_blob_offset_px =
        side_found[k]
            ? std::hypot(info.dark_cx - info.anchor_cx,
                         info.dark_cy - info.anchor_cy)
            : 0.0f;
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
