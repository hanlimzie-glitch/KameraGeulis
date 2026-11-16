#!/usr/bin/env python3
"""KameraGeulis M2m — Evaluasi kuantisasi INT8 dinamis untuk segmenter kulit.

Membandingkan model ONNX FP32 vs hasil quantize_dynamic (QUInt8/QInt8) pada
CPUExecutionProvider:
  1) Statistik persentase kelas (latar, rambut, kulit tubuh, kulit wajah,
     pakaian, aksesori);
  2) IoU per kelas dan mean IoU (mIoU) serta kesepakatan piksel argmax;
  3) Latensi inferensi rata-rata & p95 (ms) dengan variasi jumlah thread.

Penggunaan:
  python scripts/models/eval_seg_int8.py <model_fp32.onnx> [--out-int8 out.int8.onnx] [--image test.png ...]
  python scripts/models/eval_seg_int8.py --synthetic  # uji jalur dengan graf CNN sintetis
"""
import argparse
import os
import pathlib
import statistics
import tempfile
import time

import numpy as np
import onnx
import onnxruntime as ort
from onnxruntime.quantization import QuantType, quantize_dynamic

CLASS_NAMES = [
    "latar",
    "rambut",
    "kulit_tubuh",
    "kulit_wajah",
    "pakaian",
    "aksesoris",
]


def softmax(x: np.ndarray) -> np.ndarray:
    e = np.exp(x - x.max(axis=-1, keepdims=True))
    return e / e.sum(axis=-1, keepdims=True)


def run_and_time(sess: ort.InferenceSession, inp_name: str, inputs: list[np.ndarray],
                 warmup: int = 3, repeats: int = 15) -> tuple[list[np.ndarray], list[float]]:
    for _ in range(warmup):
        sess.run(None, {inp_name: inputs[0]})
    times_ms = []
    outputs = []
    for x in inputs:
        outputs.append(sess.run(None, {inp_name: x})[0])
    for r in range(repeats):
        x = inputs[r % len(inputs)]
        t0 = time.perf_counter()
        sess.run(None, {inp_name: x})
        times_ms.append((time.perf_counter() - t0) * 1000.0)
    return outputs, times_ms


def make_synthetic_onnx(path: pathlib.Path) -> None:
    """Bangun graf CNN NHWC->Conv->Relu->Conv [1,256,256,3]->[1,256,256,6] untuk uji."""
    from onnx import TensorProto, helper

    rng = np.random.default_rng(20261116)
    w1 = (rng.standard_normal((32, 3, 3, 3)) * 0.1).astype(np.float32)
    b1 = np.zeros((32,), dtype=np.float32)
    w2 = (rng.standard_normal((6, 32, 3, 3)) * 0.1).astype(np.float32)
    b2 = np.zeros((6,), dtype=np.float32)

    g = helper.make_graph(
        nodes=[
            helper.make_node("Transpose", ["input"], ["x_nchw"], perm=[0, 3, 1, 2]),
            helper.make_node("Conv", ["x_nchw", "w1", "b1"], ["c1"], pads=[1, 1, 1, 1]),
            helper.make_node("Relu", ["c1"], ["r1"]),
            helper.make_node("Conv", ["r1", "w2", "b2"], ["c2"], pads=[1, 1, 1, 1]),
            helper.make_node("Transpose", ["c2"], ["output"], perm=[0, 2, 3, 1]),
        ],
        name="SyntheticSeg",
        inputs=[helper.make_tensor_value_info("input", TensorProto.FLOAT, [1, 256, 256, 3])],
        outputs=[helper.make_tensor_value_info("output", TensorProto.FLOAT, [1, 256, 256, 6])],
        initializer=[
            helper.make_tensor("w1", TensorProto.FLOAT, w1.shape, w1.flatten().tolist()),
            helper.make_tensor("b1", TensorProto.FLOAT, b1.shape, b1.tolist()),
            helper.make_tensor("w2", TensorProto.FLOAT, w2.shape, w2.flatten().tolist()),
            helper.make_tensor("b2", TensorProto.FLOAT, b2.shape, b2.tolist()),
        ],
    )
    m = helper.make_model(g, opset_imports=[helper.make_opsetid("", 17)])
    m.ir_version = 9
    onnx.checker.check_model(m)
    onnx.save(m, str(path))


def load_test_inputs(image_paths: list[str], count: int = 4) -> list[np.ndarray]:
    if not image_paths:
        rng = np.random.default_rng(20261116)
        # Pola wajah/kulit sintetis + gradien dalam rentang [-1, 1]
        xs = []
        yy, xx = np.mgrid[0:256, 0:256].astype(np.float32)
        for k in range(count):
            img = np.zeros((1, 256, 256, 3), dtype=np.float32)
            cx, cy = 128 + (k - 1.5) * 14, 118 + (k % 2) * 8
            face = ((xx - cx) ** 2 + (yy - cy) ** 2) < (44 ** 2)
            img[0, ..., 0] = np.where(face, 0.55, -0.5)
            img[0, ..., 1] = np.where(face, 0.25, -0.45)
            img[0, ..., 2] = np.where(face, 0.05, -0.4)
            img += rng.normal(0.0, 0.08, size=img.shape).astype(np.float32)
            xs.append(np.clip(img, -1.0, 1.0))
        return xs
    from PIL import Image

    xs = []
    for p in image_paths:
        im = Image.open(p).convert("RGB").resize((256, 256), Image.BILINEAR)
        arr = np.asarray(im, dtype=np.float32) / 127.5 - 1.0
        xs.append(arr[None, ...])
    return xs


def evaluate(fp32_path: pathlib.Path, int8_path: pathlib.Path,
             inputs: list[np.ndarray], threads: int) -> int:
    quantize_dynamic(
        model_input=str(fp32_path),
        model_output=str(int8_path),
        weight_type=QuantType.QUInt8,
    )
    print(f"FP32 size: {fp32_path.stat().st_size / 1e6:.2f} MB | "
          f"INT8 size: {int8_path.stat().st_size / 1e6:.2f} MB")

    def make_sess(p: pathlib.Path) -> ort.InferenceSession:
        so = ort.SessionOptions()
        so.intra_op_num_threads = threads
        so.inter_op_num_threads = 1
        so.execution_mode = ort.ExecutionMode.ORT_SEQUENTIAL
        so.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
        return ort.InferenceSession(str(p), sess_options=so,
                                    providers=["CPUExecutionProvider"])

    s_fp32 = make_sess(fp32_path)
    s_int8 = make_sess(int8_path)
    inp_name = s_fp32.get_inputs()[0].name

    out_fp32, t_fp32 = run_and_time(s_fp32, inp_name, inputs)
    out_int8, t_int8 = run_and_time(s_int8, inp_name, inputs)

    am_fp32 = np.concatenate([y.argmax(axis=-1).reshape(-1) for y in out_fp32])
    am_int8 = np.concatenate([y.argmax(axis=-1).reshape(-1) for y in out_int8])
    agree = float((am_fp32 == am_int8).mean() * 100.0)

    print(f"\nThreads={threads} | FP32 mean={statistics.mean(t_fp32):.2f} ms "
          f"(p95={np.percentile(t_fp32, 95):.2f} ms) | "
          f"INT8 mean={statistics.mean(t_int8):.2f} ms "
          f"(p95={np.percentile(t_int8, 95):.2f} ms)")
    print(f"Argmax pixel agreement: {agree:.2f}%")
    print("\nKelas        | FP32 % | INT8 % | IoU")
    print("-------------+--------+--------+------")
    ious = []
    for c, name in enumerate(CLASS_NAMES):
        m0 = am_fp32 == c
        m1 = am_int8 == c
        inter = int(np.logical_and(m0, m1).sum())
        union = int(np.logical_or(m0, m1).sum())
        iou = (inter / union) if union > 0 else 1.0
        if union > 0:
            ious.append(iou)
        p0 = float(m0.mean() * 100.0)
        p1 = float(m1.mean() * 100.0)
        print(f"{name:12s} | {p0:5.2f}% | {p1:5.2f}% | {iou:.4f}")
    print(f"Mean IoU (kelas aktif): {statistics.mean(ious):.4f}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("fp32", nargs="?", help="Path ke selfie_multiclass_256x256.onnx")
    ap.add_argument("--out-int8", help="Path output .int8.onnx")
    ap.add_argument("--image", nargs="*", default=[], help="Gambar uji opsional")
    ap.add_argument("--threads", type=int, default=max(2, min(6, (os.cpu_count() or 4) // 2)))
    ap.add_argument("--synthetic", action="store_true", help="Uji dengan model sintetis")
    args = ap.parse_args()

    inputs = load_test_inputs(args.image)
    if args.synthetic:
        with tempfile.TemporaryDirectory() as td:
            fp32 = pathlib.Path(td) / "syn_fp32.onnx"
            int8 = pathlib.Path(td) / "syn_int8.onnx"
            make_synthetic_onnx(fp32)
            return evaluate(fp32, int8, inputs, args.threads)

    if not args.fp32:
        ap.error("berikan path model fp32 atau --synthetic")
    fp32 = pathlib.Path(args.fp32)
    int8 = pathlib.Path(args.out_int8) if args.out_int8 else fp32.with_suffix(".int8.onnx")
    return evaluate(fp32, int8, inputs, args.threads)


if __name__ == "__main__":
    raise SystemExit(main())
