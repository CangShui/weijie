"""验证保存（Ctrl+S -> 默认文件名 -> 回车）。"""
import ctypes
import os
import subprocess
import sys
import time

EXE = r"D:\项目\截图工具\cpp\build\weijie.exe"
LOG = os.path.join(os.environ["LOCALAPPDATA"], "weijie", "app.log")
SAVE_DIR = os.path.join(os.environ["USERPROFILE"], "Pictures", "微截")
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


def drag(x0, y0, x1, y1, steps=12):
    u32.SetCursorPos(x0, y0)
    time.sleep(0.12)
    u32.mouse_event(0x0002, 0, 0, 0, 0)
    for i in range(1, steps + 1):
        u32.SetCursorPos(x0 + (x1 - x0) * i // steps, y0 + (y1 - y0) * i // steps)
        time.sleep(0.02)
    time.sleep(0.12)
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


def find_dialog():
    """找保存对话框（#32770，标题「保存截图」）。"""
    found = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def cb(hwnd, _lp):
        cls = ctypes.create_unicode_buffer(64)
        u32.GetClassNameW(hwnd, cls, 64)
        if cls.value == "#32770" and u32.IsWindowVisible(hwnd):
            t = ctypes.create_unicode_buffer(128)
            u32.GetWindowTextW(hwnd, t, 128)
            if "保存" in t.value:
                found.append(hwnd)
        return True

    u32.EnumWindows(cb, 0)
    return found[0] if found else None


def wait_dialog(timeout=4.0):
    end = time.time() + timeout
    while time.time() < end:
        h = find_dialog()
        if h:
            return h
        time.sleep(0.1)
    return None


def main():
    before = set(os.listdir(SAVE_DIR)) if os.path.isdir(SAVE_DIR) else set()

    proc = subprocess.Popen([EXE])
    time.sleep(1.8)
    hk = "Alt+A"
    for line in log_tail(20).splitlines():
        if "尝试热键" in line and "-> True" in line:
            hk = line.split("尝试热键")[1].split("->")[0].strip()

    hotkey(hk)
    time.sleep(1.0)
    drag(500, 300, 1000, 650)
    time.sleep(0.5)

    key(0x53, ctrl=True)     # Ctrl+S 打开保存对话框
    dlg = wait_dialog()
    print("保存对话框:", hex(dlg) if dlg else "未出现")
    time.sleep(0.6)
    key(0x0D)                # 回车确认默认文件名
    time.sleep(1.2)

    after = set(os.listdir(SAVE_DIR)) if os.path.isdir(SAVE_DIR) else set()
    if not (after - before) and find_dialog():
        key(0x0D)            # 还在，再按一次
        time.sleep(1.2)
        after = set(os.listdir(SAVE_DIR)) if os.path.isdir(SAVE_DIR) else set()

    new = sorted(after - before)
    print("保存目录:", SAVE_DIR)
    print("新增文件:", new)

    ok = False
    for name in new:
        path = os.path.join(SAVE_DIR, name)
        print("  ", name, os.path.getsize(path), "字节")
        ok = os.path.getsize(path) > 1000
        try:
            os.remove(path)
        except OSError:
            pass

    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()

    print("结果:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
