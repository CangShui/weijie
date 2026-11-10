"""验证选区、工具条、矩形标注、OCR 面板。"""
import ctypes
import os
import subprocess
import sys
import time

from PIL import ImageGrab

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tbgeom import width as tb_width, center as tb_center

EXE = r"D:\项目\截图工具\cpp\build\weijie.exe"
LOG = os.path.join(os.environ["LOCALAPPDATA"], "weijie", "app.log")
OUT = r"D:\项目\截图工具\cpp\build"
u32 = ctypes.windll.user32

VK = {"A": 0x41, "Z": 0x5A, "F9": 0x78}
MODS = {"Alt": 0x12, "Ctrl": 0x11, "Shift": 0x10}


def log_tail(n=30):
    try:
        with open(LOG, "r", encoding="utf-8", errors="ignore") as f:
            return "".join(f.readlines()[-n:])
    except FileNotFoundError:
        return "(no log)"


def hotkey(name):
    parts = name.split("+")
    codes = [MODS[p] for p in parts[:-1] if p in MODS] + [VK.get(parts[-1].upper(), 0x41)]
    for c in codes:
        u32.keybd_event(c, 0, 0, 0)
        time.sleep(0.03)
    for c in reversed(codes):
        u32.keybd_event(c, 0, 2, 0)
        time.sleep(0.03)


def drag(x0, y0, x1, y1, steps=12):
    u32.SetCursorPos(x0, y0)
    time.sleep(0.1)
    u32.mouse_event(0x0002, 0, 0, 0, 0)   # left down
    for i in range(1, steps + 1):
        x = x0 + (x1 - x0) * i // steps
        y = y0 + (y1 - y0) * i // steps
        u32.SetCursorPos(x, y)
        time.sleep(0.02)
    time.sleep(0.1)
    u32.mouse_event(0x0004, 0, 0, 0, 0)   # left up


def click(x, y):
    u32.SetCursorPos(x, y)
    time.sleep(0.15)
    u32.mouse_event(0x0002, 0, 0, 0, 0)
    time.sleep(0.08)
    u32.mouse_event(0x0004, 0, 0, 0, 0)


def esc():
    u32.keybd_event(0x1B, 0, 0, 0)
    time.sleep(0.05)
    u32.keybd_event(0x1B, 0, 2, 0)


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
    print("热键 =", hk)

    hotkey(hk)
    time.sleep(1.0)

    # 选区：包含页面标题文字
    x0, y0, x1, y1 = 370, 60, 900, 200
    drag(x0, y0, x1, y1)
    time.sleep(0.6)
    ImageGrab.grab().save(os.path.join(OUT, "s1_sel.png"))
    print("选区截图已保存")

    # 工具条位置：水平居中于选区，位于选区下方 8px
    tb_w = tb_width()
    tb_x = (x0 + x1) // 2 - tb_w // 2
    tb_y = y1 + 8
    print("工具条推测位置 = (%d, %d)" % (tb_x, tb_y))

    click(*tb_center("ocr", tb_x, tb_y))          # 提取文字
    print("已点击「提取文字」")
    time.sleep(3.0)
    ImageGrab.grab().save(os.path.join(OUT, "s2_ocr.png"))

    click(*tb_center("rect", tb_x, tb_y))         # 矩形
    time.sleep(0.4)
    drag(430, 100, 700, 170)
    time.sleep(0.5)
    ImageGrab.grab().save(os.path.join(OUT, "s3_rect.png"))
    print("标注截图已保存")

    print("\n日志：")
    print(log_tail(15))

    esc()
    time.sleep(0.4)
    esc()
    time.sleep(0.6)

    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()


if __name__ == "__main__":
    sys.exit(main())
