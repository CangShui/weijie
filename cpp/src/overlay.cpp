#include "overlay.h"
#include "ocr.h"
#include "ocr_setup.h"
#include "longshot.h"
#include "style_popup.h"
#include "emoji_popup.h"
#include "emoji_renderer.h"
#include "fast_ocr.h"
#include "translate.h"
#include "pin.h"
#include "output.h"
#include "animated_image.h"
#include "ui_theme.h"
#include <imm.h>
#include <memory>
#include <mutex>
#include <commctrl.h>
#include <optional>

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
static uint64_t overlay_animation_time(const Overlay* o){
    uint64_t time=GetTickCount64()-o->animationStart,period=0;
    for(const auto& a:o->anns)if(a.kind==AK_EMOJI&&a.emojiAnimation)
        period=std::max(period,a.emojiAnimation->duration);
    return period?time%period:time;
}
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

static void clear_brush_cursor(Overlay* o){
    if(!o->brushCursor)return;
    if(GetCursor()==o->brushCursor)SetCursor(LoadCursorW(nullptr,IDC_ARROW));
    DestroyCursor(o->brushCursor);o->brushCursor=nullptr;
    logf(L"笔刷尺寸光标已释放：工具=%d 直径=%dpx",o->brushCursorTool,o->brushCursorDiameter);
}

static void clear_stamp_preview(Overlay* o){
    if(!o->stampPreview)return;
    LogTrace trace(o->traceId);
    DestroyWindow(o->stampPreview);o->stampPreview=nullptr;
    o->stampPreviewVisible=false;o->stampPreviewSide=0;
    logf(L"序号/表情虚影已释放，不影响已放置标注");
}

Overlay::~Overlay() {
    clear_stamp_preview(this);
    clear_brush_cursor(this);

    if(resultFont)DeleteObject(resultFont);
    free_shot(shot);
    delete mosaic;
    mosaic = nullptr;
}

// ------------------------------------------------------------------ 小工具
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
        g.SetClip(&clip,CombineModeIntersect);
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
    g.SetClip(body,CombineModeIntersect);
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
    g.SetClip(&clip,CombineModeIntersect);
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

static HCURSOR make_brush_cursor(int tool,int diameter,Color ink){
    const int pad=2,side=diameter+pad*2,hot=pad+diameter/2;
    BITMAPV5HEADER bi{};bi.bV5Size=sizeof(bi);bi.bV5Width=side;bi.bV5Height=-side;
    bi.bV5Planes=1;bi.bV5BitCount=32;bi.bV5Compression=BI_BITFIELDS;
    bi.bV5RedMask=0x00ff0000;bi.bV5GreenMask=0x0000ff00;
    bi.bV5BlueMask=0x000000ff;bi.bV5AlphaMask=0xff000000;
    void* bits=nullptr;HDC dc=GetDC(nullptr);
    HBITMAP color=CreateDIBSection(dc,(BITMAPINFO*)&bi,DIB_RGB_COLORS,&bits,nullptr,0);
    ReleaseDC(nullptr,dc);
    if(!color||!bits){if(color)DeleteObject(color);return nullptr;}
    {
        Bitmap image(side,side,side*4,PixelFormat32bppARGB,(BYTE*)bits);
        Graphics g(&image);g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);g.Clear(Color(0,0,0,0));
        RectF area=brush_footprint(PointF((float)hot,(float)hot),(float)diameter);
        if(tool==T_MOSAIC){
            const int cell=std::max(2,diameter/6);
            Bitmap tiles(cell*2,cell*2,PixelFormat32bppARGB);
            {
                Graphics tileGraphics(&tiles);tileGraphics.SetCompositingMode(CompositingModeSourceCopy);
                for(int y=0;y<2;++y)for(int x=0;x<2;++x){
                    BYTE shade=(x+y)%2?225:85;
                    SolidBrush tile(Color(130,shade,shade,shade));
                    tileGraphics.FillRectangle(&tile,x*cell,y*cell,cell,cell);
                }
            }
            TextureBrush pattern(&tiles,WrapModeTile);g.FillEllipse(&pattern,area);
        }else{
            SolidBrush fill(Color(diameter<5?255:110,ink.GetR(),ink.GetG(),ink.GetB()));
            g.FillEllipse(&fill,area);
        }
        if(diameter>=5){
            Pen dark(Color(245,20,20,20),1),light(Color(245,255,255,255),1);
            g.DrawEllipse(&dark,area.X+.5f,area.Y+.5f,area.Width-1,area.Height-1);
            if(diameter>=7)g.DrawEllipse(&light,area.X+1.5f,area.Y+1.5f,area.Width-3,area.Height-3);
        }
    }
    int stride=((side+15)/16)*2;std::vector<BYTE> zero((size_t)stride*side,0);
    HBITMAP mask=CreateBitmap(side,side,1,1,zero.data());
    ICONINFO info{};info.fIcon=FALSE;info.xHotspot=hot;info.yHotspot=hot;
    info.hbmColor=color;info.hbmMask=mask;
    HCURSOR cursor=mask?(HCURSOR)CreateIconIndirect(&info):nullptr;
    if(mask)DeleteObject(mask);DeleteObject(color);
    return cursor;
}

static int active_brush_tool(const Overlay* o){
    if(o->mode==4&&o->hasCur)
        return o->cur.kind==AK_PEN?T_PEN:o->cur.kind==AK_MOSAIC?T_MOSAIC:T_NONE;
    return o->tool==T_PEN||o->tool==T_MOSAIC?o->tool:T_NONE;
}

static bool brush_cursor_at(Overlay* o,POINT pt){
    if(!o->alive||!o->hasSel||o->editing||o->annotationDrag.active||(o->mode!=0&&o->mode!=4)||active_brush_tool(o)==T_NONE||
       !ptInRect(o->sel,pt.x,pt.y))return false;
    if(o->mode==0&&handle_at(o,pt)>=0)return false;
    if(o->sizeDrag.active&&ptInRect(o->sizeDrag.bounds,pt.x,pt.y))return false;
    if(o->tbVisible&&ptInRectF(RectF(o->tbX,o->tbY,toolbar_width(o),SF(TB_HEIGHT)),(float)pt.x,(float)pt.y))return false;
    return o->panel==PM_HIDDEN||!ptInRectF(o->panelRect,(float)pt.x,(float)pt.y);
}

static HCURSOR brush_cursor(Overlay* o){
    int tool=active_brush_tool(o);
    int diameter=(int)lround(o->mode==4&&o->hasCur?o->cur.width:o->annotation_width(tool));
    Color ink=o->mode==4&&o->hasCur?o->cur.color:o->ink;
    if(o->brushCursorDiameter==diameter&&o->brushCursorTool==tool&&o->brushCursorColor==ink.GetValue())
        return o->brushCursor;
    HCURSOR next=make_brush_cursor(tool,diameter,ink);
    DWORD error=next?ERROR_SUCCESS:GetLastError();
    clear_brush_cursor(o);
    o->brushCursor=next;o->brushCursorDiameter=diameter;o->brushCursorTool=tool;o->brushCursorColor=ink.GetValue();
    logf(L"笔刷尺寸光标更新：工具=%s 设置直径=%dpx 实际直径=%d截图像素 DPI比例=%.2f 创建成功=%d error=%lu",
         tool==T_PEN?L"画笔":L"马赛克",o->toolSizes[tool-T_RECT],diameter,g_scale,(int)(next!=nullptr),error);
    return next;
}

static float text_line_height(int size){
    Font font(FONT_UI(),SF((float)size),FontStyleRegular,UnitPixel);
    return std::max(1.0f,font.GetHeight(96.0f));
}

static RectF text_caret_rect(PointF position,int size){
    return RectF(position.X,position.Y+SF(1),SF(1.6f),text_line_height(size));
}

// 预览和落地共用尺寸、样式和中心坐标，悬停不消耗序号。
static Ann stamp_annotation(const Overlay* o,PointF position){
    Ann stamp;stamp.pos=position;stamp.color=o->ink;
    if(o->tool==T_NUMBER){
        stamp.kind=AK_NUMBER;stamp.diameter=(float)o->toolSizes[T_NUMBER-T_RECT];
        stamp.index=o->numberSeq+1;
    }else{
        stamp.kind=AK_EMOJI;stamp.emojiIndex=o->emojiIndex;
        stamp.emojiImage=o->emojiImage?o->emojiImage:emoji_image(o->emojiIndex);
        stamp.emojiAnimation=o->emojiAnimation?o->emojiAnimation:emoji_animation(o->emojiIndex);
        stamp.fontSize=(float)o->toolSizes[T_EMOJI-T_RECT];
    }
    return stamp;
}

static constexpr BYTE STAMP_PREVIEW_ALPHA=128;

static HBITMAP stamp_preview_bitmap(Ann stamp,int side,uint64_t time=0){
    BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth=side;bi.bmiHeader.biHeight=-side;
    bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;
    void* bits=nullptr;
    HBITMAP bitmap=CreateDIBSection(nullptr,&bi,DIB_RGB_COLORS,&bits,nullptr,0);
    if(!bitmap)return nullptr;
    bool ok=false;
    {
        Bitmap image(side,side,side*4,PixelFormat32bppPARGB,(BYTE*)bits);
        Graphics graphics(&image);
        graphics.Clear(Color(0,0,0,0));
        graphics.SetSmoothingMode(SmoothingModeAntiAlias);
        graphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        graphics.SetPixelOffsetMode(PixelOffsetModeHalf);
        stamp.pos=PointF((float)(side/2),(float)(side/2));
        stamp.draw(graphics,nullptr,time);
        ok=graphics.GetLastStatus()==Ok;
    }
    if(!ok){DeleteObject(bitmap);return nullptr;}
    return bitmap;
}

static LRESULT CALLBACK stamp_preview_proc(HWND h,UINT msg,WPARAM wp,LPARAM lp){
    if(msg==WM_NCHITTEST)return HTTRANSPARENT;
    if(msg==WM_MOUSEACTIVATE)return MA_NOACTIVATE;
    return DefWindowProcW(h,msg,wp,lp);
}

static void hide_stamp_preview(Overlay* o,const wchar_t* reason){
    if(!o->stampPreviewVisible)return;
    LogTrace trace(o->traceId);
    ShowWindow(o->stampPreview,SW_HIDE);o->stampPreviewVisible=false;
    logf(L"序号/表情虚影隐藏：%s；预览不写入截图",reason);
}

static bool stamp_preview_at(Overlay* o,POINT pt){
    HWND foreground=GetForegroundWindow(),capture=GetCapture();
    if(!o->alive||!o->hasSel||o->editing||o->annotationDrag.active||o->mode!=0||o->stampPressed||
       (o->tool!=T_NUMBER&&o->tool!=T_EMOJI)||!ptInRect(o->sel,pt.x,pt.y)||handle_at(o,pt)>=0||
       (capture&&capture!=o->hwnd)||
       (foreground!=o->hwnd&&(!o->stylePopup||foreground!=o->stylePopup)&&
        (!o->emojiPopup||foreground!=o->emojiPopup)))return false;
    if(o->tool==T_EMOJI&&!o->emojiImage&&emoji_count()==0)return false;
    if(o->sizeDrag.active&&ptInRect(o->sizeDrag.bounds,pt.x,pt.y))return false;
    if(o->tbVisible&&ptInRectF(RectF(o->tbX,o->tbY,toolbar_width(o),SF(TB_HEIGHT)),(float)pt.x,(float)pt.y))return false;
    return o->panel==PM_HIDDEN||!ptInRectF(o->panelRect,(float)pt.x,(float)pt.y);
}

static void update_stamp_preview(Overlay* o,POINT pt){
    if(!stamp_preview_at(o,pt)){hide_stamp_preview(o,L"指针不在可放置区域或正在操作其他控件");return;}
    Ann stamp=stamp_annotation(o,{});
    uint64_t time=GetTickCount64()-o->animationStart;
    size_t frame=stamp.emojiAnimation?stamp.emojiAnimation->frame_index(time):0;
    int side=(int)ceil(stamp.bounds().Width)+4;
    const Ann& key=o->stampPreviewKey;
    bool configChanged=!o->stampPreview||o->stampPreviewSide!=side||o->stampPreviewScale!=g_scale||
        key.kind!=stamp.kind||key.color.GetValue()!=stamp.color.GetValue()||
        key.diameter!=stamp.diameter||key.fontSize!=stamp.fontSize||
        key.index!=stamp.index||key.emojiIndex!=stamp.emojiIndex||key.emojiImage!=stamp.emojiImage||
        key.emojiAnimation!=stamp.emojiAnimation;
    bool rebuild=configChanged||frame!=o->stampPreviewFrame;
    POINT position{pt.x+o->shot.vx-side/2,pt.y+o->shot.vy-side/2};
    if(rebuild){
        LogTrace trace(o->traceId);
        if(configChanged)logf(L"序号/表情虚影请求：工具=%d 序号=%d 表情=%d 设置大小=%.0f DPI=%.2f 图像边长=%d 透明度=%d/255",
            o->tool,stamp.index,stamp.emojiIndex,stamp.kind==AK_NUMBER?stamp.diameter:stamp.fontSize,
            g_scale,side,STAMP_PREVIEW_ALPHA);
        if(!o->stampPreview){
            static bool registered=false;
            if(!registered){
                WNDCLASSW wc{};wc.lpfnWndProc=stamp_preview_proc;wc.hInstance=GetModuleHandleW(nullptr);
                wc.lpszClassName=L"weijieStampPreview";
                registered=RegisterClassW(&wc)!=0;
            }
            o->stampPreview=CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE|
                WS_EX_TOOLWINDOW|WS_EX_TOPMOST,L"weijieStampPreview",L"",WS_POPUP,
                position.x,position.y,side,side,o->hwnd,nullptr,GetModuleHandleW(nullptr),nullptr);
        }
        HBITMAP bitmap=stamp_preview_bitmap(stamp,side,time);
        HDC dc=CreateCompatibleDC(nullptr);HGDIOBJ old=bitmap&&dc?SelectObject(dc,bitmap):nullptr;
        SIZE size{side,side};POINT source{};BLENDFUNCTION blend{AC_SRC_OVER,0,STAMP_PREVIEW_ALPHA,AC_SRC_ALPHA};
        bool ok=o->stampPreview&&old&&UpdateLayeredWindow(o->stampPreview,nullptr,&position,&size,
            dc,&source,0,&blend,ULW_ALPHA);
        DWORD error=ok?ERROR_SUCCESS:GetLastError();
        if(old)SelectObject(dc,old);
        if(dc)DeleteDC(dc);
        if(bitmap)DeleteObject(bitmap);
        if(!ok){
            logf(L"序号/表情虚影创建失败：error=%lu；仍可正常点击放置标注",error);
            clear_stamp_preview(o);return;
        }
        o->stampPreviewKey=stamp;o->stampPreviewScale=g_scale;o->stampPreviewSide=side;
        o->stampPreviewFrame=frame;
        if(configChanged)logf(L"序号/表情虚影绘制完成：复用实际标注绘制，动画只刷新变化帧");
    }
    // 只在选区内显示；靠近工具条、结果面板或弹窗时不覆盖它们。
    int left=pt.x-side/2,top=pt.y-side/2;
    HRGN region=CreateRectRgn(o->sel.left-left,o->sel.top-top,o->sel.right-left,o->sel.bottom-top);
    auto exclude=[&](RECT rect){
        HRGN cut=CreateRectRgn(rect.left-left,rect.top-top,rect.right-left,rect.bottom-top);
        CombineRgn(region,region,cut,RGN_DIFF);DeleteObject(cut);
    };
    if(o->tbVisible)exclude({(LONG)floor(o->tbX),(LONG)floor(o->tbY),
        (LONG)ceil(o->tbX+toolbar_width(o)),(LONG)ceil(o->tbY+SF(TB_HEIGHT))});
    if(o->panel!=PM_HIDDEN)exclude({(LONG)floor(o->panelRect.X),(LONG)floor(o->panelRect.Y),
        (LONG)ceil(o->panelRect.GetRight()),(LONG)ceil(o->panelRect.GetBottom())});
    if(o->sizeDrag.active)exclude(o->sizeDrag.bounds);
    for(HWND popup:{o->stylePopup,o->emojiPopup})if(popup&&IsWindowVisible(popup)){
        RECT rect{};GetWindowRect(popup,&rect);OffsetRect(&rect,-o->shot.vx,-o->shot.vy);exclude(rect);
    }
    if(!SetWindowRgn(o->stampPreview,region,FALSE))DeleteObject(region);
    SetWindowPos(o->stampPreview,HWND_TOPMOST,position.x,position.y,side,side,SWP_NOACTIVATE|SWP_SHOWWINDOW);
    if(!o->stampPreviewVisible){
        LogTrace trace(o->traceId);
        o->stampPreviewVisible=true;
        logf(L"序号/表情虚影已显示：点击中心即落点，半透明预览不参与保存或复制");
    }
    if(!o->stampPreviewTracking){
        TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,o->hwnd,0};
        o->stampPreviewTracking=TrackMouseEvent(&track)!=FALSE;
    }
}

static void draw_editing(Graphics& g, Overlay* o);
static void draw_text_frame(Graphics& g,Overlay* o);
static void draw_annotation_selection(Graphics& g,Overlay* o);

static void draw_size_drag(Graphics& g,const Overlay* o){
    const auto& drag=o->sizeDrag;
    if(!drag.active)return;
    RectF area((REAL)drag.bounds.left,(REAL)drag.bounds.top,
        (REAL)(drag.bounds.right-drag.bounds.left),(REAL)(drag.bounds.bottom-drag.bounds.top));
    ui::box(g,area,Color(250,255,255,255),9,ui::line());
    ui::text(g,tool_size_name(drag.tool),RectF(area.X+SF(14),area.Y+SF(7),SF(75),SF(22)),12,ui::ink(),true);
    ui::text(g,L"按住右键 · 左小右大",RectF(area.X+SF(85),area.Y+SF(8),SF(115),SF(20)),10,ui::muted());
    RectF value(area.GetRight()-SF(76),area.Y+SF(5),SF(64),SF(25));
    ui::box(g,value,ui::tint(),6);
    ui::text(g,std::to_wstring(o->toolSizes[drag.tool-T_RECT])+L" px",value,12,ui::green(),true,StringAlignmentCenter);
    float left=area.X+SF(16),y=area.Y+SF(43),thumb=left+drag.thumb;
    Pen base(ui::line(),SF(4)),filled(ui::green(),SF(4));
    base.SetLineCap(LineCapRound,LineCapRound,DashCapRound);
    filled.SetLineCap(LineCapRound,LineCapRound,DashCapRound);
    g.DrawLine(&base,left,y,left+drag.length,y);
    if(drag.thumb>0)g.DrawLine(&filled,left,y,thumb,y);
    SolidBrush white(Color::White);Pen edge(ui::green(),SF(1.5f));
    g.FillEllipse(&white,thumb-SF(6),y-SF(6),SF(12),SF(12));
    g.DrawEllipse(&edge,thumb-SF(6),y-SF(6),SF(12),SF(12));
    ui::text(g,std::to_wstring(tool_size_minimum(drag.tool)),RectF(left,area.Y+SF(55),SF(55),SF(15)),9,ui::muted());
    ui::text(g,std::to_wstring(tool_size_maximum(drag.tool)),RectF(area.GetRight()-SF(71),area.Y+SF(55),SF(55),SF(15)),9,ui::muted(),false,StringAlignmentFar);
}

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
    int savedDC=SaveDC(g_backDC);
    IntersectClipRect(g_backDC,ps.rcPaint.left,ps.rcPaint.top,ps.rcPaint.right,ps.rcPaint.bottom);
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
        g.SetClip(Rect(ps.rcPaint.left,ps.rcPaint.top,ps.rcPaint.right-ps.rcPaint.left,
            ps.rcPaint.bottom-ps.rcPaint.top));
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);

        if (o->hasSel) {
            int sw = o->sel.right - o->sel.left;
            int sh = o->sel.bottom - o->sel.top;
            if (sw > 0 && sh > 0) {
                GraphicsState st = g.Save();
                g.SetClip(Rect(o->sel.left, o->sel.top, sw, sh),CombineModeIntersect);
                g.TranslateTransform((REAL)o->sel.left, (REAL)o->sel.top);
                Bitmap* mos = o->mosaic;
                uint64_t time=overlay_animation_time(o);
                for(size_t i=0;i<o->anns.size();++i)
                    if(!o->editing||(int)i!=o->editingIndex)o->anns[i].draw(g,mos,time);
                if (o->hasCur) o->cur.draw(g, mos);
                draw_editing(g, o);
                g.Restore(st);
                st=g.Save();g.TranslateTransform((REAL)o->sel.left,(REAL)o->sel.top);
                draw_text_frame(g,o);draw_annotation_selection(g,o);g.Restore(st);
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
        draw_size_drag(g,o);
    }

    BitBlt(hdc, 0, 0, w, h, g_backDC, 0, 0, SRCCOPY);
    RestoreDC(g_backDC,savedDC);
    EndPaint(o->hwnd, &ps);
}

// ------------------------------------------------------------------ 文字输入（内联）
static void commit_text(Overlay* o);
static void cancel_text(Overlay* o);
static void clear_annotation_selection(Overlay* o,const wchar_t* reason);

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

#include "text_editing.inc"
#include "annotation_moving.inc"

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
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    // 裁剪图的左上角就是选区原点，标注局部坐标直接落上去即可
    Bitmap* mos = o->mosaic;
    for (auto& a : o->anns) a.draw(g, mos);
    return crop;
}

static void do_copy(Overlay* o) {
    commit_text(o);
    Bitmap* img = render_result(o);
    if (img) {
        bool copied = copy_bitmap_to_clipboard(img);
        logf(L"截图复制返回：结果=%s 选区=%ldx%ld", copied ? L"成功" : L"失败",
             o->sel.right - o->sel.left, o->sel.bottom - o->sel.top);
        delete img;
        if (!copied) {
            MessageBoxW(o->hwnd, L"剪贴板暂时无法写入，请重试或保存到文件。",
                        L"复制失败", MB_OK | MB_ICONWARNING);
            return;
        }
    }
    PostMessageW(o->hwnd, WM_CLOSE, 0, 0);
}

static std::shared_ptr<AnimatedScreenshot> overlay_animation(Overlay* o,Bitmap* background=nullptr){
    bool animated=false;
    for(const auto& a:o->anns)if(a.kind==AK_EMOJI&&a.emojiAnimation){animated=true;break;}
    if(!animated)return nullptr;
    auto scene=std::make_shared<AnimatedScreenshot>();
    scene->annotations=o->anns;scene->scale=g_scale;scene->traceId=o->traceId;
    scene->background.reset(background?
        background->Clone(0,0,background->GetWidth(),background->GetHeight(),PixelFormat32bppRGB):
        o->shot.bmp->Clone(Rect((int)o->sel.left,(int)o->sel.top,(int)(o->sel.right-o->sel.left),
            (int)(o->sel.bottom-o->sel.top)),PixelFormat32bppRGB));
    if(o->mosaic)scene->mosaic.reset(o->mosaic->Clone(0,0,o->mosaic->GetWidth(),o->mosaic->GetHeight(),PixelFormat32bppRGB));
    return scene;
}

static void do_save(Overlay* o,bool saveAs=false) {
    commit_text(o);
    Bitmap* img = render_result(o);
    if (!img) return;
    Settings s = load_settings();
    logf(L"保存: 直接保存到 %ls", (s.save_dir.empty() ? default_save_dir() : s.save_dir).c_str());
    std::wstring saved;
    bool cancelled=false;
    auto animation=overlay_animation(o);
    bool ok=saveAs?save_bitmap_dialog(o->hwnd,img,s.save_dir,s.image_format,s.jpeg_quality,&saved,animation,&cancelled):
        save_bitmap_direct(img,s.save_dir,s.image_format,s.jpeg_quality,saved,o->hwnd,animation,&cancelled);
    if (ok && s.auto_copy) copy_bitmap_to_clipboard(img);
    logf(L"保存: 结果=%d path=%ls", (int)ok, saved.c_str());
    delete img;
    if(cancelled){logf(L"截图保存取消：保留编辑界面");return;}
    if (!ok) {
        MessageBoxW(o->hwnd, L"保存失败，请检查设置中的保存位置和图片格式。",
                    L"保存失败", MB_OK | MB_ICONERROR);
        return;
    }
    PostMessageW(o->hwnd, WM_CLOSE, 0, 0);
}

static void do_pin(Overlay* o) {
    commit_text(o);
    Bitmap* img = render_result(o);
    if (!img) return;
    int x = o->shot.vx + o->sel.left;
    int y = o->shot.vy + o->sel.top;
    pin_create(img, x, y,overlay_animation(o));
    PostMessageW(o->hwnd, WM_CLOSE, 0, 0);
}

static void start_longshot(Overlay* o) {
    commit_text(o);
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

    longshot_preview(img,err,overlay_animation(o,img));
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
    logf(L"图片文字请求到达：操作=%s 识别按钮=%d 翻译按钮=%d",
         translateToo?L"翻译":L"识别",(int)o->ocrEnabled,(int)o->translateEnabled);
    if (!o->hasSel || !(translateToo?o->translateEnabled:o->ocrEnabled)) {
        logf(L"图片文字请求被拒绝：没有选区或对应按钮已关闭");
        return;
    }
    if(!o->ocrEnabled||!ocr_is_enabled()){
        o->panel=PM_ERROR;
        o->panelTitle=translateToo?L"翻译":L"提取文字";
        o->panelBody=L"识别主开关已关闭。请在设置中开启文字识别后再处理图片；关闭期间不会加载模型。";
        o->panelOriginal.clear();
        place_panel(o);InvalidateRect(o->hwnd,nullptr,FALSE);
        logf(L"图片文字请求被拒绝：识别主开关关闭，不因翻译请求重新加载模型");
        return;
    }
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
        LogTrace trace(keep->traceId);
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
        logf(L"图片识别返回：成功=%d 字符数=%zu 错误=%s",(int)ok,text.size(),err.c_str());

        auto* msg = new JobMsg();
        msg->translated = translateToo;
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
                logf(L"图片翻译返回：成功=%d 字符数=%zu 错误=%s",
                     (int)tr.ok(),tr.text.size(),tr.err.c_str());
            }
        }
        std::lock_guard<std::mutex> lock(keep->jobMutex);
        if (keep->cancelOcr || !PostMessageW(hwnd, WM_JJ_OCR, 0, (LPARAM)msg)) {
            logf(L"图片文字请求结束：截图已取消或窗口已关闭，不再更新结果面板");
            delete msg;
        }
        keep->ocrRunning=false;
    }).detach();
}

static void start_translate(Overlay* o) {
    if (!o->translateEnabled) {logf(L"翻译请求被拒绝：翻译按钮已关闭");return;}
    if (o->panel == PM_LOADING) return;
    std::wstring text = o->panel==PM_TEXT?(o->showOriginal ? o->panelOriginal : o->panelBody):L"";
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
        LogTrace trace(keep->traceId);
        logf(L"文字翻译请求到达：字符数=%zu",text.size());
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
        logf(L"文字翻译返回：成功=%d 字符数=%zu 错误=%s",(int)tr.ok(),tr.text.size(),tr.err.c_str());
        std::lock_guard<std::mutex> lock(keep->jobMutex);
        if (keep->cancelOcr || !PostMessageW(hwnd, WM_JJ_TRANS, 0, (LPARAM)msg)) {
            logf(L"文字翻译请求结束：截图已取消或窗口已关闭，不再更新结果面板");
            delete msg;
        }
    }).detach();
}

static void on_ocr_done(Overlay* o, JobMsg* m) {
    if (!m) return;
    if (!m->ok) {
        o->panel = PM_ERROR;
        o->panelTitle = m->translated ? L"翻译" : L"提取文字";
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
static void set_cursor_for(Overlay* o,POINT pt);

static void end_size_drag(Overlay* o,bool cancelled,const wchar_t* reason){
    if(!o->sizeDrag.active)return;
    const auto drag=o->sizeDrag;
    int current=o->toolSizes[drag.tool-T_RECT];
    if(cancelled)o->toolSizes[drag.tool-T_RECT]=drag.original;
    o->sizeDrag.active=false;
    if(GetCapture()==o->hwnd)ReleaseCapture();
    InvalidateRect(o->hwnd,&drag.bounds,FALSE);
    logf(L"右键尺寸拖动结束：工具=%s 原px=%d 拖动px=%d 最终px=%d 取消=%d 原因=%s 调整次数=%d 耗时=%llums；内联滑条已收起",
        tool_size_name(drag.tool),drag.original,current,o->toolSizes[drag.tool-T_RECT],(int)cancelled,
        reason,drag.changes,GetTickCount64()-drag.started);
    overlay_update_brush_cursor(o);
}

static void move_size_drag(Overlay* o,POINT pt){
    auto& drag=o->sizeDrag;
    if(!drag.active)return;
    int minimum=tool_size_minimum(drag.tool),maximum=tool_size_maximum(drag.tool);
    drag.thumb=std::clamp(drag.initial+(float)(pt.x-drag.origin.x),0.0f,drag.length);
    int next=minimum+(int)lround(drag.thumb/drag.length*(maximum-minimum));
    int& value=o->toolSizes[drag.tool-T_RECT];
    if(next!=value){
        logf(L"右键尺寸调整：工具=%s 起始px=%d 鼠标横向变化=%ldpx 滑块位置=%.1f/%.1f 范围=%d..%d 原px=%d 新px=%d",
            tool_size_name(drag.tool),drag.original,pt.x-drag.origin.x,drag.thumb,drag.length,
            minimum,maximum,value,next);
        value=next;++drag.changes;
    }
    InvalidateRect(o->hwnd,&drag.bounds,FALSE);
    set_cursor_for(o,pt);
}

static bool begin_size_drag(Overlay* o,int tool,POINT pt){
    logf(L"右键尺寸拖动请求：工具=%d 按下位置=(%ld,%ld) 模式=%d 存在选区=%d",
        tool,pt.x,pt.y,o->mode,(int)o->hasSel);
    if(!o->alive||!o->hasSel||o->mode!=0||o->textDrag.kind||o->annotationDrag.active||tool<T_RECT||tool>T_EMOJI){
        logf(L"右键尺寸拖动被拒绝：当前不是可调整尺寸的标注工具或正在拖选/绘制");return false;
    }
    close_emoji_popup(o);close_style_popup(o);commit_text(o);
    clear_annotation_selection(o,L"调整后续标注的工具尺寸");o->selectedText=-1;
    SetFocus(o->hwnd);
    SetCapture(o->hwnd);
    if(GetCapture()!=o->hwnd){
        logf(L"右键尺寸拖动失败：无法取得鼠标捕获 error=%lu",GetLastError());return false;
    }
    auto& drag=o->sizeDrag;
    drag={};drag.active=true;drag.tool=tool;drag.origin=pt;drag.original=o->toolSizes[tool-T_RECT];
    drag.started=GetTickCount64();
    RECT desktop{0,0,o->shot.vw,o->shot.vh},available=desktop;
    POINT screen{o->shot.vx+pt.x,o->shot.vy+pt.y};MONITORINFO monitor{sizeof(monitor)};
    if(GetMonitorInfoW(MonitorFromPoint(screen,MONITOR_DEFAULTTONEAREST),&monitor)){
        RECT local=monitor.rcMonitor;OffsetRect(&local,-o->shot.vx,-o->shot.vy);
        RECT visible{};if(IntersectRect(&visible,&desktop,&local))available=visible;
    }
    int width=std::min(S(280),(int)(available.right-available.left));
    int height=std::min(S(76),(int)(available.bottom-available.top));
    drag.length=(float)std::max(1,width-S(32));
    drag.initial=drag.thumb=drag.length*(drag.original-tool_size_minimum(tool))/
        (float)(tool_size_maximum(tool)-tool_size_minimum(tool));
    int x=std::clamp((int)lround(pt.x-SF(16)-drag.initial),(int)available.left,(int)available.right-width);
    int y=(o->tbVisible?(int)o->tbY:pt.y)-height-S(10);
    if(y<available.top)y=(o->tbVisible?(int)(o->tbY+SF(TB_HEIGHT)):pt.y)+S(10);
    y=std::clamp(y,(int)available.top,(int)available.bottom-height);drag.bounds={x,y,x+width,y+height};
    o->tool=tool;o->sizeRightConsumed=true;o->hoverItem=-1;
    hide_stamp_preview(o,L"开始右键拖动尺寸");
    RECT toolbar{(LONG)floor(o->tbX),(LONG)floor(o->tbY),(LONG)ceil(o->tbX+toolbar_width(o)),
        (LONG)ceil(o->tbY+SF(TB_HEIGHT))};
    if(o->tbVisible)InvalidateRect(o->hwnd,&toolbar,FALSE);
    InvalidateRect(o->hwnd,&drag.bounds,FALSE);
    set_cursor_for(o,pt);
    logf(L"右键尺寸拖动开始：工具=%s 原px=%d 范围=%d..%d 滑轨=%.1fpx；不创建大小窗口，松开保存，Esc 撤销",
        tool_size_name(tool),drag.original,tool_size_minimum(tool),tool_size_maximum(tool),drag.length);
    return true;
}

static void set_tool(Overlay* o, int tool) {
    clear_annotation_selection(o,L"切换标注工具");
    o->tool = (o->tool == tool) ? T_NONE : tool;
    if(o->tool!=T_NONE&&o->tool!=T_TEXT)o->selectedText=-1;
    logf(L"标注工具选择：工具=%d；文字使用系统箭头，不随字号修改鼠标；画笔与马赛克使用截图像素直径",o->tool);
    overlay_update_brush_cursor(o);
    InvalidateRect(o->hwnd, nullptr, FALSE);
}

static void apply_annotation_change(Overlay* o,const AnnotationChange& change,bool forward){
    clear_annotation_selection(o,L"撤销或重做标注");
    const auto& value=forward?change.after:change.before;
    const auto& previous=forward?change.before:change.after;
    if(value){
        if(previous)o->anns[change.index]=*value;
        else o->anns.insert(o->anns.begin()+change.index,*value);
    }else o->anns.erase(o->anns.begin()+change.index);
    o->selectedText=-1;o->numberSeq=0;
    for(const auto& ann:o->anns)if(ann.kind==AK_NUMBER)o->numberSeq=std::max(o->numberSeq,ann.index);
    overlay_update_brush_cursor(o);InvalidateRect(o->hwnd,nullptr,FALSE);
}
static void undo(Overlay* o){
    commit_text(o);if(o->undoStack.empty())return;
    auto change=o->undoStack.back();o->undoStack.pop_back();
    apply_annotation_change(o,change,false);o->redoStack.push_back(change);
    logf(L"标注撤销：索引=%d 剩余撤销=%zu 可重做=%zu",change.index,o->undoStack.size(),o->redoStack.size());
}
static void redo(Overlay* o){
    commit_text(o);if(o->redoStack.empty())return;
    auto change=o->redoStack.back();o->redoStack.pop_back();
    apply_annotation_change(o,change,true);o->undoStack.push_back(change);
    logf(L"标注重做：索引=%d 可撤销=%zu 剩余重做=%zu",change.index,o->undoStack.size(),o->redoStack.size());
}

static void cycle_color(Overlay* o) {
    o->colorIdx = (o->colorIdx + 1) % N_PALETTE;
    InvalidateRect(o->hwnd, nullptr, FALSE);
}

static void begin_select(Overlay* o, POINT pt) {
    clear_annotation_selection(o,L"重新选择截图区域");
    o->mode = 1;
    o->dragFrom = pt;
    o->sel.left = pt.x; o->sel.top = pt.y;
    o->sel.right = pt.x + 1; o->sel.bottom = pt.y + 1;
    o->hasSel = true;
    o->anns.clear();
    o->undoStack.clear();o->redoStack.clear();
    o->selectedText=-1;
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
    update_stamp_preview(o,pt);
    if(o->annotationDrag.active){SetCursor(LoadCursorW(nullptr,IDC_SIZEALL));return;}
    if(!o->sizeDrag.active){
        int handle=text_frame_hit(o,pt);Ann text;
        if(o->textDrag.kind){
            text=current_text_drag(o);
            SetCursor(LoadCursorW(nullptr,text_handle_cursor(text,o->textDrag.handle)));return;
        }
        if(handle>=0&&selected_text(o,text)){
            SetCursor(LoadCursorW(nullptr,text_handle_cursor(text,handle)));return;
        }
    }
    if(o->sizeDrag.active){
        HCURSOR cursor=brush_cursor_at(o,pt)?brush_cursor(o):LoadCursorW(nullptr,IDC_SIZEWE);
        SetCursor(cursor?cursor:LoadCursorW(nullptr,IDC_SIZEWE));return;
    }
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

    if(brush_cursor_at(o,pt)){
        HCURSOR cursor=brush_cursor(o);
        SetCursor(cursor?cursor:pointer_cursor());
        return;
    }
    HICON pc = o->tool==T_TEXT||o->editing ? LoadCursorW(nullptr, IDC_ARROW) : pointer_cursor();
    SetCursor(pc ? pc : LoadCursorW(nullptr, IDC_ARROW));
}

// ------------------------------------------------------------------ 窗口过程
static LRESULT CALLBACK ov_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Overlay* o = (Overlay*)GetWindowLongPtrW(h, GWLP_USERDATA);
    std::optional<LogTrace> trace;
    if (o) trace.emplace(o->traceId);

    switch (msg) {
    case WM_CREATE: {
        auto* cs = (CREATESTRUCTW*)lp;
        o = (Overlay*)cs->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)o);
        o->hwnd = h;
        o->animationStart=GetTickCount64();o->animationLast=0;
        SetTimer(h,78,15,nullptr);
        layout_toolbar(o);
        // 覆盖层本身不吃输入法：否则字母快捷键会被 IME 变成 VK_PACKET
        ImmAssociateContext(h, nullptr);
        return 0;
    }

    case WM_SETFOCUS:
        if(o&&o->editing)edit_attach_ime(o);
        else ImmAssociateContext(h,nullptr);
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
        if(o&&(o->textDrag.kind||o->annotationDrag.active))return 0;
        if (!o || !o->editing) break;
        wchar_t c = (wchar_t)wp;
        if (c == L'\r' || c == L'\n') { commit_text(o); return 0; }
        if(c==27&&o->ignoreTextEscape){o->ignoreTextEscape=false;return 0;}
        if (c == 27) { cancel_text(o); return 0; }
        if (c == L'\b') {
            GetAsyncKeyState(VK_BACK);
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
                auto text=editing_text(o);
                auto point=text.text_point({0,text_line_height((int)text.fontSize)*text.textScaleY});
                cf.ptCurrentPos.x=o->sel.left+(LONG)point.X;cf.ptCurrentPos.y=o->sel.top+(LONG)point.Y;
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
        if(o&&wp==78){
            uint64_t time=overlay_animation_time(o);
            for(const auto& a:o->anns)if(a.kind==AK_EMOJI&&a.emojiAnimation&&
                a.emojiAnimation->frame_index(time)!=a.emojiAnimation->frame_index(o->animationLast)){
                auto r=a.bounds();RECT dirty{(LONG)floor(r.X)+o->sel.left-2,(LONG)floor(r.Y)+o->sel.top-2,
                    (LONG)ceil(r.GetRight())+o->sel.left+2,(LONG)ceil(r.GetBottom())+o->sel.top+2};
                InvalidateRect(h,&dirty,FALSE);
            }
            if(o->stampPreviewVisible&&o->stampPreviewKey.emojiAnimation){
                POINT pt{};GetCursorPos(&pt);ScreenToClient(h,&pt);update_stamp_preview(o,pt);
            }
            o->animationLast=time;return 0;
        }
        if (o && o->editing && !o->textDrag.kind && !o->annotationDrag.active && wp == TIMER_EDIT) {
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
        if(o&&o->cancelOcr){delete (JobMsg*)lp;logf(L"丢弃已取消的识别结果，不覆盖主开关关闭状态");return 0;}
        if (o) on_ocr_done(o, (JobMsg*)lp);
        return 0;

    case WM_JJ_TRANS:
        if (o) on_trans_done(o, (JobMsg*)lp);
        return 0;

    case WM_MOUSEMOVE: {
        if (!o) return 0;
        if(o->annotationDrag.active){
            POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
            if(wp&MK_RBUTTON)move_annotation_drag(o,point);
            else end_annotation_drag(o,false,L"右键已在其他位置松开",false);
            set_cursor_for(o,point);return 0;
        }
        if(o->textDrag.kind){
            POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
            if(wp&MK_LBUTTON)move_text_drag(o,point);
            else end_text_drag(o,false,L"鼠标已松开");
            set_cursor_for(o,point);return 0;
        }
        if(o->sizeDrag.active){
            if(wp&MK_RBUTTON)move_size_drag(o,{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)});
            else end_size_drag(o,false,L"鼠标已在其他位置松开");
            return 0;
        }
        if(o->stampPressed&&!(wp&MK_LBUTTON)){
            o->stampPressed=false;
            logf(L"标注按键状态恢复：鼠标已松开，恢复序号/表情落点虚影");
        }
        int oldHover=o->hoverItem,oldPanelHover=o->panelBtnHover;
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
                if(o->cur.pts.empty()||o->cur.pts.back().X!=local.X||o->cur.pts.back().Y!=local.Y)
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
        if(o->mode!=0||!o->hasSel||(o->tool!=T_NUMBER&&o->tool!=T_EMOJI)||
           oldHover!=o->hoverItem||oldPanelHover!=o->panelBtnHover)
            InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    case WM_MOUSELEAVE:
        if (o) {
            o->stampPreviewTracking=false;
            hide_stamp_preview(o,L"指针离开截图覆盖层");
            o->hoverItem = -1;
            o->panelBtnHover = -1;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN: {
        if (!o) return 0;
        if(o->sizeDrag.active||o->annotationDrag.active)return 0;
        hide_stamp_preview(o,L"按下鼠标，准备放置或操作控件");
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        close_emoji_popup(o);
        if(text_mouse_down(o,pt))return 0;
        clear_annotation_selection(o,L"左键绘制或放置新图案");
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
                o->cur.width = o->annotation_width(o->tool);
                logf(L"标注笔画开始：工具=%d 设置px=%d 实际宽度=%.2f截图像素",
                     o->tool,o->toolSizes[o->tool-T_RECT],o->cur.width);
                if (o->tool == T_RECT) { o->cur.kind = AK_RECT; o->cur.rect = normRect(local.X, local.Y, local.X, local.Y); }
                else if (o->tool == T_ELLIPSE) { o->cur.kind = AK_ELLIPSE; o->cur.rect = normRect(local.X, local.Y, local.X, local.Y); }
                else if (o->tool == T_ARROW) { o->cur.kind = AK_ARROW; o->cur.a = local; o->cur.b = local; }
                else if (o->tool == T_PEN) { o->cur.kind = AK_PEN; o->cur.pts.push_back(local); }
                else { o->cur.kind = AK_MOSAIC; o->cur.pts.push_back(local); ensure_mosaic(o); }
                o->hasCur = true;
                o->tbVisible = false;
                set_cursor_for(o,pt);
                return 0;
            }
            if (o->tool == T_TEXT) {
                begin_text(o, pt);
                return 0;
            }
            if (o->tool == T_NUMBER || o->tool == T_EMOJI) {
                Ann a=stamp_annotation(o,{(float)(pt.x-o->sel.left),(float)(pt.y-o->sel.top)});
                if(a.kind==AK_EMOJI&&!a.emojiImage){
                    logf(L"图片放置未执行：images 目录没有可用图片，请通过表情按钮选择图片");
                    show_emoji_picker(o);return 0;
                }
                o->stampPressed=true;
                if(a.kind==AK_NUMBER)o->numberSeq=a.index;
                add_annotation(o,a);
                logf(L"序号/表情已放置：工具=%d 序号=%d 表情=%d 中心=(%.0f,%.0f) 实际边长=%.2f截图像素 标注总数=%zu",
                    o->tool,a.index,a.emojiIndex,a.pos.X,a.pos.Y,a.bounds().Width,o->anns.size());
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
        if(o->sizeDrag.active||o->annotationDrag.active)return 0;
        if(o->textDrag.kind){
            POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
            move_text_drag(o,point);end_text_drag(o,false,L"左键松开");set_cursor_for(o,point);return 0;
        }
        o->stampPressed=false;
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
            if(o->cur.isStroke()){
                PointF local((float)(pt.x-o->sel.left),(float)(pt.y-o->sel.top));
                if(o->cur.pts.empty()||o->cur.pts.back().X!=local.X||o->cur.pts.back().Y!=local.Y)
                    o->cur.pts.push_back(local);
            }
            bool keep = true;
            if (o->cur.kind == AK_RECT || o->cur.kind == AK_ELLIPSE) {
                keep = o->cur.rect.Width > 2 || o->cur.rect.Height > 2;
            } else if (o->cur.kind == AK_ARROW) {
                float dx = o->cur.b.X - o->cur.a.X, dy = o->cur.b.Y - o->cur.a.Y;
                keep = (dx * dx + dy * dy) > 4;
            }
            if (keep) add_annotation(o,o->cur);
            logf(L"标注笔画结束：类型=%d 宽度=%.2f截图像素 轨迹点=%zu 保留=%d",
                 (int)o->cur.kind,o->cur.width,o->cur.pts.size(),(int)keep);
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
            if(o->sizeDrag.active||o->annotationDrag.active||o->textDrag.kind)return 0;
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if(o->editing&&(!o->editText.empty()||!o->composing.empty()||o->editingIndex>=0))return 0;
            int index=text_at(o,pt);
            if(index>=0){begin_text(o,pt,index);return 0;}
            if(o->editing)return 0;
            if(o->tool>=T_RECT&&o->tool<=T_EMOJI){
                SendMessageW(h,WM_LBUTTONDOWN,wp,lp);return 0;
            }
            if (ptInRect(o->sel, pt.x, pt.y)) do_copy(o);
        }
        return 0;

    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK: {
        if(!o || !o->hasSel)return 0;
        if(o->textDrag.kind)return 0;
        if(o->sizeDrag.active||o->annotationDrag.active)return 0;
        o->sizeRightConsumed=false;
        POINT pt={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        int hi=toolbar_hit(o,pt);
        int tool=hi>=0?o->items[hi].id:
            ptInRect(o->sel,pt.x,pt.y)?o->tool:T_NONE;
        if(hi<0&&o->panel!=PM_HIDDEN&&ptInRectF(o->panelRect,(float)pt.x,(float)pt.y))return 0;
        if(hi<0&&begin_annotation_drag(o,pt)){set_cursor_for(o,pt);return 0;}
        if(tool>=T_RECT&&tool<=T_EMOJI)begin_size_drag(o,tool,pt);
        return 0;
    }

    case WM_RBUTTONUP:
        if(!o)return 0;
        if(o->annotationDrag.active){
            POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
            move_annotation_drag(o,point);end_annotation_drag(o,false,L"右键松开");
            o->sizeRightConsumed=false;set_cursor_for(o,point);return 0;
        }
        if(o->textDrag.kind)return 0;
        if(o->sizeDrag.active){
            move_size_drag(o,{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)});
            end_size_drag(o,false,L"右键松开");o->sizeRightConsumed=false;return 0;
        }
        if(o->sizeRightConsumed){o->sizeRightConsumed=false;return 0;}
        if(o->hasSel){
            POINT pt{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};int hit=toolbar_hit(o,pt);
            if(hit>=0&&o->items[hit].id==T_COLOR)show_color_picker(o);
        }
        return 0;

    case WM_CAPTURECHANGED:
        if(o&&o->annotationDrag.active&&(HWND)lp!=h)
            end_annotation_drag(o,true,L"鼠标捕获已转移",false);
        if(o&&o->sizeDrag.active&&(HWND)lp!=h)end_size_drag(o,true,L"鼠标捕获已转移");
        if(o&&o->textDrag.kind&&(HWND)lp!=h)end_text_drag(o,true,L"鼠标捕获已转移",false);
        return 0;

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
        if(o->annotationDrag.active){
            if(wp==VK_ESCAPE){
                end_annotation_drag(o,true,L"Esc 撤销");o->ignoreTextEscape=o->editing;
                GetAsyncKeyState(VK_ESCAPE);
            }
            return 0;
        }
        if(o->textDrag.kind){
            if(wp==VK_ESCAPE){
                end_text_drag(o,true,L"Esc 撤销");o->ignoreTextEscape=o->editing;
                GetAsyncKeyState(VK_ESCAPE);
            }
            return 0;
        }
        if(o->sizeDrag.active){
            if(wp==VK_ESCAPE)end_size_drag(o,true,L"Esc 撤销");
            return 0;
        }
        if (o->editing) {
            if(GetKeyState(VK_CONTROL)&0x8000){
                if(wp=='Z'||wp=='Y'){
                    if(wp=='Y'||(GetKeyState(VK_SHIFT)&0x8000))redo(o);else undo(o);
                }else if(wp=='S')do_save(o,(GetKeyState(VK_SHIFT)&0x8000)!=0);
                else if(wp=='T')do_pin(o);
            }
            // 回车 / 退格 / Esc 由键盘钩子处理，其余交给输入法
            return 0;
        }

        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

        switch (wp) {
        case VK_ESCAPE:
            if(o->selectedAnnotation>=0){
                clear_annotation_selection(o,L"Esc 取消图案选择");o->selectedText=-1;
                InvalidateRect(h,nullptr,FALSE);return 0;
            }
            if(o->selectedText>=0){o->selectedText=-1;InvalidateRect(h,nullptr,FALSE);return 0;}
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
            case 'S': do_save(o,shift); return 0;
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
            if(o->selectedAnnotation>=0){
                int index=o->selectedAnnotation;
                Ann before=o->anns[index],after=before;after.translate((float)dx,(float)dy);
                invalidate_annotation(o,before);o->anns[index]=after;
                record_annotation_change(o,index,before,after);select_annotation(o,index);
                logf(L"图案键盘微调：索引=%d 类型=%s 位移=(%d,%d)截图像素，不移动截图选区",
                    index,annotation_name(after.kind),dx,dy);
                return 0;
            }
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

    case WM_KILLFOCUS:
    case WM_CANCELMODE:
        if(o){
            end_annotation_drag(o,true,msg==WM_KILLFOCUS?L"覆盖层失去焦点":L"操作取消",false);
            end_text_drag(o,true,msg==WM_KILLFOCUS?L"覆盖层失去焦点":L"操作取消",false);
            end_size_drag(o,true,msg==WM_KILLFOCUS?L"覆盖层失去焦点":L"操作取消");
            o->stampPressed=false;
            hide_stamp_preview(o,L"截图覆盖层失去焦点或操作被取消");
        }
        break;

    case WM_DESTROY:
        if (o) {
            end_annotation_drag(o,true,L"覆盖层关闭",false);
            end_text_drag(o,true,L"覆盖层关闭",false);
            end_size_drag(o,true,L"覆盖层关闭");
            logf(L"截图会话结束：存在选区=%d，关闭覆盖层", (int)o->hasSel);
            o->alive=false;
            KillTimer(h,78);
            clear_stamp_preview(o);
            clear_brush_cursor(o);

            o->input.release();
            close_emoji_popup(o);
            close_style_popup(o);
            std::lock_guard<std::mutex> lock(o->jobMutex);
            o->cancelOcr=true;
            MSG queued;
            while(PeekMessageW(&queued,h,WM_JJ_OCR,WM_JJ_TRANS,PM_REMOVE))delete (JobMsg*)queued.lParam;
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

static void apply_overlay_settings(Overlay* o,const Settings& cfg){
    o->ocrEnabled=cfg.ocr_enabled&&ocr_is_enabled();
    o->translateEnabled=cfg.translate_enabled;
    logf(L"截图工具条显示：识别主开关=%d 翻译按钮=%d",
         (int)o->ocrEnabled,(int)o->translateEnabled);
}

void overlay_apply_display_settings(const Settings& settings){
    if(!overlay_is_open())return;
    auto* o=g_ov.get();
    if(!settings.ocr_enabled&&o->ocrRunning){
        o->cancelOcr=true;
        o->panel=PM_ERROR;o->panelTitle=L"文字识别";
        o->panelBody=L"识别主开关已关闭，模型已卸载";
        o->panelOriginal.clear();
        logf(L"当前识别任务已取消，主开关关闭后不再应用识别结果");
    }
    apply_overlay_settings(o,settings);
    layout_toolbar(o);
    o->hoverItem=-1;o->panelBtnHover=-1;
    if(o->hasSel)place_toolbar(o);
    if(o->panel!=PM_HIDDEN)place_panel(o);
    InvalidateRect(o->hwnd,nullptr,FALSE);
    logf(L"已将显示开关应用到当前截图工具条");
}

bool overlay_open() {
    LogTrace trace;
    const ULONGLONG started = GetTickCount64();
    logf(L"覆盖层请求到达：当前前台=%p", GetForegroundWindow());
    if (overlay_is_open()) {
        logf(L"截图被忽略：覆盖层已存在");
        return false;
    }
    if (capture_requires_elevation(GetForegroundWindow())) {
        logf(L"截图请求被拦截：前台程序为管理员权限，微截为普通权限，未进入抓屏");
        app_game_permission_hint();
        return false;
    }
    ensure_class();

    auto o = std::make_shared<Overlay>();
    o->traceId = trace.id();
    Settings cfg = load_settings();
    apply_overlay_settings(o.get(),cfg);
    o->strokeWidth=cfg.stroke_width;o->textSize=cfg.text_size;
    for(int i=0;i<4;++i)o->toolSizes[i]=cfg.stroke_width;
    o->toolSizes[4]=std::clamp(cfg.stroke_width*8,4,120);o->toolSizes[5]=cfg.text_size;
    o->recentColors=load_recent_colors(cfg.recent_colors);
    if(!o->recentColors.empty())o->ink=Color(o->recentColors.front());
    const auto configured=GetTickCount64();
    if (!capture_desktop(o->shot,cfg)) {
        logf(L"抓屏失败");
        return false;
    }
    const auto captured=GetTickCount64();
    logf(L"抓屏 %dx%d @ (%d,%d)", o->shot.vw, o->shot.vh, o->shot.vx, o->shot.vy);

    g_ov = o;

    // 在覆盖层挡住桌面前探测鼠标下窗口，首帧直接绘制高亮，避免再次枚举和全屏重绘。
    POINT cur{};
    RECT wr{};
    if(GetCursorPos(&cur)&&window_rect_at(cur.x,cur.y,wr)){
        o->hoverWin={wr.left-o->shot.vx,wr.top-o->shot.vy,wr.right-o->shot.vx,wr.bottom-o->shot.vy};
        o->hasHover=true;
    }
    const auto probed=GetTickCount64();
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, OV_CLASS, L"微截",
                                WS_POPUP | WS_CLIPCHILDREN, o->shot.vx, o->shot.vy, o->shot.vw, o->shot.vh,
                                nullptr, nullptr, GetModuleHandleW(nullptr), o.get());
    if (!hwnd) {
        g_ov.reset();
        logf(L"创建覆盖层失败 %lu", GetLastError());
        return false;
    }

    ShowWindow(hwnd, SW_SHOW);
    const auto created=GetTickCount64();
    if (!o->input.acquire(hwnd)) {
        DestroyWindow(hwnd);
        logf(L"覆盖层请求返回：失败，未能取得游戏窗口的输入焦点，耗时=%llums",
             GetTickCount64() - started);
        return false;
    }
    const auto focused=GetTickCount64();
    UpdateWindow(hwnd);
    SetCursor(pointer_cursor());
    HWND fg = GetForegroundWindow();
    logf(L"覆盖层已显示：请求返回成功 fg=%p hwnd=%p match=%d 耗时=%llums",
         fg, hwnd, (int)(fg == hwnd), GetTickCount64() - started);
    logf(L"截图唤起分段：设置权限=%llums 抓屏=%llums 窗口命中=%llums 创建显示=%llums 接管输入=%llums 首帧绘制=%llums",
        configured-started,captured-configured,probed-captured,created-probed,focused-created,
        GetTickCount64()-focused);
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

