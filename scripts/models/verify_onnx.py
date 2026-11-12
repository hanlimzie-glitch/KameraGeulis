#!/usr/bin/env python3
"""KameraGeulis — onnxruntime shape & sanity check for the converted models.

Usage: verify_onnx.py <model_dir>
Checks (exits non-zero on any mismatch):
  * face_detection_short_range.onnx — input `input` [1,128,128,3] f32,
    outputs `regressors` [1,896,16], `classificators` [1,896,1];
  * face_mesh.onnx — input `input_1` [1,192,192,3] f32,
    outputs `conv2d_21` [1,1,1,1404], `conv2d_31` [1,1,1,1];
  * random-input inference produces finite values;
  * prints SHA-256 of both ONNX files (consumed by the release step).
"""
import hashlib
import pathlib
import sys

import numpy as np
import onnxruntime as ort

SPECS = [
    ("face_detection_short_range.onnx", "input", (1, 128, 128, 3),
     [("regressors", (1, 896, 16)), ("classificators", (1, 896, 1))]),
    ("face_mesh.onnx", "input_1", (1, 192, 192, 3),
     [("conv2d_21", (1, 1, 1, 1404)), ("conv2d_31", (1, 1, 1, 1))]),
]


def sha256_file(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    model_dir = pathlib.Path(sys.argv[1])
    ok = True
    for file_name, input_name, input_shape, expected_outputs in SPECS:
        path = model_dir / file_name
        print("=" * 8, file_name)
        so = ort.SessionOptions()
        so.log_severity_level = 3
        try:
            sess = ort.InferenceSession(str(path), sess_options=so,
                                        providers=["CPUExecutionProvider"])
        except Exception as e:  # noqa: BLE001 - report and fail
            print(f"FAIL load: {e}")
            ok = False
            continue
        ins = {i.name: (i.shape, i.type) for i in sess.get_inputs()}
        outs = {o.name: (o.shape, o.type) for o in sess.get_outputs()}
        print("inputs :", ins)
        print("outputs:", outs)
        ok &= ins.get(input_name, (None,))[0] == list(input_shape)
        for name, shape in expected_outputs:
            ok &= outs.get(name, (None,))[0] == list(shape)
        rng = np.random.default_rng(20261112)
        x = rng.standard_normal(input_shape).astype(np.float32)
        try:
            result = sess.run(None, {input_name: x})
            ok &= all(np.isfinite(a).all() for a in result)
        except Exception as e:  # noqa: BLE001
            print(f"FAIL inference: {e}")
            ok = False
    print("=" * 40)
    for file_name, *_ in SPECS:
        path = model_dir / file_name
        if path.is_file():
            print(f"SHA256 {file_name} {sha256_file(path)}")
        else:
            print(f"SHA256 {file_name} MISSING")
            ok = False
    print("RESULT", "OK" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
