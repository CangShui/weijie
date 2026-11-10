"""验证 C++ 版：启动 -> 热键截图 -> 检测覆盖层 -> Esc -> 再来一次。

判定策略（重要）：
  主判据 = 日志里 "覆盖层已显示" 的出现次数必须随轮次递增（应用自身行为，绝对可靠）。
  辅判据 = 屏幕亮度变化（仅打印，不做 PASS/FAIL 依据）。
原因：桌面内容随时会变（动画、视频、其它窗口重绘），亮度基准不可靠，
      曾出现覆盖层已弹出但亮度反而升高的假 FAIL。
"""
import os
import subprocess
import sys
import time

from PIL import ImageGrab, ImageStat

EXE = r"D:\项目\截图工具\cpp\build\weijie.exe"
LOG = os.path.join(os.environ["LOCALAPPDATA"], "weijie", "app.log")
SHOT_DIR = r"D:\项目\截图工具\cpp\build"

MARK = "覆盖层已显示"


def log_text():
    try:
        with open(LOG, "r", encoding="utf-8", errors="ignore") as f:
            return f.read()
    except FileNotFoundError:
        return ""


def log_tail(n=20):
    return "\n".join(log_text().splitlines()[-n:])


def bright():
    """取 3 次采样的中位数，抗瞬时抖动。"""
    vals = []
    for _ in range(3):
        img = ImageGrab.grab()
        vals.append(ImageStat.Stat(img.convert("L")).mean[0])
        time.sleep(0.08)
    vals.sort()
    return vals[len(vals) // 2]


def count_overlay():
    return log_text().count(MARK)


def read_hotkey():
    for line in log_text().splitlines():
        if "尝试热键" in line and "-> True" in line:
            return line.split("尝试热键")[1].split("->")[0].strip()
    return "Alt+A"


def send_hotkey(name):
    import ctypes
    VK = {"A": 0x41, "Z": 0x5A, "F9": 0x78}
    mods = {"Alt": 0x12, "Ctrl": 0x11, "Shift": 0x10}
    parts = name.split("+")
    key = parts[-1].upper()
    codes = [mods[p] for p in parts[:-1] if p in mods] + [VK.get(key, 0x41)]
    for c in codes:
        ctypes.windll.user32.keybd_event(c, 0, 0, 0)
        time.sleep(0.03)
    for c in reversed(codes):
        ctypes.windll.user32.keybd_event(c, 0, 2, 0)
        time.sleep(0.03)


def press(vk):
    import ctypes
    ctypes.windll.user32.keybd_event(vk, 0, 0, 0)
    time.sleep(0.05)
    ctypes.windll.user32.keybd_event(vk, 0, 2, 0)


def wait_for(fn, timeout=3.0, step=0.15):
    """轮询等待条件成立，返回是否成立。"""
    end = time.time() + timeout
    while time.time() < end:
        if fn():
            return True
        time.sleep(step)
    return False


def main():
    try:
        os.remove(LOG)
    except OSError:
        pass

    proc = subprocess.Popen([EXE])
    time.sleep(2.0)
    print("启动日志：")
    print(log_tail(10))

    hk = read_hotkey()
    print("热键 =", hk)

    base = bright()
    print("基准亮度 = %.1f" % base)

    ok_all = True
    for round_no in (1, 2):
        before = count_overlay()
        send_hotkey(hk)
        popped = wait_for(lambda: count_overlay() > before, timeout=3.0)
        dim = bright()
        print(
            "第 %d 次截图：日志 %s，亮度 %.1f（基准 %.1f，差 %+.1f）"
            % (round_no, "已弹出覆盖层" if popped else "没反应", dim, base, dim - base)
        )
        if not popped:
            ok_all = False
            print(log_tail(15))
            break

        ImageGrab.grab().save(os.path.join(SHOT_DIR, "shot%d.png" % round_no))

        # Esc 关闭：日志中 overlay 会重新打开下一轮，这里只等界面确实退场
        press(0x1B)
        time.sleep(0.9)
        back = bright()
        print("   按 Esc 后亮度 %.1f（%s）" % (back, "已关闭" if back > base - 8 else "仍偏暗"))

    print("\n日志：")
    print(log_tail(20))

    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()

    print("\n结果:", "PASS" if ok_all else "FAIL")
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
