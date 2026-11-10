"""工具条几何参数。

必须和 cpp/src/overlay.cpp 里的 ITEM_W / TB_HEIGHT / TB_PAD / SEP_W 以及
TOOL_DEFS 的顺序保持一致 —— 测试靠它算按钮坐标，改了 UI 记得同步这里。
"""

ITEM_W, TB_HEIGHT, TB_PAD, SEP_W = 44, 48, 4, 16

TOOLS = [
    "rect", "ellipse", "arrow", "pen", "mosaic", "text", "number", "emoji", "long", "|",
    "trans", "ocr", "color", "|",
    "undo", "save", "pin", "settings", "cancel", "confirm",
]


def visible_tools(translate=True, ocr=True):
    """按设置主开关过滤后的实际工具条项。"""
    out = []
    for t in TOOLS:
        if t == "trans" and not translate:
            continue
        if t == "ocr" and not ocr:
            continue
        if t == "|" and (not out or out[-1] == "|"):
            continue
        out.append(t)
    return out


def width(translate=True, ocr=True):
    """整条工具条的宽度（逻辑像素，100% DPI）。"""
    return TB_PAD * 2 + sum(
        SEP_W if t == "|" else ITEM_W
        for t in visible_tools(translate, ocr)
    )


def cell_x(tool, translate=True, ocr=True):
    """按钮格子左边相对工具条左边缘的偏移。"""
    x = TB_PAD
    for t in visible_tools(translate, ocr):
        if t == tool:
            return x
        x += SEP_W if t == "|" else ITEM_W
    raise KeyError(tool)


def center(tool, tb_x, tb_y, translate=True, ocr=True):
    """按钮中心点的屏幕坐标。"""
    return (tb_x + cell_x(tool, translate, ocr) + ITEM_W // 2,
            tb_y + TB_HEIGHT // 2)
