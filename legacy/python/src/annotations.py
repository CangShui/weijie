"""标注图形模型与绘制（复刻微信截图的标注体验）。

所有几何坐标均为「覆盖层逻辑坐标」，渲染时由 Overlay 负责 DPR 缩放。
"""
import math

from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import (QBrush, QColor, QFont, QFontMetricsF, QImage, QPainter,
                           QPainterPath, QPainterPathStroker, QPen, QPolygonF)

# 微信截图标注的默认色板
PALETTE = [
    QColor("#FA5151"),  # 红
    QColor("#FFC300"),  # 黄
    QColor("#07C160"),  # 绿
    QColor("#1E90FF"),  # 蓝
    QColor("#FFFFFF"),  # 白
    QColor("#000000"),  # 黑
]


class Annotation:
    kind = "base"

    def __init__(self, color, width=3):
        self.color = QColor(color)
        self.width = width

    def _pen(self):
        pen = QPen(self.color, self.width)
        pen.setCapStyle(Qt.PenCapStyle.RoundCap)
        pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
        return pen

    def draw(self, p: QPainter, mosaic: QImage = None):
        raise NotImplementedError

    def bounds(self) -> QRectF:
        return QRectF()


class RectAnn(Annotation):
    kind = "rect"

    def __init__(self, a, b, color, width=3):
        super().__init__(color, width)
        self.rect = QRectF(a, b).normalized()

    def draw(self, p, mosaic=None):
        p.setPen(self._pen())
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawRect(self.rect)

    def bounds(self):
        return self.rect.adjusted(-self.width, -self.width, self.width, self.width)


class EllipseAnn(Annotation):
    kind = "ellipse"

    def __init__(self, a, b, color, width=3):
        super().__init__(color, width)
        self.rect = QRectF(a, b).normalized()

    def draw(self, p, mosaic=None):
        p.setPen(self._pen())
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawEllipse(self.rect)

    def bounds(self):
        return self.rect.adjusted(-self.width, -self.width, self.width, self.width)


class ArrowAnn(Annotation):
    kind = "arrow"

    def __init__(self, a, b, color, width=3):
        super().__init__(color, width)
        self.a = QPointF(a)
        self.b = QPointF(b)

    def draw(self, p, mosaic=None):
        dx = self.b.x() - self.a.x()
        dy = self.b.y() - self.a.y()
        dist = math.hypot(dx, dy)
        if dist < 2:
            return
        ang = math.atan2(dy, dx)
        head = max(9.0, self.width * 3.6)
        p1 = QPointF(self.b.x() - head * math.cos(ang - 0.42),
                     self.b.y() - head * math.sin(ang - 0.42))
        p2 = QPointF(self.b.x() - head * math.cos(ang + 0.42),
                     self.b.y() - head * math.sin(ang + 0.42))
        tail = QPointF(self.b.x() - head * 0.55 * math.cos(ang),
                       self.b.y() - head * 0.55 * math.sin(ang))

        p.setPen(self._pen())
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawLine(self.a, tail)
        p.setBrush(QBrush(self.color))
        p.setPen(Qt.PenStyle.NoPen)
        p.drawPolygon(QPolygonF([self.b, p1, p2]))

    def bounds(self):
        return QRectF(self.a, self.b).normalized().adjusted(-10, -10, 10, 10)


class PenAnn(Annotation):
    kind = "pen"

    def __init__(self, color, width=3):
        super().__init__(color, width)
        self.points = []

    def add(self, pt):
        self.points.append(QPointF(pt))

    def draw(self, p, mosaic=None):
        if not self.points:
            return
        p.setPen(self._pen())
        p.setBrush(Qt.BrushStyle.NoBrush)
        if len(self.points) == 1:
            p.setBrush(QBrush(self.color))
            p.drawEllipse(self.points[0], self.width / 2, self.width / 2)
            return
        path = QPainterPath(self.points[0])
        for pt in self.points[1:]:
            path.lineTo(pt)
        p.drawPath(path)

    def bounds(self):
        if not self.points:
            return QRectF()
        r = QRectF(self.points[0], self.points[0])
        for pt in self.points[1:]:
            r = r.united(QRectF(pt, pt))
        return r.adjusted(-self.width, -self.width, self.width, self.width)


class MosaicAnn(Annotation):
    """马赛克：沿笔迹生成描边轮廓，用该轮廓裁剪「像素化底图」。"""

    kind = "mosaic"

    def __init__(self, color, width=24):
        super().__init__(color, width)
        self.points = []

    def add(self, pt):
        self.points.append(QPointF(pt))

    def draw(self, p, mosaic=None):
        if mosaic is None or not self.points:
            return
        path = QPainterPath(self.points[0])
        if len(self.points) == 1:
            path.lineTo(self.points[0] + QPointF(0.01, 0.01))
        else:
            for pt in self.points[1:]:
                path.lineTo(pt)
        stroker = QPainterPathStroker()
        stroker.setWidth(self.width)
        stroker.setCapStyle(Qt.PenCapStyle.RoundCap)
        stroker.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
        outline = stroker.createStroke(path)
        p.save()
        p.setClipPath(outline, Qt.ClipOperation.IntersectClip)
        p.drawImage(0, 0, mosaic)
        p.restore()

    def bounds(self):
        if not self.points:
            return QRectF()
        r = QRectF(self.points[0], self.points[0])
        for pt in self.points[1:]:
            r = r.united(QRectF(pt, pt))
        return r.adjusted(-self.width, -self.width, self.width, self.width)


class TextAnn(Annotation):
    kind = "text"

    def __init__(self, pos, text, color, size=15):
        super().__init__(color, 1)
        self.pos = QPointF(pos)
        self.text = text
        self.size = size

    def _font(self):
        f = QFont("Microsoft YaHei UI", self.size)
        f.setHintingPreference(QFont.HintingPreference.PreferFullHinting)
        return f

    def draw(self, p, mosaic=None):
        p.setFont(self._font())
        p.setPen(self.color)
        fm = QFontMetricsF(p.font())
        lines = self.text.split("\n")
        y = self.pos.y() + fm.ascent()
        for line in lines:
            p.drawText(QPointF(self.pos.x(), y), line)
            y += fm.lineSpacing()

    def bounds(self):
        fm = QFontMetricsF(self._font())
        lines = self.text.split("\n")
        w = max([fm.horizontalAdvance(l) for l in lines] + [1])
        h = fm.lineSpacing() * len(lines)
        return QRectF(self.pos.x(), self.pos.y(), w, h)


class NumberAnn(Annotation):
    """序号标记：实心圆 + 白色数字。"""

    kind = "number"

    def __init__(self, pos, index, color, radius=12):
        super().__init__(color, 1)
        self.pos = QPointF(pos)
        self.index = index
        self.radius = radius

    def draw(self, p, mosaic=None):
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QBrush(self.color))
        p.drawEllipse(self.pos, self.radius, self.radius)
        f = QFont("Arial", int(self.radius * 1.05))
        f.setBold(True)
        p.setFont(f)
        p.setPen(QColor("#FFFFFF"))
        p.drawText(QRectF(self.pos.x() - self.radius, self.pos.y() - self.radius,
                          self.radius * 2, self.radius * 2),
                   Qt.AlignmentFlag.AlignCenter, str(self.index))

    def bounds(self):
        return QRectF(self.pos.x() - self.radius, self.pos.y() - self.radius,
                      self.radius * 2, self.radius * 2)
