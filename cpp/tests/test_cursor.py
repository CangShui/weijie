"""核对覆盖层里的鼠标样式。

ImageGrab 走的是 BitBlt，抓不到系统光标，所以这里自己 BitBlt + DrawIconEx，
把光标一起画进图里，才是用户真正看到的画面。

产出：
  build/c9_idle.png     待机状态，光标停在空白桌面
  build/c9_idle2x.png   同上 2 倍放大
  build/c9_drag.png     拖拽选区过程中
  build/c9_sel.png      选区内部的移动光标
"""
import ctypes
import os
import subprocess
import sys
import time
from ctypes import wintypes

from PIL import Image

EXE = r"D:\项目\截图工具\cpp\build\weijie.exe"
LOG = os.path.join(os.environ["LOCALAPPDATA"], "weijie", "app.log")
OUT = r"D:\项目\截图工具\cpp\build"

u32 = ctypes.windll.user32
g32 = ctypes.windll.gdi32

u32.GetDC.restype = wintypes.HDC
u32.GetDC.argtypes = [wintypes.HWND]
u32.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
g32.CreateCompatibleDC.restype = wintypes.HDC
g32.CreateCompatibleDC.argtypes = [wintypes.HDC]
g32.CreateCompatibleBitmap.restype = wintypes.HBITMAP
g32.CreateCompatibleBitmap.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int]
g32.SelectObject.restype = wintypes.HGDIOBJ
g32.SelectObject.argtypes = [wintypes.HDC, wintypes.HGDIOBJ]
g32.BitBlt.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                       ctypes.c_int, wintypes.HDC, ctypes.c_int, ctypes.c_int, wintypes.DWORD]
u32.DrawIconEx.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int, wintypes.HICON,
                           ctypes.c_int, ctypes.c_int, wintypes.UINT, wintypes.HBRUSH,
                           wintypes.UINT]
g32.DeleteObject.argtypes = [wintypes.HGDIOBJ]
g32.DeleteDC.argtypes = [wintypes.HDC]
g32.GetDIBits.argtypes = [wintypes.HDC, wintypes.HBITMAP, wintypes.UINT, wintypes.UINT,
                          ctypes.c_void_p, ctypes.c_void_p, wintypes.UINT]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wintypes.DWORD), ("biWidth", wintypes.LONG),
                ("biHeight", wintypes.LONG), ("biPlanes", wintypes.WORD),
                ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
                ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", wintypes.LONG),
                ("biYPelsPerMeter", wintypes.LONG), ("biClrUsed", wintypes.DWORD),
                ("biClrImportant", wintypes.DWORD)]


class CURSORINFO(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("flags", wintypes.DWORD),
                ("hCursor", wintypes.HANDLE), ("ptScreenPos", wintypes.POINT)]


def grab(x0, y0, w, h):
    """截屏并叠加当前光标。"""
    hdc = u32.GetDC(None)
    mdc = g32.CreateCompatibleDC(hdc)
    bmp = g32.CreateCompatibleBitmap(hdc, w, h)
    g32.SelectObject(mdc, bmp)
    g32.BitBlt(mdc, 0, 0, w, h, hdc, x0, y0, 0x00CC0020)

    ci = CURSORINFO()
    ci.cbSize = ctypes.sizeof(ci)
    u32.GetCursorInfo(ctypes.byref(ci))
    if ci.flags & 1:
        u32.DrawIconEx(mdc, ci.ptScreenPos.x - x0, ci.ptScreenPos.y - y0,
                       ci.hCursor, 0, 0, 0, None, 3)
    bi = BITMAPINFOHEADER()
    bi.biSize = ctypes.sizeof(bi)
    bi.biWidth = w
    bi.biHeight = -h
    bi.biPlanes = 1
    bi.biBitCount = 32
    buf = ctypes.create_string_buffer(w * h * 4)
    g32.GetDIBits(mdc, bmp, 0, h, buf, ctypes.byref(bi), 0)

    img = Image.frombuffer("RGBA", (w, h), buf, "raw", "BGRA", 0, 1).convert("RGB")
    g32.DeleteObject(bmp)
    g32.DeleteDC(mdc)
    u32.ReleaseDC(None, hdc)
    return img


def log_text():
    try:
        with open(LOG, "r", encoding="utf-8", errors="ignore") as f:
            return f.read()
    except FileNotFoundError:
        return ""


VK = {"A": 0x41, "Z": 0x5A, "F9": 0x78}
MODS = {"Alt": 0x12, "Ctrl": 0x11, "Shift": 0x10}


def hotkey(name):
    parts = name.split("+")
    codes = [MODS[p] for p in parts[:-1] if p in MODS] + [VK.get(parts[-1].upper(), 0x41)]
    for c in codes:
        u32.keybd_event(c, 0, 0, 0)
        time.sleep(0.03)
    for c in reversed(codes):
        u32.keybd_event(c, 0, 2, 0)
        time.sleep(0.03)


def move(x, y):
    u32.SetCursorPos(x, y)
    time.sleep(0.3)


def down():
    u32.mouse_event(0x0002, 0, 0, 0, 0)
    time.sleep(0.1)


def up():
    u32.mouse_event(0x0004, 0, 0, 0, 0)
    time.sleep(0.25)


def esc():
    u32.keybd_event(0x1B, 0, 0, 0)
    time.sleep(0.05)
    u32.keybd_event(0x1B, 0, 2, 0)
    time.sleep(0.4)


def shot(name, cx, cy, half=52):
    img = grab(cx - half, cy - half, half * 2, half * 2)
    img.save(os.path.join(OUT, name + ".png"))
    img.resize((half * 4, half * 4), Image.NEAREST).save(os.path.join(OUT, name + "2x.png"))
    print("  已存", name + ".png")


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

    # 1) 待机：光标停在空白桌面
    move(900, 520)
    shot("c9_idle", 900, 520)

    # 2) 拖拽中
    move(400, 300)
    down()
    for i in range(1, 9):
        u32.SetCursorPos(400 + 50 * i, 300 + 25 * i)
        time.sleep(0.03)
    time.sleep(0.35)
    shot("c9_drag", 800, 500)
    up()
    time.sleep(0.4)

    # 3) 选区内部（移动光标）
    move(600, 420)
    shot("c9_sel", 600, 420)

    # 4) 工具条上
    move(600, 640)
    shot("c9_tb", 600, 640)

    esc()
    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
    print("完成")
    return 0


if __name__ == "__main__":
    sys.exit(main())
