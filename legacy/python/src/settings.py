"""轻量配置存储（%APPDATA%\\JianJie\\settings.json）。"""
import json
import os

_APP_DIR = os.path.join(os.environ.get("APPDATA", os.path.expanduser("~")), "JianJie")
_FILE = os.path.join(_APP_DIR, "settings.json")

MOD_ALT = 0x0001
MOD_CONTROL = 0x0002
MOD_SHIFT = 0x0004
MOD_WIN = 0x0008

# 名称 -> (修饰键, 虚拟键码)
HOTKEY_PRESETS = {
    "Alt+A": (MOD_ALT, 0x41),
    "Ctrl+Alt+A": (MOD_CONTROL | MOD_ALT, 0x41),
    "Ctrl+Shift+A": (MOD_CONTROL | MOD_SHIFT, 0x41),
    "Alt+Z": (MOD_ALT, 0x5A),
    "Ctrl+Alt+Z": (MOD_CONTROL | MOD_ALT, 0x5A),
    "Alt+F9": (MOD_ALT, 0x78),
}

DEFAULTS = {
    "hotkey": "Alt+A",
    "save_dir": "",
    # 翻译：留空 llm_key 时走免费在线接口；填了就走 OpenAI 兼容的大模型
    "llm_base": "https://api.deepseek.com/v1",
    "llm_key": "",
    "llm_model": "deepseek-chat",
}


def load_settings() -> dict:
    data = dict(DEFAULTS)
    try:
        with open(_FILE, "r", encoding="utf-8") as f:
            data.update(json.load(f))
    except Exception:
        pass
    return data


def save_settings(data: dict):
    try:
        os.makedirs(_APP_DIR, exist_ok=True)
        with open(_FILE, "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
    except Exception:
        pass


def resolve_hotkey(name: str):
    return HOTKEY_PRESETS.get(name, HOTKEY_PRESETS["Alt+A"])
