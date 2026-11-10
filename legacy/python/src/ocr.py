"""文字提取：封装 Windows 自带的 Windows.Media.Ocr 引擎。

注意：Qt 在主线程以 STA 初始化 COM，WinRT 的异步解码在同一线程会崩溃，
因此 OCR 必须放在独立线程里执行（线程内由 pywinrt 自行初始化公寓）。
"""
import asyncio
import re
import threading

CJK = r"\u3400-\u4dbf\u4e00-\u9fff\uf900-\ufaff"
_CJK_PUNCT = "，。！？、；：（）《》【】…—·“”‘’"


def available_languages():
    try:
        from winrt.windows.media.ocr import OcrEngine
        return [l.language_tag for l in OcrEngine.available_recognizer_languages]
    except Exception:
        return []


def pick_language(preferred=None):
    langs = available_languages()
    if preferred and preferred in langs:
        return preferred
    for tag in ("zh-Hans-CN", "zh-CN", "en-US"):
        if tag in langs:
            return tag
    return langs[0] if langs else None


def tidy(text: str) -> str:
    """Windows OCR 会把每个汉字当成独立的词，用空格隔开，这里还原成连续中文。"""
    if not text:
        return ""
    t = text
    # 汉字/中文标点之间的空格去掉
    t = re.sub(rf"(?<=[{CJK}{_CJK_PUNCT}])[ \t]+(?=[{CJK}{_CJK_PUNCT}])", "", t)
    # 中文标点前的空格去掉
    t = re.sub(rf"[ \t]+(?=[{_CJK_PUNCT}])", "", t)
    # 中文标点后的空格去掉
    t = re.sub(rf"(?<=[{_CJK_PUNCT}])[ \t]+", "", t)
    # OCR 常见的全角句点/中点误判
    t = t.replace("．", "·")
    return t.strip()


async def _recognize_async(png: bytes, lang: str):
    from winrt.windows.globalization import Language
    from winrt.windows.graphics.imaging import BitmapDecoder
    from winrt.windows.media.ocr import OcrEngine
    from winrt.windows.storage.streams import DataWriter, InMemoryRandomAccessStream

    stream = InMemoryRandomAccessStream()
    writer = DataWriter(stream)
    writer.write_bytes(png)
    await writer.store_async()
    writer.detach_stream()
    stream.seek(0)

    decoder = await BitmapDecoder.create_async(stream)
    bitmap = await decoder.get_software_bitmap_async()

    engine = OcrEngine.try_create_from_language(Language(lang))
    if engine is None:
        engine = OcrEngine.try_create_from_user_profile_languages()
    if engine is None:
        raise RuntimeError("系统未安装可用的 OCR 语言包")

    result = await engine.recognize_async(bitmap)
    return [line.text for line in result.lines]


def recognize(png: bytes, lang=None, timeout=25.0):
    """在独立线程里做 OCR，返回 (文本, 错误)。"""
    lang = pick_language(lang)
    if not lang:
        return "", "系统未安装 OCR 语言包（设置 → 时间和语言 → 语言 → 中文 → 可选功能）"

    box = {}

    def _worker():
        try:
            box["lines"] = asyncio.run(_recognize_async(png, lang))
        except BaseException as e:  # noqa: BLE001
            box["err"] = f"{type(e).__name__}: {e}"

    t = threading.Thread(target=_worker, daemon=True)
    t.start()
    t.join(timeout)
    if t.is_alive():
        return "", "识别超时"
    if "err" in box:
        return "", box["err"]
    lines = box.get("lines") or []
    return tidy("\n".join(lines)), ""
