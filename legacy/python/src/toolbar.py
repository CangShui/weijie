"""微信风格截图工具条（自绘图标，无外部资源依赖）。"""
import math

from PySide6.QtCore import QPointF, QRect, QRectF, QSize, Qt, Signal
from PySide6.QtGui import (QBrush, QColor, QFont, QPainter, QPainterPath, QPen,
                           QPolygonF)
from PySide6.QtWidgets import QToolTip, QWidget

from annotations import PALETTE

BG = QColor(31, 31, 31, 242)
BORDER = QColor(255, 255, 255, 20)
HOVER = QColor(255, 255, 255, 28)
ACTIVE = QColor(7, 193, 96, 46)
GREEN = QColor("#07C160")
ICON = QColor("#F2F2F2")
DIM = QColor("#8C8C8C")

ITEM_W = 36
HEIGHT = 44
PAD = 6
ICON_BOX = 22


def _pen(color, w=1.8):
    p = QPen(color, w)
    p.setCapStyle(Qt.PenCapStyle.RoundCap)
    p.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
    return p


# ------------------------------------------------------------------ 图标
def _i_rect(p, a, c):
    p.setPen(_pen(c))
    p.setBrush(Qt.BrushStyle.NoBrush)
    p.drawRect(a.adjusted(2.5, 4.5, -2.5, -4.5))


def _i_ellipse(p, a, c):
    p.setPen(_pen(c))
    p.setBrush(Qt.BrushStyle.NoBrush)
    p.drawEllipse(a.adjusted(2.5, 4.5, -2.5, -4.5))


def _i_arrow(p, a, c):
    x0, y0 = a.left() + 3.5, a.bottom() - 3.5
    x1, y1 = a.right() - 4.0, a.top() + 4.0
    ang = math.atan2(y1 - y0, x1 - x0)
    head = 6.4
    p.setPen(_pen(c, 1.9))
    p.drawLine(QPointF(x0, y0), QPointF(x1 - head * 0.6 * math.cos(ang),
                                        y1 - head * 0.6 * math.sin(ang)))
    p.setBrush(QBrush(c))
    p.setPen(Qt.PenStyle.NoPen)
    p.drawPolygon(QPolygonF([
        QPointF(x1, y1),
        QPointF(x1 - head * math.cos(ang - 0.42), y1 - head * math.sin(ang - 0.42)),
        QPointF(x1 - head * math.cos(ang + 0.42), y1 - head * math.sin(ang + 0.42)),
    ]))


def _i_pen(p, a, c):
    path = QPainterPath(QPointF(a.left() + 3.0, a.bottom() - 4.0))
    path.cubicTo(QPointF(a.left() + 6.0, a.top() + 2.0),
                 QPointF(a.left() + 11.0, a.bottom() - 1.0),
                 QPointF(a.left() + 13.0, a.top() + 6.0))
    path.cubicTo(QPointF(a.left() + 15.0, a.top() + 3.0),
                 QPointF(a.left() + 17.5, a.top() + 5.0),
                 QPointF(a.right() - 2.5, a.top() + 3.0))
    p.setPen(_pen(c, 1.9))
    p.setBrush(Qt.BrushStyle.NoBrush)
    p.drawPath(path)


def _i_mosaic(p, a, c):
    p.setPen(Qt.PenStyle.NoPen)
    cell = 5.0
    gap = 1.2
    x0 = a.left() + 2.5
    y0 = a.top() + 2.5
    for row in range(3):
        for col in range(3):
            if (row + col) % 2 == 0:
                continue
            p.setBrush(QBrush(c))
            p.drawRect(QRectF(x0 + col * (cell + gap), y0 + row * (cell + gap),
                              cell, cell))


def _i_text(p, a, c):
    p.setPen(_pen(c, 2.0))
    p.drawLine(QPointF(a.left() + 5.0, a.top() + 6.0),
               QPointF(a.right() - 5.0, a.top() + 6.0))
    p.drawLine(QPointF(a.center().x(), a.top() + 6.0),
               QPointF(a.center().x(), a.bottom() - 4.5))


def _i_number(p, a, c):
    p.setPen(_pen(c, 1.7))
    p.setBrush(Qt.BrushStyle.NoBrush)
    p.drawEllipse(a.adjusted(3.5, 3.5, -3.5, -3.5))
    cx, cy = a.center().x(), a.center().y()
    p.setPen(_pen(c, 1.5))
    p.drawLine(QPointF(cx - 1.6, cy - 2.6), QPointF(cx, cy - 4.2))
    p.drawLine(QPointF(cx, cy - 4.2), QPointF(cx, cy + 4.2))


def _i_undo(p, a, c):
    p.setPen(_pen(c, 1.8))
    p.setBrush(Qt.BrushStyle.NoBrush)
    r = a.adjusted(4.0, 5.0, -4.0, -5.0)
    p.drawArc(r, 40 * 16, 270 * 16)
    p.setBrush(QBrush(c))
    p.setPen(Qt.PenStyle.NoPen)
    p.drawPolygon(QPolygonF([
        QPointF(r.left() - 0.5, r.center().y() - 3.0),
        QPointF(r.left() + 5.0, r.center().y() - 3.0),
        QPointF(r.left() + 2.2, r.center().y() + 2.0),
    ]))


def _i_save(p, a, c):
    p.setPen(_pen(c, 1.8))
    p.setBrush(Qt.BrushStyle.NoBrush)
    cx = a.center().x()
    p.drawLine(QPointF(cx, a.top() + 3.5), QPointF(cx, a.bottom() - 8.0))
    p.setBrush(QBrush(c))
    p.setPen(Qt.PenStyle.NoPen)
    p.drawPolygon(QPolygonF([
        QPointF(cx, a.bottom() - 5.0),
        QPointF(cx - 4.0, a.bottom() - 10.0),
        QPointF(cx + 4.0, a.bottom() - 10.0),
    ]))
    p.setPen(_pen(c, 1.8))
    p.setBrush(Qt.BrushStyle.NoBrush)
    p.drawLine(QPointF(a.left() + 4.0, a.bottom() - 3.0),
               QPointF(a.right() - 4.0, a.bottom() - 3.0))


def _i_copy(p, a, c):
    p.setPen(_pen(c, 1.6))
    p.setBrush(Qt.BrushStyle.NoBrush)
    p.drawRoundedRect(QRectF(a.left() + 3.0, a.top() + 3.0, 11.0, 11.0), 2, 2)
    p.drawRoundedRect(QRectF(a.left() + 7.0, a.top() + 7.5, 11.0, 11.0), 2, 2)


def _i_pin(p, a, c):
    p.setPen(_pen(c, 1.6))
    p.setBrush(Qt.BrushStyle.NoBrush)
    p.drawRoundedRect(QRectF(a.left() + 3.0, a.top() + 5.5, 13.0, 11.0), 2, 2)
    p.setPen(_pen(c, 1.5))
    p.drawLine(QPointF(a.left() + 5.5, a.top() + 13.0),
               QPointF(a.left() + 9.5, a.top() + 8.5))
    p.setBrush(QBrush(c))
    p.setPen(Qt.PenStyle.NoPen)
    p.drawEllipse(QPointF(a.left() + 15.0, a.top() + 4.5), 2.6, 2.6)


def _i_cancel(p, a, c):
    p.setPen(_pen(c, 1.9))
    p.drawLine(QPointF(a.left() + 5.5, a.top() + 5.5),
               QPointF(a.right() - 5.5, a.bottom() - 5.5))
    p.drawLine(QPointF(a.right() - 5.5, a.top() + 5.5),
               QPointF(a.left() + 5.5, a.bottom() - 5.5))


def _i_ocr(p, a, c):
    """取景框 + 中间一个 T：提取文字。"""
    L, T = a.left() + 3.0, a.top() + 3.0
    R, B = a.right() - 3.0, a.bottom() - 3.0
    k = 4.6
    p.setPen(_pen(c, 1.7))
    p.setBrush(Qt.BrushStyle.NoBrush)
    p.drawLine(QPointF(L, T + k), QPointF(L, T))
    p.drawLine(QPointF(L, T), QPointF(L + k, T))
    p.drawLine(QPointF(R - k, T), QPointF(R, T))
    p.drawLine(QPointF(R, T), QPointF(R, T + k))
    p.drawLine(QPointF(R, B - k), QPointF(R, B))
    p.drawLine(QPointF(R, B), QPointF(R - k, B))
    p.drawLine(QPointF(L + k, B), QPointF(L, B))
    p.drawLine(QPointF(L, B), QPointF(L, B - k))

    cx, cy = a.center().x(), a.center().y()
    p.setPen(_pen(c, 1.6))
    p.drawLine(QPointF(cx - 4.2, cy - 3.6), QPointF(cx + 4.2, cy - 3.6))
    p.drawLine(QPointF(cx, cy - 3.6), QPointF(cx, cy + 4.0))


def _i_trans(p, a, c):
    """左「文」右「A」：翻译。"""
    cx = a.left() + 6.6
    top = a.top() + 5.2
    p.setPen(_pen(c, 1.45))
    p.drawPoint(QPointF(cx, top))
    p.drawLine(QPointF(cx - 4.0, top + 3.2), QPointF(cx + 4.0, top + 3.2))
    p.drawLine(QPointF(cx - 3.4, top + 6.0), QPointF(cx + 3.4, top + 11.2))
    p.drawLine(QPointF(cx + 3.4, top + 6.0), QPointF(cx - 3.4, top + 11.2))

    bx = a.right() - 6.4
    btop = a.top() + 5.6
    p.setPen(_pen(c, 1.55))
    p.drawLine(QPointF(bx - 3.6, btop + 10.4), QPointF(bx, btop))
    p.drawLine(QPointF(bx, btop), QPointF(bx + 3.6, btop + 10.4))
    p.drawLine(QPointF(bx - 2.0, btop + 6.4), QPointF(bx + 2.0, btop + 6.4))


ICONS = {
    "rect": _i_rect,
    "ellipse": _i_ellipse,
    "arrow": _i_arrow,
    "pen": _i_pen,
    "mosaic": _i_mosaic,
    "text": _i_text,
    "number": _i_number,
    "ocr": _i_ocr,
    "trans": _i_trans,
    "undo": _i_undo,
    "save": _i_save,
    "copy": _i_copy,
    "pin": _i_pin,
    "cancel": _i_cancel,
}

TOOLS = [
    ("rect", "矩形  R"),
    ("ellipse", "椭圆  E"),
    ("arrow", "箭头  A"),
    ("pen", "画笔  P"),
    ("mosaic", "马赛克  M"),
    ("text", "文字  T"),
    ("number", "序号  N"),
    ("sep", ""),
    ("ocr", "提取文字  O"),
    ("trans", "翻译  F"),
    ("color", "标注颜色  C"),
    ("sep", ""),
    ("undo", "撤销  Ctrl+Z"),
    ("save", "保存  Ctrl+S"),
    ("copy", "复制  Enter"),
    ("pin", "贴图  Ctrl+T"),
    ("cancel", "取消  Esc"),
]


class Toolbar(QWidget):
    action = Signal(str)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setMouseTracking(True)
        self.setAttribute(Qt.WidgetAttribute.WA_NoSystemBackground, True)
        self._items = []
        self._hover = -1
        self._active = None
        self._color = QColor("#FA5151")
        self._layout_items()

    # ---------------------------------------------------------- 布局
    def _layout_items(self):
        self._items = []
        x = PAD
        for key, tip in TOOLS:
            if key == "sep":
                self._items.append({"key": "sep", "tip": "", "rect": QRect(x + 2, 10, 1, HEIGHT - 20)})
                x += 5
                continue
            self._items.append({"key": key, "tip": tip,
                                "rect": QRect(x, (HEIGHT - ITEM_W) // 2 + 4, ITEM_W, ITEM_W - 4)})
            x += ITEM_W
        self._width = x + PAD
        self.setFixedSize(self._width, HEIGHT)

    def sizeHint(self):
        return QSize(self._width, HEIGHT)

    # ---------------------------------------------------------- 状态
    def set_active(self, key):
        self._active = key
        self.update()

    def set_color(self, color: QColor):
        self._color = QColor(color)
        self.update()

    def _hit(self, pos):
        for i, it in enumerate(self._items):
            if it["key"] != "sep" and it["rect"].adjusted(-2, -4, 2, 4).contains(pos):
                return i
        return -1

    # ---------------------------------------------------------- 绘制
    def paintEvent(self, _e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing, True)

        r = QRectF(self.rect()).adjusted(0.5, 0.5, -0.5, -0.5)
        p.setPen(QPen(BORDER, 1))
        p.setBrush(QBrush(BG))
        p.drawRoundedRect(r, 9, 9)

        for i, it in enumerate(self._items):
            key = it["key"]
            if key == "sep":
                p.setPen(QPen(QColor(255, 255, 255, 26), 1))
                rr = it["rect"]
                p.drawLine(rr.left(), rr.top() + 2, rr.left(), rr.bottom() - 2)
                continue

            cell = it["rect"]
            if i == self._hover:
                p.setPen(Qt.PenStyle.NoPen)
                p.setBrush(QBrush(HOVER))
                p.drawRoundedRect(QRectF(cell).adjusted(-1, -2, 1, 2), 6, 6)

            color = ICON
            if self._active == key:
                p.setPen(Qt.PenStyle.NoPen)
                p.setBrush(QBrush(ACTIVE))
                p.drawRoundedRect(QRectF(cell).adjusted(-1, -2, 1, 2), 6, 6)
                color = GREEN
            if key in ("undo", "save", "copy", "pin", "cancel", "ocr", "trans"):
                color = QColor("#FFFFFF") if i == self._hover else ICON

            box = QRectF(cell.center().x() - ICON_BOX / 2,
                         cell.center().y() - ICON_BOX / 2, ICON_BOX, ICON_BOX)
            if key == "color":
                p.setPen(QPen(QColor(255, 255, 255, 190), 1.3))
                p.setBrush(QBrush(self._color))
                p.drawEllipse(box.adjusted(5.5, 5.5, -5.5, -5.5))
                continue
            ICONS[key](p, box, color)
        p.end()

    # ---------------------------------------------------------- 交互
    def mouseMoveEvent(self, e):
        i = self._hit(e.position().toPoint())
        if i != self._hover:
            self._hover = i
            self.update()
            if i >= 0:
                it = self._items[i]
                QToolTip.showText(e.globalPosition().toPoint(), it["tip"], self)
            else:
                QToolTip.hideText()

    def leaveEvent(self, _e):
        self._hover = -1
        QToolTip.hideText()
        self.update()

    def mousePressEvent(self, e):
        if e.button() != Qt.MouseButton.LeftButton:
            return
        i = self._hit(e.position().toPoint())
        if i < 0:
            return
        key = self._items[i]["key"]
        if key == "color":
            idx = 0
            for n, c in enumerate(PALETTE):
                if c.name() == self._color.name():
                    idx = (n + 1) % len(PALETTE)
                    break
            self.set_color(PALETTE[idx])
            self.action.emit("color")
            return
        self.action.emit(key)
