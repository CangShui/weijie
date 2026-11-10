"""工具条外观预览：拉一个选区，把工具条裁出来存成 PNG，方便肉眼比对微信截图。

用法：python preview_toolbar.py [输出前缀]
产物：<前缀>_tb.png（原尺寸）、<前缀>_tb2x.png（放大 3 倍，看清笔画）
"""
import ctypes
import os
import subprocess
import sys
import time

from PIL import Image, ImageGrab
from tbgeom import width as tb_width, ITEM_W, TB_HEIGHT, TB_PAD

EXE = r"D:\项目\截图工具\cpp\build\weijie.exe"
LOG = os.path.join(os.environ["LOCALAPPDATA"], "weijie", "app.log")
OUT = r"D:\项目\截图工具\cpp\build"
u32 = ctypes.windll.user32

VK = {"A": 0x41, "Z": 0x5A, "F9": 0x78}
MODS = {"Alt": 0x12, "Ctrl": 0x11, "Shift": 0x10}

# 工具条几何统一从 tbgeom.py 读取，避免按钮数量变化后坐标漂移。
TB_W = tb_width()


def log_tail(n=25):
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


def drag(x0, y0, x1, y1, steps=14):
    u32.SetCursorPos(x0, y0)
    time.sleep(0.12)
    u32.mouse_event(0x0002, 0, 0, 0, 0)
    for i in range(1, steps + 1):
        u32.SetCursorPos(x0 + (x1 - x0) * i // steps, y0 + (y1 - y0) * i // steps)
        time.sleep(0.02)
    time.sleep(0.1)
    u32.mouse_event(0x0004, 0, 0, 0, 0)


def move(x, y):
    u32.SetCursorPos(x, y)
    time.sleep(0.25)


def main():
    prefix = sys.argv[1] if len(sys.argv) > 1 else "tb"
    subprocess.run(["taskkill", "/F", "/IM", "微截.exe"], capture_output=True)
    subprocess.run(["taskkill", "/F", "/IM", "weijie.exe"], capture_output=True)
    time.sleep(0.4)
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

    x0, y0, x1, y1 = 300, 80, 1040, 300
    drag(x0, y0, x1, y1)
    time.sleep(0.6)

    tb_x = (x0 + x1) // 2 - TB_W // 2
    tb_y = y1 + 8
    print("工具条 = (%d, %d) %dx%d" % (tb_x, tb_y, TB_W, TB_HEIGHT))

    move(tb_x + TB_W + 40, tb_y + 5)   # 鼠标挪到工具条外面，避免 hover 干扰
    time.sleep(0.3)
    img = ImageGrab.grab()
    pad = 6
    crop = img.crop((tb_x - pad, tb_y - pad, tb_x + TB_W + pad, tb_y + TB_HEIGHT + pad))
    crop.save(os.path.join(OUT, prefix + "_tb.png"))
    crop.resize((crop.width * 3, crop.height * 3), Image.NEAREST).save(
        os.path.join(OUT, prefix + "_tb2x.png"))

    # 悬停「矩形」和「完成」，看 hover / 红绿配色
    move(tb_x + TB_PAD + ITEM_W // 2, tb_y + TB_HEIGHT // 2)
    time.sleep(0.35)
    ImageGrab.grab().crop((tb_x - pad, tb_y - pad,
                           tb_x + TB_W + pad, tb_y + TB_HEIGHT + pad)).resize(
        (crop.width * 3, crop.height * 3), Image.NEAREST).save(
        os.path.join(OUT, prefix + "_hover.png"))

    print("\n日志：")
    print(log_tail(8))

    u32.keybd_event(0x1B, 0, 0, 0)
    time.sleep(0.05)
    u32.keybd_event(0x1B, 0, 2, 0)
    time.sleep(0.5)
    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()


if __name__ == "__main__":
    main()
