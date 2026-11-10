"""验证贴图、放大镜、翻译。"""
import ctypes
import os
import subprocess
import sys
import time
from ctypes import wintypes

from PIL import ImageGrab

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tbgeom import width as tb_width, center as tb_center

EXE = r"D:\项目\截图工具\cpp\build\weijie.exe"
LOG = os.path.join(os.environ["LOCALAPPDATA"], "weijie", "app.log")
OUT = r"D:\项目\截图工具\cpp\build"
u32 = ctypes.windll.user32

VK = {"A": 0x41, "Z": 0x5A, "F9": 0x78}
MODS = {"Alt": 0x12, "Ctrl": 0x11, "Shift": 0x10}


def log_tail(n=20):
    try:
        with open(LOG, "r", encoding="utf-8", errors="ignore") as f:
            return "".join(f.readlines()[-n:])
    except FileNotFoundError:
        return ""


def hotkey(name):
    parts = name.split("+")
    codes = [MODS[p] for p in parts[:-1] if p in MODS] + [VK.get(parts[-1].upper(), 0x41)]
    for c in codes:
        u32.keybd_event(c, 0, 0, 0)
        time.sleep(0.03)
    for c in reversed(codes):
        u32.keybd_event(c, 0, 2, 0)
        time.sleep(0.03)


def drag(x0, y0, x1, y1, steps=14, mid_cb=None):
    u32.SetCursorPos(x0, y0)
    time.sleep(0.12)
    u32.mouse_event(0x0002, 0, 0, 0, 0)
    for i in range(1, steps + 1):
        u32.SetCursorPos(x0 + (x1 - x0) * i // steps, y0 + (y1 - y0) * i // steps)
        time.sleep(0.02)
        if mid_cb and i == steps // 2:
            mid_cb()
    time.sleep(0.12)
    u32.mouse_event(0x0004, 0, 0, 0, 0)
    time.sleep(0.2)


def click(x, y):
    u32.SetCursorPos(x, y)
    time.sleep(0.15)
    u32.mouse_event(0x0002, 0, 0, 0, 0)
    time.sleep(0.08)
    u32.mouse_event(0x0004, 0, 0, 0, 0)
    time.sleep(0.25)


def key(vk, ctrl=False):
    if ctrl:
        u32.keybd_event(0x11, 0, 0, 0)
        time.sleep(0.03)
    u32.keybd_event(vk, 0, 0, 0)
    time.sleep(0.06)
    u32.keybd_event(vk, 0, 2, 0)
    if ctrl:
        time.sleep(0.03)
        u32.keybd_event(0x11, 0, 2, 0)
    time.sleep(0.2)


def pin_windows():
    found = []
    CB = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def cb(h, l):
        buf = ctypes.create_unicode_buffer(256)
        u32.GetClassNameW(h, buf, 256)
        if buf.value == "weijiePin" and u32.IsWindowVisible(h):
            r = wintypes.RECT()
            u32.GetWindowRect(h, ctypes.byref(r))
            found.append((h, r.left, r.top, r.right - r.left, r.bottom - r.top))
        return True

    u32.EnumWindows(CB(cb), 0)
    return found


def main():
    try:
        os.remove(LOG)
    except OSError:
        pass

    proc = subprocess.Popen([EXE])
    time.sleep(1.8)
    hk = "Alt+A"
    for line in log_tail(20).splitlines():
        if "尝试热键" in line and "-> True" in line:
            hk = line.split("尝试热键")[1].split("->")[0].strip()

    hotkey(hk)
    time.sleep(1.0)

    # ---- 放大镜：拖到一半截图
    shot = {"done": False}

    def mid():
        ImageGrab.grab().save(os.path.join(OUT, "p1_magnifier.png"))
        shot["done"] = True

    drag(500, 250, 950, 620, mid_cb=mid)
    print("放大镜截图:", "已保存" if shot["done"] else "失败")

    # ---- 贴图
    key(0x54, ctrl=True)      # Ctrl+T
    time.sleep(1.0)
    pins = pin_windows()
    print("贴图窗口:", pins)
    ImageGrab.grab().save(os.path.join(OUT, "p2_pin.png"))

    key(0x1B)                 # Esc 关贴图
    time.sleep(0.6)
    print("Esc 后贴图窗口:", pin_windows())

    # ---- 翻译：重新截一块含中文的区域
    hotkey(hk)
    time.sleep(1.0)
    x0, y0, x1, y1 = 370, 60, 900, 200
    drag(x0, y0, x1, y1)
    tb_x = (x0 + x1) // 2 - tb_width() // 2
    tb_y = y1 + 8
    click(*tb_center("ocr", tb_x, tb_y))      # 提取文字
    time.sleep(2.5)
    click(*tb_center("trans", tb_x, tb_y))    # 翻译
    time.sleep(6.0)
    ImageGrab.grab().save(os.path.join(OUT, "p3_translate.png"))
    print("翻译截图已保存")

    key(0x1B)
    time.sleep(0.4)
    key(0x1B)
    time.sleep(0.5)

    print("\n日志：")
    print(log_tail(10))

    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()


if __name__ == "__main__":
    sys.exit(main())
