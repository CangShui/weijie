#include "annotate.h"
#include "emoji_renderer.h"
#include <memory>

using namespace Gdiplus;

static void setup_pen(Pen& pen, const Color& c, float w) {
    pen.SetColor(c);
    pen.SetWidth(w);
    pen.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
    pen.SetLineJoin(LineJoinRound);
}

Bitmap* make_mosaic(Bitmap* crop) {
    if (!crop) return nullptr;
    int w = (int)crop->GetWidth();
    int h = (int)crop->GetHeight();
    if (w < 2 || h < 2) return nullptr;

    int sw = std::max(1, w / 9);
    int sh = std::max(1, h / 9);

    Bitmap tiny(sw, sh, PixelFormat32bppRGB);
    {
        Graphics gs(&tiny);
        gs.SetInterpolationMode(InterpolationModeHighQualityBilinear);
        gs.SetPixelOffsetMode(PixelOffsetModeHalf);
        gs.DrawImage(crop, Rect(0, 0, sw, sh), 0, 0, w, h, UnitPixel);
    }

    auto* out = new Bitmap(w, h, PixelFormat32bppRGB);
    {
        Graphics go(out);
        go.SetInterpolationMode(InterpolationModeNearestNeighbor);
        go.SetPixelOffsetMode(PixelOffsetModeHalf);
        go.DrawImage(&tiny, Rect(0, 0, w, h), 0, 0, sw, sh, UnitPixel);
    }
    return out;
}

void Ann::draw(Graphics& g, Bitmap* mosaic) const {
    switch (kind) {
    case AK_RECT: {
        Pen pen(Color(255, 0, 0, 0), 1);
        setup_pen(pen, color, width);
        g.DrawRectangle(&pen, rect.X, rect.Y, rect.Width, rect.Height);
        break;
    }
    case AK_ELLIPSE: {
        Pen pen(Color(255, 0, 0, 0), 1);
        setup_pen(pen, color, width);
        g.DrawEllipse(&pen, rect.X, rect.Y, rect.Width, rect.Height);
        break;
    }
    case AK_ARROW: {
        float dx = b.X - a.X, dy = b.Y - a.Y;
        float dist = sqrtf(dx * dx + dy * dy);
        if (dist < 2.0f) break;
        float ang = atan2f(dy, dx);
        float head = std::max(SF(9.0f), width * 3.6f);
        PointF p1(b.X - head * cosf(ang - 0.42f), b.Y - head * sinf(ang - 0.42f));
        PointF p2(b.X - head * cosf(ang + 0.42f), b.Y - head * sinf(ang + 0.42f));
        PointF tail(b.X - head * 0.55f * cosf(ang), b.Y - head * 0.55f * sinf(ang));

        Pen pen(Color(255, 0, 0, 0), 1);
        setup_pen(pen, color, width);
        g.DrawLine(&pen, a, tail);

        SolidBrush br(color);
        PointF tri[3] = {b, p1, p2};
        g.FillPolygon(&br, tri, 3);
        break;
    }
    case AK_PEN: {
        if (pts.empty()) break;
        Pen pen(Color(255, 0, 0, 0), 1);
        setup_pen(pen, color, width);
        if (pts.size() == 1) {
            SolidBrush br(color);
            float r = width / 2.0f;
            g.FillEllipse(&br, pts[0].X - r, pts[0].Y - r, r * 2, r * 2);
            break;
        }
        GraphicsPath path;
        path.AddLines(pts.data(), (INT)pts.size());
        g.DrawPath(&pen, &path);
        break;
    }
    case AK_MOSAIC: {
        if (!mosaic || pts.empty()) break;
        GraphicsPath path;
        if (pts.size() == 1) {
            PointF two[2] = {pts[0], PointF(pts[0].X + 0.01f, pts[0].Y + 0.01f)};
            path.AddLines(two, 2);
        } else {
            path.AddLines(pts.data(), (INT)pts.size());
        }
        Pen wide(color, 1);
        setup_pen(wide, color, width);

        GraphicsState st = g.Save();
        GraphicsPath outline;
        outline.AddPath(&path, FALSE);
        outline.Widen(&wide);
        g.SetClip(&outline, CombineModeIntersect);
        ColorMatrix cm={1,0,0,0,0, 0,1,0,0,0, 0,0,1,0,0, 0,0,0,color.GetA()/255.0f,0, 0,0,0,0,1};
        ImageAttributes attributes;attributes.SetColorMatrix(&cm);
        g.DrawImage(mosaic, RectF(0, 0, (REAL)mosaic->GetWidth(), (REAL)mosaic->GetHeight()),
                    0, 0, (REAL)mosaic->GetWidth(), (REAL)mosaic->GetHeight(), UnitPixel,&attributes);
        g.Restore(st);
        break;
    }
    case AK_TEXT: {
        if (text.empty()) break;
        Font font(FONT_UI(), SF(fontSize), FontStyleRegular, UnitPixel);
        SolidBrush br(color);
        g.DrawString(text.c_str(), -1, &font, PointF(pos.X, pos.Y), nullptr, &br);
        break;
    }
    case AK_NUMBER: {
        float r = SF(diameter/2);
        SolidBrush br(color);
        g.FillEllipse(&br, pos.X - r, pos.Y - r, r * 2, r * 2);
        Font font(FONT_NUM(), SF(diameter*0.54f), FontStyleBold, UnitPixel);
        SolidBrush white(Color(color.GetA(), 255, 255, 255));
        StringFormat fmt;
        fmt.SetAlignment(StringAlignmentCenter);
        fmt.SetLineAlignment(StringAlignmentCenter);
        wchar_t num[16];
        _snwprintf_s(num, _countof(num), _TRUNCATE, L"%d", index);
        g.DrawString(num, -1, &font, RectF(pos.X - r, pos.Y - r, r * 2, r * 2), &fmt, &white);
        break;
    }
    case AK_EMOJI: {
        draw_emoji(g, emojiIndex, pos, SF(fontSize));
        break;
    }
    }
}

RectF Ann::bounds() const {
    switch (kind) {
    case AK_RECT:
    case AK_ELLIPSE:
        return RectF(rect.X - width, rect.Y - width, rect.Width + width * 2, rect.Height + width * 2);
    case AK_ARROW: {
        float l = std::min(a.X, b.X), t = std::min(a.Y, b.Y);
        float r = std::max(a.X, b.X), bo = std::max(a.Y, b.Y);
        return RectF(l - 10, t - 10, r - l + 20, bo - t + 20);
    }
    case AK_PEN:
    case AK_MOSAIC: {
        if (pts.empty()) return RectF();
        float l = pts[0].X, t = pts[0].Y, r = l, bo = t;
        for (auto& p : pts) {
            l = std::min(l, p.X); t = std::min(t, p.Y);
            r = std::max(r, p.X); bo = std::max(bo, p.Y);
        }
        return RectF(l - width, t - width, r - l + width * 2, bo - t + width * 2);
    }
    case AK_TEXT: {
        return RectF(pos.X, pos.Y, SF(200.0f), SF(30.0f));
    }
    case AK_NUMBER: {
        float rr = SF(diameter/2);
        return RectF(pos.X - rr, pos.Y - rr, rr * 2, rr * 2);
    }
    case AK_EMOJI: {
        const float s = SF(fontSize);
        return RectF(pos.X - s / 2, pos.Y - s / 2, s, s);
    }
    }
    return RectF();
}
