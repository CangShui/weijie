import os
import json
import sys
import time
import math
from datetime import datetime
from pathlib import Path

import numpy as np


ROOT = Path(__file__).resolve().parent
MODELS = Path(os.environ.get("JJ_OCR_MODELS", str(ROOT / "models")))
CACHE = Path(os.environ.get("JJ_OV_CACHE", str(ROOT / "ov-cache")))
DIAGNOSTICS = Path(os.environ.get("JJ_OCR_DIAGNOSTICS", str(
    Path(os.environ.get("LOCALAPPDATA", str(ROOT))) / "JianJie" / "ocr-diagnostics.jsonl"
)))
SESSIONS = []


def diagnostic(event, **fields):
    # Never record screenshot pixels, file paths, recognized text or API keys.
    record = dict(time=datetime.now().astimezone().isoformat(), pid=os.getpid(),
                  event=event, **fields)
    try:
        DIAGNOSTICS.parent.mkdir(parents=True, exist_ok=True)
        with DIAGNOSTICS.open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(record, ensure_ascii=False) + "\n")
    except OSError:
        pass  # Diagnostics must not break recognition.


def patch_openvino(device: str):
    import openvino as ov
    from omegaconf import DictConfig
    from rapidocr.inference_engine.base import FileInfo, InferSession
    from rapidocr.inference_engine.openvino.device_config import CPUConfig

    class DeviceOpenVINOInferSession(InferSession):
        def __init__(self, cfg: DictConfig):
            started = time.perf_counter()
            core = ov.Core()
            CACHE.mkdir(exist_ok=True)
            core.set_property({"CACHE_DIR": str(CACHE)})
            model_root = Path(cfg.get("model_root_dir", None) or MODELS)
            model_info = self.get_model_url(
                FileInfo(
                    engine_type=cfg.engine_type,
                    ocr_version=cfg.ocr_version,
                    task_type=cfg.task_type,
                    lang_type=cfg.lang_type,
                    model_type=cfg.model_type,
                )
            )
            model_path = model_root / Path(model_info["model_dir"]).name
            self._verify_model(model_path)
            if device == "CPU":
                core.set_property("CPU", CPUConfig(cfg.get("engine_cfg", {})).get_config())
            self.model = core.read_model(model_path)
            compile_config = {}
            if device == "GPU":
                compile_config = {
                    "PERFORMANCE_HINT": "THROUGHPUT",
                }
            self.compiled = core.compile_model(
                model=self.model, device_name=device, config=compile_config or None
            )
            self.session = self.compiled.create_infer_request()
            self.task = str(getattr(cfg.task_type, "value", cfg.task_type))
            self.device = device
            self.executed = self.compiled.get_property("EXECUTION_DEVICES")
            self.calls = []
            SESSIONS.append(self)
            diagnostic("model_compile", task=self.task, requested=device,
                       execution_devices=self.executed,
                       device_name=core.get_property(device, "FULL_DEVICE_NAME"),
                       elapsed_ms=round((time.perf_counter() - started) * 1000, 1))

        def __call__(self, input_content: np.ndarray) -> np.ndarray:
            started = time.perf_counter()
            self.session.infer(inputs=[input_content])
            self.calls.append(dict(shape=list(input_content.shape),
                elapsed_ms=round((time.perf_counter() - started) * 1000, 1)))
            return self.session.get_output_tensor().data

        def have_key(self, key: str = "character") -> bool:
            try:
                self.get_character_list(key)
                return True
            except Exception:
                return False

        def get_character_list(self, key: str = "character"):
            rt_info = self.model.get_rt_info()
            value = rt_info["framework"]
            return getattr(value[key], "value", None).splitlines()

    import rapidocr.inference_engine.openvino as ov_mod
    ov_mod.OpenVINOInferSession = DeviceOpenVINOInferSession


def make_ocr(device: str):
    from rapidocr import EngineType, RapidOCR
    import rapidocr.main as rapid_main
    from rapidocr.ch_ppocr_rec.main import TextRecognizer as BaseTextRecognizer
    from rapidocr.ch_ppocr_rec.typings import TextRecInput, TextRecOutput

    class FixedTextRecognizer(BaseTextRecognizer):
        """Fixed [8,3,48,640] recognition shape: one GPU compile, no shape JIT."""
        def resize_norm_img(self, img, max_wh_ratio=None):
            import cv2
            height, width = 48, 640
            h, w = img.shape[:2]
            resized_w = min(width, int(math.ceil(height * (w / float(h)))))
            resized = cv2.resize(img, (max(1, resized_w), height)).astype("float32")
            resized = resized.transpose((2, 0, 1)) / 255.0
            resized -= 0.5
            resized /= 0.5
            padded = np.zeros((3, height, width), dtype=np.float32)
            padded[:, :, :resized_w] = resized
            return padded

        def __call__(self, args: TextRecInput) -> TextRecOutput:
            imgs = [args.img] if isinstance(args.img, np.ndarray) else list(args.img)
            if not imgs:
                return TextRecOutput([], (), [], (), 0.0)
            blank = np.full_like(imgs[0], 255)
            batch = max(1, int(self.rec_batch_num))
            padded_count = int(math.ceil(len(imgs) / batch) * batch)
            padded = imgs + [blank] * (padded_count - len(imgs))
            texts, scores = [], []
            started = time.perf_counter()
            for begin in range(0, padded_count, batch):
                group = padded[begin:begin + batch]
                norm = np.concatenate([self.resize_norm_img(x)[np.newaxis, :]
                                       for x in group]).astype(np.float32)
                preds = self.session(norm)
                line_results, _ = self.postprocess_op(
                    preds, False,
                    wh_ratio_list=[x.shape[1] / float(x.shape[0]) for x in group],
                    max_wh_ratio=640 / 48.0)
                for text, score in line_results:
                    texts.append(text)
                    scores.append(score)
            texts = tuple(texts[:len(imgs)])
            scores = list(scores[:len(imgs)])
            elapsed = time.perf_counter() - started
            return TextRecOutput(imgs, texts, scores, tuple(() for _ in imgs), elapsed)

    rapid_main.TextRecognizer = FixedTextRecognizer

    patch_openvino(device)
    params = {
        "Global.log_level": "error",
        "Global.model_root_dir": str(MODELS),
        "Global.use_preprocess_img": True,
        "Det.limit_side_len": 736,
        "Det.engine_type": EngineType.OPENVINO,
        "Rec.engine_type": EngineType.OPENVINO,
        "Cls.engine_type": EngineType.OPENVINO,
        "EngineConfig.openvino.inference_num_threads": 8,
        "EngineConfig.openvino.performance_hint": "LATENCY",
        "Rec.rec_batch_num": 8,
    }
    return RapidOCR(params=params)


ENGINES = {}


def get_engine(device: str):
    if device not in ENGINES:
        ENGINES[device] = make_ocr(device)
    return ENGINES[device]


def read_line(stream):
    line = stream.readline()
    if not line:
        return None
    return line.decode("utf-8", "replace").rstrip("\r\n")


def write_response(status: str, payload: str):
    data = payload.encode("utf-8")
    out = sys.stdout.buffer
    out.write(f"{status} {len(data)}\n".encode("ascii"))
    out.write(data)
    out.write(b"\n")
    out.flush()


def pad_to_fixed_canvas(img):
    """Letterbox to one of two fixed detection shapes, preserving text scale."""
    import cv2
    h, w = img.shape[:2]
    target_w, target_h = (1312, 736) if w >= h else (736, 1312)
    scale = min(1.0, target_w / float(w), target_h / float(h))
    new_w, new_h = max(1, int(round(w * scale))), max(1, int(round(h * scale)))
    if (new_w, new_h) != (w, h):
        img = cv2.resize(img, (new_w, new_h), interpolation=cv2.INTER_AREA)
    canvas = np.zeros((target_h, target_w, 3), dtype=np.uint8)
    canvas[:new_h, :new_w] = img
    return canvas


def main():
    import cv2

    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    print("READY 1", flush=True)
    diagnostic("worker_ready")
    while True:
        device = read_line(sys.stdin.buffer)
        if device is None or device == "quit":
            break
        path = read_line(sys.stdin.buffer)
        if path is None:
            break
        t0 = time.perf_counter()
        try:
            img = cv2.imdecode(np.fromfile(path, dtype=np.uint8), cv2.IMREAD_COLOR)
            if img is None:
                raise RuntimeError("无法读取图片")
            img = pad_to_fixed_canvas(img)
            chosen = device
            if device == "auto":
                # Pixel count alone does not predict a GPU win. Prefer CPU
                # latency in auto mode; explicit GPU never silently falls back.
                chosen = "CPU"
            if chosen not in ("CPU", "GPU"):
                raise ValueError("Unknown OCR device")
            reused = chosen in ENGINES
            for session in SESSIONS:
                session.calls.clear()
            diagnostic("request_start", requested=device, selected=chosen,
                       image_wh=[int(img.shape[1]), int(img.shape[0])], reused=reused)
            engine = get_engine(chosen)
            result = engine(img)
            texts = list(getattr(result, "txts", None) or ())
            text = "\n".join(t.strip() for t in texts if t and t.strip())
            elapsed = time.perf_counter() - t0
            print(f"OCR {chosen} {elapsed:.3f}s lines={len(texts)}", file=sys.stderr, flush=True)
            diagnostic("request_end", requested=device, selected=chosen, reused=reused,
                       total_ms=round(elapsed * 1000, 1), lines=len(texts),
                       pipeline_seconds=getattr(result, "elapse_list", None),
                       stages=[dict(task=session.task, execution_devices=session.executed,
                                    calls=session.calls)
                               for session in SESSIONS if session.calls])
            write_response("OK", text)
        except Exception as exc:
            print(f"OCR error: {exc}", file=sys.stderr, flush=True)
            diagnostic("request_error", error_type=type(exc).__name__,
                       elapsed_ms=round((time.perf_counter()-t0) * 1000, 1))
            write_response("ERR", str(exc))


if __name__ == "__main__":
    main()
