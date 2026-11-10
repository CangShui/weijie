"""翻译：默认走免费在线接口，可在设置里换成任意 OpenAI 兼容的大模型。"""
import html
import json
import re
import urllib.parse
import urllib.request

UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/120.0 Safari/537.36")

CJK_RE = re.compile(r"[\u4e00-\u9fff\u3400-\u4dbf]")
_MAX_CHUNK = 450


def detect(text):
    """返回 (源语言, 目标语言)。"""
    cjk = len(CJK_RE.findall(text or ""))
    letters = len(re.findall(r"[A-Za-z]", text or ""))
    if cjk >= letters:
        return "zh-CN", "en"
    return "en", "zh-CN"


def _chunks(text, limit=_MAX_CHUNK):
    """按行/句切块，保证每块不超过 limit 字符。"""
    parts = []
    buf = ""
    for piece in re.split(r"(?<=\n)", text):
        while len(piece) > limit:
            cut = piece.rfind("。", 0, limit)
            if cut < limit // 3:
                cut = piece.rfind(". ", 0, limit)
            if cut < limit // 3:
                cut = limit
            parts.append(piece[:cut + 1])
            piece = piece[cut + 1:]
        if len(buf) + len(piece) > limit:
            if buf:
                parts.append(buf)
            buf = piece
        else:
            buf += piece
    if buf:
        parts.append(buf)
    return [p for p in parts if p.strip()] or [text]


def _mymemory(text, src, dst):
    out = []
    for chunk in _chunks(text):
        q = urllib.parse.quote(chunk)
        url = (f"https://api.mymemory.translated.net/get?q={q}"
               f"&langpair={src}|{dst}")
        req = urllib.request.Request(url, headers={"User-Agent": UA})
        with urllib.request.urlopen(req, timeout=15) as r:
            data = json.loads(r.read().decode("utf-8", "ignore"))
        rd = data.get("responseData") or {}
        t = rd.get("translatedText") or ""
        if not t:
            raise RuntimeError(data.get("responseDetails") or "翻译服务返回为空")
        out.append(html.unescape(t))
    return "".join(out)


def _llm(text, src, dst, cfg):
    base = (cfg.get("llm_base") or "https://api.deepseek.com/v1").rstrip("/")
    model = cfg.get("llm_model") or "deepseek-chat"
    body = {
        "model": model,
        "temperature": 0.2,
        "messages": [
            {"role": "system",
             "content": "你是一个翻译引擎。只输出译文，不要解释、不要引号、不要加任何前后缀。"
                        f"把用户给的文本翻译成{'英文' if dst == 'en' else '简体中文'}，"
                        "保持原有的换行与分段。"},
            {"role": "user", "content": text},
        ],
    }
    req = urllib.request.Request(
        f"{base}/chat/completions",
        data=json.dumps(body).encode("utf-8"),
        headers={"Content-Type": "application/json",
                 "Authorization": f"Bearer {cfg.get('llm_key', '')}"},
    )
    with urllib.request.urlopen(req, timeout=45) as r:
        data = json.loads(r.read().decode("utf-8", "ignore"))
    try:
        return data["choices"][0]["message"]["content"].strip()
    except Exception:
        raise RuntimeError(f"大模型返回异常: {str(data)[:120]}")


def translate(text, cfg=None):
    """返回 (译文, 源语言, 目标语言, 错误)。"""
    text = (text or "").strip()
    if not text:
        return "", "", "", "没有可翻译的文字"
    cfg = cfg or {}
    src, dst = detect(text)
    try:
        if cfg.get("llm_key"):
            return _llm(text, src, dst, cfg), src, dst, ""
        return _mymemory(text, src, dst), src, dst, ""
    except Exception as e:  # noqa: BLE001
        return "", src, dst, f"{type(e).__name__}: {e}"
