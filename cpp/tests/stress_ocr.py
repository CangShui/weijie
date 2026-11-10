"""压测 OCR：反复触发提取文字，观察进程是否会崩。

用法: python stress_ocr.py [轮数]
"""
import ctypes
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tbgeom import width as tb_width, center as tb_center

EXE = r"D:\项目\截图工具\cpp\build\weijie.exe"
LOG = os.path.join(os.environ["LOCALAPPDATA"], "weijie", "app.log")
u32 = ctypes.windll.user32
VK = {"A": 0x41, "Z": 0x5A, "F9": 0x78}
MODS = {"Alt": 0x12, "Ctrl": 0x11, "Shift": 0x10}


def log_text():
    try:
        with open(LOG, "r", encoding="utf-8", errors="ignore") as f:
            return f.read()
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


def drag(x0, y0, x1, y1, steps=10):
    u32.SetCursorPos(x0, y0)
    time.sleep(0.1)
    u32.mouse_event(0x0002, 0, 0, 0, 0)
    for i in range(1, steps + 1):
        u32.SetCursorPos(x0 + (x1 - x0) * i // steps, y0 + (y1 - y0) * i // steps)
        time.sleep(0.02)
    time.sleep(0.1)
    u32.mouse_event(0x0004, 0, 0, 0, 0)
    time.sleep(0.25)


def click(x, y):
    u32.SetCursorPos(x, y)
    time.sleep(0.12)
    u32.mouse_event(0x0002, 0, 0, 0, 0)
    time.sleep(0.06)
    u32.mouse_event(0x0004, 0, 0, 0, 0)


def esc():
    u32.keybd_event(0x1B, 0, 0, 0)
    time.sleep(0.04)
    u32.keybd_event(0x1B, 0, 2, 0)


def alive(proc):
    return proc.poll() is None


def main():
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 12
    try:
        os.remove(LOG)
    except OSError:
        pass

    proc = subprocess.Popen([EXE])
    time.sleep(1.8)
    hk = "Alt+Z"
    for line in log_text().splitlines():
        if "尝试热键" in line and "-> True" in line:
            hk = line.split("尝试热键")[1].split("->")[0].strip()
    print("热键 =", hk, " 轮数 =", rounds)

    # 不同位置的选区，覆盖屏幕各处
    boxes = [
        (370, 60, 900, 200),
        (4, 45, 566, 180),
        (1200, 300, 1700, 500),
        (300, 700, 800, 900),
        (60, 200, 360, 260),
        (1500, 800, 1900, 1000),
    ]

    for i in range(rounds):
        if not alive(proc):
            print("第 %d 轮前进程已退出，code=%s" % (i + 1, proc.returncode))
            return 1
        x0, y0, x1, y1 = boxes[i % len(boxes)]
        hotkey(hk)
        time.sleep(0.9)
        drag(x0, y0, x1, y1)
        tbx = (x0 + x1) // 2 - tb_width() // 2
        tby = y1 + 8
        click(*tb_center("ocr", tbx, tby))     # 提取文字
        time.sleep(1.8)
        if not alive(proc):
            print("第 %d 轮 OCR 后崩溃（选区 %s），exit=%s" % (i + 1, (x0, y0, x1, y1), proc.returncode))
            print("最后日志：")
            print("\n".join(log_text().splitlines()[-8:]))
            return 1
        esc()
        time.sleep(0.3)
        esc()
        time.sleep(0.5)
        print("第 %d 轮 OK（选区 %s）" % (i + 1, (x0, y0, x1, y1)))

    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
    print("全部通过，未复现崩溃")
    return 0


if __name__ == "__main__":
    sys.exit(main())
