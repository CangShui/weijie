"""Windows 平台相关封装：窗口探测、全局热键、开机自启。"""
import ctypes
import os
import sys
from ctypes import wintypes

from PySide6.QtCore import QAbstractNativeEventFilter, QPoint, QRect, QSize, Qt
from PySide6.QtGui import QGuiApplication, QImage, QPainter

user32 = ctypes.WinDLL("user32", use_last_error=True)
dwmapi = ctypes.WinDLL("dwmapi", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

WM_HOTKEY = 0x0312
MOD_ALT = 0x0001
MOD_CONTROL = 0x0002
MOD_SHIFT = 0x0004
MOD_NOREPEAT = 0x4000

GA_ROOT = 2
DWMWA_CLOAKED = 14
WS_EX_TOOLWINDOW = 0x00000080
WS_EX_NOACTIVATE = 0x08000000

_SKIP_CLASSES = {
    "Progman", "WorkerW", "Shell_TrayWnd", "Shell_SecondaryTrayWnd",
    "Windows.UI.Core.CoreWindow", "ApplicationManager_DesktopShellWindow",
    "MultitaskingViewFrame", "ForegroundStaging", "XamlExplorerHostIslandWindow",
    "TaskListThumbnailWnd", "SysShadow", "tooltips_class32",
}

WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)


# ---------------------------------------------------------------- 截图抓取
def capture_virtual_desktop():
    """抓取整个虚拟桌面（所有显示器），返回 (QImage 物理像素, 逻辑原点, DPR)。"""
    screens = QGuiApplication.screens()
    if not screens:
        return None, QPoint(0, 0), 1.0

    logical = QRect()
    for s in screens:
        logical = logical.united(s.geometry())

    dpr = max(s.devicePixelRatio() for s in screens)
    if dpr <= 0:
        dpr = 1.0

    w = int(round(logical.width() * dpr))
    h = int(round(logical.height() * dpr))
    shot = QImage(w, h, QImage.Format.Format_RGB32)
    shot.fill(Qt.GlobalColor.black)

    p = QPainter(shot)
    p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform, False)
    for s in screens:
        pm = s.grabWindow(0)
        if pm.isNull():
            continue
        g = s.geometry()
        target = QRect(int(round((g.x() - logical.x()) * dpr)),
                       int(round((g.y() - logical.y()) * dpr)),
                       int(round(g.width() * dpr)),
                       int(round(g.height() * dpr)))
        p.drawPixmap(target, pm, pm.rect())
    p.end()
    return shot, logical.topLeft(), float(dpr)


def screen_dpr_at(pt: QPoint) -> float:
    s = QGuiApplication.screenAt(pt)
    if s is None:
        s = QGuiApplication.primaryScreen()
    return float(s.devicePixelRatio()) if s else 1.0


# ---------------------------------------------------------------- 窗口探测
def _is_cloaked(hwnd) -> bool:
    val = wintypes.DWORD()
    try:
        dwmapi.DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, ctypes.byref(val),
                                     ctypes.sizeof(val))
        return bool(val.value)
    except Exception:
        return False


def enum_top_windows():
    """按 Z 序（最上层优先）返回可见顶层窗口的物理像素矩形列表。"""
    result = []

    def _cb(hwnd, _lparam):
        try:
            if not user32.IsWindowVisible(hwnd):
                return True
            pid = wintypes.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value == os.getpid():
                return True

            buf = ctypes.create_unicode_buffer(256)
            user32.GetClassNameW(hwnd, buf, 256)
            if buf.value in _SKIP_CLASSES:
                return True

            ex = user32.GetWindowLongW(hwnd, -20)
            if ex & WS_EX_TOOLWINDOW:
                return True
            if _is_cloaked(hwnd):
                return True

            rect = wintypes.RECT()
            if not user32.GetWindowRect(hwnd, ctypes.byref(rect)):
                return True
            w = rect.right - rect.left
            h = rect.bottom - rect.top
            if w < 48 or h < 48:
                return True
            if rect.left <= -30000 or rect.top <= -30000:  # 最小化窗口
                return True
            result.append(QRect(rect.left, rect.top, w, h))
        except Exception:
            pass
        return True

    user32.EnumWindows(WNDENUMPROC(_cb), 0)
    return result


def window_rect_at(logical_pt: QPoint):
    """返回逻辑坐标点下最上层窗口的逻辑矩形（找不到返回 None）。"""
    dpr = screen_dpr_at(logical_pt)
    px = int(round(logical_pt.x() * dpr))
    py = int(round(logical_pt.y() * dpr))
    for r in enum_top_windows():
        if r.contains(px, py):
            return QRect(int(round(r.x() / dpr)), int(round(r.y() / dpr)),
                         int(round(r.width() / dpr)), int(round(r.height() / dpr)))
    return None


# ---------------------------------------------------------------- 全局热键
class HotkeyFilter(QAbstractNativeEventFilter):
    def __init__(self, callback):
        super().__init__()
        self._callback = callback

    def nativeEventFilter(self, eventType, message):
        try:
            if eventType in (b"windows_generic_MSG", b"windows_dispatcher_MSG"):
                msg = wintypes.MSG.from_address(int(message))
                if msg.message == WM_HOTKEY:
                    self._callback(int(msg.wParam))
                    return True, 0
        except Exception:
            pass
        return False, 0


def register_hotkey(hwnd: int, hotkey_id: int, mods: int, vk: int) -> bool:
    user32.UnregisterHotKey(wintypes.HWND(hwnd), hotkey_id)
    ok = user32.RegisterHotKey(wintypes.HWND(hwnd), hotkey_id,
                               mods | MOD_NOREPEAT, vk)
    return bool(ok)


def unregister_hotkey(hwnd: int, hotkey_id: int):
    try:
        user32.UnregisterHotKey(wintypes.HWND(hwnd), hotkey_id)
    except Exception:
        pass


# ---------------------------------------------------------------- 前台焦点
def force_foreground(hwnd: int):
    """把窗口强行提到前台并抢到键盘焦点。

    覆盖层是 Tool + 全屏窗口，如果拿不到焦点，Esc / Enter / 快捷键全都收不到。
    Windows 有前台锁（SetForegroundWindow 会被静默拒绝），用 AttachThreadInput
    临时并入前台线程可以绕过。
    """
    SW_RESTORE = 9
    h = wintypes.HWND(hwnd)
    try:
        user32.ShowWindow(h, SW_RESTORE)
        fg = user32.GetForegroundWindow()
        cur = kernel32.GetCurrentThreadId()
        other = user32.GetWindowThreadProcessId(fg, None) if fg else 0
        attached = False
        if other and other != cur:
            attached = bool(user32.AttachThreadInput(other, cur, True))
        user32.SetForegroundWindow(h)
        user32.SetFocus(h)
        if attached:
            user32.AttachThreadInput(other, cur, False)
    except Exception:
        pass


# ---------------------------------------------------------------- 单实例
_ACTIVATE_EVENT = "JianJie_Activate"
EVENT_MODIFY_STATE = 0x0002


def acquire_single_instance(name="JianJie_SingleInstance"):
    handle = kernel32.CreateMutexW(None, False, name)
    if not handle:
        return None, True
    ERROR_ALREADY_EXISTS = 183
    if ctypes.get_last_error() == ERROR_ALREADY_EXISTS:
        return handle, True
    return handle, False


def signal_activate(name=_ACTIVATE_EVENT) -> bool:
    """通知已在运行的实例开始截图。"""
    h = kernel32.OpenEventW(EVENT_MODIFY_STATE, False, name)
    if not h:
        return False
    try:
        return bool(kernel32.SetEvent(h))
    finally:
        kernel32.CloseHandle(h)


def watch_activate(callback, stop_flag, name=_ACTIVATE_EVENT):
    """在后台线程里等待「再次启动」的信号。"""
    h = kernel32.CreateEventW(None, False, False, name)
    if not h:
        return
    try:
        while not stop_flag.is_set():
            if kernel32.WaitForSingleObject(h, 200) == 0:  # WAIT_OBJECT_0
                try:
                    callback()
                except Exception:
                    pass
    finally:
        kernel32.CloseHandle(h)


# ---------------------------------------------------------------- 开机自启
_RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
_APP_NAME = "JianJie"


def _startup_command() -> str:
    if getattr(sys, "frozen", False):
        return f'"{sys.executable}"'
    script = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                          "src", "main.py")
    return f'"{sys.executable}" "{script}"'


def set_autostart(enabled: bool) -> bool:
    import winreg
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, _RUN_KEY, 0,
                            winreg.KEY_SET_VALUE) as k:
            if enabled:
                winreg.SetValueEx(k, _APP_NAME, 0, winreg.REG_SZ, _startup_command())
            else:
                try:
                    winreg.DeleteValue(k, _APP_NAME)
                except FileNotFoundError:
                    pass
        return True
    except Exception:
        return False


def is_autostart_enabled() -> bool:
    import winreg
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, _RUN_KEY) as k:
            winreg.QueryValueEx(k, _APP_NAME)
            return True
    except Exception:
        return False
