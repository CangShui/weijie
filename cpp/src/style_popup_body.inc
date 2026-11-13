#include "ui_theme.h"
namespace {
const wchar_t* CLASS=L"weijieStylePopup";
struct Popup {
    Overlay* o;
    HWND h{},slider{},alpha{},hex{}; HFONT font{};
    double hue=0,sat=1,val=1;
    bool dragging=false,syncing=false,changed=false;
    bool sliderDragging=false;
    std::vector<DWORD> swatches;
};
RectF rect(float x,float y,float w,float h){return RectF(SF(x),SF(y),SF(w),SF(h));}
HWND ctl(Popup*p,const wchar_t*cls,const wchar_t*t,DWORD style,int x,int y,int w,int h,int id,bool primary=false){
    if(!_wcsicmp(cls,TRACKBAR_CLASSW))style|=TBS_NOTICKS;
    HWND c=CreateWindowExW(0,cls,t,WS_CHILD|WS_VISIBLE|style,S(x),S(y),S(w),S(h),
        p->h,(HMENU)(INT_PTR)id,GetModuleHandleW(nullptr),nullptr);
    SendMessageW(c,WM_SETFONT,(WPARAM)p->font,TRUE);ui::style(c,primary);return c;
}
void update_label(Popup*p){
    overlay_update_brush_cursor(p->o);
    {
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
    if((m==WM_LBUTTONDOWN||m==WM_LBUTTONDBLCLK||m==WM_LBUTTONUP)&&(h==p->slider||h==p->alpha)){
        LogTrace trace(p->o->traceId);
        auto geometry=ui::trackbar_geometry(h);
        logf(L"样式滑条鼠标%s：控件=%d 工具=%d 坐标=(%d,%d) 原生滑块=(%ld,%ld)-(%ld,%ld) 绘制中心=(%.1f,%.1f) 当前值=%ld",
             m==WM_LBUTTONUP?L"抬起":L"按下",GetDlgCtrlID(h),T_COLOR,GET_X_LPARAM(l),GET_Y_LPARAM(l),
             geometry.thumb.left,geometry.thumb.top,geometry.thumb.right,geometry.thumb.bottom,
             geometry.cx,geometry.cy,(long)SendMessageW(h,TBM_GETPOS,0,0));
        return DefSubclassProc(h,m,w,l);
    }
    if(m==WM_KEYDOWN&&(w==VK_ESCAPE||w==VK_RETURN)){
        if(w==VK_RETURN&&h==p->hex){read_hex(p);SetFocus(p->h);}
        else PostMessageW(p->h,WM_CLOSE,0,0);
        return 0;
    }
    return DefSubclassProc(h,m,w,l);
}
void paint(Popup*p,HDC dc){
    RECT rc;GetClientRect(p->h,&rc);
    Bitmap back(rc.right,rc.bottom,PixelFormat32bppARGB);
    back.SetResolution((REAL)(96*g_scale),(REAL)(96*g_scale));
    Graphics g(&back);g.SetPageUnit(UnitPixel);
    g.Clear(Color::White);g.SetSmoothingMode(SmoothingModeAntiAlias);g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    ui::box(g,RectF(.5f,.5f,(REAL)rc.right-1,(REAL)rc.bottom-1),Color::White,12,ui::line());
    {
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
        ctl(p,L"BUTTON",L"×",WS_TABSTOP,308,12,28,28,4);
        {
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
        }
        for(HWND child=GetWindow(h,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT))SetWindowSubclass(child,child_keys,1,(DWORD_PTR)p);
        update_label(p);return 0;
    }
    if(!p)return DefWindowProcW(h,m,w,l);
    LogTrace trace(p->o->traceId);
    switch(m){
    case WM_CTLCOLORSTATIC:case WM_CTLCOLOREDIT:case WM_CTLCOLORBTN:return ui::colors(m,w,l);
    case WM_HSCROLL: {
        UINT code=LOWORD(w);
        if(code==TB_THUMBTRACK&&!p->sliderDragging){
            p->sliderDragging=true;
            logf(L"颜色面板滑条拖动开始：控件=%d 工具=%d，仅更新颜色和面板，不改变已绘制标注",
                 GetDlgCtrlID((HWND)l),T_COLOR);
        }
        p->hue=(double)SendMessageW(p->slider,TBM_GETPOS,0,0);apply_hsv(p);
        InvalidateRect(p->slider,nullptr,FALSE);
        UpdateWindow(p->slider);
        InvalidateRect(p->alpha,nullptr,FALSE);
        if(code==TB_ENDTRACK||code==TB_THUMBPOSITION){
            p->sliderDragging=false;
        }
        if(code==TB_ENDTRACK)
            logf(L"滑条交互结束：控件=%d 工具=%d 最终值=%ld，鼠标跟随已结束",
                 GetDlgCtrlID((HWND)l),T_COLOR,(long)SendMessageW((HWND)l,TBM_GETPOS,0,0));
        return 0;
    }
    case WM_COMMAND:
        if(LOWORD(w)==4){read_hex(p);DestroyWindow(h);}
        else if(LOWORD(w)==3&&HIWORD(w)==EN_KILLFOCUS&&!p->syncing)read_hex(p);
        return 0;
    case WM_LBUTTONDOWN:{
        int x=GET_X_LPARAM(l),y=GET_Y_LPARAM(l);
        SetFocus(h);
        if(y>=S(116)&&y<S(137)&&x>=S(24)){
            int i=(x-S(24))/S(26);if(i<(int)p->swatches.size()&&(x-S(24))%S(26)<S(21))select_color(p,Color(p->swatches[i]));
        }
        if(x>=S(24)&&x<S(336)&&y>=S(154)&&y<S(316)){p->dragging=true;SetCapture(h);palette_point(p,x,y);}return 0;
    }
    case WM_MOUSEMOVE:if(p->dragging)palette_point(p,GET_X_LPARAM(l),GET_Y_LPARAM(l));return 0;
    case WM_LBUTTONUP:if(p->dragging){p->dragging=false;ReleaseCapture();}return 0;
    case WM_CAPTURECHANGED:p->dragging=false;return 0;
    case WM_ACTIVATE:
        if(LOWORD(w)==WA_INACTIVE){
            logf(L"样式面板失去激活：目标窗口=%p，关闭面板",(HWND)l);
            PostMessageW(h,WM_CLOSE,0,0);
        }
        return 0;
    case WM_KEYDOWN:if(w==VK_ESCAPE||w==VK_RETURN)DestroyWindow(h);return 0;
    case WM_ERASEBKGND:return 1;
    case WM_PRINTCLIENT:paint(p,(HDC)w);return 0;
    case WM_PAINT:{PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);paint(p,dc);EndPaint(h,&ps);return 0;}
    case WM_CLOSE:DestroyWindow(h);return 0;
    case WM_DESTROY:
        for(HWND child=GetWindow(h,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT))
            RemoveWindowSubclass(child,child_keys,1);
        if(p->changed){
            auto&v=p->o->recentColors;DWORD c=p->o->ink.GetValue();v.erase(std::remove(v.begin(),v.end(),c),v.end());v.insert(v.begin(),c);if(v.size()>12)v.resize(12);
            auto cfg=load_settings();cfg.recent_colors=encode_recent_colors(v);save_settings(cfg);
        }
        if(p->o->stylePopup==h)p->o->stylePopup=nullptr;
        if(p->font)DeleteObject(p->font);SetWindowLongPtrW(h,GWLP_USERDATA,0);
        overlay_update_brush_cursor(p->o);InvalidateRect(p->o->hwnd,nullptr,FALSE);delete p;return 0;
    }
    return DefWindowProcW(h,m,w,l);
}
void open(Overlay*o){
    LogTrace trace(o->traceId);
    logf(L"颜色面板打开请求：DPI比例=%.2f",g_scale);
    close_style_popup(o);
    WNDCLASSW wc{};wc.style=CS_DROPSHADOW;wc.lpfnWndProc=proc;wc.hInstance=GetModuleHandleW(nullptr);
    wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.lpszClassName=CLASS;RegisterClassW(&wc);
    auto*p=new Popup{o};int width=S(360),height=S(546);
    int x=o->shot.vx+(int)o->tbX,y=o->shot.vy+(int)o->tbY-height-S(10);
    for(auto&item:o->items)if(item.id==T_COLOR){x+=(int)item.rect.X;break;}
    POINT anchor{x,y};MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromPoint(anchor,MONITOR_DEFAULTTONEAREST),&mi);
    x=std::clamp(x,(int)mi.rcWork.left,std::max((int)mi.rcWork.left,(int)mi.rcWork.right-width));
    y=std::clamp(y,(int)mi.rcWork.top,std::max((int)mi.rcWork.top,(int)mi.rcWork.bottom-height));
    HWND h=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW,CLASS,L"颜色与透明度",
        WS_POPUP|WS_CLIPCHILDREN,x,y,width,height,o->hwnd,nullptr,wc.hInstance,p);
    if(!h){logf(L"样式面板打开失败：error=%lu",GetLastError());delete p;return;}o->stylePopup=h;ui::round_window(h);
    ShowWindow(h,SW_SHOW);SetForegroundWindow(h);SetFocus(p->slider);
    logf(L"样式面板已打开：窗口=%p 位置=(%d,%d) 尺寸=%dx%d",h,x,y,width,height);
}
}
void show_color_picker(Overlay*o){open(o);}
void close_style_popup(Overlay*o){if(o->stylePopup&&IsWindow(o->stylePopup))DestroyWindow(o->stylePopup);o->stylePopup=nullptr;}
