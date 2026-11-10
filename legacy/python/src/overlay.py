"""全屏冻结覆盖层：选区、放大镜、窗口探测、标注、工具条。"""
import threading

from PySide6.QtCore import (QEvent, QObject, QPoint, QPointF, QRect, QRectF, QSize,
                            Qt, QTimer, Signal)
from PySide6.QtGui import (QColor, QCursor, QFont, QGuiApplication, QImage, QKeyEvent,
                           QMouseEvent, QPainter, QPainterPath, QPen)
from PySide6.QtWidgets import QLineEdit, QWidget

import ocr as ocr_mod
import translate as translate_mod
from annotations import (ArrowAnn, EllipseAnn, MosaicAnn, NumberAnn, PALETTE,
                         PenAnn, RectAnn, TextAnn)
from settings import load_settings
from textpanel import TextPanel
from toolbar import Toolbar
from win_utils import force_foreground, window_rect_at

GREEN = QColor("#07C160")
MASK = QColor(0, 0, 0, 122)
LABEL_BG = QColor(0, 0, 0, 170)

HANDLE = 7
HIT = 12
MIN_SIZE = 8

CURSORS = {
    "nw": Qt.CursorShape.SizeFDiagCursor,
    "se": Qt.CursorShape.SizeFDiagCursor,
    "ne": Qt.CursorShape.SizeBDiagCursor,
    "sw": Qt.CursorShape.SizeBDiagCursor,
    "n": Qt.CursorShape.SizeVerCursor,
    "s": Qt.CursorShape.SizeVerCursor,
    "e": Qt.CursorShape.SizeHorCursor,
    "w": Qt.CursorShape.SizeHorCursor,
}


class _Worker(QObject):
    """把耗时任务（OCR / 翻译）丢到后台线程，用信号回主线程。"""

    finished = Signal(object, object)

    def __init__(self, fn, parent=None):
        super().__init__(parent)
        self._fn = fn

    def start(self):
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        try:
            self.finished.emit(self._fn(), None)
        except BaseException as e:  # noqa: BLE001
            self.finished.emit(None, e)


class Overlay(QWidget):
    done = Signal()

    def __init__(self, shot: QImage, origin: QPoint, dpr: float):
        super().__init__(None, Qt.WindowType.FramelessWindowHint
                         | Qt.WindowType.WindowStaysOnTopHint
                         | Qt.WindowType.Tool)
        self.shot = shot
        self.dpr = dpr
        self.setMouseTracking(True)
        self.setCursor(Qt.CursorShape.CrossCursor)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose, False)
        self.setGeometry(origin.x(), origin.y(),
                         int(round(shot.width() / dpr)),
                         int(round(shot.height() / dpr)))

        # 状态
        self.sel = None                 # QRect 逻辑坐标（控件坐标）
        self.mode = None                # select / resize / move / draw
        self.drag_from = QPointF()
        self.sel_origin = QRect()
        self.resize_edge = ""
        self.mag = None                 # QPointF 光标位置
        self.hover_win = None           # QRect 窗口探测结果
        self.annotations = []
        self.current = None
        self.active_tool = None
        self.color = QColor(PALETTE[0])
        self.number_seq = 0
        self._mosaic = None
        self._mosaic_key = None
        self._text_editor = None
        self._text_pos = QPointF()
        self._closed = False
        self.force_magnifier = False
        self.panel = None
        self._workers = []

        self.toolbar = Toolbar(self)
        self.toolbar.action.connect(self._on_toolbar)
        self.toolbar.hide()

    # ------------------------------------------------------------ 生命周期
    def showEvent(self, e):
        super().showEvent(e)
        self.raise_()
        self.activateWindow()
        self.setFocus(Qt.FocusReason.ActiveWindowFocusReason)
        # 稍等一下再抢前台：窗口刚 show 出来时前台锁还没释放
        QTimer.singleShot(0, self._grab_focus)

    def _grab_focus(self):
        try:
            force_foreground(int(self.winId()))
        except Exception:
            pass
        self.activateWindow()
        self.setFocus(Qt.FocusReason.ActiveWindowFocusReason)

    def closeEvent(self, e):
        if not self._closed:
            self._closed = True
            self.done.emit()
        super().closeEvent(e)

    def finish(self):
        # 不要在这里置 _closed，否则 closeEvent 不再发 done，主程序无法回收覆盖层
        if self._closed:
            return
        self.close()
        if not self._closed:      # 极端情况下 close 没触发 closeEvent
            self._closed = True
            self.done.emit()

    # ------------------------------------------------------------ 选区辅助
    def _local(self, pt: QPointF) -> QPointF:
        return QPointF(pt.x() - self.sel.x(), pt.y() - self.sel.y())

    def _handles(self):
        if self.sel is None:
            return {}
        s = self.sel
        cx = s.center().x()
        cy = s.center().y()
        pts = {
            "nw": (s.left(), s.top()), "n": (cx, s.top()), "ne": (s.right(), s.top()),
            "e": (s.right(), cy), "se": (s.right(), s.bottom()),
            "s": (cx, s.bottom()), "sw": (s.left(), s.bottom()), "w": (s.left(), cy),
        }
        out = {}
        for k, (x, y) in pts.items():
            out[k] = QRect(int(x - HANDLE // 2), int(y - HANDLE // 2), HANDLE, HANDLE)
        return out

    def _handle_at(self, pt: QPointF):
        for k, r in self._handles().items():
            if r.adjusted(-HIT // 2, -HIT // 2, HIT // 2, HIT // 2).contains(pt.toPoint()):
                return k
        return None

    def _mosaic_image(self):
        key = (self.sel.x(), self.sel.y(), self.sel.width(), self.sel.height())
        if self._mosaic is not None and self._mosaic_key == key:
            return self._mosaic
        d = self.dpr
        src = self.shot.copy(QRect(int(self.sel.x() * d), int(self.sel.y() * d),
                                   max(1, int(self.sel.width() * d)),
                                   max(1, int(self.sel.height() * d))))
        w = max(1, int(self.sel.width()))
        h = max(1, int(self.sel.height()))
        sw = max(1, w // 9)
        sh = max(1, h // 9)
        small = src.scaled(sw, sh, Qt.AspectRatioMode.IgnoreAspectRatio,
                           Qt.TransformationMode.SmoothTransformation)
        self._mosaic = small.scaled(w, h, Qt.AspectRatioMode.IgnoreAspectRatio,
                                    Qt.TransformationMode.FastTransformation)
        self._mosaic_key = key
        return self._mosaic

    # ------------------------------------------------------------ 绘制
    def paintEvent(self, _e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform, False)
        p.drawImage(self.rect(), self.shot, self.shot.rect())

        sel = self.sel
        if sel is None or sel.width() < 1 or sel.height() < 1:
            p.fillRect(self.rect(), MASK)
            if self.hover_win is not None:
                self._paint_window_hint(p, self.hover_win)
            if self.mag is not None and self.mode == "select":
                self._paint_magnifier(p, self.mag)
            p.end()
            return

        # 选区外压暗
        r = self.rect()
        s = sel.intersected(r)
        for m in (QRect(r.left(), r.top(), r.width(), s.top() - r.top()),
                  QRect(r.left(), s.bottom() + 1, r.width(), r.bottom() - s.bottom()),
                  QRect(r.left(), s.top(), s.left() - r.left(), s.height()),
                  QRect(s.right() + 1, s.top(), r.right() - s.right(), s.height())):
            if m.width() > 0 and m.height() > 0:
                p.fillRect(m, MASK)

        # 标注
        p.save()
        p.setClipRect(sel)
        p.translate(sel.topLeft())
        mosaic = self._mosaic_image()
        for ann in self.annotations:
            ann.draw(p, mosaic)
        if self.current is not None:
            self.current.draw(p, mosaic)
        p.restore()

        # 边框
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.setPen(QPen(GREEN, 1))
        p.drawRect(QRectF(sel).adjusted(0.5, 0.5, -0.5, -0.5))

        # 控制点
        if self.mode is None and self.active_tool is None:
            p.setPen(QPen(GREEN, 1))
            p.setBrush(QColor("#FFFFFF"))
            for hr in self._handles().values():
                p.drawRect(QRectF(hr).adjusted(0.5, 0.5, -0.5, -0.5))

        self._paint_size_label(p, sel)

        if self.mag is not None and (self.mode in ("select", "resize")
                                     or self.force_magnifier):
            self._paint_magnifier(p, self.mag)
        p.end()

    def _paint_window_hint(self, p, rect):
        rr = QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5)
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.setPen(QPen(GREEN, 2))
        p.drawRect(rr)
        self._paint_size_label(p, rect, inside=True)

    def _paint_size_label(self, p, rect: QRect, inside=False):
        text = f"{rect.width()} × {rect.height()}"
        f = QFont("Microsoft YaHei UI", 9)
        p.setFont(f)
        fm = p.fontMetrics()
        w = fm.horizontalAdvance(text) + 14
        h = fm.height() + 6
        if inside:
            x = rect.left() + 4
            y = rect.top() + 4
        else:
            x = rect.left()
            y = rect.top() - h - 5
            if y < 2:
                x = rect.left() + 2
                y = rect.top() + 2
        x = max(2, min(x, self.width() - w - 2))
        box = QRectF(x, y, w, h)
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(LABEL_BG)
        p.drawRoundedRect(box, 3, 3)
        p.setPen(QColor("#FFFFFF"))
        p.drawText(box, Qt.AlignmentFlag.AlignCenter, text)

    def _paint_magnifier(self, p, pos: QPointF):
        R = 50.0
        zoom = 7.0
        cx = pos.x() + R + 26
        cy = pos.y() + R + 26
        if cx + R > self.width() - 2:
            cx = pos.x() - R - 26
        if cy + R > self.height() - 46:
            cy = pos.y() - R - 26
        cx = max(R + 2, min(cx, self.width() - R - 2))
        cy = max(R + 2, min(cy, self.height() - R - 2))

        d = self.dpr
        span = 2 * R / zoom
        src = QRectF(pos.x() * d - span / 2, pos.y() * d - span / 2, span, span)

        path = QPainterPath()
        path.addEllipse(QPointF(cx, cy), R, R)
        p.save()
        p.setClipPath(path)
        p.drawImage(QRectF(cx - R, cy - R, 2 * R, 2 * R), self.shot, src)
        p.restore()

        p.setBrush(Qt.BrushStyle.NoBrush)
        p.setPen(QPen(QColor(0, 0, 0, 105), 3.2))
        p.drawEllipse(QPointF(cx, cy), R + 1, R + 1)
        p.setPen(QPen(QColor(255, 255, 255, 235), 1.6))
        p.drawEllipse(QPointF(cx, cy), R, R)
        p.setPen(QPen(QColor(7, 193, 96, 210), 1))
        p.drawLine(QPointF(cx - R, cy), QPointF(cx + R, cy))
        p.drawLine(QPointF(cx, cy - R), QPointF(cx, cy + R))
        p.setPen(QPen(QColor(255, 255, 255, 180), 1))
        p.drawEllipse(QPointF(cx, cy), 1.6, 1.6)

        # 像素信息
        px = int(pos.x() * d)
        py = int(pos.y() * d)
        col = self.shot.pixelColor(px, py) if self.shot.rect().contains(px, py) else QColor("black")
        text = f"RGB({col.red()}, {col.green()}, {col.blue()})   ({int(pos.x())}, {int(pos.y())})"
        f = QFont("Consolas", 8)
        p.setFont(f)
        fm = p.fontMetrics()
        w = fm.horizontalAdvance(text) + 12
        h = fm.height() + 5
        x = max(2, min(cx - w / 2, self.width() - w - 2))
        y = cy + R + 6
        if y + h > self.height() - 2:
            y = cy - R - h - 6
        box = QRectF(x, y, w, h)
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(LABEL_BG)
        p.drawRoundedRect(box, 3, 3)
        p.setPen(QColor("#E8E8E8"))
        p.drawText(box, Qt.AlignmentFlag.AlignCenter, text)

    # ------------------------------------------------------------ 鼠标
    def mousePressEvent(self, e: QMouseEvent):
        if e.button() != Qt.MouseButton.LeftButton:
            return
        pos = e.position()
        self._commit_text()

        if self.sel is None or self.sel.width() < MIN_SIZE or self.sel.height() < MIN_SIZE:
            self._begin_select(pos)
            return

        edge = self._handle_at(pos)
        if edge:
            self.mode = "resize"
            self.resize_edge = edge
            self.drag_from = pos
            self.sel_origin = QRect(self.sel)
            self.toolbar.hide()
            return

        if self.sel.contains(pos.toPoint()):
            if self.active_tool in ("rect", "ellipse", "arrow", "pen", "mosaic"):
                self.mode = "draw"
                local = self._local(pos)
                if self.active_tool == "rect":
                    self.current = RectAnn(local, local, self.color)
                elif self.active_tool == "ellipse":
                    self.current = EllipseAnn(local, local, self.color)
                elif self.active_tool == "arrow":
                    self.current = ArrowAnn(local, local, self.color)
                elif self.active_tool == "pen":
                    self.current = PenAnn(self.color)
                    self.current.add(local)
                else:
                    self.current = MosaicAnn(self.color)
                    self.current.add(local)
                self.toolbar.hide()
                return
            if self.active_tool == "text":
                self._begin_text(pos)
                return
            if self.active_tool == "number":
                self.number_seq += 1
                self.annotations.append(NumberAnn(self._local(pos), self.number_seq, self.color))
                self.update()
                return
            self.mode = "move"
            self.drag_from = pos
            self.sel_origin = QRect(self.sel)
            self.toolbar.hide()
            return

        self._begin_select(pos)

    def _begin_select(self, pos: QPointF):
        self.mode = "select"
        self.drag_from = pos
        self.sel = QRect(pos.toPoint(), QSize(1, 1))
        self.annotations = []
        self.current = None
        self.number_seq = 0
        self.hover_win = None
        self.toolbar.hide()
        if self.panel is not None:
            self.panel.hide()
        self.update()

    def mouseMoveEvent(self, e: QMouseEvent):
        pos = e.position()
        self.mag = pos

        if self.mode == "select":
            self.sel = QRect(self.drag_from.toPoint(), pos.toPoint()).normalized()
            self._mosaic = None
        elif self.mode == "resize":
            self._apply_resize(pos)
        elif self.mode == "move":
            dx = int(pos.x() - self.drag_from.x())
            dy = int(pos.y() - self.drag_from.y())
            self.sel = self.sel_origin.translated(dx, dy)
        elif self.mode == "draw" and self.current is not None:
            local = self._local(pos)
            if isinstance(self.current, (RectAnn, EllipseAnn)):
                self.current.rect = QRectF(self.current.rect.topLeft(), local).normalized()
            elif isinstance(self.current, ArrowAnn):
                self.current.b = local
            elif isinstance(self.current, (PenAnn, MosaicAnn)):
                self.current.add(local)
        else:
            if self.sel is None:
                self.hover_win = window_rect_at(pos.toPoint())
            elif self.sel.contains(pos.toPoint()):
                self.hover_win = None
            else:
                self.hover_win = None
        self._update_cursor(pos)
        self.update()

    def _apply_resize(self, pos: QPointF):
        dx = int(pos.x() - self.drag_from.x())
        dy = int(pos.y() - self.drag_from.y())
        r = QRect(self.sel_origin)
        if "w" in self.resize_edge:
            r.setLeft(self.sel_origin.left() + dx)
        if "e" in self.resize_edge:
            r.setRight(self.sel_origin.right() + dx)
        if "n" in self.resize_edge:
            r.setTop(self.sel_origin.top() + dy)
        if "s" in self.resize_edge:
            r.setBottom(self.sel_origin.bottom() + dy)
        r = r.normalized()
        if r.width() >= MIN_SIZE and r.height() >= MIN_SIZE:
            self.sel = r
            self._mosaic = None

    def mouseReleaseEvent(self, e: QMouseEvent):
        if e.button() != Qt.MouseButton.LeftButton:
            return
        pos = e.position()
        if self.mode == "select":
            self.sel = QRect(self.drag_from.toPoint(), pos.toPoint()).normalized()
            if self.sel.width() < MIN_SIZE or self.sel.height() < MIN_SIZE:
                if self.hover_win is not None:
                    self.sel = QRect(self.hover_win)
                else:
                    self.sel = QRect(0, 0, self.width(), self.height())
            self._mosaic = None
        elif self.mode == "draw":
            if self.current is not None:
                b = self.current.bounds()
                if b.width() > 2 or b.height() > 2 or isinstance(self.current, (PenAnn, MosaicAnn)):
                    self.annotations.append(self.current)
                self.current = None
        self.mode = None
        self.resize_edge = ""
        if self.sel is not None:
            self._place_toolbar()
            self.toolbar.show()
            self.toolbar.raise_()
        self._update_cursor(pos)
        self.update()

    def mouseDoubleClickEvent(self, e: QMouseEvent):
        if self.sel is not None and self.sel.contains(e.position().toPoint()):
            self.copy_and_close()

    def _update_cursor(self, pos: QPointF):
        if self.sel is None:
            self.setCursor(Qt.CursorShape.CrossCursor)
            return
        edge = self._handle_at(pos) if self.active_tool is None else None
        if edge:
            self.setCursor(CURSORS[edge])
        elif self.sel.contains(pos.toPoint()):
            if self.active_tool is None:
                self.setCursor(Qt.CursorShape.SizeAllCursor)
            else:
                self.setCursor(Qt.CursorShape.CrossCursor)
        else:
            self.setCursor(Qt.CursorShape.CrossCursor)

    # ------------------------------------------------------------ 键盘
    def keyPressEvent(self, e: QKeyEvent):
        key = e.key()
        mods = e.modifiers()
        ctrl = bool(mods & Qt.KeyboardModifier.ControlModifier)

        if key == Qt.Key.Key_Escape:
            if self._text_editor is not None:
                self._cancel_text()
            else:
                self.finish()
            return
        if key in (Qt.Key.Key_Return, Qt.Key.Key_Enter):
            self.copy_and_close()
            return
        if ctrl and key == Qt.Key.Key_C:
            self.copy_and_close()
            return
        if ctrl and key == Qt.Key.Key_S:
            self.save()
            return
        if ctrl and key == Qt.Key.Key_Z:
            self.undo()
            return
        if ctrl and key == Qt.Key.Key_T:
            self.pin()
            return
        if key == Qt.Key.Key_C and not ctrl:
            self._cycle_color()
            return
        if self.sel is not None and key in (Qt.Key.Key_Left, Qt.Key.Key_Right,
                                            Qt.Key.Key_Up, Qt.Key.Key_Down):
            step = 10 if (mods & Qt.KeyboardModifier.ShiftModifier) else 1
            dx = dy = 0
            if key == Qt.Key.Key_Left:
                dx = -step
            elif key == Qt.Key.Key_Right:
                dx = step
            elif key == Qt.Key.Key_Up:
                dy = -step
            else:
                dy = step
            self.sel = self.sel.translated(dx, dy)
            self._place_toolbar()
            self.update()
            return

        mapping = {Qt.Key.Key_R: "rect", Qt.Key.Key_E: "ellipse", Qt.Key.Key_A: "arrow",
                   Qt.Key.Key_P: "pen", Qt.Key.Key_M: "mosaic", Qt.Key.Key_T: "text",
                   Qt.Key.Key_N: "number"}
        if key in mapping and self.sel is not None:
            self._set_tool(mapping[key])
            return
        if self.sel is not None and key == Qt.Key.Key_O:
            self.do_ocr()
            return
        if self.sel is not None and key == Qt.Key.Key_F:
            self.do_translate()
            return
        super().keyPressEvent(e)

    def _cycle_color(self):
        idx = 0
        for i, c in enumerate(PALETTE):
            if c.name() == self.color.name():
                idx = (i + 1) % len(PALETTE)
                break
        self.color = QColor(PALETTE[idx])
        self.toolbar.set_color(self.color)

    def _set_tool(self, tool):
        self.active_tool = None if self.active_tool == tool else tool
        self.toolbar.set_active(self.active_tool)
        self._update_cursor(self.mag or QPointF())
        self.update()

    # ------------------------------------------------------------ 工具条
    def _place_toolbar(self):
        tb = self.toolbar
        w, h = tb.width(), tb.height()
        x = self.sel.center().x() - w // 2
        y = self.sel.bottom() + 8
        if y + h > self.height() - 2:
            y = self.sel.top() - h - 8
        x = max(4, min(x, self.width() - w - 4))
        y = max(4, min(y, self.height() - h - 4))
        tb.move(x, y)

    def _on_toolbar(self, key):
        if key == "undo":
            self.undo()
        elif key == "save":
            self.save()
        elif key == "copy":
            self.copy_and_close()
        elif key == "pin":
            self.pin()
        elif key == "cancel":
            self.finish()
        elif key == "ocr":
            self.do_ocr()
        elif key == "trans":
            self.do_translate()
        elif key == "color":
            self.color = QColor(self.toolbar._color)
        else:
            self._set_tool(key)

    def undo(self):
        if self.annotations:
            self.annotations.pop()
            if any(isinstance(a, NumberAnn) for a in self.annotations):
                self.number_seq = max(a.index for a in self.annotations
                                      if isinstance(a, NumberAnn))
            else:
                self.number_seq = 0
            self.update()

    # ------------------------------------------------------------ 文字
    def _begin_text(self, pos: QPointF):
        self._commit_text()
        self._text_pos = self._local(pos)
        ed = QLineEdit(self)
        ed.setStyleSheet(
            "QLineEdit{background:rgba(0,0,0,110);border:1px dashed #07C160;"
            f"color:{self.color.name()};padding:1px 3px;"
            "font-family:'Microsoft YaHei UI';font-size:14px;}")
        ed.move(int(pos.x()), int(pos.y()) - 4)
        width = max(90, min(360, self.sel.right() - int(pos.x()) - 4))
        ed.resize(width, 26)
        ed.show()
        ed.setFocus()
        ed.returnPressed.connect(self._commit_text)
        ed.installEventFilter(self)
        self._text_editor = ed

    def eventFilter(self, obj, ev):
        if obj is self._text_editor:
            if ev.type() == QEvent.Type.KeyPress and ev.key() == Qt.Key.Key_Escape:
                self._cancel_text()
                return True
            if ev.type() == QEvent.Type.FocusOut:
                self._commit_text()
        return super().eventFilter(obj, ev)

    def _commit_text(self):
        ed = self._text_editor
        if ed is None:
            return
        text = ed.text()
        self._text_editor = None
        ed.hide()
        ed.deleteLater()
        if text.strip():
            self.annotations.append(TextAnn(self._text_pos, text, self.color))
        self.setFocus()
        self.update()

    def _cancel_text(self):
        ed = self._text_editor
        self._text_editor = None
        if ed is not None:
            ed.hide()
            ed.deleteLater()
        self.setFocus()
        self.update()

    # ------------------------------------------------------------ 输出
    def render_result(self) -> QImage:
        d = self.dpr
        sel = self.sel
        dev = QRect(int(sel.x() * d), int(sel.y() * d),
                    max(1, int(sel.width() * d)), max(1, int(sel.height() * d)))
        base = self.shot.copy(dev)
        out = QImage(base.size(), QImage.Format.Format_ARGB32_Premultiplied)
        out.fill(Qt.GlobalColor.transparent)
        p = QPainter(out)
        p.drawImage(0, 0, base)
        p.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        p.scale(d, d)
        mosaic = self._mosaic_image()
        for ann in self.annotations:
            ann.draw(p, mosaic)
        p.end()
        out.setDevicePixelRatio(d)
        return out

    def copy_and_close(self):
        if self.sel is None:
            self.finish()
            return
        img = self.render_result()
        QGuiApplication.clipboard().setImage(img)
        self.finish()

    def save(self):
        from PySide6.QtCore import QStandardPaths
        from PySide6.QtWidgets import QFileDialog
        from datetime import datetime
        import os
        img = self.render_result()
        base = QStandardPaths.writableLocation(
            QStandardPaths.StandardLocation.PicturesLocation) or os.path.expanduser("~")
        folder = os.path.join(base, "简截")
        os.makedirs(folder, exist_ok=True)
        name = datetime.now().strftime("简截_%Y%m%d_%H%M%S.png")
        path, _ = QFileDialog.getSaveFileName(self, "保存截图",
                                              os.path.join(folder, name),
                                              "PNG 图片 (*.png);;JPEG 图片 (*.jpg)")
        if not path:
            return
        img.save(path)
        self.finish()

    def pin(self):
        if self.sel is None:
            return
        from pinwindow import PinWindow
        img = self.render_result()
        pos = self.mapToGlobal(self.sel.topLeft())
        w = PinWindow(img)
        w.move(pos)
        w.show()
        self.finish()

    # ------------------------------------------------------------ 提取文字 / 翻译
    def _ensure_panel(self) -> TextPanel:
        if self.panel is None:
            self.panel = TextPanel(self)
            self.panel.translate_requested.connect(self.do_translate)
            self.panel.closed.connect(self.setFocus)
        return self.panel

    def _place_panel(self):
        p = self.panel
        if p is None or self.sel is None:
            return
        w = p.width()
        h = max(200, min(self.sel.height(), 360))
        p.resize(w, h)
        x = self.sel.right() + 10
        y = self.sel.top()
        if x + w > self.width() - 6:
            x = self.sel.left() - w - 10
        if x < 6:
            x = max(6, min(self.sel.center().x() - w // 2, self.width() - w - 6))
            y = self.sel.bottom() + 10
            if y + h > self.height() - 6:
                y = self.sel.top() - h - 10
        y = max(6, min(y, self.height() - h - 6))
        p.move(x, y)

    def _crop_png(self) -> bytes:
        from PySide6.QtCore import QBuffer, QByteArray, QIODevice
        d = self.dpr
        sel = self.sel
        dev = QRect(int(sel.x() * d), int(sel.y() * d),
                    max(1, int(sel.width() * d)), max(1, int(sel.height() * d)))
        img = self.shot.copy(dev)
        # 小图放大再识别，小字号的中文准确率会明显提高
        if img.width() <= 1400 and img.height() <= 1000:
            img = img.scaled(img.width() * 2, img.height() * 2,
                             Qt.AspectRatioMode.IgnoreAspectRatio,
                             Qt.TransformationMode.SmoothTransformation)
        ba = QByteArray()
        buf = QBuffer(ba)
        buf.open(QIODevice.OpenModeFlag.WriteOnly)
        img.save(buf, "PNG")
        buf.close()
        return bytes(ba)

    def _run_job(self, fn, on_done):
        w = _Worker(fn, self)

        def _done(result, err):
            if w in self._workers:
                self._workers.remove(w)
            on_done(result, err)

        w.finished.connect(_done)
        self._workers.append(w)
        w.start()

    def _show_panel(self, title):
        panel = self._ensure_panel()
        panel.show_loading(title)
        self._place_panel()
        panel.show()
        panel.raise_()
        return panel

    def do_ocr(self, translate_too=False):
        if self.sel is None:
            return
        self._show_panel("翻译" if translate_too else "提取文字")
        png = self._crop_png()
        cfg = load_settings()

        def job():
            text, err = ocr_mod.recognize(png)
            if err or not translate_too or not text.strip():
                return text, err, None
            tr, src, dst, terr = translate_mod.translate(text, cfg)
            return text, err, (tr, src, dst, terr)

        self._run_job(job, self._ocr_done)

    def _ocr_done(self, result, err):
        panel = self.panel
        if panel is None:
            return
        if err is not None:
            panel.show_error(str(err))
            return
        text, oerr, tr = result
        if oerr:
            panel.show_error(oerr)
            return
        if not text.strip():
            panel.show_error("没有识别到文字")
            return
        panel.show_text(text)
        if tr is not None:
            t, src, dst, terr = tr
            if terr:
                panel.hint.setText("翻译失败")
            else:
                panel.show_translation(t, src, dst)

    def do_translate(self):
        if self.sel is None:
            return
        panel = self._ensure_panel()
        text = (panel._original or panel.current_text()).strip()
        if not text:
            self.do_ocr(translate_too=True)
            return
        panel.hint.setText("翻译中…")
        panel.btn_trans.setEnabled(False)
        self._place_panel()
        panel.show()
        panel.raise_()
        cfg = load_settings()
        self._run_job(lambda: translate_mod.translate(text, cfg), self._trans_done)

    def _trans_done(self, result, err):
        panel = self.panel
        if panel is None:
            return
        panel.btn_trans.setEnabled(True)
        if err is not None:
            panel.hint.setText("翻译失败")
            panel.show_error(str(err), "翻译")
            return
        t, src, dst, terr = result
        if terr:
            panel.show_error(terr, "翻译")
            return
        panel.show_translation(t, src, dst)
