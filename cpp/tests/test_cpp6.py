"""验证结果面板与工具条不会互相遮挡（复现用户反馈的重叠问题）。

场景：选一个小选区（155x32）放在屏幕靠上位置，点「提取文字」，
此时工具条在选区下方、面板想放在选区右侧 —— 两者水平方向必然相交，
修复后面板应自动下移到工具条下方。
"""
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


def drag(x0, y0, x1, y1, steps=12):
    u32.SetCursorPos(x0, y0)
    time.sleep(0.12)
    u32.mouse_event(0x0002, 0, 0, 0, 0)
    for i in range(1, steps + 1):
        u32.SetCursorPos(x0 + (x1 - x0) * i // steps, y0 + (y1 - y0) * i // steps)
        time.sleep(0.02)
    time.sleep(0.12)
    u32.mouse_event(0x0004, 0, 0, 0, 0)
    time.sleep(0.3)


def click(x, y):
    u32.SetCursorPos(x, y)
    time.sleep(0.15)
    u32.mouse_event(0x0002, 0, 0, 0, 0)
    time.sleep(0.08)
    u32.mouse_event(0x0004, 0, 0, 0, 0)
    time.sleep(0.25)


def esc():
    u32.keybd_event(0x1B, 0, 0, 0)
    time.sleep(0.05)
    u32.keybd_event(0x1B, 0, 2, 0)
    time.sleep(0.4)


def layout_line():
    for line in reversed(log_text().splitlines()):
        if "布局 面板" in line:
            return line
    return None


def parse(line):
    """从日志里抠出面板/工具条的矩形与重叠标记。"""
    import re
    m = re.search(r"面板\((-?\d+),(-?\d+) (\d+)x(\d+)\) 工具条\((-?\d+),(-?\d+) (\d+)x(\d+)\) 候选#(\d+) 重叠=(\d)", line)
    if not m:
        return None
    v = [int(x) for x in m.groups()]
    return {"panel": v[0:4], "tb": v[4:8], "cand": v[8], "overlap": v[9]}


def main():
    try:
        os.remove(LOG)
    except OSError:
        pass

    proc = subprocess.Popen([EXE])
    time.sleep(1.8)
    hk = "Alt+A"
    for line in log_text().splitlines():
        if "尝试热键" in line and "-> True" in line:
            hk = line.split("尝试热键")[1].split("->")[0].strip()
    print("热键 =", hk)

    hotkey(hk)
    time.sleep(1.0)

    # 小选区，靠上：复现截图里的 155x32
    x0, y0, x1, y1 = 300, 40, 455, 72
    drag(x0, y0, x1, y1)
    time.sleep(0.5)

    tb_w = tb_width()
    tb_x = (x0 + x1) // 2 - tb_w // 2
    tb_y = y1 + 8
    print("工具条推测位置 = (%d, %d)" % (tb_x, tb_y))

    click(*tb_center("ocr", tb_x, tb_y))     # 提取文字
    time.sleep(2.5)

    line = layout_line()
    print("布局日志:", line)
    if not line:
        print("结果: FAIL（没拿到布局日志）")
        esc()
        proc.terminate()
        return 1

    d = parse(line)
    print("面板 rect  =", d["panel"])
    print("工具条 rect =", d["tb"])
    print("选中候选   = #%d" % d["cand"])
    print("重叠       =", d["overlap"])

    ImageGrab.grab().save(os.path.join(OUT, "layout_fix.png"))
    print("截图已保存 layout_fix.png")

    ok = (d["overlap"] == 0)

    # 再验一次：按方向键挪选区，工具条重排后仍不应压面板
    for _ in range(3):
        u32.keybd_event(0x28, 0, 0, 0)   # Down
        time.sleep(0.04)
        u32.keybd_event(0x28, 0, 2, 0)
        time.sleep(0.15)
    time.sleep(0.4)
    img = ImageGrab.grab()
    img.save(os.path.join(OUT, "layout_after_move.png"))
    print("移动后截图已保存 layout_after_move.png")

    esc()
    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()

    print("结果:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
