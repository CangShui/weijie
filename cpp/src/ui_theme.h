#pragma once
#include "common.h"
#include <commctrl.h>
#include <dwmapi.h>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")

// Shared native UI tokens. Controls retain Win32 keyboard/accessibility behavior.
namespace ui {
using namespace Gdiplus;
inline Color ink() { return Color(255, 35, 43, 39); }
inline Color muted() { return Color(255, 132, 140, 135); }
inline Color line() { return Color(255, 229, 234, 230); }
inline Color green() { return Color(255, 7, 193, 96); }
inline Color tint() { return Color(255, 232, 248, 239); }
inline Color canvas() { return Color(255, 246, 248, 247); }
inline HBRUSH white_brush() { static HBRUSH b = CreateSolidBrush(RGB(255,255,255)); return b; }
inline HBRUSH canvas_brush() { static HBRUSH b = CreateSolidBrush(RGB(246,248,247)); return b; }

inline void path(GraphicsPath& p, RectF r, float radius) {
    radius = std::min(radius, std::min(r.Width, r.Height) / 2);
    float d = radius * 2;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.GetRight()-d, r.Y, d, d, 270, 90);
    p.AddArc(r.GetRight()-d, r.GetBottom()-d, d, d, 0, 90);
    p.AddArc(r.X, r.GetBottom()-d, d, d, 90, 90);
    p.CloseFigure();
}
inline void box(Graphics& g, RectF r, Color fill, float radius=8, Color border=Color(0,0,0,0)) {
    GraphicsPath p; path(p,r,SF(radius)); SolidBrush b(fill); g.FillPath(&b,&p);
    if(border.GetA()) { Pen pen(border,1); g.DrawPath(&pen,&p); }
}
inline void text(Graphics& g, const std::wstring& t, RectF r, float size=13,
                 Color color=ink(), bool bold=false, StringAlignment align=StringAlignmentNear) {
    Font f(L"Microsoft YaHei UI",SF(size),bold?FontStyleBold:FontStyleRegular,UnitPixel);
    SolidBrush b(color); StringFormat fmt; fmt.SetAlignment(align); fmt.SetLineAlignment(StringAlignmentCenter);
    fmt.SetTrimming(StringTrimmingEllipsisCharacter);
    g.DrawString(t.c_str(),-1,&f,r,&fmt,&b);
}
inline void separator(Graphics& g, float x, float y, float width) {
    Pen p(line(),1); g.DrawLine(&p,SF(x),SF(y),SF(x+width),SF(y));
}
inline void checker(Graphics& g, RectF r) {
    auto st=g.Save(); g.SetClip(r);
    int cell=std::max(3,S(5));
    for(int y=(int)r.Y;y<r.GetBottom();y+=cell)for(int x=(int)r.X;x<r.GetRight();x+=cell){
        SolidBrush b(((x-(int)r.X)/cell+(y-(int)r.Y)/cell)%2
                     ? Color(255,224,230,226):Color(255,248,250,249));
        g.FillRectangle(&b,x,y,cell,cell);
    }
    g.Restore(st);
}
inline void round_window(HWND h, int radius=12) {
    RECT r{};GetWindowRect(h,&r);
    SetWindowRgn(h,CreateRoundRectRgn(0,0,r.right-r.left+1,r.bottom-r.top+1,S(radius*2),S(radius*2)),TRUE);
    int corner=2; DwmSetWindowAttribute(h,33,&corner,sizeof(corner));
}
inline std::wstring caption(HWND h) {
    int n=GetWindowTextLengthW(h);std::wstring s(n+1,L'\0');GetWindowTextW(h,s.data(),n+1);s.resize(n);return s;
}
struct TrackbarGeometry {
    RECT channel{},thumb{};
    float cx=0,cy=0,radius=7;
};
inline TrackbarGeometry trackbar_geometry(HWND h){
    TrackbarGeometry geometry;
    SendMessageW(h,TBM_GETCHANNELRECT,0,(LPARAM)&geometry.channel);
    SendMessageW(h,TBM_GETTHUMBRECT,0,(LPARAM)&geometry.thumb);
    int minimum=(int)SendMessageW(h,TBM_GETRANGEMIN,0,0),maximum=(int)SendMessageW(h,TBM_GETRANGEMAX,0,0);
    int position=(int)SendMessageW(h,TBM_GETPOS,0,0);
    geometry.cx=geometry.channel.left+(geometry.channel.right-geometry.channel.left)*
        (position-minimum)/(float)std::max(1,maximum-minimum);
    auto visualX=(INT_PTR)GetPropW(h,L"JjSliderVisualX");
    auto visualValue=(INT_PTR)GetPropW(h,L"JjSliderVisualValue");
    if(visualX&&visualValue&&visualValue-1==position)geometry.cx=(float)(visualX-1);
    geometry.cy=(geometry.thumb.top+geometry.thumb.bottom)/2.0f;
    auto* scale=(const double*)GetPropW(GetAncestor(h,GA_ROOT),L"JjScale");
    geometry.radius=(float)(7*(scale?*scale:g_scale));
    return geometry;
}
inline void trackbar_notify(HWND h,UINT code){
    int position=(int)SendMessageW(h,TBM_GETPOS,0,0);
    SendMessageW(GetParent(h),WM_HSCROLL,MAKEWPARAM(code,position),(LPARAM)h);
}
inline bool trackbar_mouse_position(HWND h,int x){
    auto geometry=trackbar_geometry(h);
    int minimum=(int)SendMessageW(h,TBM_GETRANGEMIN,0,0),maximum=(int)SendMessageW(h,TBM_GETRANGEMAX,0,0);
    double fraction=std::clamp((x-geometry.channel.left)/(double)std::max(1L,geometry.channel.right-geometry.channel.left),0.0,1.0);
    int position=minimum+(int)lround(fraction*(maximum-minimum));
    int previous=(int)SendMessageW(h,TBM_GETPOS,0,0);
    if(position!=previous){
        SendMessageW(h,TBM_SETPOS,FALSE,position);
    }
    int visualX=std::clamp(x,(int)geometry.channel.left,(int)geometry.channel.right);
    SetPropW(h,L"JjSliderVisualX",(HANDLE)(INT_PTR)(visualX+1));
    SetPropW(h,L"JjSliderVisualValue",(HANDLE)(INT_PTR)(position+1));
    InvalidateRect(h,nullptr,FALSE);
    UpdateWindow(h);
    if(position!=previous){
        logf(L"滑条鼠标数值更新：控件=%d 横坐标=%d 绘制中心=%d 范围=%d..%d 原值=%d 新值=%d",
             GetDlgCtrlID(h),x,visualX,minimum,maximum,previous,position);
        trackbar_notify(h,TB_THUMBTRACK);
    }
    return position!=previous;
}
inline void trackbar_end_mouse(HWND h,bool cancelled){
    RemovePropW(h,L"JjSliderDragging");RemovePropW(h,L"JjSliderOffset");
    if(GetCapture()==h)ReleaseCapture();
    if(!cancelled)trackbar_notify(h,TB_THUMBPOSITION);
    trackbar_notify(h,TB_ENDTRACK);
    logf(L"滑条鼠标交互返回：控件=%d 取消=%d 当前值=%ld 捕获已释放=%d",
         GetDlgCtrlID(h),(int)cancelled,(long)SendMessageW(h,TBM_GETPOS,0,0),(int)(GetCapture()!=h));
    RemovePropW(h,L"JjSliderTrace");
}
inline LRESULT CALLBACK control_proc(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR kind) {
    if(kind==4&&(m==TBM_SETPOS||m==TBM_SETRANGE||m==TBM_SETRANGEMIN||m==TBM_SETRANGEMAX||
                m==WM_KEYDOWN||m==WM_MOUSEWHEEL||m==WM_SIZE)){
        RemovePropW(h,L"JjSliderVisualX");RemovePropW(h,L"JjSliderVisualValue");
    }
    if(m==WM_MOUSEMOVE) {
        if(!GetPropW(h,L"JjHover")) {
            SetPropW(h,L"JjHover",(HANDLE)1);
            TRACKMOUSEEVENT tr{sizeof(tr),TME_LEAVE,h,0};TrackMouseEvent(&tr);InvalidateRect(h,nullptr,FALSE);
        }
    }
    if(m==WM_MOUSELEAVE){RemovePropW(h,L"JjHover");InvalidateRect(h,nullptr,FALSE);}
    if(kind==4){
        if((m==WM_LBUTTONDOWN||m==WM_LBUTTONDBLCLK)&&IsWindowEnabled(h)){
            LogTrace trace;
            SetFocus(h);
            auto geometry=trackbar_geometry(h);
            int x=GET_X_LPARAM(l),y=GET_Y_LPARAM(l);
            float dx=x-geometry.cx,dy=y-geometry.cy,radius=geometry.radius+1;
            bool onThumb=dx*dx+dy*dy<=radius*radius;
            SetCapture(h);
            if(GetCapture()!=h){
                logf(L"滑条鼠标请求被拒绝：控件=%d 无法取得鼠标捕获 error=%lu",GetDlgCtrlID(h),GetLastError());
                return 0;
            }
            SetPropW(h,L"JjSliderDragging",(HANDLE)1);
            SetPropW(h,L"JjSliderTrace",(HANDLE)(ULONG_PTR)trace.id());
            SetPropW(h,L"JjSliderOffset",(HANDLE)(INT_PTR)(onThumb?x-(int)lround(geometry.cx):0));
            logf(L"滑条鼠标请求到达：控件=%d 点击=(%d,%d) 命中滑块=%d 当前值=%ld",
                 GetDlgCtrlID(h),x,y,(int)onThumb,(long)SendMessageW(h,TBM_GETPOS,0,0));
            if(onThumb)trackbar_notify(h,TB_THUMBTRACK);
            else if(!trackbar_mouse_position(h,x))trackbar_notify(h,TB_THUMBTRACK);
            return 0;
        }
        if((m==WM_MOUSEMOVE||m==WM_LBUTTONUP)&&GetPropW(h,L"JjSliderDragging")){
            LogTrace trace((unsigned long long)(ULONG_PTR)GetPropW(h,L"JjSliderTrace"));
            trackbar_mouse_position(h,GET_X_LPARAM(l)-(int)(INT_PTR)GetPropW(h,L"JjSliderOffset"));
            if(m==WM_LBUTTONUP)trackbar_end_mouse(h,false);
            return 0;
        }
        if((m==WM_CAPTURECHANGED||m==WM_CANCELMODE)&&GetPropW(h,L"JjSliderDragging")){
            LogTrace trace((unsigned long long)(ULONG_PTR)GetPropW(h,L"JjSliderTrace"));
            trackbar_end_mouse(h,true);return 0;
        }
    }
    if(m==WM_SETFOCUS||m==WM_KILLFOCUS||m==WM_ENABLE||m==BM_SETCHECK||m==BM_SETSTATE||
       m==TBM_SETPOS||m==WM_LBUTTONDOWN||m==WM_LBUTTONUP||m==WM_KEYUP) {
        LRESULT r=DefSubclassProc(h,m,w,l);InvalidateRect(h,nullptr,FALSE);return r;
    }
    // EDIT / HOTKEY 必须让系统用 WM_CTLCOLOR 的白色画刷擦背景。
    // 之前统一返回 1 会让热键控件透出上一页残留的文字和绿色提示块。
    if(m==WM_ERASEBKGND)return kind==3?DefSubclassProc(h,m,w,l):1;
    if((m==WM_PAINT&&kind!=3)||m==WM_PRINTCLIENT) {
        auto* windowScale=(const double*)GetPropW(GetAncestor(h,GA_ROOT),L"JjScale");
        UiScaleScope scale(windowScale?*windowScale:g_scale);
        PAINTSTRUCT ps{};HDC dc=m==WM_PAINT?BeginPaint(h,&ps):(HDC)w;RECT rc;GetClientRect(h,&rc);
        if(rc.right>0&&rc.bottom>0){
            Bitmap bmp(rc.right,rc.bottom,PixelFormat32bppARGB);
            bmp.SetResolution((REAL)(96*g_scale),(REAL)(96*g_scale));
            Graphics g(&bmp);g.SetPageUnit(UnitPixel);
            g.SetSmoothingMode(SmoothingModeAntiAlias);g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
            g.Clear(GetPropW(h,L"JjCanvas")?canvas():Color::White);
            bool enabled=IsWindowEnabled(h),hover=GetPropW(h,L"JjHover")!=nullptr;
            bool focus=GetFocus()==h;
            RectF r(1,1,(REAL)rc.right-2,(REAL)rc.bottom-2);
            if(kind==1) { // ordinary button, primary, or sidebar item
                bool primary=GetPropW(h,L"JjPrimary")!=nullptr;
                bool nav=GetPropW(h,L"JjNav")!=nullptr, selected=GetPropW(h,L"JjSelected")!=nullptr;
                if(nav)g.Clear(canvas());
                Color fill=!enabled?Color(255,244,246,245):
                  primary?(hover?Color(255,5,172,85):green()):
                  selected?tint():hover?Color(255,241,245,242):nav?canvas():Color::White;
                Color border=primary||nav?Color(0,0,0,0):line();
                box(g,r,fill,8,focus?green():border);
                text(g,caption(h),r,13,!enabled?muted():primary?Color::White:selected?Color(255,0,143,71):ink(),primary||selected,StringAlignmentCenter);
            } else if(kind==2) { // checkbox
                bool check=SendMessageW(h,BM_GETCHECK,0,0)==BST_CHECKED;
                float d=SF(17),y=(rc.bottom-d)/2;
                box(g,RectF(SF(1),y,d,d),check?green():Color::White,4,focus?green():check?green():Color(255,202,212,205));
                if(check){Pen p(Color::White,SF(1.7f));p.SetStartCap(LineCapRound);p.SetEndCap(LineCapRound);
                    g.DrawLine(&p,SF(5),y+SF(9),SF(8),y+SF(12));g.DrawLine(&p,SF(8),y+SF(12),SF(14),y+SF(5));}
                text(g,caption(h),RectF(SF(29),0,(REAL)rc.right-SF(30),(REAL)rc.bottom),13,enabled?ink():muted());
            } else if(kind==3) {
                auto value=caption(h);wchar_t cls[64]{};GetClassNameW(h,cls,64);
                if(!_wcsicmp(cls,HOTKEY_CLASSW)){
                    WORD key=(WORD)SendMessageW(h,HKM_GETHOTKEY,0,0);BYTE flags=HIBYTE(key);value.clear();
                    if(flags&HOTKEYF_CONTROL)value+=L"Ctrl + ";
                    if(flags&HOTKEYF_ALT)value+=L"Alt + ";
                    if(flags&HOTKEYF_SHIFT)value+=L"Shift + ";
                    BYTE vk=LOBYTE(key);
                    if(vk>=VK_F1&&vk<=VK_F24)value+=L"F"+std::to_wstring(vk-VK_F1+1);
                    else if(vk)value+=(wchar_t)vk;
                }else if(GetWindowLongW(h,GWL_STYLE)&ES_PASSWORD)value.assign(value.size(),L'•');
                text(g,value,RectF(0,0,(REAL)rc.right,(REAL)rc.bottom),13);
            } else if(kind==5) {
                box(g,r,enabled?Color::White:canvas(),7,focus?green():line());
                text(g,caption(h),RectF(SF(12),0,rc.right-SF(44),(REAL)rc.bottom),13,ink());
                Pen arrow(muted(),SF(1.5f));float x=rc.right-SF(18),y=rc.bottom/2.0f;
                g.DrawLine(&arrow,x-SF(3),y-SF(1),x,y+SF(2));
                g.DrawLine(&arrow,x,y+SF(2),x+SF(3),y-SF(1));
            } else if(kind==6) {
                box(g,RectF(0,0,(REAL)rc.right,(REAL)rc.bottom),Color(255,232,239,234),3);
                int pos=(int)SendMessageW(h,PBM_GETPOS,0,0);PBRANGE range{};
                SendMessageW(h,PBM_GETRANGE,FALSE,(LPARAM)&range);
                float part=std::clamp((pos-range.iLow)/(float)std::max(1,range.iHigh-range.iLow),0.0f,1.0f);
                if(part>0)box(g,RectF(0,0,rc.right*part,(REAL)rc.bottom),green(),3);
            } else if(kind==4) { // Win32 trackbar, with custom track and thumb
                auto geometry=trackbar_geometry(h);
                RECT ch=geometry.channel;
                float x=(float)ch.left, width=(float)(ch.right-ch.left);
                float cy=geometry.cy,cx=geometry.cx;
                int gradient=(int)(INT_PTR)GetPropW(h,L"JjGradient");
                RectF track(x,cy-SF(2),width,SF(4));
                if(gradient==1) {
                    Color stops[]={Color(255,255,59,48),Color(255,255,220,0),Color(255,0,220,80),
                                   Color(255,0,210,230),Color(255,65,80,255),Color(255,235,40,220),Color(255,255,59,48)};
                    for(int i=0;i<6;++i){
                        RectF piece(x+i*width/6,cy-SF(3),width/6+1,SF(6));
                        LinearGradientBrush b(piece,stops[i],stops[i+1],LinearGradientModeHorizontal);g.FillRectangle(&b,piece);
                    }
                } else if(gradient==2) {
                    track.Y=cy-SF(4);track.Height=SF(8);checker(g,track);
                    DWORD v=(DWORD)(ULONG_PTR)GetPropW(h,L"JjColor");
                    Color a(v|0xff000000),b(v&0xffffff);
                    LinearGradientBrush grad(track,a,b,LinearGradientModeHorizontal);g.FillRectangle(&grad,track);
                } else {
                    SolidBrush track_bg(Color(255,225,233,227));
                    SolidBrush track_fg(green());
                    g.FillRectangle(&track_bg,track);
                    g.FillRectangle(&track_fg,RectF(x,cy-SF(2),std::max(SF(2),cx-x),SF(4)));
                }
                SolidBrush shadow(Color(30,0,45,20));g.FillEllipse(&shadow,cx-SF(8),cy-SF(7),SF(16),SF(16));
                SolidBrush white(Color::White);Pen edge(focus?green():Color(255,202,215,207),SF(1));
                g.FillEllipse(&white,cx-SF(7),cy-SF(7),SF(14),SF(14));g.DrawEllipse(&edge,cx-SF(7),cy-SF(7),SF(14),SF(14));
                if(focus){SolidBrush b(green());g.FillEllipse(&b,cx-SF(2),cy-SF(2),SF(4),SF(4));}
            }
            // The natural-size overload uses the bitmap's DPI metadata. On a
            // 175% desktop it shrinks our already-scaled backing store again.
            Graphics dest(dc);dest.SetPageUnit(UnitPixel);
            dest.DrawImage(&bmp,0,0,rc.right,rc.bottom);
        }
        if(m==WM_PAINT)EndPaint(h,&ps);return 0;
    }
    if(m==WM_NCDESTROY) {
        RemovePropW(h,L"JjHover");RemovePropW(h,L"JjPrimary");RemovePropW(h,L"JjNav");
        RemovePropW(h,L"JjSelected");RemovePropW(h,L"JjGradient");RemovePropW(h,L"JjColor");RemovePropW(h,L"JjCanvas");
        RemovePropW(h,L"JjSliderDragging");RemovePropW(h,L"JjSliderOffset");RemovePropW(h,L"JjSliderTrace");
        RemovePropW(h,L"JjSliderVisualX");RemovePropW(h,L"JjSliderVisualValue");
        RemoveWindowSubclass(h,control_proc,731);
    }
    return DefSubclassProc(h,m,w,l);
}
inline void style(HWND h,bool primary=false) {
    wchar_t cls[64]{};GetClassNameW(h,cls,64);int kind=0;
    if(!_wcsicmp(cls,L"BUTTON")){
        LONG type=GetWindowLongW(h,GWL_STYLE)&BS_TYPEMASK;
        kind=(type==BS_AUTOCHECKBOX||type==BS_CHECKBOX)?2:1;
        if(primary)SetPropW(h,L"JjPrimary",(HANDLE)1);
    }else if(!_wcsicmp(cls,TRACKBAR_CLASSW)){
        kind=4;auto s=GetWindowLongW(h,GWL_STYLE);SetWindowLongW(h,GWL_STYLE,s|TBS_NOTICKS);
    }else if(!_wcsicmp(cls,L"COMBOBOX")){
        kind=5;
    }else if(!_wcsicmp(cls,PROGRESS_CLASSW)){
        kind=6;
    }else if(!_wcsicmp(cls,L"EDIT")||!_wcsicmp(cls,HOTKEY_CLASSW)){kind=3;}
    if(kind)SetWindowSubclass(h,control_proc,731,kind);
}
inline LRESULT colors(UINT m,WPARAM w,LPARAM l) {
    HDC dc=(HDC)w;SetBkMode(dc,TRANSPARENT);
    SetTextColor(dc,GetPropW((HWND)l,L"JjMuted")?RGB(132,140,135):RGB(45,54,48));
    SetBkColor(dc,RGB(255,255,255));
    return (LRESULT)white_brush();
}
inline bool draw_item(DRAWITEMSTRUCT* item) {
    if(!item||item->CtlType!=ODT_COMBOBOX)return false;
    Graphics g(item->hDC);g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    bool selected=item->itemState&ODS_SELECTED;
    SolidBrush b(selected?tint():Color::White);
    RectF r((REAL)item->rcItem.left,(REAL)item->rcItem.top,
            (REAL)(item->rcItem.right-item->rcItem.left),(REAL)(item->rcItem.bottom-item->rcItem.top));
    g.FillRectangle(&b,r);std::wstring value;
    if(item->itemID!=(UINT)-1) {
        LRESULT n=SendMessageW(item->hwndItem,CB_GETLBTEXTLEN,item->itemID,0);
        if(n>=0&&n<4096){value.resize((size_t)n+1);SendMessageW(item->hwndItem,CB_GETLBTEXT,item->itemID,(LPARAM)value.data());value.resize((size_t)n);}
    }
    r.X+=SF(10);r.Width-=SF(20);
    text(g,value,r,13,selected?Color(255,0,145,70):ink());return true;
}
} // namespace ui
