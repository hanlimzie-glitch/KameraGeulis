#!/usr/bin/env python3
"""M2k: generate face_mesh_topology_data.h dari face_mesh_connections.py resmi
MediaPipe (Apache-2.0). Sumber di-fetch sekali; output di-commit (data statis).
Jalankan: python3 scripts/topology/gen_face_topology.py /tmp/fmc.py
"""
import re, sys, ast

src = open(sys.argv[1], encoding='utf-8').read()

def grab(name):
    m = re.search(rf'{name} = frozenset\(\[(.*?)\]\)', src, re.S)
    assert m, name
    return [tuple(int(x) for x in p) for p in re.findall(r'\((\d+),\s*(\d+)\)', m.group(1))]

tess = grab('FACEMESH_TESSELATION')
lips = grab('FACEMESH_LIPS')
reye = grab('FACEMESH_RIGHT_EYE')
leye = grab('FACEMESH_LEFT_EYE')
rbrow = grab('FACEMESH_RIGHT_EYEBROW')
lbrow = grab('FACEMESH_LEFT_EYEBROW')
oval = grab('FACEMESH_FACE_OVAL')

# Triangulasi: grup 3 edge berturutan pada literal resmi = 1 segitiga
# (tervalidasi: semua grup cycle, pemakaian edge <=2, 852 segitiga).
tri_list = []
for i in range(0, len(tess) - 2, 3):
    group = tess[i:i + 3]
    edges = {tuple(sorted(e)) for e in group}
    vertices = {v for e in group for v in e}
    if len(vertices) == 3 and len(edges) == 3:
        a, b, c = sorted(vertices)
        if {(a, b), (a, c), (b, c)} == edges:
            tri_list.append((a, b, c))
tris = set(tri_list)
assert len(tris) == len(tri_list) == 852, len(tris)
from collections import Counter
ec = Counter()
for t in tris:
    for e in [(t[0], t[1]), (t[1], t[2]), (t[0], t[2])]:
        ec[e] += 1
assert max(ec.values()) <= 2, 'bukan triangulasi manifold'
assert max(max(t) for t in tris) < 468, 'indeks >=468 (iris) harus terbuang'
print(f'triangles={len(tris)} manifold=OK max_idx={max(max(t) for t in tris)}')

def edges_block(name, edges):
    body = ', '.join(f'{{{a},{b}}}' for a, b in edges)
    return f'inline constexpr Edge {name}[] = {{{body}}};\ninline constexpr int {name}_N = {len(edges)};'

tris_sorted = sorted(tris)
tri_body = ', '.join(f'{{{a},{b},{c}}}' for a, b, c in tris_sorted)

hdr = f'''#pragma once
// DIHASILKAN oleh scripts/topology/gen_face_topology.py dari
// mediapipe/python/solutions/face_mesh_connections.py (MediaPipe, Apache-2.0,
// (c) Google LLC) - M2k. Jangan edit manual.
#include <cstdint>
namespace kgcore {{
namespace topology {{
struct Edge {{ uint16_t a, b; }};
struct Tri {{ uint16_t a, b, c; }};
{edges_block('kLipsEdges', lips)}
{edges_block('kRightEyeEdges', reye)}
{edges_block('kLeftEyeEdges', leye)}
{edges_block('kRightBrowEdges', rbrow)}
{edges_block('kLeftBrowEdges', lbrow)}
{edges_block('kOvalEdges', oval)}
inline constexpr Tri kTessTris[] = {{{tri_body}}};
inline constexpr int kTessTris_N = {len(tris_sorted)};
}}  // namespace topology
}}  // namespace kgcore
'''
out = 'src/kg_core/include/kgcore/face_mesh_topology_data.h'
open('/home/user/KameraGeulis/' + out, 'w', encoding='utf-8').write(hdr)
print('wrote', out, len(hdr), 'bytes;', len(tris_sorted), 'triangles')
