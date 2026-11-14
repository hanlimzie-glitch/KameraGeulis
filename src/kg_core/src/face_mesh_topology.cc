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
    const int x0 = std::clamp(static_cast<int>((e.cx - e.rx) * mw / fw), 0, mw - 1);
    const int x1 = std::clamp(static_cast<int>((e.cx + e.rx) * mw / fw), 0, mw - 1);
    const int y0 = std::clamp(static_cast<int>((e.cy - e.ry) * mh / fh), 0, mh - 1);
    const int y1 = std::clamp(static_cast<int>((e.cy + e.ry) * mh / fh), 0, mh - 1);
    for (int py = y0; py <= y1; ++py) for (int px = x0; px <= x1; ++px) {
      const float fx = (px + 0.5f) * fw / mw, fy = (py + 0.5f) * fh / mh;
      const float dx = (fx - e.cx) / std::max(e.rx, 1e-3f);
      const float dy = (fy - e.cy) / std::max(e.ry, 1e-3f);
      if (dx * dx + dy * dy <= 1.0f)
        (*exclusion)[static_cast<size_t>(py) * mw + px] = 255;
    }
  }
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
  // Lubang hidung: elips kecil di antara titik sayap hidung resmi (48/49
  // dan 278/279). FaceMesh 468 tidak memberi kontur lubang hidung tertutup,
  // jadi elips mengambil lebar dari jarak sayap-ke-titik dalam; bentuknya
  // kecil (bukan cakram 75% yang dulu membuat ring besar).
  {
    const std::pair<int, int> nostril_pairs[2] = {{48, 49}, {278, 279}};
    for (const auto& pair : nostril_pairs) {
      const float* a = xy + pair.first * 2;
      const float* b = xy + pair.second * 2;
      ExclusionEllipse e;
      e.cx = 0.5f * (a[0] + b[0]);
      e.cy = 0.5f * (a[1] + b[1]);
      const float width = std::hypot(a[0] - b[0], a[1] - b[1]);
      e.rx = std::max(1.5f, 0.65f * width);
      e.ry = std::max(1.2f, 0.55f * e.rx);
      out.ellipses.push_back(e);
    }
  }
  return out;
}

}  // namespace kgcore
