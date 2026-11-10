"""复现「提取文字」结果面板的正文重影/重叠问题。

打开一个已知文字的记事本窗口，截取选区并点击工具条上的 OCR 按钮，
然后把整屏截图保存到 Release/verification/ocr-repro.png。
"""
import ctypes
import json
import os
import re
import subprocess
import sys
import time

from PIL import ImageGrab

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tbgeom import TOOLS, cell_x, center, width as tb_width

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXE = os.path.join(ROOT, "Release", "weijie", "微截.exe")
LOG = os.path.join(os.environ["LOCALAPPDATA"], "weijie", "app.log")
OUT = os.path.join(ROOT, "Release", "verification")
TXT = os.path.join(OUT, "ocr-repro-input.txt")

u32 = ctypes.windll.user32


def log_text():
    try:
        with open(LOG, "r", encoding="utf-8", errors="ignore") as f:
            return f.read()
    except FileNotFoundError:
        return ""


def hotkey():
    u32.keybd_event(0x75, 0, 0, 0)  # F6
    time.sleep(0.05)
    u32.keybd_event(0x75, 0, 2, 0)


def drag(x0, y0, x1, y1, steps=14):
    u32.SetCursorPos(x0, y0)
    time.sleep(0.15)
    u32.mouse_event(0x0002, 0, 0, 0, 0)
    for i in range(1, steps + 1):
        u32.SetCursorPos(x0 + (x1 - x0) * i // steps, y0 + (y1 - y0) * i // steps)
        time.sleep(0.02)
    time.sleep(0.15)
    u32.mouse_event(0x0004, 0, 0, 0, 0)
    time.sleep(0.35)


def click(x, y):
    u32.SetCursorPos(x, y)
    time.sleep(0.15)
    u32.mouse_event(0x0002, 0, 0, 0, 0)
    time.sleep(0.08)
    u32.mouse_event(0x0004, 0, 0, 0, 0)
    time.sleep(0.3)


def find_window(title):
    found = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def cb(hwnd, _):
        n = u32.GetWindowTextLengthW(hwnd)
        buf = ctypes.create_unicode_buffer(n + 1)
        u32.GetWindowTextW(hwnd, buf, n + 1)
        if title in buf.value and u32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True

    u32.EnumWindows(cb, 0)
    return found[0] if found else None


def find_overlay_edit():
    overlay = u32.FindWindowW("weijieOverlay", None)
    if not overlay:
        return None
    found = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def cb(hwnd, _):
        buf = ctypes.create_unicode_buffer(64)
        u32.GetClassNameW(hwnd, buf, 64)
        if buf.value.lower() == "edit":
            found.append(hwnd)
        return True

    u32.EnumChildWindows(overlay, cb, 0)
    return found[0] if found else None


def last_layout():
    for line in reversed(log_text().splitlines()):
        if "布局 面板" in line:
            m = re.search(
                r"面板\((-?\d+),(-?\d+) (\d+)x(\d+)\) 工具条\((-?\d+),(-?\d+) (\d+)x(\d+)\)",
                line,
            )
            if m:
                v = [int(x) for x in m.groups()]
                return {"panel": v[0:4], "tb": v[4:8]}
    return None


def main():
    dark = "--dark" in sys.argv
    suffix = "-dark" if dark else ""
    try:
        with open(os.path.join(os.environ["APPDATA"], "weijie", "settings.json"),
                  encoding="utf-8-sig") as f:
            settings = json.load(f)
    except (OSError, ValueError):
        settings = {}
    translate_on = bool(settings.get("translate_enabled", True))
    ocr_on = bool(settings.get("ocr_enabled", True))
    os.makedirs(OUT, exist_ok=True)
    if not dark:
        with open(TXT, "w", encoding="utf-8") as f:
            for i in range(1, 41):
                f.write(f"第{i:02d}行 提取文本会造成文本重叠 测试编号{i * 137:04d}\n")

    subprocess.run(["taskkill", "/F", "/IM", "微截.exe"], capture_output=True)
    subprocess.run(["taskkill", "/F", "/IM", "notepad.exe"], capture_output=True)
    subprocess.run(["taskkill", "/F", "/IM", "python.exe", "/FI",
                    "WINDOWTITLE eq OCR Dark Text*"], capture_output=True)
    time.sleep(0.5)

    if dark:
        helper = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dark_text_window.py")
        np = subprocess.Popen([sys.executable, helper])
        time.sleep(1.5)
        hwnd = find_window("OCR Dark Text")
        if hwnd:
            u32.SetWindowPos(hwnd, 0, 80, 40, 1300, 920, 0x0040)
            time.sleep(0.5)
    else:
        np = subprocess.Popen(["notepad.exe", TXT])
        time.sleep(1.2)
        hwnd = find_window("ocr-repro-input")
        if hwnd:
            u32.SetWindowPos(hwnd, 0, 100, 100, 1100, 620, 0x0040)
            time.sleep(0.5)

    proc = subprocess.Popen([EXE])
    time.sleep(1.8)
    hotkey()
    time.sleep(1.0)

    if dark:
        x0, y0, x1, y1 = 110, 60, 1250, 900
    else:
        x0, y0, x1, y1 = 130, 150, 1000, 520
    drag(x0, y0, x1, y1)
    time.sleep(0.6)

    lay = last_layout()
    print("layout:", lay)
    tb_x = (x0 + x1) // 2 - tb_width(translate_on, ocr_on) // 2
    tb_y = y1 + 8
    ox, oy = center("ocr", tb_x, tb_y, translate_on, ocr_on)
    print("ocr click:", ox, oy)
    before = log_text().count("PP-OCR completed")
    click(ox, oy)
    deadline = time.time() + 90
    while time.time() < deadline and log_text().count("PP-OCR completed") == before:
        time.sleep(0.5)
    time.sleep(0.5)

    img = ImageGrab.grab()
    img.save(os.path.join(OUT, f"ocr-repro{suffix}.png"))

    # 在面板正文上滚轮，验证滚屏后不会留下旧文字残影。
    edit = find_overlay_edit()
    print("result edit hwnd:", edit,
          "text length:", u32.GetWindowTextLengthW(edit) if edit else 0,
          "scroll pos:", u32.GetScrollPos(edit, 2) if edit else -1)
    lay2 = last_layout()
    px, py, pw, ph = lay2["panel"] if lay2 else (910, 120, 340, 360)
    u32.SetCursorPos(px + pw // 2, py + ph // 2)
    time.sleep(0.2)
    if edit:
        # 用户实际是在结果 EDIT 的滚动条上来回拖动。这里直接发
        # WM_VSCROLL，覆盖同一套滚动/擦除绘制路径，且不受焦点影响。
        for _ in range(8):
            for _ in range(24):
                u32.SendMessageW(edit, 0x0115, 1, 0)   # SB_LINEDOWN
            for _ in range(24):
                u32.SendMessageW(edit, 0x0115, 0, 0)   # SB_LINEUP
            for _ in range(6):
                u32.SendMessageW(edit, 0x0115, 3, 0)   # SB_PAGEDOWN
            for _ in range(6):
                u32.SendMessageW(edit, 0x0115, 2, 0)   # SB_PAGEUP
    print("scroll pos after:", u32.GetScrollPos(edit, 2) if edit else -1)
    time.sleep(0.8)
    ImageGrab.grab().save(os.path.join(OUT, f"ocr-repro{suffix}-scrolled.png"))
    print(f"saved ocr-repro{suffix}.png / ocr-repro{suffix}-scrolled.png")
    print("\n".join(log_text().splitlines()[-12:]))

    u32.keybd_event(0x1B, 0, 0, 0)
    time.sleep(0.05)
    u32.keybd_event(0x1B, 0, 2, 0)
    time.sleep(0.4)
    proc.terminate()
    np.terminate()
    return 0


if __name__ == "__main__":
    sys.exit(main())
