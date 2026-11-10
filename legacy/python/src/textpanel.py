"""提取文字 / 翻译 的结果面板。"""
from PySide6.QtCore import QEvent, QRectF, Qt, Signal
from PySide6.QtGui import QColor, QFont, QPainter, QPen
from PySide6.QtWidgets import (QHBoxLayout, QLabel, QPushButton, QTextEdit,
                               QToolButton, QVBoxLayout, QWidget)

BG = QColor(30, 30, 30, 246)
BORDER = QColor(255, 255, 255, 26)
GREEN = "#07C160"

STYLE = f"""
QTextEdit {{
    background:#141414; color:#EDEDED; border:none; border-radius:6px;
    padding:6px; font-family:'Microsoft YaHei UI'; font-size:13px;
    selection-background-color:{GREEN};
}}
QLabel#title {{ color:#F0F0F0; font-family:'Microsoft YaHei UI'; font-size:13px; }}
QLabel#hint  {{ color:#8A8A8A; font-family:'Microsoft YaHei UI'; font-size:11px; }}
QPushButton {{
    background:#2E2E2E; color:#DCDCDC; border:none; border-radius:5px;
    padding:5px 12px; font-family:'Microsoft YaHei UI'; font-size:12px;
}}
QPushButton:hover {{ background:#3A3A3A; }}
QPushButton#primary {{ background:{GREEN}; color:#FFFFFF; }}
QPushButton#primary:hover {{ background:#06AD56; }}
QToolButton {{ background:transparent; color:#9A9A9A; border:none; font-size:16px; }}
QToolButton:hover {{ color:#FFFFFF; }}
QScrollBar:vertical {{ background:transparent; width:8px; margin:2px; }}
QScrollBar::handle:vertical {{ background:#3D3D3D; border-radius:4px; min-height:24px; }}
QScrollBar::add-line, QScrollBar::sub-line {{ height:0; }}
"""


class TextPanel(QWidget):
    closed = Signal()
    translate_requested = Signal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setAttribute(Qt.WidgetAttribute.WA_NoSystemBackground, True)
        self.setStyleSheet(STYLE)
        self._original = ""
        self._translated = False

        root = QVBoxLayout(self)
        root.setContentsMargins(12, 10, 12, 10)
        root.setSpacing(8)

        head = QHBoxLayout()
        head.setSpacing(6)
        self.title = QLabel("提取文字")
        self.title.setObjectName("title")
        self.hint = QLabel("")
        self.hint.setObjectName("hint")
        self.btn_close = QToolButton()
        self.btn_close.setText("×")
        self.btn_close.setCursor(Qt.CursorShape.PointingHandCursor)
        self.btn_close.clicked.connect(self.close_panel)
        head.addWidget(self.title)
        head.addStretch(1)
        head.addWidget(self.hint)
        head.addWidget(self.btn_close)
        root.addLayout(head)

        self.text = QTextEdit()
        self.text.setReadOnly(True)
        self.text.setFrameShape(QTextEdit.Shape.NoFrame)
        self.text.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        self.text.installEventFilter(self)
        root.addWidget(self.text, 1)

        foot = QHBoxLayout()
        foot.setSpacing(8)
        self.btn_copy = QPushButton("复制文字")
        self.btn_copy.clicked.connect(self._copy)
        self.btn_trans = QPushButton("翻译")
        self.btn_trans.setObjectName("primary")
        self.btn_trans.clicked.connect(self.translate_requested.emit)
        self.btn_orig = QPushButton("显示原文")
        self.btn_orig.clicked.connect(self._toggle_original)
        self.btn_orig.hide()
        foot.addStretch(1)
        foot.addWidget(self.btn_orig)
        foot.addWidget(self.btn_copy)
        foot.addWidget(self.btn_trans)
        root.addLayout(foot)

        self.setFixedWidth(340)
        self.resize(340, 300)

    # ------------------------------------------------------------ 绘制
    def paintEvent(self, _e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        r = QRectF(self.rect()).adjusted(0.5, 0.5, -0.5, -0.5)
        p.setPen(QPen(BORDER, 1))
        p.setBrush(BG)
        p.drawRoundedRect(r, 9, 9)
        p.end()

    # ------------------------------------------------------------ 状态
    def show_loading(self, title="提取文字"):
        self.title.setText(title)
        self.hint.setText("处理中…")
        self.text.setPlainText("")
        self.text.setPlaceholderText("正在识别…")
        self.btn_trans.setEnabled(False)
        self.btn_copy.setEnabled(False)
        self.btn_orig.hide()
        self._translated = False

    def show_text(self, text, title="提取文字"):
        self.title.setText(title)
        self.hint.setText(f"{len(text)} 字")
        self.text.setPlainText(text)
        self._original = text
        self._translated = False
        self.btn_trans.setEnabled(bool(text.strip()))
        self.btn_copy.setEnabled(bool(text.strip()))
        self.btn_orig.hide()

    def show_error(self, msg, title="提取文字"):
        self.title.setText(title)
        self.hint.setText("失败")
        self.text.setPlainText(msg)
        self.btn_trans.setEnabled(False)
        self.btn_copy.setEnabled(False)
        self.btn_orig.hide()

    def show_translation(self, translated, src, dst):
        name = {"zh-CN": "中文", "en": "英文"}
        self.title.setText(f"翻译 · {name.get(src, src)} → {name.get(dst, dst)}")
        self.hint.setText(f"{len(translated)} 字")
        self.text.setPlainText(translated)
        self._translated = True
        self.btn_orig.show()
        self.btn_trans.setEnabled(True)
        self.btn_copy.setEnabled(True)

    def _toggle_original(self):
        if self._translated:
            self.text.setPlainText(self._original)
            self._translated = False
            self.btn_orig.setText("显示译文")
        else:
            self.btn_orig.setText("显示原文")
            self.translate_requested.emit()

    def current_text(self):
        return self.text.toPlainText()

    def _copy(self):
        from PySide6.QtGui import QGuiApplication
        QGuiApplication.clipboard().setText(self.current_text())
        self.btn_copy.setText("已复制")
        self.btn_copy.setEnabled(False)

    def close_panel(self):
        self.closed.emit()
        self.hide()

    def eventFilter(self, obj, ev):
        if obj is self.text and ev.type() == QEvent.Type.KeyPress:
            if ev.key() == Qt.Key.Key_Escape:
                self.close_panel()
                return True
        return super().eventFilter(obj, ev)
