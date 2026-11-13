"""Compare the native GIF export against independently decoded source frames."""
from pathlib import Path
from datetime import datetime
import sys
from PIL import Image, ImageChops, ImageStat

root = Path(sys.argv[1])
logs = Path(__file__).resolve().parents[2] / "logs"
logs.mkdir(exist_ok=True)
trace_path = root / "trace.txt"
trace = trace_path.read_text() if trace_path.exists() else "gif-verification"


def log(message):
    with (logs / f"gif-verification-{datetime.now():%Y-%m-%d}.log").open("a", encoding="utf-8") as stream:
        stream.write(f"[{datetime.now():%H:%M:%S}] traceId={trace} {message}\n")


def exception_log(kind, value, traceback):
    log(f"独立 GIF 校验失败：{kind.__name__} {value}")
    sys.__excepthook__(kind, value, traceback)


sys.excepthook = exception_log
log(f"独立 GIF 校验请求：目录={root}，使用 Pillow 解码并比对源帧合成结果")
gif = Image.open(root / "mixed.gif")
timeline = [tuple(map(int, line.split())) for line in (root / "timeline.txt").read_text().splitlines()]
assert gif.n_frames == len(timeline), (gif.n_frames, timeline)
assert gif.info.get("loop") == 0
first = Image.open(root / "mixed.gif").convert("RGB")
total = 0
encoded_pixels = 0
for i, (time, duration) in enumerate(timeline):
    gif.seek(i)
    assert gif.info["duration"] == duration, (i, gif.info["duration"], duration)
    total += duration
    bbox = gif.tile[0][1]
    log(f"解码帧={i} 开始={time}ms 时长={duration}ms 编码区域={bbox}")
    encoded_pixels += (bbox[2] - bbox[0]) * (bbox[3] - bbox[1])
    actual = gif.convert("RGB")
    expected = Image.open(root / f"expected-{time}.png").convert("RGB")
    difference = ImageChops.difference(actual, expected)
    assert max(ImageStat.Stat(difference).mean) < 2, (i, ImageStat.Stat(difference).mean)
    # No later frame may alter the static background outside both sticker bounds.
    for box in [(0, 0, 960, 90), (0, 280, 960, 640), (500, 0, 960, 640)]:
        assert ImageChops.difference(actual.crop(box), first.crop(box)).getbbox() is None, (i, box)
assert total == 550, total
assert encoded_pixels < 960 * 640 * gif.n_frames // 3, encoded_pixels
assert (root / "mixed.gif").stat().st_size < sum(
    (root / f"expected-{time}.png").stat().st_size for time, _ in timeline
), "Encoded background is being repeated"
print(f"PASS independent GIF decode: {gif.n_frames} frames, {total}ms, "
      f"{encoded_pixels} encoded pixels versus {960 * 640 * gif.n_frames} full-frame pixels")
log(f"独立 GIF 校验返回：成功，帧={gif.n_frames} 时长={total}ms 编码像素={encoded_pixels}，静态背景未变化")

transforms = root.parent / "text-transform-export"
for scale in (100, 175):
    transformed = Image.open(transforms / f"combined-{scale}.gif")
    assert transformed.n_frames == 2
    for i, time in enumerate((0, 70)):
        transformed.seek(i)
        expected = Image.open(transforms / f"combined-{scale}-{time}.png").convert("RGB")
        actual = transformed.convert("RGB")
        difference = ImageChops.difference(actual, expected)
        assert max(ImageStat.Stat(difference).mean) < 2, (scale, i, ImageStat.Stat(difference).mean)
        content = ImageChops.difference(expected, Image.new("RGB", expected.size, "white")).getbbox()
        assert content and max(ImageStat.Stat(difference.crop(content)).mean) < 2, (scale, i, content)
    log(f"旋转拉伸文字与 GIF 叠加独立校验返回：DPI={scale}% 两帧均与参考合成一致")
print("PASS independent affine-text GIF decode at 100% and 175% DPI")

moving = root.parent / "annotation-moving-export"
for scale in (100, 175):
    moved = Image.open(moving / f"moved-{scale}.gif")
    assert moved.n_frames == 2
    duration = 0
    for i, time in enumerate((0, 60)):
        moved.seek(i)
        duration += moved.info["duration"]
        expected = Image.open(moving / f"moved-{scale}-{time}.png").convert("RGB")
        difference = ImageChops.difference(moved.convert("RGB"), expected)
        assert max(ImageStat.Stat(difference).mean) < 2, (scale, i)
    assert duration == 180, (scale, duration)
    log(f"移动动态贴图独立校验返回：DPI={scale}% 两帧位置与参考合成一致，总时长={duration}ms")
print("PASS independent moved-sticker GIF decode at 100% and 175% DPI")
