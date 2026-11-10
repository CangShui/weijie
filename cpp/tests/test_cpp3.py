"""综合验证：多种标注 + 文字输入 + 复制到剪贴板。"""
import ctypes
import os
import subprocess
import sys
import time

from PIL import ImageGrab

EXE = r"D:\项目\截图工具\cpp\build\weijie.exe"
LOG = os.path.join(os.environ["LOCALAPPDATA"], "weijie", "app.log")
OUT = r"D:\项目\截图工具\cpp\build"
u32 = ctypes.windll.user32

VK = {"A": 0x41, "Z": 0x5A, "F9": 0x78}
MODS = {"Alt": 0x12, "Ctrl": 0x11, "Shift": 0x10}


def log_tail(n=40):
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


def drag(x0, y0, x1, y1, steps=14):
    u32.SetCursorPos(x0, y0)
    time.sleep(0.12)
    u32.mouse_event(0x0002, 0, 0, 0, 0)
    for i in range(1, steps + 1):
        u32.SetCursorPos(x0 + (x1 - x0) * i // steps, y0 + (y1 - y0) * i // steps)
        time.sleep(0.02)
    time.sleep(0.12)
    u32.mouse_event(0x0004, 0, 0, 0, 0)
    time.sleep(0.15)


def click(x, y):
    u32.SetCursorPos(x, y)
    time.sleep(0.15)
    u32.mouse_event(0x0002, 0, 0, 0, 0)
    time.sleep(0.08)
    u32.mouse_event(0x0004, 0, 0, 0, 0)
    time.sleep(0.2)


def key(vk, ctrl=False):
    if ctrl:
        u32.keybd_event(0x11, 0, 0, 0)
        time.sleep(0.03)
    u32.keybd_event(vk, 0, 0, 0)
    time.sleep(0.05)
    u32.keybd_event(vk, 0, 2, 0)
    if ctrl:
        time.sleep(0.03)
        u32.keybd_event(0x11, 0, 2, 0)
    time.sleep(0.15)


def type_text(s):
    for ch in s:
        vk = u32.VkKeyScanW(ord(ch)) & 0xFF
        shift = (u32.VkKeyScanW(ord(ch)) >> 8) & 1
        if shift:
            u32.keybd_event(0x10, 0, 0, 0)
        u32.keybd_event(vk, 0, 0, 0)
        time.sleep(0.02)
        u32.keybd_event(vk, 0, 2, 0)
        if shift:
            u32.keybd_event(0x10, 0, 2, 0)
        time.sleep(0.03)


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

    x0, y0, x1, y1 = 400, 300, 1000, 600
    drag(x0, y0, x1, y1)
    print("选区完成")

    # 箭头
    key(0x41)
    drag(450, 360, 640, 460)
    # 椭圆
    key(0x45)
    drag(700, 350, 860, 450)
    # 序号
    key(0x4E)
    click(900, 380)
    click(900, 480)
    # 文字
    key(0x54)
    click(450, 520)
    time.sleep(0.4)
    type_text("Hello")
    key(0x0D)
    time.sleep(0.3)
    # 马赛克
    key(0x4D)
    drag(700, 500, 900, 560)

    time.sleep(0.5)
    ImageGrab.grab().save(os.path.join(OUT, "m1_all.png"))
    print("综合标注截图已保存")

    # 复制到剪贴板
    key(0x43, ctrl=True)
    time.sleep(1.2)

    img = ImageGrab.grabclipboard()
    if img is None:
        print("剪贴板：空")
    else:
        print("剪贴板图像尺寸 =", img.size, "期望 = (%d, %d)" % (x1 - x0, y1 - y0))
        img.save(os.path.join(OUT, "m2_clip.png"))

    print("\n日志：")
    print(log_tail(12))

    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()


if __name__ == "__main__":
    sys.exit(main())
