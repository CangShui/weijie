#include "annotate.h"
#include "emoji_renderer.h"
#include <memory>

using namespace Gdiplus;

SizeF Ann::text_natural_size() const{
    if(measuredFont==fontSize&&measuredScale==g_scale&&measuredText==text)return measuredSize;
    Font font(FONT_UI(),SF(fontSize),FontStyleRegular,UnitPixel);
    Bitmap image(1,1);Graphics graphics(&image);RectF measured;
    if(!text.empty())graphics.MeasureString(text.c_str(),-1,&font,PointF(0,0),&measured);
    measuredSize={std::max(text.empty()?SF(48):SF(12),measured.Width),
                  std::max(font.GetHeight(96.0f),measured.Height)};
    measuredText=text;measuredFont=fontSize;measuredScale=g_scale;return measuredSize;
}
SizeF Ann::text_frame_size() const{
    auto size=text_natural_size();return {size.Width*textScaleX,size.Height*textScaleY};
}
PointF Ann::text_point(PointF local) const{
    float radians=textAngle*3.14159265359f/180,cs=cosf(radians),sn=sinf(radians);
    return {pos.X+local.X*cs-local.Y*sn,pos.Y+local.X*sn+local.Y*cs};
}
PointF Ann::text_local(PointF world) const{
    float radians=textAngle*3.14159265359f/180,cs=cosf(radians),sn=sinf(radians);
    float x=world.X-pos.X,y=world.Y-pos.Y;return {x*cs+y*sn,-x*sn+y*cs};
}
bool Ann::text_contains(PointF world) const{
    auto p=text_local(world);auto size=text_frame_size();
    return p.X>=0&&p.Y>=0&&p.X<=size.Width&&p.Y<=size.Height;
}
void Ann::apply_text_transform(Graphics& graphics) const{
    graphics.TranslateTransform(pos.X,pos.Y);graphics.RotateTransform(textAngle);
    graphics.ScaleTransform(textScaleX,textScaleY);
}

static void setup_pen(Pen& pen, const Color& c, float w) {
    pen.SetColor(c);
    pen.SetWidth(w);
    pen.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
    pen.SetLineJoin(LineJoinRound);
}

bool Ann::hit_test(PointF point,float tolerance) const {
    GraphicsPath path;
    switch(kind){
    case AK_RECT: path.AddRectangle(rect);break;
    case AK_ELLIPSE: path.AddEllipse(rect);break;
    case AK_ARROW: {
        float dx=b.X-a.X,dy=b.Y-a.Y,angle=atan2f(dy,dx);
        if(dx*dx+dy*dy<4)return false;
        float head=std::max(SF(9),width*3.6f);
        PointF triangle[]={b,{b.X-head*cosf(angle-.42f),b.Y-head*sinf(angle-.42f)},
                            {b.X-head*cosf(angle+.42f),b.Y-head*sinf(angle+.42f)}};
        GraphicsPath tip;tip.AddPolygon(triangle,3);
        Pen padding(Color::Black,tolerance*2);
        if(tip.IsVisible(point)||tip.IsOutlineVisible(point,&padding))return true;
        path.AddLine(a,{b.X-head*.55f*cosf(angle),b.Y-head*.55f*sinf(angle)});break;
    }
    case AK_PEN:
    case AK_MOSAIC:
        if(pts.empty())return false;
        if(pts.size()==1)return hypotf(point.X-pts[0].X,point.Y-pts[0].Y)<=width/2+tolerance;
        path.AddLines(pts.data(),(INT)pts.size());break;
    case AK_TEXT: {
        auto local=text_local(point);auto size=text_frame_size();
        return local.X>=-tolerance&&local.Y>=-tolerance&&local.X<=size.Width+tolerance&&local.Y<=size.Height+tolerance;
    }
    case AK_NUMBER:
        return hypotf(point.X-pos.X,point.Y-pos.Y)<=SF(diameter/2)+tolerance;
    case AK_EMOJI: {
        auto image=emojiAnimation&&!emojiAnimation->frames.empty()?emojiAnimation->frames.front():
                   emojiImage?emojiImage:emoji_image(emojiIndex);
        if(!image)return false;
        auto area=stamp_image_rect(image.get(),pos,SF(fontSize));area.Inflate(tolerance,tolerance);
        return area.Contains(point);
    }
    }
    Pen pen(Color::Black,1);
    setup_pen(pen,Color::Black,width+tolerance*2);
    return path.IsOutlineVisible(point,&pen)!=FALSE;
}

void Ann::translate(float dx,float dy){
    switch(kind){
    case AK_RECT:case AK_ELLIPSE:rect.X+=dx;rect.Y+=dy;break;
    case AK_ARROW:a.X+=dx;a.Y+=dy;b.X+=dx;b.Y+=dy;break;
    case AK_PEN:case AK_MOSAIC:for(auto& point:pts){point.X+=dx;point.Y+=dy;}break;
    case AK_TEXT:case AK_NUMBER:case AK_EMOJI:pos.X+=dx;pos.Y+=dy;break;
    }
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

void Ann::draw(Graphics& g, Bitmap* mosaic,unsigned long long time) const {
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
            g.FillEllipse(&br,brush_footprint(pts[0],width));
            break;
        }
        GraphicsPath path;
        path.AddLines(pts.data(), (INT)pts.size());
        g.DrawPath(&pen, &path);
        break;
    }
    case AK_MOSAIC: {
        if (!mosaic || pts.empty()) break;
        GraphicsPath outline;
        if (pts.size() == 1) {
            outline.AddEllipse(brush_footprint(pts[0],width));
        } else {
            outline.AddLines(pts.data(), (INT)pts.size());
            Pen wide(color,1);
            setup_pen(wide,color,width);
            outline.Widen(&wide);
        }
        ColorMatrix cm={1,0,0,0,0, 0,1,0,0,0, 0,0,1,0,0, 0,0,0,color.GetA()/255.0f,0, 0,0,0,0,1};
        ImageAttributes attributes;attributes.SetColorMatrix(&cm);
        TextureBrush texture(mosaic,Rect(0,0,(INT)mosaic->GetWidth(),(INT)mosaic->GetHeight()),&attributes);
        g.FillPath(&texture,&outline);
        break;
    }
    case AK_TEXT: {
        if (text.empty()) break;
        auto state=g.Save();apply_text_transform(g);
        Font font(FONT_UI(), SF(fontSize), FontStyleRegular, UnitPixel);
        SolidBrush br(color);
        g.DrawString(text.c_str(), -1, &font, PointF(0,0), nullptr, &br);g.Restore(state);
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
        auto* image=emojiAnimation&&!emojiAnimation->frames.empty()?
            emojiAnimation->frames[emojiAnimation->frame_index(time)].get():
            emojiImage?emojiImage.get():emoji_image(emojiIndex).get();
        draw_stamp_image(g,image,pos,SF(fontSize));
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
        float angle=atan2f(b.Y-a.Y,b.X-a.X),head=std::max(SF(9),width*3.6f);
        for(float side:{-.42f,.42f}){
            float x=b.X-head*cosf(angle+side),y=b.Y-head*sinf(angle+side);
            l=std::min(l,x);t=std::min(t,y);r=std::max(r,x);bo=std::max(bo,y);
        }
        return RectF(l-width,t-width,r-l+width*2,bo-t+width*2);
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
        auto size=text_frame_size();
        PointF corners[]={text_point({0,0}),text_point({size.Width,0}),
            text_point({size.Width,size.Height}),text_point({0,size.Height})};
        float left=corners[0].X,top=corners[0].Y,right=left,bottom=top;
        for(auto point:corners){
            left=std::min(left,point.X);top=std::min(top,point.Y);
            right=std::max(right,point.X);bottom=std::max(bottom,point.Y);
        }
        return {left,top,right-left,bottom-top};
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
