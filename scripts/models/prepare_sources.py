#!/usr/bin/env python3
"""KameraGeulis — extract & verify the MediaPipe model sources.

Reads the two npm tarballs `@mediapipe/face_detection` (BlazeFace, short range)
and `@mediapipe/face_mesh` (468-pt mesh, packed in the WASM solution assets),
verifies every byte against pinned SHA-256, and writes the two .tflite files
that tf2onnx consumes.

Exits non-zero on any mismatch. Nothing here is downloaded — the workflow
does that with pinned sha256 checks before calling this script.

Usage: prepare_sources.py <workdir> <outdir>
"""
import hashlib
import os
import json
import re
import sys
import tarfile
import pathlib

def safe_extractall(tf: tarfile.TarFile, dest: pathlib.Path) -> None:
    dest = dest.resolve()
    for member in tf.getmembers():
        target = (dest / member.name).resolve()
        if not str(target).startswith(str(dest) + os.sep) and target != dest:
            raise SystemExit(f"tarball path traversal blocked: {member.name}")
    tf.extractall(dest)


DET_TGZ = "mediapipe-face_detection-0.4.1646425229.tgz"
MESH_TGZ = "mediapipe-face_mesh-0.4.1633559619.tgz"

# npm tarballs (registry.npmjs.org), pinned by us 2026-11-12.
TARBALL_SHA256 = {
    DET_TGZ: "0860724d3ad0de680dd5da59c0b1333f2330aa4ea969157d0aea5d7c0d6e2a95",
    MESH_TGZ: "a65c9a7509560f29e574c026dc6a667d2154617eb3ba0c287db02350de0c116f",
}

# Model sources (SHA-256, pinned by us 2026-11-12; chain documented in
# docs/LANDMARKS.md §2). Byte-compared after extraction/carving.
TFLITE_SHA256 = {
    "face_detection_short_range.tflite":
        "3bc182eb9f33925d9e58b5c8d59308a760f4adea8f282370e428c51212c26633",
    "face_landmark.tflite":
        "c603fa6149219a3e9487dc9abd7a0c24474c77263273d24868378cdf40aa26d1",
    # Cross-checks (not needed for conversion; they strengthen the chain):
    "face_landmark_with_attention.tflite":
        "883b7411747bac657c30c462d305d312e9dec6adbf8b85e2f5d8d722fca9455d",
    "geometry_pipeline_metadata_landmarks.binarypb":
        "bdbcda96dfcb7da883da124aaa2c55dee49770d934f0fcc71747f8c21bdc75b4",
}


def sha256_file(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_packed_assets_metadata(loader_js: str):
    """Parse loadPackage({...}) out of *_packed_assets_loader.js."""
    m = re.search(r"loadPackage\((\{.*?\})\)\s*;", loader_js, re.DOTALL)
    if not m:
        raise SystemExit("packed-assets metadata not found in loader (format changed?)")
    return json.loads(m.group(1))


def main() -> int:
    workdir = pathlib.Path(sys.argv[1])
    outdir = pathlib.Path(sys.argv[2])
    outdir.mkdir(parents=True, exist_ok=True)

    # 1) verify tarballs
    for name, want in TARBALL_SHA256.items():
        got = sha256_file(workdir / name)
        status = "OK " if got == want else "FAIL"
        print(f"{status} tarball {name} sha256={got}")
        if got != want:
            return 1

    # 2) face_detection package: plain .tflite inside
    det_dir = workdir / "det_pkg"
    mesh_dir = workdir / "mesh_pkg"
    for d in (det_dir, mesh_dir):
        d.mkdir(exist_ok=True)
    with tarfile.open(workdir / DET_TGZ) as tf:
        safe_extractall(tf, det_dir)
    det_src = det_dir / "package" / "face_detection_short_range.tflite"

    # 3) face_mesh package: models are packed in *_packed_assets.data,
    #    offsets listed in *_packed_assets_loader.js
    with tarfile.open(workdir / MESH_TGZ) as tf:
        safe_extractall(tf, mesh_dir)
    pkg = mesh_dir / "package"
    data = (pkg / "face_mesh_solution_packed_assets.data").read_bytes()
    meta = read_packed_assets_metadata(
        (pkg / "face_mesh_solution_packed_assets_loader.js").read_text())
    print("packed assets metadata:",
          ", ".join(f"{pathlib.PurePosixPath(f['filename']).name} "
                    f"[{f['start']}:{f['end']}]" for f in meta["files"]))

    carved = {}
    for f in meta["files"]:
        name = pathlib.PurePosixPath(f["filename"]).name
        start, end = int(f["start"]), int(f["end"])
        if not (0 <= start < end <= len(data)):
            print(f"FAIL packed-assets range for {name}")
            return 1
        carved[name] = data[start:end]

    # 4) verify every model byte we care about
    checks = {
        det_src: None,  # hashed from disk below
        **{outdir / k: v for k, v in TFLITE_SHA256.items()},
    }
    det_bytes = det_src.read_bytes()
    got_det = hashlib.sha256(det_bytes).hexdigest()
    ok = got_det == TFLITE_SHA256["face_detection_short_range.tflite"]
    print(f"{'OK ' if ok else 'FAIL'} face_detection_short_range.tflite "
          f"sha256={got_det}")
    if not ok:
        return 1
    (outdir / "face_detection_short_range.tflite").write_bytes(det_bytes)

    for name, want in TFLITE_SHA256.items():
        if name == "face_detection_short_range.tflite":
            continue
        blob = carved.get(name)
        if blob is None:
            print(f"FAIL {name}: not found in packed assets")
            return 1
        got = hashlib.sha256(blob).hexdigest()
        status = "OK " if got == want else "FAIL"
        print(f"{status} {name} sha256={got}")
        if got != want:
            return 1
    (outdir / "face_landmark.tflite").write_bytes(carved["face_landmark.tflite"])

    print("model sources verified:", ", ".join(sorted(
        p.name for p in outdir.iterdir())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
