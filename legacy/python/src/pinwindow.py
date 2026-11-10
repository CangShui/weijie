"""贴图窗口：把截图钉在屏幕上，可拖动、滚轮缩放、右键菜单。"""
import os
from datetime import datetime

from PySide6.QtCore import QPoint, QRectF, Qt
from PySide6.QtGui import QColor, QGuiApplication, QImage, QPainter, QPen
from PySide6.QtWidgets import QFileDialog, QMenu, QWidget

# 保持引用，避免贴图窗口被 Python 回收
_ALIVE = []


class PinWindow(QWidget):
    def __init__(self, img: QImage):
        super().__init__(None, Qt.WindowType.FramelessWindowHint
                         | Qt.WindowType.WindowStaysOnTopHint
                         | Qt.WindowType.Tool)
        _ALIVE.append(self)
        self.img = img
        dpr = img.devicePixelRatio() or 1.0
        self.base_w = max(1, int(round(img.width() / dpr)))
        self.base_h = max(1, int(round(img.height() / dpr)))
        self.scale = 1.0
        self._drag = None
        self._hover = False
        self.setMouseTracking(True)
        self.setCursor(Qt.CursorShape.OpenHandCursor)
        self.setWindowTitle("贴图")
        self.resize(self.base_w, self.base_h)

    def closeEvent(self, e):
        if self in _ALIVE:
            _ALIVE.remove(self)
        super().closeEvent(e)

    # ------------------------------------------------------------ 绘制
    def paintEvent(self, _e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform, True)
        p.drawImage(QRectF(self.rect()), self.img, QRectF(self.img.rect()))
        if self._hover:
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.setPen(QPen(QColor("#07C160"), 1))
            p.drawRect(QRectF(self.rect()).adjusted(0.5, 0.5, -0.5, -0.5))
        p.end()

    def enterEvent(self, e):
        self._hover = True
        self.update()

    def leaveEvent(self, e):
        self._hover = False
        self.update()

    # ------------------------------------------------------------ 交互
    def mousePressEvent(self, e):
        if e.button() == Qt.MouseButton.LeftButton:
            self._drag = e.globalPosition().toPoint() - self.frameGeometry().topLeft()
            self.setCursor(Qt.CursorShape.ClosedHandCursor)
            e.accept()
        elif e.button() == Qt.MouseButton.RightButton:
            self._menu(e.globalPosition().toPoint())

    def mouseMoveEvent(self, e):
        if self._drag is not None and e.buttons() & Qt.MouseButton.LeftButton:
            self.move(e.globalPosition().toPoint() - self._drag)

    def mouseReleaseEvent(self, e):
        self._drag = None
        self.setCursor(Qt.CursorShape.OpenHandCursor)

    def mouseDoubleClickEvent(self, e):
        if e.button() == Qt.MouseButton.LeftButton:
            self.close()

    def wheelEvent(self, e):
        step = e.angleDelta().y()
        if step == 0:
            return
        old = self.scale
        new = old * (1.1 if step > 0 else 1 / 1.1)
        new = max(0.1, min(8.0, new))
        if abs(new - old) < 1e-6:
            return
        local = e.position()
        anchor = (local.x() / old, local.y() / old)   # 光标下的图像坐标
        self.scale = new
        self.resize(max(1, int(self.base_w * new)), max(1, int(self.base_h * new)))
        new_local = QPoint(int(anchor[0] * new), int(anchor[1] * new))
        self.move(e.globalPosition().toPoint() - new_local)
        self.update()

    def keyPressEvent(self, e):
        if e.key() == Qt.Key.Key_Escape:
            self.close()
        elif e.key() == Qt.Key.Key_C and e.modifiers() & Qt.KeyboardModifier.ControlModifier:
            QGuiApplication.clipboard().setImage(self.img)
        else:
            super().keyPressEvent(e)

    # ------------------------------------------------------------ 菜单
    def _menu(self, gpos: QPoint):
        m = QMenu(self)
        m.setStyleSheet("QMenu{background:#2B2B2B;color:#EDEDED;border:1px solid #444;"
                        "padding:4px;} QMenu::item{padding:6px 22px;border-radius:4px;}"
                        "QMenu::item:selected{background:#07C160;color:#fff;}")
        a_copy = m.addAction("复制图片")
        a_save = m.addAction("另存为…")
        m.addSeparator()
        a_close = m.addAction("关闭贴图")
        act = m.exec(gpos)
        if act == a_copy:
            QGuiApplication.clipboard().setImage(self.img)
        elif act == a_save:
            self._save()
        elif act == a_close:
            self.close()

    def _save(self):
        folder = os.path.join(os.path.expanduser("~"), "Pictures", "简截")
        os.makedirs(folder, exist_ok=True)
        name = datetime.now().strftime("贴图_%Y%m%d_%H%M%S.png")
        path, _ = QFileDialog.getSaveFileName(self, "保存贴图",
                                              os.path.join(folder, name),
                                              "PNG 图片 (*.png)")
        if path:
            self.img.save(path)
