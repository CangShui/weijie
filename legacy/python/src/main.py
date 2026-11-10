"""简截 —— 微信风格的独立截图工具。

托盘常驻，Alt+A 截图；无需登录微信。
"""
import os
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from PySide6.QtCore import (QObject, QPointF, QRect, QRectF, Qt, QTimer,  # noqa: E402
                            Signal)
from PySide6.QtGui import (QAction, QColor, QGuiApplication, QIcon, QPainter,  # noqa: E402
                           QPen, QPixmap)
from PySide6.QtWidgets import (QApplication, QMenu, QMessageBox,  # noqa: E402
                               QSystemTrayIcon, QWidget)

import win_utils  # noqa: E402
from overlay import Overlay  # noqa: E402
from settings import (HOTKEY_PRESETS, load_settings, resolve_hotkey,  # noqa: E402
                      save_settings)

APP_NAME = "简截"
HOTKEY_ID = 0xA1

LOG_PATH = os.path.join(os.environ.get("LOCALAPPDATA") or os.environ.get("TEMP", "."),
                        "JianJie", "app.log")


def log(msg):
    """轻量运行日志，出问题时看 %LOCALAPPDATA%\\JianJie\\app.log。"""
    try:
        from datetime import datetime
        os.makedirs(os.path.dirname(LOG_PATH), exist_ok=True)
        with open(LOG_PATH, "a", encoding="utf-8") as f:
            f.write(f"[{datetime.now():%m-%d %H:%M:%S}] {msg}\n")
    except Exception:
        pass


def make_icon(size=64) -> QPixmap:
    pm = QPixmap(size, size)
    pm.fill(Qt.GlobalColor.transparent)
    p = QPainter(pm)
    p.setRenderHint(QPainter.RenderHint.Antialiasing, True)
    s = size / 64.0
    p.setPen(Qt.PenStyle.NoPen)
    p.setBrush(QColor("#07C160"))
    p.drawRoundedRect(QRectF(3 * s, 3 * s, 58 * s, 58 * s), 15 * s, 15 * s)

    pen = QPen(QColor("#FFFFFF"), 4.6 * s)
    pen.setCapStyle(Qt.PenCapStyle.RoundCap)
    pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
    p.setPen(pen)
    p.setBrush(Qt.BrushStyle.NoBrush)
    a, b = 18 * s, 46 * s
    k = 8 * s
    for (x, y, dx, dy) in ((a, a, 1, 1), (b, a, -1, 1), (b, b, -1, -1), (a, b, 1, -1)):
        p.drawLine(QPointF(x, y + dy * k), QPointF(x, y))
        p.drawLine(QPointF(x, y), QPointF(x + dx * k, y))
    p.end()
    return pm


class _ActivateBridge(QObject):
    triggered = Signal()


class JianJie(QObject):
    def __init__(self, qapp: QApplication):
        super().__init__()
        self.qapp = qapp
        self.overlay = None
        self._hotkey_ok = False
        self._pending = False
        self.settings = load_settings()
        self.hotkey_name = self.settings.get("hotkey", "Alt+A")

        self.icon = QIcon(make_icon(64))
        qapp.setWindowIcon(self.icon)

        self.tray = QSystemTrayIcon(self.icon, self)
        self._build_menu()
        self.tray.activated.connect(self._on_tray)
        self.tray.show()

        self._filter = win_utils.HotkeyFilter(self._on_hotkey)
        qapp.installNativeEventFilter(self._filter)
        self._register_hotkeys()
        log(f"启动完成，热键={self.hotkey_name} ok={self._hotkey_ok}")

        # 再次双击 exe 时，让本实例直接开始截图
        self._stop = threading.Event()
        self._bridge = _ActivateBridge(self)
        self._bridge.triggered.connect(self.capture)
        threading.Thread(target=win_utils.watch_activate,
                         args=(self._bridge.triggered.emit, self._stop),
                         daemon=True).start()

        QTimer.singleShot(600, self._greet)

    # ------------------------------------------------------------ 热键
    def _register_hotkeys(self, preferred=None):
        """按「配置值 → 预设列表」顺序尝试注册，成功后更新界面文案。"""
        win_utils.unregister_hotkey(0, HOTKEY_ID)
        order = []
        if preferred:
            order.append(preferred)
        order.append(self.settings.get("hotkey", "Alt+A"))
        order.extend(HOTKEY_PRESETS.keys())
        seen = []
        for name in order:
            if name in seen:
                continue
            seen.append(name)
            mods, vk = resolve_hotkey(name)
            ok = win_utils.register_hotkey(0, HOTKEY_ID, mods, vk)
            log(f"尝试热键 {name} -> {ok}")
            if ok:
                self.hotkey_name = name
                self._hotkey_ok = True
                self._update_hotkey_ui()
                return True
        self._hotkey_ok = False
        self._update_hotkey_ui()
        return False

    def _update_hotkey_ui(self):
        if self._hotkey_ok:
            self.tray.setToolTip(f"{APP_NAME} · {self.hotkey_name} 截图")
            self.act_capture.setText(f"截图\t{self.hotkey_name}")
        else:
            self.tray.setToolTip(f"{APP_NAME} · 快捷键不可用")
            self.act_capture.setText("截图（快捷键不可用）")
        for name, act in getattr(self, "_hotkey_actions", {}).items():
            act.setChecked(name == self.hotkey_name and self._hotkey_ok)

    def _choose_hotkey(self, name):
        if self._register_hotkeys(preferred=name):
            self.settings["hotkey"] = name
            save_settings(self.settings)
        else:
            QMessageBox.warning(None, APP_NAME, f"「{name}」已被其他程序占用，请换一个。")
            self._update_hotkey_ui()

    def _on_hotkey(self, _hid):
        log("收到热键")
        self.capture()

    # ------------------------------------------------------------ 托盘菜单
    def _build_menu(self):
        m = QMenu()
        self.act_capture = QAction(f"截图\t{self.hotkey_name}", m)
        self.act_capture.triggered.connect(self.capture)

        self._hotkey_actions = {}
        sub = m.addMenu("快捷键")
        for name in HOTKEY_PRESETS:
            a = QAction(name, sub)
            a.setCheckable(True)
            a.triggered.connect(lambda _c=False, n=name: self._choose_hotkey(n))
            sub.addAction(a)
            self._hotkey_actions[name] = a

        self.act_auto = QAction("开机自动启动", m)
        self.act_auto.setCheckable(True)
        self.act_auto.setChecked(win_utils.is_autostart_enabled())
        self.act_auto.toggled.connect(self._toggle_autostart)
        act_trans = QAction("翻译服务…", m)
        act_trans.triggered.connect(self._config_translate)
        act_about = QAction(f"关于 {APP_NAME}", m)
        act_about.triggered.connect(self._about)
        act_quit = QAction("退出", m)
        act_quit.triggered.connect(self.quit)

        m.addAction(self.act_capture)
        m.addSeparator()
        m.addAction(act_trans)
        m.addAction(self.act_auto)
        m.addAction(act_about)
        m.addSeparator()
        m.addAction(act_quit)
        m.setStyleSheet(
            "QMenu{background:#2B2B2B;color:#EDEDED;border:1px solid #454545;padding:5px;}"
            "QMenu::item{padding:7px 26px;border-radius:4px;}"
            "QMenu::item:selected{background:#07C160;color:#ffffff;}"
            "QMenu::separator{height:1px;background:#454545;margin:4px 8px;}")
        self.menu = m
        self.tray.setContextMenu(m)

    def _toggle_autostart(self, on):
        if not win_utils.set_autostart(on):
            self.act_auto.setChecked(not on)

    def _config_translate(self):
        from PySide6.QtWidgets import (QDialog, QDialogButtonBox, QFormLayout,
                                       QLabel, QLineEdit)
        dlg = QDialog()
        dlg.setWindowTitle("翻译服务")
        dlg.setMinimumWidth(440)
        dlg.setStyleSheet(
            "QDialog{background:#2B2B2B;} QLabel{color:#DDDDDD;}"
            "QLineEdit{background:#1B1B1B;color:#EEEEEE;border:1px solid #454545;"
            "border-radius:4px;padding:5px;}")
        form = QFormLayout(dlg)
        tip = QLabel("留空 API Key 时用免费在线翻译（MyMemory）；\n"
                     "填写后改用 OpenAI 兼容的大模型，译文质量更好。")
        tip.setWordWrap(True)
        form.addRow(tip)
        e_base = QLineEdit(self.settings.get("llm_base", ""))
        e_key = QLineEdit(self.settings.get("llm_key", ""))
        e_key.setEchoMode(QLineEdit.EchoMode.Password)
        e_model = QLineEdit(self.settings.get("llm_model", ""))
        form.addRow("接口地址", e_base)
        form.addRow("API Key", e_key)
        form.addRow("模型名", e_model)
        bb = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok
                              | QDialogButtonBox.StandardButton.Cancel)
        bb.accepted.connect(dlg.accept)
        bb.rejected.connect(dlg.reject)
        form.addRow(bb)
        if dlg.exec():
            self.settings["llm_base"] = e_base.text().strip()
            self.settings["llm_key"] = e_key.text().strip()
            self.settings["llm_model"] = e_model.text().strip()
            save_settings(self.settings)

    def _on_tray(self, reason):
        if reason == QSystemTrayIcon.ActivationReason.Trigger:
            self.capture()
        elif reason == QSystemTrayIcon.ActivationReason.DoubleClick:
            self.capture()

    def _greet(self):
        tip = f"{self.hotkey_name} 截图" if self._hotkey_ok else "快捷键被占用，请从托盘菜单截图"
        self.tray.showMessage(APP_NAME, f"已在后台运行 · {tip}",
                              QSystemTrayIcon.MessageIcon.Information, 2500)

    def _about(self):
        hk = self.hotkey_name if self._hotkey_ok else "（不可用）"
        QMessageBox.information(
            None, f"关于 {APP_NAME}",
            f"<b>{APP_NAME}</b> · 微信风格的独立截图工具<br><br>"
            f"{hk} 截图　Enter 复制　Ctrl+S 保存<br>"
            "Ctrl+T 贴图　Ctrl+Z 撤销　Esc 取消<br><br>"
            "拖拽选区 / 单击自动框选窗口<br>"
            "R 矩形　E 椭圆　A 箭头　P 画笔<br>"
            "M 马赛克　T 文字　N 序号　C 换颜色<br>"
            "O 提取文字　F 翻译")

    # ------------------------------------------------------------ 截图
    def capture(self):
        # 兜底：万一覆盖层已关闭但引用没清掉，这里也要放行
        if self.overlay is not None and (getattr(self.overlay, "_closed", False)
                                         or not self.overlay.isVisible()):
            log("清理失效的覆盖层引用")
            self.overlay = None
        if self.overlay is not None:
            log("截图被忽略：覆盖层已存在")
            return
        if self._pending:
            log("截图被忽略：pending")
            return
        self._pending = True
        # 等托盘菜单/气泡收起后再抓屏，避免被抓进去
        QTimer.singleShot(160, self._do_capture)

    def _do_capture(self):
        self._pending = False
        if self.overlay is not None:
            log("已有覆盖层，忽略")
            return
        shot, origin, dpr = win_utils.capture_virtual_desktop()
        if shot is None or shot.isNull():
            log("抓屏失败")
            return
        log(f"抓屏 {shot.width()}x{shot.height()} dpr={dpr}")
        self.overlay = Overlay(shot, origin, dpr)
        self.overlay.done.connect(self._on_overlay_done)
        self.overlay.show()
        log("覆盖层已显示")

    def _on_overlay_done(self):
        ov = self.overlay
        self.overlay = None
        if ov is not None:
            ov.deleteLater()

    def quit(self):
        try:
            win_utils.unregister_hotkey(0, HOTKEY_ID)
        except Exception:
            pass
        if getattr(self, "_stop", None) is not None:
            self._stop.set()
        self.tray.hide()
        self.qapp.quit()


# ---------------------------------------------------------------- 自检模式
def selftest(out_dir):
    """渲染一次覆盖层与输出结果，便于离线校验视觉效果。"""
    shot, origin, dpr = win_utils.capture_virtual_desktop()
    ov = Overlay(shot, origin, dpr)
    ov.show()
    QApplication.processEvents()

    from annotations import ArrowAnn, EllipseAnn, MosaicAnn, NumberAnn, PenAnn, RectAnn, TextAnn
    sel = QRect(min(240, ov.width() // 6), min(180, ov.height() // 5), 780, 430)
    ov.sel = sel
    ov.annotations = [
        RectAnn(QPointF(30, 40), QPointF(300, 190), QColor("#FA5151")),
        EllipseAnn(QPointF(330, 40), QPointF(520, 190), QColor("#FFC300")),
        ArrowAnn(QPointF(560, 200), QPointF(730, 70), QColor("#FA5151")),
        TextAnn(QPointF(40, 240), "微信风格截图 · 简截", QColor("#FA5151"), 18),
        NumberAnn(QPointF(420, 260), 1, QColor("#07C160")),
        NumberAnn(QPointF(470, 260), 2, QColor("#07C160")),
    ]
    pen = PenAnn(QColor("#1E90FF"))
    for i in range(24):
        pen.add(QPointF(60 + i * 12, 330 + 26 * ((i % 6) / 5.0)))
    ov.annotations.append(pen)

    mos = MosaicAnn(QColor("#FA5151"))
    for i in range(20):
        mos.add(QPointF(560 + i * 9, 350))
    ov.annotations.append(mos)

    ov._place_toolbar()
    ov.toolbar.set_active("rect")
    ov.toolbar.show()
    ov.toolbar.raise_()
    ov.mag = QPointF(sel.center().x() + 60, sel.center().y() + 40)
    ov.update()
    QApplication.processEvents()
    QApplication.processEvents()

    os.makedirs(out_dir, exist_ok=True)
    ov.grab().save(os.path.join(out_dir, "overlay.png"))
    ov.render_result().save(os.path.join(out_dir, "result.png"))
    ov.close()
    print("selftest ok", shot.width(), shot.height(), "dpr=", dpr)
    return 0


def main():
    QGuiApplication.setHighDpiScaleFactorRoundingPolicy(
        Qt.HighDpiScaleFactorRoundingPolicy.PassThrough)

    app = QApplication(sys.argv)
    app.setApplicationName(APP_NAME)
    app.setOrganizationName("JianJie")
    app.setQuitOnLastWindowClosed(False)

    args = sys.argv[1:]
    if "--selftest" in args:
        idx = args.index("--selftest")
        out = args[idx + 1] if len(args) > idx + 1 else os.path.join(
            os.path.dirname(os.path.abspath(__file__)), "..", "build")
        return selftest(out)

    if "--ocr-test" in args:
        # 诊断用：简截.exe --ocr-test in.png out.txt
        import ocr as ocr_mod
        idx = args.index("--ocr-test")
        src = args[idx + 1]
        dst = args[idx + 2] if len(args) > idx + 2 else src + ".txt"
        with open(src, "rb") as f:
            text, err = ocr_mod.recognize(f.read())
        with open(dst, "w", encoding="utf-8") as f:
            f.write(text if not err else f"ERROR: {err}")
        return 0

    _handle, exists = win_utils.acquire_single_instance()
    if exists:
        # 已经在后台跑：直接让那个实例开始截图
        if win_utils.signal_activate():
            return 0
        QMessageBox.information(None, APP_NAME,
                                f"{APP_NAME} 已在后台运行，请点任务栏托盘图标截图。")
        return 0

    if not QSystemTrayIcon.isSystemTrayAvailable():
        QMessageBox.critical(None, APP_NAME, "系统托盘不可用，无法启动。")
        return 1

    jj = JianJie(app)
    if "--capture" in args:
        QTimer.singleShot(400, jj.capture)
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
