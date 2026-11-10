#include "overlay.h"
#include "ocr.h"
#include "ocr_setup.h"
#include "longshot.h"
#include "style_popup.h"
#include "emoji_popup.h"
#include "fast_ocr.h"
#include "translate.h"
#include "pin.h"
#include "output.h"
#include "ui_theme.h"
#include <imm.h>
#include <memory>
#include <mutex>
#include <commctrl.h>

#pragma comment(lib, "imm32.lib")

using namespace Gdiplus;

// ------------------------------------------------------------------ 常量
// 尺寸对齐微信截图工具条的比例：高度 48 时图标 20、格子 44、分隔 16
#define ITEM_W     44
#define TB_HEIGHT  48
#define TB_PAD     4
#define SEP_W      16
#define ICON_BOX   20
#define HANDLE_SZ  7
#define HANDLE_HIT 12
#define MIN_SEL    8

static const wchar_t* OV_CLASS = L"weijieOverlay";
static const UINT WM_JJ_OCR = WM_APP + 22;
static const UINT WM_JJ_TRANS = WM_APP + 23;

// 工具条走浅色：底色 #FAFAFA、图标 #4C4C4C、分隔线 #E0E0E0，
// 与微信截图一致（浮在压暗的桌面上，浅色反而更清楚）。
static Color C_TB_BG(255, 250, 250, 250);
static Color C_TB_EDGE(255, 226, 226, 226);
static Color C_TB_LINE(255, 224, 224, 224);
static Color C_TB_ICON(255, 76, 76, 76);
static Color C_TB_HOVER(255, 236, 236, 236);
static Color C_TB_SEL_BG(255, 231, 248, 239);
static Color C_TB_SEL_ICON(255, 7, 193, 96);
static Color C_TB_OFF(255, 208, 208, 208);
static Color C_TB_RED(255, 250, 81, 81);
static Color C_TB_RED_BG(255, 255, 240, 240);
static Color C_TB_GREEN(255, 0, 195, 117);
static Color C_TB_GREEN_BG(255, 232, 249, 241);

static Color C_GREEN(255, 7, 193, 96);
static Color C_LABEL_BG(170, 0, 0, 0);

static std::shared_ptr<Overlay> g_ov;
static HBITMAP g_backBuf = nullptr;
static HDC g_backDC = nullptr;
static int g_bw = 0, g_bh = 0;

struct JobMsg {
    bool ok = false;
    std::wstring text;
    std::wstring err;
    TranslateResult tr;
    bool translated = false;
};

Overlay::~Overlay() {
    if(resultFont)DeleteObject(resultFont);
    free_shot(shot);
    delete mosaic;
    mosaic = nullptr;
}

// ------------------------------------------------------------------ 小工具
static void force_foreground(HWND h) {
    ShowWindow(h, SW_RESTORE);
    HWND fg = GetForegroundWindow();
    DWORD cur = GetCurrentThreadId();
    DWORD other = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    bool attached = false;
    if (other && other != cur) attached = !!AttachThreadInput(other, cur, TRUE);
    SetForegroundWindow(h);
    SetFocus(h);
    if (attached) AttachThreadInput(other, cur, FALSE);
}

static RectF handle_rect(const RECT& s, int which, float sz) {
    float cx = 0, cy = 0;
    float l = (float)s.left, t = (float)s.top, r = (float)s.right, b = (float)s.bottom;
    switch (which) {
    case 0: cx = l; cy = t; break;                 // nw
    case 1: cx = (l + r) / 2; cy = t; break;       // n
    case 2: cx = r; cy = t; break;                 // ne
    case 3: cx = r; cy = (t + b) / 2; break;       // e
    case 4: cx = r; cy = b; break;                 // se
    case 5: cx = (l + r) / 2; cy = b; break;       // s
    case 6: cx = l; cy = b; break;                 // sw
    default: cx = l; cy = (t + b) / 2; break;      // w
    }
    return RectF(cx - sz / 2, cy - sz / 2, sz, sz);
}

static LPCWSTR HANDLE_CURSORS[8] = {
    IDC_SIZENWSE, IDC_SIZENS, IDC_SIZENESW, IDC_SIZEWE,
    IDC_SIZENWSE, IDC_SIZENS, IDC_SIZENESW, IDC_SIZEWE,
};

static int handle_at(const Overlay* o, POINT pt) {
    if (!o->hasSel) return -1;
    float sz = SF((float)HANDLE_SZ);
    float hit = SF((float)HANDLE_HIT);
    for (int i = 0; i < 8; ++i) {
        RectF r = handle_rect(o->sel, i, sz);
        RectF big(r.X - hit / 2 + sz / 2, r.Y - hit / 2 + sz / 2, hit, hit);
        if (ptInRectF(big, (float)pt.x, (float)pt.y)) return i;
    }
    return -1;
}

static Bitmap* ensure_mosaic(Overlay* o) {
    if (!o->hasSel) return nullptr;
    if (o->sel.right - o->sel.left < 2 || o->sel.bottom - o->sel.top < 2) return nullptr;
    if (o->mosaic && EqualRect(&o->mosaicKey, &o->sel)) return o->mosaic;

    delete o->mosaic;
    o->mosaic = nullptr;
    o->mosaicKey = o->sel;

    int w = o->sel.right - o->sel.left;
    int h = o->sel.bottom - o->sel.top;
    Bitmap* crop = o->shot.bmp->Clone(o->sel.left, o->sel.top, w, h, PixelFormat32bppRGB);
    if (!crop) return nullptr;
    o->mosaic = make_mosaic(crop);
    delete crop;
    return o->mosaic;
}

// ------------------------------------------------------------------ 图标
// 微信截图的图标是 1.5px 细线、20×20 见方、深灰 #4C4C4C。
// 这里统一在 20×20 的虚拟画布上量坐标，再按实际盒子等比缩放，
// 换尺寸或换 DPI 都不用重算一堆偏移量。
struct Ink {
    float x = 0, y = 0, u = 1;
    explicit Ink(const RectF& a) : x(a.X), y(a.Y), u(a.Width / 20.0f) {}
    float X(float v) const { return x + v * u; }
    float Y(float v) const { return y + v * u; }
    float S(float v) const { return v * u; }
};

static void setup_icon_pen(Pen& pen, Color c, float w) {
    pen.SetColor(c);
    pen.SetWidth(w);
    pen.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
    pen.SetLineJoin(LineJoinRound);
}

static void ink_pen(Pen& pen, Color c, const Ink& k, float w = 1.5f) {
    setup_icon_pen(pen, c, k.S(w));
}

static void round_path(GraphicsPath& p, const RectF& r, float rad) {
    p.AddArc(r.X, r.Y, rad * 2, rad * 2, 180, 90);
    p.AddArc(r.X + r.Width - rad * 2, r.Y, rad * 2, rad * 2, 270, 90);
    p.AddArc(r.X + r.Width - rad * 2, r.Y + r.Height - rad * 2, rad * 2, rad * 2, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - rad * 2, rad * 2, rad * 2, 90, 90);
    p.CloseFigure();
}

// 在图标里居中写一个小字（翻译图标的 A / 文）
static void ink_glyph(Graphics& g, const Ink& k, const wchar_t* s, float size,
                      float cx, float cy, Color c) {
    Font f(FONT_UI(), k.S(size), FontStyleRegular, UnitPixel);
    StringFormat fmt;
    fmt.SetAlignment(StringAlignmentCenter);
    fmt.SetLineAlignment(StringAlignmentCenter);
    SolidBrush br(c);
    RectF box(k.X(cx) - k.S(6.0f), k.Y(cy) - k.S(5.0f), k.S(12.0f), k.S(10.0f));
    g.DrawString(s, -1, &f, box, &fmt, &br);
}

static void i_rect(Graphics& g, const RectF& a, Color c) {
    Ink k(a);
    Pen pen(Color(255, 0, 0, 0), 1);
    ink_pen(pen, c, k);
    GraphicsPath p;
    round_path(p, RectF(k.X(3.2f), k.Y(3.6f), k.S(13.6f), k.S(12.8f)), k.S(2.0f));
    g.DrawPath(&pen, &p);
}

static void i_ellipse(Graphics& g, const RectF& a, Color c) {
    Ink k(a);
    Pen pen(Color(255, 0, 0, 0), 1);
    ink_pen(pen, c, k);
    g.DrawEllipse(&pen, k.X(3.2f), k.Y(3.2f), k.S(13.6f), k.S(13.6f));
}

static void i_arrow(Graphics& g, const RectF& a, Color c) {
    Ink k(a);
    Pen pen(Color(255, 0, 0, 0), 1);
    ink_pen(pen, c, k, 1.6f);
    const float x0 = k.X(4.0f), y0 = k.Y(16.4f);
    const float x1 = k.X(16.2f), y1 = k.Y(4.2f);
    const float ang = atan2f(y1 - y0, x1 - x0);
    const float L = k.S(5.4f);
    g.DrawLine(&pen, PointF(x0, y0), PointF(x1, y1));
    g.DrawLine(&pen, PointF(x1, y1),
               PointF(x1 - L * cosf(ang - 0.5f), y1 - L * sinf(ang - 0.5f)));
    g.DrawLine(&pen, PointF(x1, y1),
               PointF(x1 - L * cosf(ang + 0.5f), y1 - L * sinf(ang + 0.5f)));
}

static void i_pen(Graphics& g, const RectF& a, Color c) {
    Ink k(a);
    Pen pen(Color(255, 0, 0, 0), 1);
    ink_pen(pen, c, k);
    GraphicsPath p;
    p.AddLine(k.X(3.4f), k.Y(16.6f), k.X(5.4f), k.Y(11.2f));
    p.AddLine(k.X(5.4f), k.Y(11.2f), k.X(13.0f), k.Y(3.6f));
    p.AddLine(k.X(13.0f), k.Y(3.6f), k.X(16.4f), k.Y(7.0f));
    p.AddLine(k.X(16.4f), k.Y(7.0f), k.X(8.8f), k.Y(14.6f));
    p.CloseFigure();
    g.DrawPath(&pen, &p);
    g.DrawLine(&pen, k.X(5.4f), k.Y(11.2f), k.X(8.8f), k.Y(14.6f));   // 笔尖分界
}

static void i_mosaic(Graphics& g, const RectF& a, Color c) {
    Ink k(a);
    Pen pen(Color(255, 0, 0, 0), 1);
    ink_pen(pen, c, k);
    const RectF box(k.X(3.0f), k.Y(3.0f), k.S(14.0f), k.S(14.0f));
    const float cell = k.S(3.0f), step = k.S(3.5f);
    {
        GraphicsState st = g.Save();
        GraphicsPath clip;
        round_path(clip, box, k.S(1.6f));
        g.SetClip(&clip);
        SolidBrush br(c);
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col)
                if ((row + col) % 2 == 0)
                    g.FillRectangle(&br, box.X + col * step, box.Y + row * step, cell, cell);
        g.Restore(st);
    }
    GraphicsPath p;
    round_path(p, box, k.S(1.6f));
    g.DrawPath(&pen, &p);
}

static void i_text(Graphics& g, const RectF& a, Color c) {
    Ink k(a);
    Pen pen(Color(255, 0, 0, 0), 1);
    ink_pen(pen, c, k);
    GraphicsPath p;
    round_path(p, RectF(k.X(3.0f), k.Y(3.0f), k.S(14.0f), k.S(14.0f)), k.S(2.0f));
    g.DrawPath(&pen, &p);
    g.DrawLine(&pen, k.X(6.6f), k.Y(6.8f), k.X(13.4f), k.Y(6.8f));
    g.DrawLine(&pen, k.X(10.0f), k.Y(6.8f), k.X(10.0f), k.Y(14.2f));
}

static void i_number(Graphics& g, const RectF& a, Color c) {
    Ink k(a);
    Pen pen(Color(255, 0, 0, 0), 1);
    ink_pen(pen, c, k);
    g.DrawEllipse(&pen, k.X(3.0f), k.Y(3.0f), k.S(14.0f), k.S(14.0f));
    g.DrawLine(&pen, k.X(8.6f), k.Y(7.6f), k.X(10.2f), k.Y(6.2f));
    g.DrawLine(&pen, k.X(10.2f), k.Y(6.2f), k.X(10.2f), k.Y(13.8f));
}

static void i_emoji(Graphics& g, const RectF& a, Color c) {
    Ink k(a);
    Pen pen(Color(255, 0, 0, 0), 1);
    ink_pen(pen, c, k, 1.4f);
    g.DrawEllipse(&pen, k.X(3.0f), k.Y(3.0f), k.S(14.0f), k.S(14.0f));
    SolidBrush br(c);
    g.FillEllipse(&br, k.X(6.7f), k.Y(7.4f), k.S(1.7f), k.S(1.7f));
    g.FillEllipse(&br, k.X(11.6f), k.Y(7.4f), k.S(1.7f), k.S(1.7f));
    GraphicsPath smile;
    smile.AddArc(k.X(6.2f), k.Y(8.8f), k.S(7.6f), k.S(5.4f), 10, 160);
    g.DrawPath(&pen, &smile);
}

// 翻译：两个错开的圆角方框，后面的写 A，前面的写 文
static void i_trans(Graphics& g, const RectF& a, Color c, Color bg) {
    Ink k(a);
    Pen pen(Color(255, 0, 0, 0), 1);
    ink_pen(pen, c, k, 1.3f);

    const RectF back(k.X(7.6f), k.Y(2.4f), k.S(9.4f), k.S(7.8f));
    GraphicsPath pb;
    round_path(pb, back, k.S(1.5f));
    g.DrawPath(&pen, &pb);
    ink_glyph(g, k, L"A", 7.4f, 12.3f, 6.3f, c);

    const RectF front(k.X(3.0f), k.Y(9.4f), k.S(9.4f), k.S(7.8f));
    GraphicsPath pf;
    round_path(pf, front, k.S(1.5f));
    SolidBrush fill(bg);          // 用底色盖掉后面方框的重叠部分
    g.FillPath(&fill, &pf);
    g.DrawPath(&pen, &pf);
    ink_glyph(g, k, L"文", 7.4f, 7.7f, 13.3f, c);
}

// 提取文字：虚线扫描框 + 文
static void i_ocr(Graphics& g, const RectF& a, Color c) {
    Ink k(a);
    Pen pen(Color(255, 0, 0, 0), 1);
    ink_pen(pen, c, k, 1.4f);
    const float L = k.X(3.0f), T = k.Y(3.0f), R = k.X(17.0f), B = k.Y(17.0f);
    const float arm = k.S(5.2f);
    // 四角括号，每边中间留缺口 —— 微信那个「虚线扫描框」的样子
    g.DrawLine(&pen, L, T + arm, L, T);
    g.DrawLine(&pen, L, T, L + arm, T);
    g.DrawLine(&pen, R - arm, T, R, T);
    g.DrawLine(&pen, R, T, R, T + arm);
    g.DrawLine(&pen, R, B - arm, R, B);
    g.DrawLine(&pen, R, B, R - arm, B);
    g.DrawLine(&pen, L + arm, B, L, B);
    g.DrawLine(&pen, L, B, L, B - arm);
    ink_glyph(g, k, L"文", 8.0f, 10.0f, 10.0f, c);
}

static void i_long(Graphics& g, const RectF& a, Color c) {
    Ink k(a);
    Pen pen(Color(255, 0, 0, 0), 1);
    ink_pen(pen, c, k, 1.5f);
    g.DrawRectangle(&pen, k.X(4.0f), k.Y(3.0f), k.S(12.0f), k.S(14.0f));
    g.DrawLine(&pen, k.X(7.0f), k.Y(3.0f), k.X(7.0f), k.Y(17.0f));
    g.DrawLine(&pen, k.X(7.0f), k.Y(17.0f), k.X(5.0f), k.Y(15.0f));
    g.DrawLine(&pen, k.X(7.0f), k.Y(17.0f), k.X(9.0f), k.Y(15.0f));
}

// 撤销：底部一条短横当尾巴，绕右侧兜上去，箭头收在左上方朝左
static void i_undo(Graphics& g, const RectF& a, Color c) {
    Ink k(a);
    Pen pen(Color(255, 0, 0, 0), 1);
    ink_pen(pen, c, k);
    // 圆心 (10.5,10.5) 半径 5.2：从正下方逆时针扫 250°，绕右侧到左上
    g.DrawArc(&pen, k.X(5.3f), k.Y(5.3f), k.S(10.4f), k.S(10.4f), 90.0f, -250.0f);
    g.DrawLine(&pen, k.X(4.4f), k.Y(15.7f), k.X(10.5f), k.Y(15.7f));
    g.DrawLine(&pen, k.X(5.6f), k.Y(8.7f), k.X(9.6f), k.Y(7.2f));
    g.DrawLine(&pen, k.X(5.6f), k.Y(8.7f), k.X(9.6f), k.Y(10.4f));
}

// 保存：圆角方框（顶边留缺口）+ 一支粗箭头插进去
static void i_save(Graphics& g, const RectF& a, Color c) {
    Ink k(a);
    Pen pen(Color(255, 0, 0, 0), 1);
    ink_pen(pen, c, k, 1.5f);
    GraphicsPath p;
    p.AddArc(k.X(12.4f), k.Y(5.0f), k.S(4.2f), k.S(4.2f), 270, 90);   // 右上角
    p.AddLine(k.X(16.6f), k.Y(7.1f), k.X(16.6f), k.Y(14.4f));          // 右
    p.AddArc(k.X(12.4f), k.Y(12.3f), k.S(4.2f), k.S(4.2f), 0, 90);     // 右下角
    p.AddLine(k.X(14.5f), k.Y(16.5f), k.X(5.5f), k.Y(16.5f));          // 下
    p.AddArc(k.X(3.4f), k.Y(12.3f), k.S(4.2f), k.S(4.2f), 90, 90);     // 左下角
    p.AddLine(k.X(3.4f), k.Y(14.4f), k.X(3.4f), k.Y(7.1f));            // 左
    p.AddArc(k.X(3.4f), k.Y(5.0f), k.S(4.2f), k.S(4.2f), 180, 90);     // 左上角
    p.AddLine(k.X(5.5f), k.Y(5.0f), k.X(7.6f), k.Y(5.0f));             // 上边左段
    g.DrawPath(&pen, &p);

    Pen shaft(Color(255, 0, 0, 0), 1);
    ink_pen(shaft, c, k, 1.8f);
    g.DrawLine(&shaft, k.X(10.0f), k.Y(2.2f), k.X(10.0f), k.Y(10.8f));
    SolidBrush br(c);
    PointF tri[3] = {PointF(k.X(10.0f), k.Y(14.4f)),
                     PointF(k.X(7.2f), k.Y(10.9f)),
                     PointF(k.X(12.8f), k.Y(10.9f))};
    g.FillPolygon(&br, tri, 3);
}

// 设置：齿轮（外圈 + 8 个齿 + 中心孔）
static void i_settings(Graphics& g, const RectF& a, Color c) {
    Ink k(a);
    Pen pen(Color(255, 0, 0, 0), 1);
    ink_pen(pen, c, k, 1.4f);
    const float cx = 10.0f, cy = 10.0f, r = 4.6f, tooth = 7.2f;
    for (int i = 0; i < 8; ++i) {
        const float ang = (float)i * 3.14159265f / 4.0f;
        g.DrawLine(&pen, k.X(cx + cosf(ang) * r), k.Y(cy + sinf(ang) * r),
                         k.X(cx + cosf(ang) * tooth), k.Y(cy + sinf(ang) * tooth));
    }
    g.DrawEllipse(&pen, k.X(cx - r), k.Y(cy - r), k.S(r * 2), k.S(r * 2));
    const float hole = 1.9f;
    g.DrawEllipse(&pen, k.X(cx - hole), k.Y(cy - hole),
                  k.S(hole * 2), k.S(hole * 2));
}

// 贴图：图钉（圆头、收腰、横杠、针）
static void i_pin(Graphics& g, const RectF& a, Color c) {
    Ink k(a);
    Pen pen(Color(255, 0, 0, 0), 1);
    ink_pen(pen, c, k);
    GraphicsPath p;
    p.AddLine(k.X(6.6f), k.Y(9.8f), k.X(8.4f), k.Y(5.0f));
    p.AddBezier(PointF(k.X(8.4f), k.Y(5.0f)), PointF(k.X(8.4f), k.Y(2.5f)),
                PointF(k.X(11.6f), k.Y(2.5f)), PointF(k.X(11.6f), k.Y(5.0f)));
    p.AddLine(k.X(11.6f), k.Y(5.0f), k.X(13.4f), k.Y(9.8f));
    g.DrawPath(&pen, &p);                                           // 钟形头，底边敞口
    g.DrawLine(&pen, k.X(5.6f), k.Y(9.8f), k.X(14.4f), k.Y(9.8f));  // 横杠
    g.DrawLine(&pen, k.X(10.0f), k.Y(9.8f), k.X(10.0f), k.Y(16.8f)); // 针
}

static void i_cancel(Graphics& g, const RectF& a, Color c) {
    Ink k(a);
    Pen pen(Color(255, 0, 0, 0), 1);
    ink_pen(pen, c, k, 1.9f);
    g.DrawLine(&pen, k.X(5.4f), k.Y(5.4f), k.X(14.6f), k.Y(14.6f));
    g.DrawLine(&pen, k.X(14.6f), k.Y(5.4f), k.X(5.4f), k.Y(14.6f));
}

static void i_confirm(Graphics& g, const RectF& a, Color c) {
    Ink k(a);
    Pen pen(Color(255, 0, 0, 0), 1);
    ink_pen(pen, c, k, 1.9f);
    GraphicsPath p;
    p.AddLine(k.X(4.6f), k.Y(10.6f), k.X(8.4f), k.Y(14.4f));
    p.AddLine(k.X(8.4f), k.Y(14.4f), k.X(15.6f), k.Y(5.8f));
    g.DrawPath(&pen, &p);
}

// ------------------------------------------------------------------ 工具条
struct ToolDef { int id; const wchar_t* tip; };

static const ToolDef TOOL_DEFS[] = {
    {T_RECT,    L"矩形  R"},
    {T_ELLIPSE, L"椭圆  E"},
    {T_ARROW,   L"箭头  A"},
    {T_PEN,     L"画笔  P"},
    {T_MOSAIC,  L"马赛克  M"},
    {T_TEXT,    L"文字  T"},
    {T_NUMBER,  L"序号  N"},
    {T_EMOJI,   L"表情  J"},
    {T_LONG,    L"滚动截图（实验，Esc 停止） L"},
    {T_SEP,     L""},
    {T_TRANS,   L"翻译  F"},
    {T_OCR,     L"提取文字  O"},
    {T_COLOR,   L"标注颜色  C"},
    {T_SEP,     L""},
    {T_UNDO,    L"撤销  Ctrl+Z"},
    {T_SAVE,    L"保存到设置目录  Ctrl+S"},
    {T_PIN,     L"贴图  Ctrl+T"},
    {T_SETTINGS, L"设置"},
    {T_CANCEL,  L"取消  Esc"},
    {T_CONFIRM, L"完成  Enter"},
};
static const int N_TOOLS = (int)(sizeof(TOOL_DEFS) / sizeof(TOOL_DEFS[0]));

// 每个格子占满整条高度，命中测试直接拿格子矩形比，不用再算图标偏移
static void layout_toolbar(Overlay* o) {
    o->items.clear();
    const float x0 = SF((float)TB_PAD);
    const float h = SF((float)TB_HEIGHT);
    float x = x0;
    for (int i = 0; i < N_TOOLS; ++i) {
        if (TOOL_DEFS[i].id == T_OCR && !o->ocrEnabled) {
            continue;
        }
        if (TOOL_DEFS[i].id == T_TRANS && !o->translateEnabled) {
            continue;
        }
        if (TOOL_DEFS[i].id == T_SEP &&
            (o->items.empty() || o->items.back().id == T_SEP)) {
            continue;
        }
        ToolItem it;
        it.id = TOOL_DEFS[i].id;
        it.tip = TOOL_DEFS[i].tip;
        if (it.id == T_SEP) {
            it.rect = RectF(x + SF((float)SEP_W) / 2 - 0.5f, SF(14.0f),
                            1.0f, h - SF(28.0f));
            x += SF((float)SEP_W);
        } else {
            it.rect = RectF(x, 0.0f, SF((float)ITEM_W), h);
            x += SF((float)ITEM_W);
        }
        o->items.push_back(it);
    }
}

static float toolbar_width(const Overlay* o) {
    float w = SF((float)TB_PAD) * 2;
    for (const auto& it : o->items)
        w += (it.id == T_SEP) ? SF((float)SEP_W) : SF((float)ITEM_W);
    return w;
}

static bool rect_overlap(const RectF& a, const RectF& b) {
    return a.X < b.X + b.Width && b.X < a.X + a.Width &&
           a.Y < b.Y + b.Height && b.Y < a.Y + a.Height;
}

static void place_toolbar(Overlay* o) {
    float w = toolbar_width(o);
    float h = SF((float)TB_HEIGHT);
    float cx = (o->sel.left + o->sel.right) / 2.0f;
    float x = cx - w / 2;
    float y = (float)o->sel.bottom + SF(8.0f);
    if (y + h > (float)o->shot.vh - 2) y = (float)o->sel.top - h - SF(8.0f);
    x = std::max(4.0f, std::min(x, (float)o->shot.vw - w - 4.0f));
    y = std::max(4.0f, std::min(y, (float)o->shot.vh - h - 4.0f));

    // 结果面板开着的时候别压上去：先试翻到选区另一侧
    if (o->panel != PM_HIDDEN && rect_overlap(RectF(x, y, w, h), o->panelRect)) {
        bool below = (y >= (float)o->sel.bottom);
        float y2 = below ? (float)o->sel.top - h - SF(8.0f)
                         : (float)o->sel.bottom + SF(8.0f);
        y2 = std::max(4.0f, std::min(y2, (float)o->shot.vh - h - 4.0f));
        if (!rect_overlap(RectF(x, y2, w, h), o->panelRect)) y = y2;
    }

    o->tbX = x;
    o->tbY = y;
}

static int toolbar_hit(Overlay* o, POINT pt) {
    if (!o->tbVisible) return -1;
    const float lx = (float)pt.x - o->tbX;
    const float ly = (float)pt.y - o->tbY;
    for (size_t i = 0; i < o->items.size(); ++i) {
        if (o->items[i].id == T_SEP) continue;
        if (ptInRectF(o->items[i].rect, lx, ly)) return (int)i;
    }
    return -1;
}

static void draw_toolbar(Graphics& g, Overlay* o) {
    if (!o->tbVisible) return;
    const float w = toolbar_width(o);
    const float h = SF((float)TB_HEIGHT);
    GraphicsState st = g.Save();
    g.TranslateTransform(o->tbX, o->tbY);

    // 白底圆角条 + 一圈浅灰描边
    {
        GraphicsPath path;
        round_path(path, RectF(0.5f, 0.5f, w - 1.0f, h - 1.0f), SF(6.0f));
        SolidBrush bg(C_TB_BG);
        g.FillPath(&bg, &path);
        Pen border(C_TB_EDGE, 1.0f);
        g.DrawPath(&border, &path);
    }

    for (size_t i = 0; i < o->items.size(); ++i) {
        const auto& it = o->items[i];
        if (it.id == T_SEP) {
            Pen pen(C_TB_LINE, 1.0f);
            g.DrawLine(&pen, it.rect.X + 0.5f, it.rect.Y,
                       it.rect.X + 0.5f, it.rect.Y + it.rect.Height);
            continue;
        }

        const RectF cell = it.rect;
        const bool off = (it.id == T_UNDO && o->anns.empty());   // 没东西可撤就置灰
        const bool hover = (!off && (int)i == o->hoverItem);
        const bool active = (o->tool == it.id);

        if (hover || active) {
            Color fill = C_TB_HOVER;
            if (active) fill = C_TB_SEL_BG;
            else if (it.id == T_CANCEL) fill = C_TB_RED_BG;
            else if (it.id == T_CONFIRM) fill = C_TB_GREEN_BG;
            GraphicsPath hp;
            round_path(hp, RectF(cell.X + SF(2.0f), cell.Y + SF(5.0f),
                                 cell.Width - SF(4.0f), cell.Height - SF(10.0f)),
                       SF(6.0f));
            SolidBrush br(fill);
            g.FillPath(&br, &hp);
        }

        Color ic = C_TB_ICON;
        if (off) ic = C_TB_OFF;
        else if (active) ic = C_TB_SEL_ICON;
        else if (it.id == T_CANCEL) ic = C_TB_RED;
        else if (it.id == T_CONFIRM) ic = C_TB_GREEN;

        const Color cellBg = active ? C_TB_SEL_BG : (hover ? C_TB_HOVER : C_TB_BG);
        const float box = SF((float)ICON_BOX);
        const RectF a(cell.X + (cell.Width - box) / 2,
                      cell.Y + (cell.Height - box) / 2, box, box);

        switch (it.id) {
        case T_RECT:    i_rect(g, a, ic); break;
        case T_ELLIPSE: i_ellipse(g, a, ic); break;
        case T_ARROW:   i_arrow(g, a, ic); break;
        case T_PEN:     i_pen(g, a, ic); break;
        case T_MOSAIC:  i_mosaic(g, a, ic); break;
        case T_TEXT:    i_text(g, a, ic); break;
        case T_NUMBER:  i_number(g, a, ic); break;
        case T_EMOJI:   i_emoji(g, a, ic); break;
        case T_LONG:    i_long(g, a, ic); break;
        case T_TRANS:   i_trans(g, a, ic, cellBg); break;
        case T_OCR:     i_ocr(g, a, ic); break;
        case T_UNDO:    i_undo(g, a, ic); break;
        case T_SAVE:    i_save(g, a, ic); break;
        case T_SETTINGS: i_settings(g, a, ic); break;
        case T_PIN:     i_pin(g, a, ic); break;
        case T_CANCEL:  i_cancel(g, a, ic); break;
        case T_CONFIRM: i_confirm(g, a, ic); break;
        case T_COLOR: {
            Pen ring(Color(255, 210, 210, 210), SF(1.2f));
            SolidBrush br(o->ink);
            const float d = SF(5.0f);
            g.DrawEllipse(&ring, a.X + d, a.Y + d, a.Width - d * 2, a.Height - d * 2);
            g.FillEllipse(&br, a.X + d + SF(1.4f), a.Y + d + SF(1.4f),
                          a.Width - d * 2 - SF(2.8f), a.Height - d * 2 - SF(2.8f));
            break;
        }
        default: break;
        }
    }

    // 悬浮提示
    if (o->hoverItem >= 0 && o->hoverItem < (int)o->items.size()) {
        const wchar_t* tip = o->items[o->hoverItem].tip;
        if (tip && tip[0]) {
            Font font(FONT_UI(), SF(12.0f), FontStyleRegular, UnitPixel);
            RectF bounds;
            g.MeasureString(tip, -1, &font, PointF(0, 0), &bounds);
            const float tw = bounds.Width + SF(14.0f);
            const float th = bounds.Height + SF(6.0f);
            float tx = o->items[o->hoverItem].rect.X +
                       o->items[o->hoverItem].rect.Width / 2 - tw / 2;
            float ty = -th - SF(6.0f);
            if (o->tbY + ty < 2) ty = h + SF(6.0f);   // 顶部放不下就翻到下面
            tx = std::max(-o->tbX + 2.0f,
                          std::min(tx, (float)o->shot.vw - o->tbX - tw - 2.0f));
            GraphicsPath tp;
            round_path(tp, RectF(tx, ty, tw, th), SF(4.0f));
            SolidBrush bg(Color(240, 58, 58, 58));
            g.FillPath(&bg, &tp);
            SolidBrush fg(Color(255, 245, 245, 245));
            StringFormat fmt;
            fmt.SetAlignment(StringAlignmentCenter);
            fmt.SetLineAlignment(StringAlignmentCenter);
            g.DrawString(tip, -1, &font, RectF(tx, ty, tw, th), &fmt, &fg);
        }
    }

    g.Restore(st);
}

// ------------------------------------------------------------------ 面板
static float panel_width() { return SF(340.0f); }

static void place_panel(Overlay* o) {
    const float w = panel_width();
    const float selH = (float)(o->sel.bottom - o->sel.top);
    const float h = std::max(SF(200.0f), std::min(selH, SF(360.0f)));
    const float GAP = SF(10.0f), M = SF(6.0f);
    const float cx = (o->sel.left + o->sel.right) / 2.0f;

    const RectF tb(o->tbX, o->tbY, toolbar_width(o), SF((float)TB_HEIGHT));
    const bool tbOn = o->tbVisible;

    // 候选位置按优先级排列：
    //   0 选区右侧  1 选区左侧  2 选区下方  3 选区上方
    //   4 右侧但下移到工具条之下  5 右侧但上移到工具条之上
    const RectF cand[] = {
        RectF((float)o->sel.right + GAP,     (float)o->sel.top,          w, h),
        RectF((float)o->sel.left - w - GAP,  (float)o->sel.top,          w, h),
        RectF(cx - w / 2,                    (float)o->sel.bottom + GAP, w, h),
        RectF(cx - w / 2,                    (float)o->sel.top - h - GAP, w, h),
        RectF((float)o->sel.right + GAP,     tb.Y + tb.Height + GAP,     w, h),
        RectF((float)o->sel.right + GAP,     tb.Y - h - GAP,             w, h),
    };
    const int N_CAND = (int)(sizeof(cand) / sizeof(cand[0]));

    int best = 0;
    float bestScore = 1e30f;
    RectF bestRect = cand[0];
    for (int i = 0; i < N_CAND; ++i) {
        RectF r = cand[i];

        // 侧边候选只是被工具条压住一点点时，先尝试水平挪到工具条外侧，
        // 别为了几像素的重叠把整块面板挤到下面去
        if (tbOn && i < 2 && rect_overlap(r, tb)) {
            float rightX = tb.X + tb.Width + GAP;
            float leftX = tb.X - w - GAP;
            bool fitR = (rightX + w <= (float)o->shot.vw - M);
            bool fitL = (leftX >= M);
            if (fitR && fitL) {
                r.X = (fabsf(rightX - r.X) <= fabsf(leftX - r.X)) ? rightX : leftX;
            } else if (fitR) {
                r.X = rightX;
            } else if (fitL) {
                r.X = leftX;
            }
        }

        // 出界量（像素），优先保证整块面板都在屏幕里
        float out = 0.0f;
        if (r.X < M) out += M - r.X;
        if (r.Y < M) out += M - r.Y;
        if (r.X + w > (float)o->shot.vw - M) out += r.X + w - ((float)o->shot.vw - M);
        if (r.Y + h > (float)o->shot.vh - M) out += r.Y + h - ((float)o->shot.vh - M);

        float score = out * 1000.0f + (float)i;          // 出界远大于优先级差异
        if (tbOn && rect_overlap(r, tb)) score += 5000.0f;  // 压住工具条重罚
        if (score < bestScore) { bestScore = score; best = i; bestRect = r; }
    }

    RectF r = bestRect;
    r.X = std::max(M, std::min(r.X, (float)o->shot.vw - w - M));
    r.Y = std::max(M, std::min(r.Y, (float)o->shot.vh - h - M));
    o->panelRect = r;

    if (tbOn) {
        logf(L"布局 面板(%d,%d %dx%d) 工具条(%d,%d %dx%d) 候选#%d 重叠=%d",
             (int)r.X, (int)r.Y, (int)r.Width, (int)r.Height,
             (int)tb.X, (int)tb.Y, (int)tb.Width, (int)tb.Height,
             best, (int)rect_overlap(r, tb));
    }
}

// Button action ids are stable even when the optional translate button is
// hidden: 0 = copy, 1 = translate/toggle, 2 = close.
static int panel_buttons(const Overlay* o, RectF out[3], const wchar_t* labels[3],
                         int actions[3]) {
    const RectF& r = o->panelRect;
    float pad = SF(10.0f);
    float bh = SF(26.0f);
    const bool showTranslate = o->translateEnabled;
    const int count = showTranslate ? 3 : 2;
    float bw = (r.Width - pad * (float)(count + 1)) / (float)count;
    float y = r.Y + r.Height - pad - bh;
    int i = 0;
    auto add = [&](const wchar_t* label, int action) {
        out[i] = RectF(r.X + pad + i * (bw + pad), y, bw, bh);
        labels[i] = label;
        actions[i] = action;
        ++i;
    };
    add(L"复制", 0);
    if (showTranslate) {
        add((o->panel == PM_TEXT && !o->panelOriginal.empty())
                ? (o->showOriginal ? L"译文" : L"原文")
                : L"翻译", 1);
    }
    add(L"关闭", 2);
    return count;
}

static void panel_body_rect(const Overlay* o, RectF& out) {
    const RectF& r = o->panelRect;
    float pad = SF(10.0f);
    float top = r.Y + SF(38.0f);
    float bottom = r.Y + r.Height - pad - SF(26.0f) - SF(10.0f);
    out = RectF(r.X + pad, top, r.Width - pad * 2, bottom - top);
}

static const wchar_t* panel_current_text(const Overlay* o) {
    if (o->showOriginal && !o->panelOriginal.empty()) return o->panelOriginal.c_str();
    return o->panelBody.c_str();
}

static LRESULT CALLBACK result_edit_proc(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR){
    if(m==WM_KEYDOWN&&w==VK_ESCAPE){PostMessageW(GetParent(h),WM_KEYDOWN,VK_ESCAPE,0);return 0;}
    if(m==WM_MOUSEWHEEL){
        // A read-only EDIT only scrolls the wheel when it owns focus, which
        // is surprising when the panel is just being read. Translate the
        // wheel to the edit's own vertical scroll command instead.
        int notch=GET_WHEEL_DELTA_WPARAM(w)/WHEEL_DELTA;
        if(notch==0)notch=GET_WHEEL_DELTA_WPARAM(w)>0?1:-1;
        SendMessageW(h,EM_LINESCROLL,0,(LPARAM)(-notch*3));
        return 0;
    }
    if(m==WM_ERASEBKGND){
        // The result EDIT sits on a white card. Erase it explicitly instead
        // of relying on the class brush: the overlay is a WS_POPUP window
        // with a frozen screenshot underneath, and any transparent pixels
        // left in the child control show the old screen text through the
        // panel as a second, offset copy of the result.
        RECT rc{};
        GetClientRect(h,&rc);
        FillRect((HDC)w,&rc,ui::white_brush());
        return 1;
    }
    return DefSubclassProc(h,m,w,l);
}
static void update_result_edit(Overlay*o){
    if(o->panel!=PM_TEXT){if(o->resultEdit)ShowWindow(o->resultEdit,SW_HIDE);return;}
    RectF body;panel_body_rect(o,body);
    if(!o->resultEdit){
        o->resultEdit=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_TABSTOP|WS_VSCROLL|ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY,
            (int)body.X,(int)body.Y,(int)body.Width,(int)body.Height,o->hwnd,nullptr,GetModuleHandleW(nullptr),nullptr);
        o->resultFont=CreateFontW(-S(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
        SendMessageW(o->resultEdit,WM_SETFONT,(WPARAM)o->resultFont,TRUE);SetWindowSubclass(o->resultEdit,result_edit_proc,1,0);
        SendMessageW(o->resultEdit,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,MAKELPARAM(S(8),S(8)));
    }
    std::wstring content=panel_current_text(o);
    // Win32 multiline edit uses CRLF, while model outputs LF.
    for(size_t i=0;i<content.size();++i)if(content[i]==L'\n'&&(i==0||content[i-1]!=L'\r')){content.insert(i,L"\r");++i;}
    if(content!=o->resultDisplayed){SetWindowTextW(o->resultEdit,content.c_str());o->resultDisplayed=content;}
    MoveWindow(o->resultEdit,(int)body.X,(int)body.Y,(int)body.Width,(int)body.Height,FALSE);
    ShowWindow(o->resultEdit,SW_SHOW);
}

static void draw_panel(Graphics& g, Overlay* o) {
    update_result_edit(o);
    if (o->panel == PM_HIDDEN) return;
    const RectF& r = o->panelRect;

    GraphicsPath path;
    float rad = SF(10.0f);
    path.AddArc(r.X, r.Y, rad * 2, rad * 2, 180, 90);
    path.AddArc(r.X + r.Width - rad * 2, r.Y, rad * 2, rad * 2, 270, 90);
    path.AddArc(r.X + r.Width - rad * 2, r.Y + r.Height - rad * 2, rad * 2, rad * 2, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - rad * 2, rad * 2, rad * 2, 90, 90);
    path.CloseFigure();

    SolidBrush bg(Color::White);
    g.FillPath(&bg, &path);
    Pen border(ui::line(), 1.0f);
    g.DrawPath(&border, &path);

    // 标题
    Font tf(FONT_UI(), SF(16.0f), FontStyleBold, UnitPixel);
    SolidBrush white(Color(255, 26, 26, 26));
    g.DrawString(o->panelTitle.c_str(), -1, &tf, PointF(r.X + SF(12.0f), r.Y + SF(9.0f)), nullptr, &white);

    // 正文
    RectF body;
    panel_body_rect(o, body);
    GraphicsState st = g.Save();
    g.SetClip(body);
    if (o->panel == PM_LOADING) {
        Font f(FONT_UI(), SF(13.0f), FontStyleRegular, UnitPixel);
        SolidBrush dim(Color(255, 140, 140, 140));
        const wchar_t* status = (o->panelTitle == L"提取文字" && fast_ocr_initializing())
                                    ? L"正在加载模型…"
                                    : L"正在处理…";
        g.DrawString(status, -1, &f, PointF(body.X, body.Y), nullptr, &dim);
    } else if (o->panel == PM_ERROR) {
        Font f(FONT_UI(), SF(13.0f), FontStyleRegular, UnitPixel);
        SolidBrush red(Color(255, 220, 60, 60));
        g.DrawString(o->panelBody.c_str(), -1, &f,
                     RectF(body.X, body.Y, body.Width, body.Height + SF(400.0f)), nullptr, &red);
    } else if (!o->resultEdit) {
        Font f(FONT_UI(), SF(14.0f), FontStyleRegular, UnitPixel);
        SolidBrush fg(Color(255, 51, 51, 51));
        RectF layout(body.X, body.Y - o->panelScroll, body.Width, body.Height + SF(4000.0f));
        g.DrawString(panel_current_text(o), -1, &f, layout, nullptr, &fg);
    }
    g.Restore(st);

    // 滚动条
    if (o->panel == PM_TEXT && !o->resultEdit) {
        Font f(FONT_UI(), SF(14.0f), FontStyleRegular, UnitPixel);
        RectF measure;
        StringFormat fmt;
        g.MeasureString(panel_current_text(o), -1, &f,
                        RectF(0, 0, body.Width, SF(20000.0f)), &fmt, &measure);
        if (measure.Height > body.Height) {
            float trackH = body.Height;
            float thumbH = std::max(SF(24.0f), trackH * body.Height / measure.Height);
            float maxScroll = measure.Height - body.Height;
            float t = maxScroll > 0 ? o->panelScroll / maxScroll : 0;
            float ty = body.Y + (trackH - thumbH) * t;
            SolidBrush thumb(Color(90, 0, 0, 0));
            g.FillRectangle(&thumb, RectF(r.X + r.Width - SF(5.0f), ty, SF(3.0f), thumbH));
        }
    }

    // 按钮
    RectF btns[3];
    const wchar_t* labels[3];
    int actions[3];
    int n = panel_buttons(o, btns, labels, actions);
    for (int i = 0; i < n; ++i) {
        bool hover = (o->panelBtnHover == actions[i]);
        bool primary = (actions[i] == 1);
        Color fill = primary ? Color(255, 7, 193, 96)
                             : (hover ? Color(255, 232, 232, 232) : Color(255, 243, 243, 243));
        if (primary && hover) fill = Color(255, 6, 173, 86);
        SolidBrush br(fill);
        GraphicsPath bp;
        float rr = SF(6.0f);
        bp.AddArc(btns[i].X, btns[i].Y, rr * 2, rr * 2, 180, 90);
        bp.AddArc(btns[i].X + btns[i].Width - rr * 2, btns[i].Y, rr * 2, rr * 2, 270, 90);
        bp.AddArc(btns[i].X + btns[i].Width - rr * 2, btns[i].Y + btns[i].Height - rr * 2, rr * 2, rr * 2, 0, 90);
        bp.AddArc(btns[i].X, btns[i].Y + btns[i].Height - rr * 2, rr * 2, rr * 2, 90, 90);
        bp.CloseFigure();
        g.FillPath(&br, &bp);

        Font bf(FONT_UI(), SF(12.5f), FontStyleRegular, UnitPixel);
        SolidBrush txt(primary ? Color(255, 255, 255, 255) : Color(255, 76, 76, 76));
        StringFormat cf;
        cf.SetAlignment(StringAlignmentCenter);
        cf.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(labels[i], -1, &bf, btns[i], &cf, &txt);
    }
}

static int panel_button_at(Overlay* o, POINT pt) {
    if (o->panel == PM_HIDDEN) return -1;
    if (!ptInRectF(o->panelRect, (float)pt.x, (float)pt.y)) return -1;
    RectF btns[3];
    const wchar_t* labels[3];
    int actions[3];
    int n = panel_buttons(o, btns, labels, actions);
    for (int i = 0; i < n; ++i) {
        if (ptInRectF(btns[i], (float)pt.x, (float)pt.y)) return actions[i];
    }
    return -2;   // 面板内但不在按钮上
}

// ------------------------------------------------------------------ 绘制
static void draw_size_label(Graphics& g, const RECT& r, bool inside) {
    wchar_t text[64];
    _snwprintf_s(text, _countof(text), _TRUNCATE, L"%d × %d", r.right - r.left, r.bottom - r.top);
    Font font(FONT_UI(), SF(12.0f), FontStyleRegular, UnitPixel);
    RectF bounds;
    g.MeasureString(text, -1, &font, PointF(0, 0), &bounds);
    float w = bounds.Width + SF(14.0f);
    float h = bounds.Height + SF(6.0f);

    float x, y;
    if (inside) {
        x = (float)r.left + SF(4.0f);
        y = (float)r.top + SF(4.0f);
    } else {
        x = (float)r.left;
        y = (float)r.top - h - SF(5.0f);
        if (y < 2) { x = (float)r.left + SF(2.0f); y = (float)r.top + SF(2.0f); }
    }

    SolidBrush bg(C_LABEL_BG);
    g.FillRectangle(&bg, x, y, w, h);
    SolidBrush fg(Color(255, 255, 255, 255));
    StringFormat fmt;
    fmt.SetAlignment(StringAlignmentCenter);
    fmt.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(text, -1, &font, RectF(x, y, w, h), &fmt, &fg);
}

static void draw_magnifier(Graphics& g, Overlay* o, POINT pos) {
    float R = SF(50.0f);
    float zoom = 7.0f;
    float cx = (float)pos.x + R + SF(26.0f);
    float cy = (float)pos.y + R + SF(26.0f);
    if (cx + R > (float)o->shot.vw - 2) cx = (float)pos.x - R - SF(26.0f);
    if (cy + R > (float)o->shot.vh - SF(46.0f)) cy = (float)pos.y - R - SF(26.0f);
    cx = std::max(R + 2, std::min(cx, (float)o->shot.vw - R - 2));
    cy = std::max(R + 2, std::min(cy, (float)o->shot.vh - R - 2));

    float span = 2 * R / zoom;
    float sx = (float)pos.x - span / 2;
    float sy = (float)pos.y - span / 2;

    GraphicsPath clip;
    clip.AddEllipse(cx - R, cy - R, R * 2, R * 2);
    GraphicsState st = g.Save();
    g.SetClip(&clip);
    g.SetInterpolationMode(InterpolationModeNearestNeighbor);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.DrawImage(o->shot.bmp, RectF(cx - R, cy - R, R * 2, R * 2), sx, sy, span, span, UnitPixel);
    g.Restore(st);

    Pen shadow(Color(105, 0, 0, 0), SF(3.2f));
    g.DrawEllipse(&shadow, cx - R - 1, cy - R - 1, R * 2 + 2, R * 2 + 2);
    Pen ring(Color(235, 255, 255, 255), SF(1.6f));
    g.DrawEllipse(&ring, cx - R, cy - R, R * 2, R * 2);
    Pen cross(Color(210, 7, 193, 96), 1.0f);
    g.DrawLine(&cross, cx - R, cy, cx + R, cy);
    g.DrawLine(&cross, cx, cy - R, cx, cy + R);
    SolidBrush dot(Color(180, 255, 255, 255));
    g.FillEllipse(&dot, cx - SF(1.6f), cy - SF(1.6f), SF(3.2f), SF(3.2f));

    // 像素信息
    int px = pos.x, py = pos.y;
    BYTE rr = 0, gg = 0, bb = 0;
    if (px >= 0 && py >= 0 && px < o->shot.vw && py < o->shot.vh) {
        const BYTE* p = (const BYTE*)o->shot.bits + ((size_t)py * o->shot.vw + px) * 4;
        bb = p[0]; gg = p[1]; rr = p[2];
    }
    wchar_t text[128];
    _snwprintf_s(text, _countof(text), _TRUNCATE, L"RGB(%d, %d, %d)   (%d, %d)", rr, gg, bb, px, py);
    Font font(FONT_MONO(), SF(11.0f), FontStyleRegular, UnitPixel);
    RectF bounds;
    g.MeasureString(text, -1, &font, PointF(0, 0), &bounds);
    float w = bounds.Width + SF(12.0f);
    float h = bounds.Height + SF(5.0f);
    float x = std::max(2.0f, std::min(cx - w / 2, (float)o->shot.vw - w - 2));
    float y = cy + R + SF(6.0f);
    if (y + h > (float)o->shot.vh - 2) y = cy - R - h - SF(6.0f);
    SolidBrush bg(C_LABEL_BG);
    g.FillRectangle(&bg, x, y, w, h);
    SolidBrush fg(Color(255, 232, 232, 232));
    StringFormat fmt;
    fmt.SetAlignment(StringAlignmentCenter);
    fmt.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(text, -1, &font, RectF(x, y, w, h), &fmt, &fg);
}

// ------------------------------------------------------------------ 自定义光标
// 压暗整屏之后，系统那个细十字光标在花哨的背景上很容易看丢；而十字加圆环的
// 准星又太抢眼。这里干脆自己画一个蓝色箭头（macOS 风格）做成 HICON 交给
// SetCursor —— 由系统合成，鼠标移动时零延迟，比每帧在画布上重描跟手得多，
// 也不会被误画进截图结果里。
static HICON g_pointer = nullptr;

// 箭头轮廓：单位框内的经典指针形状，尖端在左上角。
// 九个点都是照着参考图逐行量出来的（包围盒 90x128），别随手改。
static void pointer_path(GraphicsPath& p, float w, float h, float pad) {
    auto X = [&](float v) { return pad + v * (w - pad * 2); };
    auto Y = [&](float v) { return pad + v * (h - pad * 2); };
    p.AddLine(X(0.033f), Y(0.000f), X(0.000f), Y(0.797f));  // 尖端 -> 左边底
    p.AddLine(X(0.000f), Y(0.797f), X(0.067f), Y(0.813f));  // 头部左下角
    p.AddLine(X(0.067f), Y(0.813f), X(0.322f), Y(0.664f));  // 尾部缺口
    p.AddLine(X(0.322f), Y(0.664f), X(0.556f), Y(0.891f));  // 尾巴左沿
    p.AddLine(X(0.556f), Y(0.891f), X(0.700f), Y(0.992f));  // 尾巴底
    p.AddLine(X(0.700f), Y(0.992f), X(0.878f), Y(0.891f));  // 尾巴右沿
    p.AddLine(X(0.878f), Y(0.891f), X(0.644f), Y(0.539f));  // 尾巴根
    p.AddLine(X(0.644f), Y(0.539f), X(0.989f), Y(0.422f));  // 头部下沿
    p.CloseFigure();                                        // 右斜边 -> 尖端
}

static HICON make_pointer_icon() {
    // 基准 33px 再缩 20%（用户觉得原来偏大），高 DPI 下同比放大
    const float want = SF(33.0f * 0.8f);
    int h = (int)lround(want);
    int w = (int)lround(want * 0.70f);
    if (w < 12) w = 12;
    if (h < 15) h = 15;
    const float pad = std::max(1.2f, h * 0.05f);

    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = w;
    bi.bV5Height = -h;              // 负高度 = 自上而下，与 GDI+ 的行序一致
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, (BITMAPINFO*)&bi, DIB_RGB_COLORS,
                                     &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color || !bits) {
        if (color) DeleteObject(color);
        return nullptr;
    }

    {
        Bitmap bmp(w, h, w * 4, PixelFormat32bppARGB, (BYTE*)bits);
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.Clear(Color(0, 0, 0, 0));

        GraphicsPath path;
        pointer_path(path, (float)w, (float)h, pad);

        // 左上偏亮、右下偏深，跟参考图的三段渐变对齐
        LinearGradientBrush fill(PointF(0.0f, 0.0f), PointF((REAL)w, (REAL)h),
                                 Color(255, 118, 204, 255), Color(255, 0, 135, 203));
        Color stops[3] = {
            Color(255, 118, 204, 255),
            Color(255, 0, 170, 255),
            Color(255, 0, 135, 203),
        };
        REAL pos[3] = { 0.0f, 0.55f, 1.0f };
        fill.SetInterpolationColors(stops, pos, 3);
        g.FillPath(&fill, &path);

        // 极淡的深蓝描边：浅色背景上也能压住边缘，但不至于变成一圈黑边
        Pen edge(Color(105, 0, 92, 140), std::max(1.2f, h * 0.042f));
        edge.SetLineJoin(LineJoinRound);
        g.DrawPath(&edge, &path);
    }

    // AND 掩码全 0：透明交给 alpha 通道，32 位光标的标准做法
    const int mstride = ((w + 15) / 16) * 2;
    std::vector<BYTE> zero((size_t)mstride * h, 0);
    HBITMAP mask = CreateBitmap(w, h, 1, 1, zero.data());

    ICONINFO ii = {};
    ii.fIcon = FALSE;
    ii.xHotspot = (UINT)lround(pad + 0.033f * (w - pad * 2));
    ii.yHotspot = (UINT)lround(pad);
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON ic = CreateIconIndirect(&ii);

    if (mask) DeleteObject(mask);
    DeleteObject(color);
    return ic;
}

static HICON pointer_cursor() {
    if (!g_pointer) g_pointer = make_pointer_icon();
    return g_pointer;
}

static void draw_editing(Graphics& g, Overlay* o);

static void paint(Overlay* o) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(o->hwnd, &ps);

    int w = o->shot.vw, h = o->shot.vh;
    if (!g_backDC || g_bw != w || g_bh != h) {
        if (g_backBuf) DeleteObject(g_backBuf);
        if (g_backDC) DeleteDC(g_backDC);
        HDC screen = GetDC(nullptr);
        g_backDC = CreateCompatibleDC(screen);
        g_backBuf = CreateCompatibleBitmap(screen, w, h);
        SelectObject(g_backDC, g_backBuf);
        ReleaseDC(nullptr, screen);
        g_bw = w; g_bh = h;
    }

    HDC src = CreateCompatibleDC(hdc);
    HGDIOBJ oldSrc = SelectObject(src, o->shot.hbmDim);
    BitBlt(g_backDC, 0, 0, w, h, src, 0, 0, SRCCOPY);

    if (o->hasSel) {
        SelectObject(src, o->shot.hbm);
        int sw = o->sel.right - o->sel.left;
        int sh = o->sel.bottom - o->sel.top;
        if (sw > 0 && sh > 0) {
            BitBlt(g_backDC, o->sel.left, o->sel.top, sw, sh, src, o->sel.left, o->sel.top, SRCCOPY);
        }
    }
    SelectObject(src, oldSrc);
    DeleteDC(src);

    {
        Graphics g(g_backDC);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);

        if (o->hasSel) {
            int sw = o->sel.right - o->sel.left;
            int sh = o->sel.bottom - o->sel.top;
            if (sw > 0 && sh > 0) {
                GraphicsState st = g.Save();
                g.SetClip(Rect(o->sel.left, o->sel.top, sw, sh));
                g.TranslateTransform((REAL)o->sel.left, (REAL)o->sel.top);
                Bitmap* mos = o->mosaic;
                for (auto& a : o->anns) a.draw(g, mos);
                if (o->hasCur) o->cur.draw(g, mos);
                draw_editing(g, o);
                g.Restore(st);
            }

            // 边框
            Pen border(C_GREEN, 1.0f);
            g.DrawRectangle(&border, (float)o->sel.left + 0.5f, (float)o->sel.top + 0.5f,
                            (float)sw - 1.0f, (float)sh - 1.0f);

            // 控制点
            if (o->mode == 0 && o->tool == T_NONE) {
                Pen pen(C_GREEN, 1.0f);
                SolidBrush white(Color(255, 255, 255, 255));
                float sz = SF((float)HANDLE_SZ);
                for (int i = 0; i < 8; ++i) {
                    RectF r = handle_rect(o->sel, i, sz);
                    g.FillRectangle(&white, r);
                    g.DrawRectangle(&pen, r);
                }
            }
            draw_size_label(g, o->sel, false);
        } else if (o->hasHover && o->mode == 0) {
            // SnapCut-style window hover hint: a light accent outline makes
            // it clear that a simple click will capture the whole window.
            SolidBrush fill(Color(18, 7, 193, 96));
            Pen border(C_GREEN, SF(2.0f));
            g.FillRectangle(&fill, (float)o->hoverWin.left, (float)o->hoverWin.top,
                            (float)(o->hoverWin.right - o->hoverWin.left),
                            (float)(o->hoverWin.bottom - o->hoverWin.top));
            g.DrawRectangle(&border, (float)o->hoverWin.left + 0.5f,
                            (float)o->hoverWin.top + 0.5f,
                            (float)(o->hoverWin.right - o->hoverWin.left) - 1.0f,
                            (float)(o->hoverWin.bottom - o->hoverWin.top) - 1.0f);
        }

        // 光标实时位置：每帧现取，不依赖 WM_MOUSEMOVE ——
        // 覆盖层刚弹出、鼠标还没动的时候也要能定位。
        POINT cur = {0, 0};
        GetCursorPos(&cur);
        ScreenToClient(o->hwnd, &cur);
        const bool curIn = (cur.x >= 0 && cur.y >= 0 && cur.x < w && cur.y < h);

        if (curIn && (o->mode == 1 || o->mode == 2)) {
            draw_magnifier(g, o, cur);
        }

        draw_panel(g, o);
        draw_toolbar(g, o);
    }

    BitBlt(hdc, 0, 0, w, h, g_backDC, 0, 0, SRCCOPY);
    EndPaint(o->hwnd, &ps);
}

// ------------------------------------------------------------------ 文字输入（内联）
static void commit_text(Overlay* o);
static void cancel_text(Overlay* o);

static bool ime_composing(HWND h) {
    HIMC imc = ImmGetContext(h);
    if (!imc) return false;
    LONG n = ImmGetCompositionStringW(imc, GCS_COMPSTR, nullptr, 0);
    ImmReleaseContext(h, imc);
    return n > 0;
}

static std::wstring ime_string(HWND h, DWORD which) {
    std::wstring out;
    HIMC imc = ImmGetContext(h);
    if (!imc) return out;
    LONG n = ImmGetCompositionStringW(imc, which, nullptr, 0);
    if (n > 0) {
        out.resize((size_t)n / sizeof(wchar_t));
        ImmGetCompositionStringW(imc, which, &out[0], n);
    }
    ImmReleaseContext(h, imc);
    return out;
}

static void edit_attach_ime(Overlay* o) {
    if (!o->himc) o->himc = ImmCreateContext();
    if (o->himc) ImmAssociateContext(o->hwnd, o->himc);
}

static void edit_detach_ime(Overlay* o) {
    ImmAssociateContext(o->hwnd, nullptr);
    o->composing.clear();
}

// 输入法（TSF）在消息队列之前就把按键吃掉了，键盘钩子也抓不到。
// 用 GetAsyncKeyState 的「自上次查询以来按下过」位轮询物理键，与输入法无关。
static const UINT_PTR TIMER_EDIT = 77;

static void edit_poll_start(HWND h) {
    GetAsyncKeyState(VK_RETURN);
    GetAsyncKeyState(VK_ESCAPE);
    GetAsyncKeyState(VK_BACK);
    SetTimer(h, TIMER_EDIT, 25, nullptr);
}

static void edit_poll_stop(HWND h) {
    KillTimer(h, TIMER_EDIT);
}

static void begin_text(Overlay* o, POINT pt) {
    commit_text(o);
    o->editing = true;
    o->editText.clear();
    o->composing.clear();
    o->editPos = PointF((float)(pt.x - o->sel.left), (float)(pt.y - o->sel.top));
    edit_attach_ime(o);
    edit_poll_start(o->hwnd);
    InvalidateRect(o->hwnd, nullptr, FALSE);
}

static void commit_text(Overlay* o) {
    if (!o->editing) return;
    o->editing = false;
    std::wstring t = o->editText + o->composing;
    o->editText.clear();
    edit_detach_ime(o);
    edit_poll_stop(o->hwnd);

    size_t b = t.find_first_not_of(L" \t\r\n");
    size_t e = t.find_last_not_of(L" \t\r\n");
    if (b != std::wstring::npos) {
        Ann a;
        a.kind = AK_TEXT;
        a.color = o->ink;
        a.pos = o->editPos;
        a.text = t.substr(b, e - b + 1);
        o->redos.clear();
        a.fontSize=(float)o->toolSizes[T_TEXT-T_RECT];
        o->anns.push_back(a);
    }
    InvalidateRect(o->hwnd, nullptr, FALSE);
}

static void cancel_text(Overlay* o) {
    if (!o->editing) return;
    o->editing = false;
    o->editText.clear();
    edit_detach_ime(o);
    edit_poll_stop(o->hwnd);
    InvalidateRect(o->hwnd, nullptr, FALSE);
}

// 正在编辑的文字（局部坐标，随标注一起裁剪/平移）
static void draw_editing(Graphics& g, Overlay* o) {
    if (!o->editing) return;
    std::wstring shown = o->editText + o->composing;
    Font font(FONT_UI(), SF((float)o->toolSizes[T_TEXT-T_RECT]), FontStyleRegular, UnitPixel);
    SolidBrush br(o->ink);
    if (!shown.empty()) {
        g.DrawString(shown.c_str(), -1, &font, o->editPos, nullptr, &br);
    }
    RectF b;
    g.MeasureString(shown.c_str(), -1, &font, PointF(0, 0), &b);
    float w = shown.empty() ? 0.0f : b.Width;
    float cx = o->editPos.X + w;
    Pen caret(Color(255, 7, 193, 96), SF(1.6f));
    g.DrawLine(&caret, cx, o->editPos.Y + SF(1.0f), cx, o->editPos.Y + SF(18.0f));
    if (!o->composing.empty()) {
        Pen ul(Color(255, 7, 193, 96), SF(1.0f));
        float left = o->editPos.X + (o->editText.empty() ? 0.0f : 0.0f);
        RectF pb;
        g.MeasureString(o->editText.c_str(), -1, &font, PointF(0, 0), &pb);
        float x0 = o->editPos.X + (o->editText.empty() ? 0.0f : pb.Width);
        g.DrawLine(&ul, x0, o->editPos.Y + SF(19.0f), cx, o->editPos.Y + SF(19.0f));
    }
}

// ------------------------------------------------------------------ 输出
static Bitmap* render_result(Overlay* o) {
    if (!o->hasSel) return nullptr;
    int sw = o->sel.right - o->sel.left;
    int sh = o->sel.bottom - o->sel.top;
    if (sw <= 0 || sh <= 0) return nullptr;

    Bitmap* crop = o->shot.bmp->Clone(o->sel.left, o->sel.top, sw, sh, PixelFormat32bppRGB);
    if (!crop) return nullptr;

    Graphics g(crop);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    // 裁剪图的左上角就是选区原点，标注局部坐标直接落上去即可
    Bitmap* mos = o->mosaic;
    for (auto& a : o->anns) a.draw(g, mos);
    return crop;
}

static void do_copy(Overlay* o) {
    Bitmap* img = render_result(o);
    if (img) {
        bool copied = copy_bitmap_to_clipboard(img);
        delete img;
        if (!copied) {
            MessageBoxW(o->hwnd, L"剪贴板暂时无法写入，请重试或保存到文件。",
                        L"复制失败", MB_OK | MB_ICONWARNING);
            return;
        }
    }
    PostMessageW(o->hwnd, WM_CLOSE, 0, 0);
}

static void do_save(Overlay* o) {
    Bitmap* img = render_result(o);
    if (!img) return;
    Settings s = load_settings();
    logf(L"保存: 直接保存到 %ls", (s.save_dir.empty() ? default_save_dir() : s.save_dir).c_str());
    std::wstring saved;
    bool ok = save_bitmap_direct(img, s.save_dir, s.image_format, s.jpeg_quality, saved);
    if (ok && s.auto_copy) copy_bitmap_to_clipboard(img);
    logf(L"保存: 结果=%d path=%ls", (int)ok, saved.c_str());
    delete img;
    if (!ok) {
        MessageBoxW(o->hwnd, L"保存失败，请检查设置中的保存位置和图片格式。",
                    L"保存失败", MB_OK | MB_ICONERROR);
        return;
    }
    PostMessageW(o->hwnd, WM_CLOSE, 0, 0);
}

static void do_pin(Overlay* o) {
    Bitmap* img = render_result(o);
    if (!img) return;
    int x = o->shot.vx + o->sel.left;
    int y = o->shot.vy + o->sel.top;
    pin_create(img, x, y);
    PostMessageW(o->hwnd, WM_CLOSE, 0, 0);
}

static void start_longshot(Overlay* o) {
    if (!o->hasSel) return;
    if (o->panel == PM_LOADING) return;

    RECT sel = o->sel;
    ShowWindow(o->hwnd, SW_HIDE);
    Sleep(180);

    Gdiplus::Bitmap* img = nullptr;
    std::wstring err;
    bool ok = false;
    try {
        ok = longshot_capture(o->shot, sel, img, err);
    } catch (...) {
        err = L"长截图发生异常";
        logf(L"长截图异常（已拦截）");
    }

    ShowWindow(o->hwnd, SW_SHOW);
    SetForegroundWindow(o->hwnd);
    if (!ok) {
        if (img) { delete img; img = nullptr; }
        o->panel = PM_ERROR;
        o->panelTitle = L"滚动截图";
        o->panelBody = err.empty() ? L"长截图失败" : err;
        o->panelOriginal.clear();
        o->panelHint.clear();
        place_panel(o);
        InvalidateRect(o->hwnd, nullptr, FALSE);
        return;
    }

    longshot_preview(img);
    PostMessageW(o->hwnd, WM_CLOSE, 0, 0);
}

// ------------------------------------------------------------------ OCR / 翻译
static void set_panel_loading(Overlay* o, const wchar_t* title) {
    o->panel = PM_LOADING;
    o->panelTitle = title;
    o->panelBody.clear();
    o->panelOriginal.clear();
    o->showOriginal = false;
    o->panelScroll = 0;
    place_panel(o);
    InvalidateRect(o->hwnd, nullptr, FALSE);
}

static void start_ocr(Overlay* o, bool translateToo) {
    if (!o->hasSel || !o->ocrEnabled) return;
    int sw = o->sel.right - o->sel.left;
    int sh = o->sel.bottom - o->sel.top;
    if (sw < 2 || sh < 2) return;

    // 上一个任务还在跑就别再起一个：连点会同时开好几个线程抢 WinRT 的
    // 进程级激活工厂缓存，正是闪退的诱因之一
    if (o->panel == PM_LOADING) return;

    Bitmap* crop = o->shot.bmp->Clone(o->sel.left, o->sel.top, sw, sh, PixelFormat32bppRGB);
    if (!crop) return;

    if(o->ocrRunning.exchange(true)){delete crop;return;}
    o->cancelOcr=false;
    set_panel_loading(o, translateToo ? L"翻译" : L"提取文字");

    HWND hwnd = o->hwnd;
    std::shared_ptr<Overlay> keep = g_ov;
    Settings cfg = load_settings();

    std::thread([hwnd, keep, crop, translateToo, cfg]() {
        std::wstring text, err;
        bool ok = false;
        // 这里绝不能有异常逃出去：std::thread 里未捕获的异常会直接 std::terminate
        // （配合 /EHa，连访问违例也会被兜住），表现为「闪退」。
        try {
            ok = ocr_recognize(crop, text, err, &keep->cancelOcr);
        } catch (...) {
            ok = false;
            err = L"识别时发生异常";
            logf(L"OCR 线程异常（已拦截）");
        }
        delete crop;

        auto* msg = new JobMsg();
        if (!ok) {
            msg->ok = false;
            msg->err = err;
        } else if (text.empty()) {
            msg->ok = false;
            msg->err = L"没有识别到文字";
        } else {
            msg->ok = true;
            msg->text = text;
            if (translateToo) {
                TranslateResult tr;
                try {
                    tr = translate_text(text, cfg);
                } catch (...) {
                    tr.err = L"翻译时发生异常";
                    logf(L"翻译线程异常（已拦截）");
                }
                msg->translated = true;
                msg->tr = tr;
            }
        }
        std::lock_guard<std::mutex> lock(keep->jobMutex);
        if (keep->cancelOcr || !PostMessageW(hwnd, WM_JJ_OCR, 0, (LPARAM)msg)) delete msg;
        keep->ocrRunning=false;
    }).detach();
}

static void start_translate(Overlay* o) {
    if (!o->translateEnabled) return;
    if (o->panel == PM_LOADING) return;
    std::wstring text = o->showOriginal ? o->panelOriginal : o->panelBody;
    if (text.empty()) {
        start_ocr(o, true);
        return;
    }
    o->panel = PM_LOADING;
    o->panelTitle = L"翻译";
    InvalidateRect(o->hwnd, nullptr, FALSE);

    HWND hwnd = o->hwnd;
    std::shared_ptr<Overlay> keep = g_ov;
    Settings cfg = load_settings();

    std::thread([hwnd, keep, text, cfg]() {
        TranslateResult tr;
        // 同上：任何异常都必须在这里被吃掉，否则进程直接没了
        try {
            tr = translate_text(text, cfg);
        } catch (...) {
            tr.err = L"翻译时发生异常";
            logf(L"翻译线程异常（已拦截）");
        }
        auto* msg = new JobMsg();
        msg->ok = tr.ok();
        msg->err = tr.err;
        msg->tr = tr;
        msg->translated = true;
        msg->text = text;
        std::lock_guard<std::mutex> lock(keep->jobMutex);
        if (keep->cancelOcr || !PostMessageW(hwnd, WM_JJ_TRANS, 0, (LPARAM)msg)) delete msg;
    }).detach();
}

static void on_ocr_done(Overlay* o, JobMsg* m) {
    if (!m) return;
    if (!m->ok) {
        o->panel = PM_ERROR;
        o->panelTitle = L"提取文字";
        o->panelBody = m->err.empty() ? L"识别失败" : m->err;
        o->panelOriginal.clear();
    } else {
        o->panel = PM_TEXT;
        o->panelTitle = m->translated ? L"翻译" : L"提取文字";
        o->panelBody = m->text;
        o->panelOriginal.clear();
        o->showOriginal = false;
        o->panelScroll = 0;
        if (m->translated) {
            if (m->tr.ok()) {
                o->panelOriginal = m->text;
                o->panelBody = m->tr.text;
            } else {
                o->panelTitle = L"提取文字";
                o->panelHint = L"翻译失败：" + m->tr.err;
            }
        }
    }
    place_panel(o);
    InvalidateRect(o->hwnd, nullptr, FALSE);
    delete m;
}

static void on_trans_done(Overlay* o, JobMsg* m) {
    if (!m) return;
    if (!m->ok) {
        o->panel = PM_ERROR;
        o->panelTitle = L"翻译";
        o->panelBody = m->err.empty() ? L"翻译失败" : m->err;
    } else {
        o->panel = PM_TEXT;
        o->panelTitle = L"翻译";
        o->panelOriginal = m->text;
        o->panelBody = m->tr.text;
        o->showOriginal = false;
        o->panelScroll = 0;
    }
    place_panel(o);
    InvalidateRect(o->hwnd, nullptr, FALSE);
    delete m;
}

// ------------------------------------------------------------------ 工具动作
static void set_tool(Overlay* o, int tool) {
    o->tool = (o->tool == tool) ? T_NONE : tool;
    InvalidateRect(o->hwnd, nullptr, FALSE);
}

static void undo(Overlay* o) {
    if (o->anns.empty()) return;
    o->redos.push_back(o->anns.back());
    o->anns.pop_back();
    o->numberSeq = 0;
    for (auto& a : o->anns) {
        if (a.kind == AK_NUMBER) o->numberSeq = std::max(o->numberSeq, a.index);
    }
    InvalidateRect(o->hwnd, nullptr, FALSE);
}

static void redo(Overlay* o) {
    if(o->redos.empty())return;
    o->anns.push_back(o->redos.back());o->redos.pop_back();
    o->numberSeq=0;for(auto&a:o->anns)o->numberSeq=std::max(o->numberSeq,a.index);
    InvalidateRect(o->hwnd,nullptr,FALSE);
}

static void cycle_color(Overlay* o) {
    o->colorIdx = (o->colorIdx + 1) % N_PALETTE;
    InvalidateRect(o->hwnd, nullptr, FALSE);
}

static void begin_select(Overlay* o, POINT pt) {
    o->mode = 1;
    o->dragFrom = pt;
    o->sel.left = pt.x; o->sel.top = pt.y;
    o->sel.right = pt.x + 1; o->sel.bottom = pt.y + 1;
    o->hasSel = true;
    o->anns.clear();
    o->redos.clear();
    o->hasCur = false;
    o->numberSeq = 0;
    o->hasHover = false;
    o->windowSnapPending = false;
    o->tbVisible = false;
    o->panel = PM_HIDDEN;
    delete o->mosaic;
    o->mosaic = nullptr;
}

static void begin_region_or_window_select(Overlay* o, POINT pt) {
    RECT windowRect{};
    const bool hasWindow = window_rect_at(o->shot.vx + pt.x, o->shot.vy + pt.y, windowRect);
    begin_select(o, pt);
    if (!hasWindow) return;

    o->windowSnapPending = true;
    o->windowSnapRect.left = std::max(0, (int)windowRect.left - o->shot.vx);
    o->windowSnapRect.top = std::max(0, (int)windowRect.top - o->shot.vy);
    o->windowSnapRect.right = std::min(o->shot.vw, (int)windowRect.right - o->shot.vx);
    o->windowSnapRect.bottom = std::min(o->shot.vh, (int)windowRect.bottom - o->shot.vy);
    if (o->windowSnapRect.right - o->windowSnapRect.left < S(MIN_SEL) ||
        o->windowSnapRect.bottom - o->windowSnapRect.top < S(MIN_SEL)) {
        o->windowSnapPending = false;
    }
}

// 拖拽中的边掩码 -> 对应的缩放光标（1 左 2 右 4 上 8 下）
static LPCWSTR edge_cursor(int mask) {
    switch (mask) {
    case 1 | 4: return IDC_SIZENWSE;
    case 2 | 4: return IDC_SIZENESW;
    case 2 | 8: return IDC_SIZENWSE;
    case 1 | 8: return IDC_SIZENESW;
    case 4:
    case 8: return IDC_SIZENS;
    case 1:
    case 2: return IDC_SIZEWE;
    default: return IDC_ARROW;
    }
}

static void set_cursor_for(Overlay* o, POINT pt) {
    // 拖拽中不能再用 hover 判定：光标一直压在刚拖出来的角上，会被认成
    // 控制点，缩放箭头就甩不掉了。先按当前状态分流。
    switch (o->mode) {
    case 1:                                   // 拉新选区：箭头尖端就是落点
        SetCursor(pointer_cursor());
        return;
    case 2:                                   // 拖控制点
        SetCursor(LoadCursorW(nullptr, edge_cursor(o->edge)));
        return;
    case 3:                                   // 拖动整个选区
        SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
        return;
    default:
        break;
    }

    // 空闲 / 绘制状态：控制点和选区内部保留系统光标，方向感比箭头清楚
    if (o->hasSel && o->tool == T_NONE) {
        int h = handle_at(o, pt);
        if (h >= 0) {
            SetCursor(LoadCursorW(nullptr, HANDLE_CURSORS[h]));
            return;
        }
        if (ptInRect(o->sel, pt.x, pt.y)) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            return;
        }
    }

    HICON pc = pointer_cursor();
    SetCursor(pc ? pc : LoadCursorW(nullptr, IDC_ARROW));
}

// ------------------------------------------------------------------ 窗口过程
static LRESULT CALLBACK ov_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Overlay* o = (Overlay*)GetWindowLongPtrW(h, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE: {
        auto* cs = (CREATESTRUCTW*)lp;
        o = (Overlay*)cs->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)o);
        o->hwnd = h;
        layout_toolbar(o);
        // 覆盖层本身不吃输入法：否则字母快捷键会被 IME 变成 VK_PACKET
        ImmAssociateContext(h, nullptr);
        return 0;
    }

    case WM_SETFOCUS:
        // 焦点回来时再解绑一次（子编辑框有自己的输入法上下文，不影响中文输入）
        ImmAssociateContext(h, nullptr);
        return 0;

    case WM_ERASEBKGND:
        return 1;
    case WM_CTLCOLORSTATIC: {
        // A read-only EDIT sends WM_CTLCOLORSTATIC (not WM_CTLCOLOREDIT).
        // Without an opaque white brush the frozen screenshot behind the
        // overlay shows through the result text and looks like doubled text.
        if (o && (HWND)lp == o->resultEdit) {
            HDC dc = (HDC)wp;
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, RGB(255, 255, 255));
            SetTextColor(dc, RGB(45, 54, 48));
            return (LRESULT)ui::white_brush();
        }
        return ui::colors(msg,wp,lp);
    }
    case WM_CTLCOLOREDIT: {
        // The OCR result is a read-only EDIT control. Keep its background
        // opaque, otherwise the frozen screenshot behind the panel shows
        // through and appears as overlapping text.
        LRESULT brush = ui::colors(msg,wp,lp);
        SetBkMode((HDC)wp, OPAQUE);
        SetBkColor((HDC)wp, RGB(255,255,255));
        return brush;
    }

    case WM_PAINT:
        if (o) paint(o);
        return 0;

    case WM_CHAR: {
        if (!o || !o->editing) break;
        wchar_t c = (wchar_t)wp;
        if (c == L'\r' || c == L'\n') { commit_text(o); return 0; }
        if (c == 27) { cancel_text(o); return 0; }
        if (c == L'\b') {
            if (!o->editText.empty()) o->editText.pop_back();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (c >= 32) {
            o->editText.push_back(c);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }

    case WM_IME_CHAR:
        if (o && o->editing) return 0;   // 已在 WM_IME_COMPOSITION 里取过
        break;

    case WM_IME_STARTCOMPOSITION: {
        if (o && o->editing) {
            HIMC imc = ImmGetContext(h);
            if (imc) {
                COMPOSITIONFORM cf = {};
                cf.dwStyle = CFS_POINT;
                cf.ptCurrentPos.x = o->sel.left + (LONG)o->editPos.X;
                cf.ptCurrentPos.y = o->sel.top + (LONG)o->editPos.Y + S(20);
                ImmSetCompositionWindow(imc, &cf);
                ImmReleaseContext(h, imc);
            }
        }
        break;
    }

    case WM_IME_COMPOSITION: {
        if (!o || !o->editing) break;
        if (lp & GCS_RESULTSTR) {
            o->editText += ime_string(h, GCS_RESULTSTR);
            o->composing.clear();
        }
        if (lp & GCS_COMPSTR) {
            o->composing = ime_string(h, GCS_COMPSTR);
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    case WM_TIMER:
        if (o && o->editing && wp == TIMER_EDIT) {
            bool composing = ime_composing(h);
            if (GetAsyncKeyState(VK_RETURN) & 0x0001) {
                if (!composing) { commit_text(o); return 0; }
            }
            if (GetAsyncKeyState(VK_ESCAPE) & 0x0001) {
                if (!composing) { cancel_text(o); return 0; }
            }
            if (GetAsyncKeyState(VK_BACK) & 0x0001) {
                if (!composing) {
                    if (!o->editText.empty()) o->editText.pop_back();
                    InvalidateRect(h, nullptr, FALSE);
                }
            }
            return 0;
        }
        return 0;

    case WM_JJ_OCR:
        if (o) on_ocr_done(o, (JobMsg*)lp);
        return 0;

    case WM_JJ_TRANS:
        if (o) on_trans_done(o, (JobMsg*)lp);
        return 0;

    case WM_MOUSEMOVE: {
        if (!o) return 0;
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};

        if (o->mode == 1) {
            o->sel = normRECT(o->dragFrom.x, o->dragFrom.y, pt.x, pt.y);
            if (o->windowSnapPending &&
                (abs(pt.x - o->dragFrom.x) >= S(4) || abs(pt.y - o->dragFrom.y) >= S(4))) {
                o->windowSnapPending = false;
            }
            delete o->mosaic;
            o->mosaic = nullptr;
        } else if (o->mode == 2) {
            int dx = pt.x - o->dragFrom.x;
            int dy = pt.y - o->dragFrom.y;
            RECT r = o->selOrigin;
            if (o->edge & 1) r.left = o->selOrigin.left + dx;
            if (o->edge & 2) r.right = o->selOrigin.right + dx;
            if (o->edge & 4) r.top = o->selOrigin.top + dy;
            if (o->edge & 8) r.bottom = o->selOrigin.bottom + dy;
            r = normRECT(r.left, r.top, r.right, r.bottom);
            if (r.right - r.left >= S(MIN_SEL) && r.bottom - r.top >= S(MIN_SEL)) {
                o->sel = r;
                delete o->mosaic;
                o->mosaic = nullptr;
            }
        } else if (o->mode == 3) {
            int dx = pt.x - o->dragFrom.x;
            int dy = pt.y - o->dragFrom.y;
            o->sel = o->selOrigin;
            dx=std::clamp(dx,-(int)o->sel.left,o->shot.vw-(int)o->sel.right);
            dy=std::clamp(dy,-(int)o->sel.top,o->shot.vh-(int)o->sel.bottom);
            o->sel.left += dx; o->sel.right += dx;
            o->sel.top += dy; o->sel.bottom += dy;
            delete o->mosaic;
            o->mosaic = nullptr;
        } else if (o->mode == 4 && o->hasCur) {
            PointF local((float)(pt.x - o->sel.left), (float)(pt.y - o->sel.top));
            if (o->cur.kind == AK_RECT || o->cur.kind == AK_ELLIPSE) {
                o->cur.rect = normRect(o->cur.rect.X, o->cur.rect.Y, local.X, local.Y);
            } else if (o->cur.kind == AK_ARROW) {
                o->cur.b = local;
            } else {
                o->cur.pts.push_back(local);
            }
        } else {
            if (!o->hasSel) {
                RECT wr;
                if (window_rect_at(o->shot.vx + pt.x, o->shot.vy + pt.y, wr)) {
                    o->hoverWin.left = wr.left - o->shot.vx;
                    o->hoverWin.top = wr.top - o->shot.vy;
                    o->hoverWin.right = wr.right - o->shot.vx;
                    o->hoverWin.bottom = wr.bottom - o->shot.vy;
                    o->hasHover = true;
                } else {
                    o->hasHover = false;
                }
            } else {
                o->hasHover = false;
            }
        }

        if (o->tbVisible) {
            int hi = toolbar_hit(o, pt);
            if (hi != o->hoverItem) o->hoverItem = hi;
        } else {
            o->hoverItem = -1;
        }

        if (o->panel != PM_HIDDEN) {
            int b = panel_button_at(o, pt);
            o->panelBtnHover = (b >= 0) ? b : -1;
        }

        set_cursor_for(o, pt);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    case WM_MOUSELEAVE:
        if (o) {
            o->hoverItem = -1;
            o->panelBtnHover = -1;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN: {
        if (!o) return 0;
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        close_emoji_popup(o);
        commit_text(o);

        // 工具条
        int hi = toolbar_hit(o, pt);
        if (hi >= 0) {
            int id = o->items[hi].id;
            switch (id) {
            case T_UNDO:   undo(o); break;
            case T_SAVE:   do_save(o); break;
            case T_SETTINGS:
                PostMessageW(h, WM_CLOSE, 0, 0);
                app_open_settings();
                break;
            case T_PIN:    do_pin(o); break;
            case T_LONG:   start_longshot(o); break;
            case T_CANCEL: PostMessageW(h, WM_CLOSE, 0, 0); break;
            case T_CONFIRM: do_copy(o); break;
            case T_OCR:    start_ocr(o, false); break;
            case T_TRANS:  start_translate(o); break;
            case T_COLOR:  show_color_picker(o); break;
            case T_EMOJI:  show_emoji_picker(o); break;
            default:       set_tool(o, id); break;
            }
            return 0;
        }

        // 面板按钮
        if (o->panel != PM_HIDDEN) {
            int b = panel_button_at(o, pt);
            if (b == 0) {
                const wchar_t* txt = panel_current_text(o);
                if (OpenClipboard(h)) {
                    EmptyClipboard();
                    size_t len = wcslen(txt);
                    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (len + 1) * sizeof(wchar_t));
                    if (mem) {
                        void* dst = GlobalLock(mem);
                        memcpy(dst, txt, (len + 1) * sizeof(wchar_t));
                        GlobalUnlock(mem);
                        SetClipboardData(CF_UNICODETEXT, mem);
                    }
                    CloseClipboard();
                }
                return 0;
            }
            if (b == 1) {
                if (o->panel == PM_TEXT && !o->panelOriginal.empty()) {
                    o->showOriginal = !o->showOriginal;
                    o->panelScroll = 0;
                    InvalidateRect(h, nullptr, FALSE);
                } else {
                    start_translate(o);
                }
                return 0;
            }
            if (b == 2) {
                o->panel = PM_HIDDEN;
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (b == -2) return 0;   // 点在面板空白处
            // 面板外点击：关掉面板继续
            o->panel = PM_HIDDEN;
        }

        if (!o->hasSel || o->sel.right - o->sel.left < S(MIN_SEL) ||
            o->sel.bottom - o->sel.top < S(MIN_SEL)) {
            begin_region_or_window_select(o, pt);
            return 0;
        }

        int edge = handle_at(o, pt);
        if (edge >= 0) {
            o->mode = 2;
            o->edge = (1 << edge) & 0x0F;
            // 0 nw->l+t, 1 n->t, 2 ne->r+t, 3 e->r, 4 se->r+b, 5 s->b, 6 sw->l+b, 7 w->l
            static const int EDGE_MASK[8] = {1 | 4, 4, 2 | 4, 2, 2 | 8, 8, 1 | 8, 1};
            o->edge = EDGE_MASK[edge];
            o->dragFrom = pt;
            o->selOrigin = o->sel;
            o->tbVisible = false;
            return 0;
        }

        if (ptInRect(o->sel, pt.x, pt.y)) {
            if (o->tool == T_RECT || o->tool == T_ELLIPSE || o->tool == T_ARROW ||
                o->tool == T_PEN || o->tool == T_MOSAIC) {
                o->mode = 4;
                PointF local((float)(pt.x - o->sel.left), (float)(pt.y - o->sel.top));
                o->cur = Ann();
                o->cur.color = o->ink;
                o->cur.width = SF((float)o->toolSizes[o->tool-T_RECT]);
                if (o->tool == T_RECT) { o->cur.kind = AK_RECT; o->cur.rect = normRect(local.X, local.Y, local.X, local.Y); }
                else if (o->tool == T_ELLIPSE) { o->cur.kind = AK_ELLIPSE; o->cur.rect = normRect(local.X, local.Y, local.X, local.Y); }
                else if (o->tool == T_ARROW) { o->cur.kind = AK_ARROW; o->cur.a = local; o->cur.b = local; }
                else if (o->tool == T_PEN) { o->cur.kind = AK_PEN; o->cur.pts.push_back(local); }
                else { o->cur.kind = AK_MOSAIC; o->cur.pts.push_back(local); ensure_mosaic(o); }
                o->hasCur = true;
                o->tbVisible = false;
                return 0;
            }
            if (o->tool == T_TEXT) {
                begin_text(o, pt);
                return 0;
            }
            if (o->tool == T_NUMBER) {
                o->numberSeq += 1;
                Ann a;
                a.kind = AK_NUMBER;
                a.diameter=(float)o->toolSizes[T_NUMBER-T_RECT];
                a.color = o->ink;
                a.pos = PointF((float)(pt.x - o->sel.left), (float)(pt.y - o->sel.top));
                a.index = o->numberSeq;
                o->redos.clear();
        a.fontSize=(float)o->toolSizes[T_TEXT-T_RECT];
        o->anns.push_back(a);
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (o->tool == T_EMOJI) {
                Ann a;
                a.kind = AK_EMOJI;
                a.emojiIndex = o->emojiIndex;
                a.fontSize = (float)o->toolSizes[T_EMOJI - T_RECT];
                a.pos = PointF((float)(pt.x - o->sel.left), (float)(pt.y - o->sel.top));
                o->redos.clear();
                o->anns.push_back(a);
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            o->mode = 3;
            o->dragFrom = pt;
            o->selOrigin = o->sel;
            o->tbVisible = false;
            return 0;
        }

        begin_region_or_window_select(o, pt);
        return 0;
    }

    case WM_LBUTTONUP: {
        if (!o) return 0;
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (o->mode == 1) {
            o->sel = normRECT(o->dragFrom.x, o->dragFrom.y, pt.x, pt.y);
            if (o->sel.right - o->sel.left < S(MIN_SEL) ||
                o->sel.bottom - o->sel.top < S(MIN_SEL)) {
                if (o->windowSnapPending) {
                    o->sel = o->windowSnapRect;
                } else if (o->hasHover) {
                    o->sel = o->hoverWin;
                } else {
                    o->sel.left = 0; o->sel.top = 0;
                    o->sel.right = o->shot.vw; o->sel.bottom = o->shot.vh;
                }
            }
            o->windowSnapPending = false;
            delete o->mosaic;
            o->mosaic = nullptr;
        } else if (o->mode == 4 && o->hasCur) {
            bool keep = true;
            if (o->cur.kind == AK_RECT || o->cur.kind == AK_ELLIPSE) {
                keep = o->cur.rect.Width > 2 || o->cur.rect.Height > 2;
            } else if (o->cur.kind == AK_ARROW) {
                float dx = o->cur.b.X - o->cur.a.X, dy = o->cur.b.Y - o->cur.a.Y;
                keep = (dx * dx + dy * dy) > 4;
            }
            if (keep) { o->redos.clear(); o->anns.push_back(o->cur); }
            o->hasCur = false;
        }
        o->mode = 0;
        o->edge = 0;
        if (o->hasSel) {
            place_toolbar(o);
            o->tbVisible = true;
            o->hoverItem = -1;
        }
        set_cursor_for(o, pt);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONDBLCLK:
        if (o && o->hasSel) {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (ptInRect(o->sel, pt.x, pt.y)) do_copy(o);
        }
        return 0;

    case WM_RBUTTONUP: {
        if(!o || !o->hasSel)return 0;
        POINT pt={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        int hi=toolbar_hit(o,pt);
        int tool=hi>=0?o->items[hi].id:o->tool;
        logf(L"右键 x=%d y=%d tbX=%.0f tbY=%.0f hit=%d tool=%d",pt.x,pt.y,o->tbX,o->tbY,hi,tool);
        if(tool==T_COLOR){show_color_picker(o);}
        else if(tool>=T_RECT&&tool<=T_NUMBER){commit_text(o);o->tool=tool;show_tool_slider(o,tool);}
        return 0;
    }

    case WM_MOUSEWHEEL: {
        if (!o || o->panel == PM_HIDDEN) return 0;
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(h, &pt);
        if (!ptInRectF(o->panelRect, (float)pt.x, (float)pt.y)) return 0;
        // The OCR result is a native read-only EDIT with its own scrollbar.
        // Forward the wheel so hovering the result and scrolling works even
        // while keyboard focus stays on the overlay.
        if (o->panel == PM_TEXT && o->resultEdit) {
            SendMessageW(o->resultEdit, WM_MOUSEWHEEL, wp, lp);
            return 0;
        }
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        o->panelScroll -= delta / 120.0f * SF(48.0f);
        if (o->panelScroll < 0) o->panelScroll = 0;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    case WM_KEYDOWN: {
        if (!o) return 0;
        if (o->editing) {
            // 回车 / 退格 / Esc 由键盘钩子处理，其余交给输入法
            return 0;
        }

        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

        switch (wp) {
        case VK_ESCAPE:
            if (o->panel != PM_HIDDEN) {
                if(o->ocrRunning)o->cancelOcr=true;
                o->panel = PM_HIDDEN;
                InvalidateRect(h, nullptr, FALSE);
            } else {
                PostMessageW(h, WM_CLOSE, 0, 0);
            }
            return 0;
        case VK_TAB:
            o->tool=T_NONE;InvalidateRect(h,nullptr,FALSE);return 0;
        case VK_RETURN:
            do_copy(o);
            return 0;
        default: break;
        }

        if (ctrl) {
            switch (wp) {
            case 'C': do_copy(o); return 0;
            case 'S': do_save(o); return 0;
            case 'Z': if(shift)redo(o);else undo(o); return 0;
            case 'Y': redo(o);return 0;
            case 'T': do_pin(o); return 0;
            default: break;
            }
        } else {
            if (wp == 'C') { show_color_picker(o); return 0; }
            if (wp == 'R') { set_tool(o, T_RECT); return 0; }
            if (wp == 'E') { set_tool(o, T_ELLIPSE); return 0; }
            if (wp == 'A') { set_tool(o, T_ARROW); return 0; }
            if (wp == 'P') { set_tool(o, T_PEN); return 0; }
            if (wp == 'M') { set_tool(o, T_MOSAIC); return 0; }
            if (wp == 'T') { set_tool(o, T_TEXT); return 0; }
            if (wp == 'N') { set_tool(o, T_NUMBER); return 0; }
            if (wp == 'J') { set_tool(o, T_EMOJI); return 0; }
            if (wp == 'L') { start_longshot(o); return 0; }
            if (wp == 'O' && o->ocrEnabled) { start_ocr(o, false); return 0; }
            if (wp == 'F' && o->translateEnabled) { start_translate(o); return 0; }
        }

        if (o->hasSel && (wp == VK_LEFT || wp == VK_RIGHT || wp == VK_UP || wp == VK_DOWN)) {
            int step = shift ? S(10) : 1;
            int dx = 0, dy = 0;
            if (wp == VK_LEFT) dx = -step;
            else if (wp == VK_RIGHT) dx = step;
            else if (wp == VK_UP) dy = -step;
            else dy = step;
            dx=std::clamp(dx,-(int)o->sel.left,o->shot.vw-(int)o->sel.right);
            dy=std::clamp(dy,-(int)o->sel.top,o->shot.vh-(int)o->sel.bottom);
            o->sel.left += dx; o->sel.right += dx;
            o->sel.top += dy; o->sel.bottom += dy;
            if (o->tbVisible) place_toolbar(o);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        return 0;
    }

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(h, &pt);
            if (o) set_cursor_for(o, pt);
            return TRUE;
        }
        break;

    case WM_DESTROY:
        if (o) {
            close_emoji_popup(o);
            close_style_popup(o);
            std::lock_guard<std::mutex> lock(o->jobMutex);
            o->cancelOcr=true;
            MSG queued;
            while(PeekMessageW(&queued,h,WM_JJ_OCR,WM_JJ_TRANS,PM_REMOVE))delete (JobMsg*)queued.lParam;
            o->alive = false;
            if (o->editing) edit_poll_stop(h);
            if (o->himc) {
                ImmAssociateContext(h, nullptr);
                ImmDestroyContext(o->himc);
                o->himc = nullptr;
            }
            delete o->mosaic;
            o->mosaic = nullptr;
        }
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        if (g_ov && g_ov->hwnd == h) {
            g_ov.reset();
        }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ------------------------------------------------------------------ 对外接口
static void ensure_class() {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = ov_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = OV_CLASS;
    RegisterClassExW(&wc);
    done = true;
}

bool overlay_is_open() {
    return g_ov && g_ov->alive;
}

bool overlay_open() {
    if (overlay_is_open()) {
        logf(L"截图被忽略：覆盖层已存在");
        return false;
    }
    ensure_class();

    auto o = std::make_shared<Overlay>();
    Settings cfg = load_settings();
    o->ocrEnabled = cfg.ocr_enabled &&
                    (cfg.ocr_engine == L"ppocr" ? fast_ocr_available() :
                     cfg.ocr_engine == L"hunyuan_fast" ? hunyuan_fast_model_ready() :
                     hunyuan_model_ready());
    o->translateEnabled = o->ocrEnabled && cfg.translate_enabled;
    o->strokeWidth=cfg.stroke_width;o->textSize=cfg.text_size;
    for(int i=0;i<4;++i)o->toolSizes[i]=cfg.stroke_width;
    o->toolSizes[4]=std::clamp(cfg.stroke_width*8,4,120);o->toolSizes[5]=cfg.text_size;
    o->recentColors=load_recent_colors(cfg.recent_colors);
    if(!o->recentColors.empty())o->ink=Color(o->recentColors.front());
    if (!capture_desktop(o->shot)) {
        logf(L"抓屏失败");
        return false;
    }
    logf(L"抓屏 %dx%d @ (%d,%d)", o->shot.vw, o->shot.vh, o->shot.vx, o->shot.vy);

    g_ov = o;

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, OV_CLASS, L"微截",
                                WS_POPUP | WS_CLIPCHILDREN, o->shot.vx, o->shot.vy, o->shot.vw, o->shot.vh,
                                nullptr, nullptr, GetModuleHandleW(nullptr), o.get());
    if (!hwnd) {
        g_ov.reset();
        logf(L"创建覆盖层失败 %lu", GetLastError());
        return false;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetCursor(pointer_cursor());   // 覆盖层一出来就是蓝色箭头，不用等鼠标动
    {
        POINT cur{};
        RECT wr{};
        if (GetCursorPos(&cur) && window_rect_at(cur.x, cur.y, wr)) {
            o->hoverWin.left = wr.left - o->shot.vx;
            o->hoverWin.top = wr.top - o->shot.vy;
            o->hoverWin.right = wr.right - o->shot.vx;
            o->hoverWin.bottom = wr.bottom - o->shot.vy;
            o->hasHover = true;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    }
    force_foreground(hwnd);
    HWND fg = GetForegroundWindow();
    logf(L"覆盖层已显示 fg=%p hwnd=%p match=%d", fg, hwnd, (int)(fg == hwnd));
    return true;
}

void overlay_destroy() {
    if (g_ov && g_ov->hwnd) {
        DestroyWindow(g_ov->hwnd);
    }
    g_ov.reset();
    if (g_backBuf) { DeleteObject(g_backBuf); g_backBuf = nullptr; }
    if (g_backDC) { DeleteDC(g_backDC); g_backDC = nullptr; }
    if (g_pointer) {
        // 先把光标换回系统箭头再销毁，否则屏幕上可能挂着一个已经释放的句柄
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        DestroyIcon(g_pointer);
        g_pointer = nullptr;
    }
    g_bw = g_bh = 0;
}

void overlay_request_capture() {
    overlay_open();
}

