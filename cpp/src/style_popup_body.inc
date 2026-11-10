#include "ui_theme.h"
namespace {
const wchar_t* CLASS=L"weijieStylePopup";
struct Popup {
    Overlay* o; bool color; int tool;
    HWND h{},slider{},alpha{},hex{}; HFONT font{};
    double hue=0,sat=1,val=1;
    bool dragging=false,syncing=false,changed=false;
    // Native trackbars post a WM_HSCROLL for every mouse move. Invalidating
    // the full-screen overlay on each one starves the slider paint, so the
    // thumb visibly lags behind the cursor. Coalesce overlay refreshes.
    bool sliderDragging=false;
    UINT_PTR overlayTimer=0;
    std::vector<DWORD> swatches;
};
const UINT_PTR TIMER_OVERLAY=91;
const wchar_t* tool_name(int tool) {
    const wchar_t* names[]={L"矩形",L"椭圆",L"箭头",L"画笔",L"马赛克",L"文字",L"序号"};
    return names[tool-T_RECT];
}
int low(Popup*p){return p->tool==T_TEXT?10:p->tool==T_NUMBER?12:p->tool==T_MOSAIC?4:1;}
int high(Popup*p){return p->tool==T_TEXT||p->tool==T_NUMBER?96:p->tool==T_MOSAIC?120:30;}
RectF rect(float x,float y,float w,float h){return RectF(SF(x),SF(y),SF(w),SF(h));}
HWND ctl(Popup*p,const wchar_t*cls,const wchar_t*t,DWORD style,int x,int y,int w,int h,int id,bool primary=false){
    if(!_wcsicmp(cls,TRACKBAR_CLASSW))style|=TBS_NOTICKS;
    HWND c=CreateWindowExW(0,cls,t,WS_CHILD|WS_VISIBLE|style,S(x),S(y),S(w),S(h),
        p->h,(HMENU)(INT_PTR)id,GetModuleHandleW(nullptr),nullptr);
    SendMessageW(c,WM_SETFONT,(WPARAM)p->font,TRUE);ui::style(c,primary);return c;
}
void overlay_refresh(Popup*p,bool immediate){
    if(!p->o||!p->o->hwnd)return;
    if(immediate){
        if(p->overlayTimer){KillTimer(p->h,TIMER_OVERLAY);p->overlayTimer=0;}
        InvalidateRect(p->o->hwnd,nullptr,FALSE);
        return;
    }
    if(!p->overlayTimer)p->overlayTimer=SetTimer(p->h,TIMER_OVERLAY,33,nullptr);
}
void update_label(Popup*p){
    if(p->color) {
        auto c=p->o->ink;wchar_t hex[10];
        swprintf_s(hex,L"#%02X%02X%02X",c.GetR(),c.GetG(),c.GetB());
        // Do not reset the caret while the user is editing a partial HEX value.
        if(GetFocus()!=p->hex){
            p->syncing=true;SetWindowTextW(p->hex,hex);p->syncing=false;
        }
        SetPropW(p->alpha,L"JjColor",(HANDLE)(ULONG_PTR)c.GetValue());
        InvalidateRect(p->alpha,nullptr,FALSE);
    }
    InvalidateRect(p->h,nullptr,FALSE);
    // The colour picker does not change anything already drawn on the
    // overlay, so only refresh it when the popup closes.
    if(!p->color) overlay_refresh(p,!p->sliderDragging);
}
void apply_hsv(Popup*p){
    int transparency=(int)SendMessageW(p->alpha,TBM_GETPOS,0,0);
    p->o->ink=hsv_color(p->hue,p->sat,p->val,(BYTE)lround((100-transparency)*255.0/100));
    p->changed=true;update_label(p);
}
void select_color(Popup*p,Color c){
    p->o->ink=c;color_hsv(c,p->hue,p->sat,p->val);
    SendMessageW(p->slider,TBM_SETPOS,TRUE,(int)p->hue);
    SendMessageW(p->alpha,TBM_SETPOS,TRUE,100-(int)lround(c.GetA()*100.0/255));
    p->changed=true;update_label(p);
}
void read_hex(Popup*p){
    wchar_t b[16]{};GetWindowTextW(p->hex,b,16);std::wstring t=b;
    if(t.size()==7&&t[0]==L'#')t.erase(0,1);
    if(t.size()==6&&std::all_of(t.begin(),t.end(),[](wchar_t c){return iswxdigit(c)!=0;})){
        auto rgb=(DWORD)std::stoul(t,nullptr,16);
        select_color(p,Color(((DWORD)p->o->ink.GetA()<<24)|rgb));
    }
}
LRESULT CALLBACK child_keys(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR ref){
    auto*p=(Popup*)ref;
    if(m==WM_KEYDOWN&&(w==VK_ESCAPE||w==VK_RETURN)){
        if(w==VK_RETURN&&p->color&&h==p->hex){read_hex(p);SetFocus(p->h);}
        else PostMessageW(p->h,WM_CLOSE,0,0);
        return 0;
    }
    return DefSubclassProc(h,m,w,l);
}
void preview(Graphics&g,Popup*p){
    auto r=rect(20,150,296,54);ui::box(g,r,ui::canvas(),8);
    int n=p->o->toolSizes[p->tool-T_RECT];
    Color color=p->o->ink;Pen pen(color,SF((float)std::min(n,16)));pen.SetStartCap(LineCapRound);pen.SetEndCap(LineCapRound);
    SolidBrush brush(color);
    if(p->tool==T_TEXT) {
        Font f(L"Microsoft YaHei UI",SF((float)std::min(n,32)),FontStyleRegular,UnitPixel);
        g.DrawString(L"文字预览 Aa",-1,&f,rect(34,155,260,44),nullptr,&brush);
    }else if(p->tool==T_NUMBER){
        float diameter=SF((float)std::clamp(n,18,40));
        g.FillEllipse(&brush,SF(34),SF(177)-diameter/2,diameter,diameter);
        ui::text(g,L"1",RectF(SF(34),SF(177)-diameter/2,diameter,diameter),13,Color::White,true,StringAlignmentCenter);
        ui::text(g,L"序号标记",rect(96,158,190,36),12,ui::muted());
    }else if(p->tool==T_MOSAIC){
        int size=std::clamp(n/5,4,12);
        for(int y=0;y<3;++y)for(int x=0;x<7;++x){
            SolidBrush b(Color(255,95+x*14,155+y*15,123+x*9));
            g.FillRectangle(&b,S(34+x*size),S(163+y*size),S(size),S(size));
        }
        ui::text(g,L"马赛克笔刷",rect(160,158,130,36),12,ui::muted());
    }else if(p->tool==T_RECT){g.DrawRectangle(&pen,SF(44),SF(164),SF(64),SF(26));}
    else if(p->tool==T_ELLIPSE){g.DrawEllipse(&pen,SF(44),SF(164),SF(64),SF(26));}
    else if(p->tool==T_ARROW){
        pen.SetEndCap(LineCapArrowAnchor);g.DrawLine(&pen,SF(40),SF(179),SF(140),SF(179));
    }else{
        GraphicsPath path;path.AddBezier(SF(34),SF(183),SF(74),SF(145),SF(103),SF(210),SF(150),SF(169));g.DrawPath(&pen,&path);
    }
    if(p->tool<=T_PEN)ui::text(g,L"实时预览",rect(188,160,106,32),12,ui::muted());
}
void paint(Popup*p,HDC dc){
    RECT rc;GetClientRect(p->h,&rc);
    Bitmap back(rc.right,rc.bottom,PixelFormat32bppARGB);
    back.SetResolution((REAL)(96*g_scale),(REAL)(96*g_scale));
    Graphics g(&back);g.SetPageUnit(UnitPixel);
    g.Clear(Color::White);g.SetSmoothingMode(SmoothingModeAntiAlias);g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    ui::box(g,RectF(.5f,.5f,(REAL)rc.right-1,(REAL)rc.bottom-1),Color::White,12,ui::line());
    if(p->color){
        ui::text(g,L"颜色与透明度",rect(24,17,264,30),17,ui::ink(),true);
        ui::text(g,L"为你的标注选择合适的颜色",rect(24,47,312,22),12,ui::muted());
        ui::separator(g,24,78,312);
        ui::text(g,L"最近使用",rect(24,88,250,20),12,ui::muted());
        for(size_t i=0;i<p->swatches.size();++i){
            RectF r=rect(24+(float)i*26,116,21,21);
            auto st=g.Save();GraphicsPath clip;ui::path(clip,r,SF(6));g.SetClip(&clip);
            ui::checker(g,r);SolidBrush c{Color(p->swatches[i])};g.FillRectangle(&c,r);g.Restore(st);
            GraphicsPath edge;ui::path(edge,r,SF(6));Pen pen(p->swatches[i]==p->o->ink.GetValue()?ui::green():ui::line(),SF(1));
            g.DrawPath(&pen,&edge);
        }
        int pw=S(312),ph=S(162);
        Bitmap palette(pw,ph,PixelFormat32bppARGB);BitmapData bits{};Rect all(0,0,pw,ph);
        if(palette.LockBits(&all,ImageLockModeWrite,PixelFormat32bppARGB,&bits)==Ok){
            for(int y=0;y<ph;++y){DWORD*row=(DWORD*)((BYTE*)bits.Scan0+y*bits.Stride);
                for(int x=0;x<pw;++x)row[x]=hsv_color(p->hue,(double)x/(pw-1),1.0-(double)y/(ph-1)).GetValue();}
            palette.UnlockBits(&bits);
        }
        auto st=g.Save();GraphicsPath clip;ui::path(clip,rect(24,154,312,162),SF(8));g.SetClip(&clip);
        g.DrawImage(&palette,S(24),S(154),pw,ph);g.Restore(st);
        float x=SF(24)+(float)p->sat*(pw-1),y=SF(154)+(float)(1-p->val)*(ph-1);
        Pen shadow(Color(150,0,0,0),SF(3)),white(Color::White,SF(2));
        g.DrawEllipse(&shadow,x-SF(5),y-SF(5),SF(10),SF(10));g.DrawEllipse(&white,x-SF(5),y-SF(5),SF(10),SF(10));
        ui::text(g,L"色相",rect(24,324,250,18),12,ui::muted());
        ui::text(g,L"透明度",rect(24,378,240,18),12,ui::muted());
        ui::text(g,std::to_wstring(100-(int)lround(p->o->ink.GetA()*100.0/255))+L"%",
                 rect(276,378,60,18),12,ui::ink(),false,StringAlignmentFar);
        ui::box(g,rect(24,440,172,36),Color::White,7,GetFocus()==p->hex?ui::green():ui::line());
        ui::text(g,L"HEX",rect(34,444,40,28),11,ui::muted());
        RectF pr=rect(216,440,120,36);auto save=g.Save();GraphicsPath shape;ui::path(shape,pr,SF(7));g.SetClip(&shape);
        ui::checker(g,pr);SolidBrush c(p->o->ink);g.FillRectangle(&c,pr);g.Restore(save);
    }else{
        ui::text(g,tool_name(p->tool),rect(20,16,180,28),16,ui::ink(),true);
        ui::box(g,rect(242,18,74,28),ui::tint(),7);
        ui::text(g,std::to_wstring(p->o->toolSizes[p->tool-T_RECT])+L" px",rect(242,18,74,28),13,Color(255,0,150,74),true,StringAlignmentCenter);
        const wchar_t* label=p->tool==T_TEXT?L"文字大小":p->tool==T_NUMBER?L"标记直径":p->tool==T_MOSAIC?L"笔刷大小":L"笔画粗细";
        ui::text(g,label,rect(20,51,270,22),12,ui::muted());
        ui::text(g,std::to_wstring(low(p)),rect(22,115,90,20),11,ui::muted());
        ui::text(g,std::to_wstring(high(p)),rect(240,115,74,20),11,ui::muted(),false,StringAlignmentFar);
        preview(g,p);
        ui::text(g,L"即时生效 · 点击外部收起",rect(20,210,296,24),11,ui::muted());
    }
    Graphics dest(dc);dest.SetPageUnit(UnitPixel);dest.DrawImage(&back,0,0,rc.right,rc.bottom);
}
void palette_point(Popup*p,int x,int y){
    p->sat=std::clamp((x-S(24))/(double)(S(312)-1),0.0,1.0);
    p->val=1-std::clamp((y-S(154))/(double)(S(162)-1),0.0,1.0);apply_hsv(p);
}
LRESULT CALLBACK proc(HWND h,UINT m,WPARAM w,LPARAM l){
    auto*p=(Popup*)GetWindowLongPtrW(h,GWLP_USERDATA);
    if(m==WM_CREATE){
        p=(Popup*)((CREATESTRUCTW*)l)->lpCreateParams;p->h=h;SetWindowLongPtrW(h,GWLP_USERDATA,(LONG_PTR)p);
        p->font=CreateFontW(-S(13),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
        if(p->color)ctl(p,L"BUTTON",L"×",WS_TABSTOP,308,12,28,28,4);
        if(p->color){
            p->swatches=p->o->recentColors;
            for(auto c:PALETTE)if(p->swatches.size()<12&&std::find(p->swatches.begin(),p->swatches.end(),c.GetValue())==p->swatches.end())p->swatches.push_back(c.GetValue());
            color_hsv(p->o->ink,p->hue,p->sat,p->val);
            p->slider=ctl(p,TRACKBAR_CLASSW,L"",WS_TABSTOP|TBS_HORZ,18,342,324,28,1);
            SetPropW(p->slider,L"JjGradient",(HANDLE)1);
            SendMessageW(p->slider,TBM_SETRANGE,TRUE,MAKELPARAM(0,359));SendMessageW(p->slider,TBM_SETPOS,TRUE,(int)p->hue);
            p->alpha=ctl(p,TRACKBAR_CLASSW,L"",WS_TABSTOP|TBS_HORZ,18,396,324,28,2);
            SetPropW(p->alpha,L"JjGradient",(HANDLE)2);
            SendMessageW(p->alpha,TBM_SETRANGE,TRUE,MAKELPARAM(0,100));SendMessageW(p->alpha,TBM_SETPOS,TRUE,100-(int)lround(p->o->ink.GetA()*100.0/255));
            p->hex=ctl(p,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,80,448,106,24,3);
            SendMessageW(p->hex,EM_SETLIMITTEXT,7,0);
            ctl(p,L"BUTTON",L"完成",WS_TABSTOP,24,494,312,34,4,true);
        }else{
            p->slider=ctl(p,TRACKBAR_CLASSW,L"",WS_TABSTOP|TBS_HORZ,16,80,304,32,1);
            SendMessageW(p->slider,TBM_SETRANGE,TRUE,MAKELPARAM(low(p),high(p)));
            SendMessageW(p->slider,TBM_SETPOS,TRUE,p->o->toolSizes[p->tool-T_RECT]);
        }
        for(HWND child=GetWindow(h,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT))SetWindowSubclass(child,child_keys,1,(DWORD_PTR)p);
        update_label(p);return 0;
    }
    if(!p)return DefWindowProcW(h,m,w,l);
    switch(m){
    case WM_CTLCOLORSTATIC:case WM_CTLCOLOREDIT:case WM_CTLCOLORBTN:return ui::colors(m,w,l);
    case WM_HSCROLL: {
        UINT code=LOWORD(w);
        if(code==TB_THUMBTRACK)p->sliderDragging=true;
        if(p->color){p->hue=(double)SendMessageW(p->slider,TBM_GETPOS,0,0);apply_hsv(p);}
        else{p->o->toolSizes[p->tool-T_RECT]=(int)SendMessageW(p->slider,TBM_GETPOS,0,0);update_label(p);}
        InvalidateRect(p->slider,nullptr,FALSE);
        // Repaint the thumb synchronously; the full overlay refresh below is
        // coalesced and must not make the slider feel like it is animating.
        UpdateWindow(p->slider);
        if(p->color)InvalidateRect(p->alpha,nullptr,FALSE);
        if(code==TB_ENDTRACK||code==TB_THUMBPOSITION){
            p->sliderDragging=false;
            if(!p->color)overlay_refresh(p,true);
        }
        return 0;
    }
    case WM_TIMER:
        if(w==TIMER_OVERLAY&&p->overlayTimer){
            KillTimer(h,TIMER_OVERLAY);p->overlayTimer=0;
            if(p->o&&p->o->hwnd)InvalidateRect(p->o->hwnd,nullptr,FALSE);
        }
        return 0;
    case WM_COMMAND:
        if(LOWORD(w)==4){if(p->color)read_hex(p);DestroyWindow(h);}
        else if(LOWORD(w)==3&&HIWORD(w)==EN_KILLFOCUS&&!p->syncing)read_hex(p);
        return 0;
    case WM_LBUTTONDOWN:{
        int x=GET_X_LPARAM(l),y=GET_Y_LPARAM(l);
        if(p->color)SetFocus(h);
        if(p->color&&y>=S(116)&&y<S(137)&&x>=S(24)){
            int i=(x-S(24))/S(26);if(i<(int)p->swatches.size()&&(x-S(24))%S(26)<S(21))select_color(p,Color(p->swatches[i]));
        }
        if(p->color&&x>=S(24)&&x<S(336)&&y>=S(154)&&y<S(316)){p->dragging=true;SetCapture(h);palette_point(p,x,y);}return 0;
    }
    case WM_MOUSEMOVE:if(p->dragging)palette_point(p,GET_X_LPARAM(l),GET_Y_LPARAM(l));return 0;
    case WM_LBUTTONUP:if(p->dragging){p->dragging=false;ReleaseCapture();}return 0;
    case WM_CAPTURECHANGED:p->dragging=false;return 0;
    case WM_ACTIVATE:if(LOWORD(w)==WA_INACTIVE)PostMessageW(h,WM_CLOSE,0,0);return 0;
    case WM_KEYDOWN:if(w==VK_ESCAPE||w==VK_RETURN)DestroyWindow(h);return 0;
    case WM_ERASEBKGND:return 1;
    case WM_PRINTCLIENT:paint(p,(HDC)w);return 0;
    case WM_PAINT:{PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);paint(p,dc);EndPaint(h,&ps);return 0;}
    case WM_CLOSE:DestroyWindow(h);return 0;
    case WM_DESTROY:
        if(p->overlayTimer){KillTimer(h,TIMER_OVERLAY);p->overlayTimer=0;}
        if(p->color&&p->changed){
            auto&v=p->o->recentColors;DWORD c=p->o->ink.GetValue();v.erase(std::remove(v.begin(),v.end(),c),v.end());v.insert(v.begin(),c);if(v.size()>12)v.resize(12);
            auto cfg=load_settings();cfg.recent_colors=encode_recent_colors(v);save_settings(cfg);
        }
        if(p->o->stylePopup==h)p->o->stylePopup=nullptr;
        if(p->font)DeleteObject(p->font);SetWindowLongPtrW(h,GWLP_USERDATA,0);InvalidateRect(p->o->hwnd,nullptr,FALSE);delete p;return 0;
    }
    return DefWindowProcW(h,m,w,l);
}
void open(Overlay*o,bool color,int tool){
    close_style_popup(o);
    WNDCLASSW wc{};wc.style=CS_DROPSHADOW;wc.lpfnWndProc=proc;wc.hInstance=GetModuleHandleW(nullptr);
    wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.lpszClassName=CLASS;RegisterClassW(&wc);
    auto*p=new Popup{o,color,tool};int width=S(color?360:336),height=S(color?546:244);
    int x=o->shot.vx+(int)o->tbX,y=o->shot.vy+(int)o->tbY-height-S(10);
    for(auto&item:o->items)if(item.id==(color?T_COLOR:tool)){x+=(int)item.rect.X;break;}
    POINT anchor{x,y};MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromPoint(anchor,MONITOR_DEFAULTTONEAREST),&mi);
    x=std::clamp(x,(int)mi.rcWork.left,std::max((int)mi.rcWork.left,(int)mi.rcWork.right-width));
    y=std::clamp(y,(int)mi.rcWork.top,std::max((int)mi.rcWork.top,(int)mi.rcWork.bottom-height));
    HWND h=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW,CLASS,color?L"颜色与透明度":tool_name(tool),
        WS_POPUP|WS_CLIPCHILDREN,x,y,width,height,o->hwnd,nullptr,wc.hInstance,p);
    if(!h){delete p;return;}o->stylePopup=h;ui::round_window(h);
    ShowWindow(h,SW_SHOW);SetForegroundWindow(h);SetFocus(p->slider);
}
}
void show_tool_slider(Overlay*o,int tool){if(tool>=T_RECT&&tool<=T_NUMBER)open(o,false,tool);}
void show_color_picker(Overlay*o){open(o,true,0);}
void close_style_popup(Overlay*o){if(o->stylePopup&&IsWindow(o->stylePopup))DestroyWindow(o->stylePopup);o->stylePopup=nullptr;}
