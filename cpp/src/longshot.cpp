#include "longshot.h"

#include <vector>

struct LongFrame {
    int w = 0, h = 0;
    std::vector<BYTE> px;
};

static bool grab_region(int sx, int sy, int w, int h, LongFrame& f) {
    if (w < 2 || h < 2) return false;
    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) {
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return false;
    }
    HGDIOBJ old = SelectObject(mem, bmp);
    BOOL ok = BitBlt(mem, 0, 0, w, h, screen, sx, sy, SRCCOPY | CAPTUREBLT);
    SelectObject(mem, old);
    if (ok) {
        f.w = w;
        f.h = h;
        f.px.resize((size_t)w * h * 4);
        memcpy(f.px.data(), bits, f.px.size());
        for (size_t i = 3; i < f.px.size(); i += 4) f.px[i] = 255;
    }
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok != FALSE;
}

// 找上一帧底部与下一帧顶部的最大重合高度。返回 0 表示没找到可信重合。
int longshot_overlap(const std::vector<BYTE>& prev,int ph,const std::vector<BYTE>& cur,int ch,int w) {
    if(w<16||ch<48||ph<ch||prev.size()<(size_t)w*ph*4||cur.size()<(size_t)w*ch*4)return 0;
    // Compare several bands over most of the width. A narrow uniform center strip is not evidence.
    int margin=std::max(4,w/12),step=std::max(1,(w-2*margin)/72);
    double best=1e20;int bestShift=-1;std::vector<double> scores(ch,1e20);
    for(int shift=0;shift<ch-48;++shift){
        int overlap=ch-shift;double diff=0,mean=0,squares=0;int n=0;
        for(int y=0;y<overlap;y+=3)for(int x=margin;x<w-margin;x+=step){
            size_t ai=((size_t)(ph-ch+shift+y)*w+x)*4,bi=((size_t)y*w+x)*4;
            for(int c=0;c<3;++c){double a=prev[ai+c],b=cur[bi+c];diff+=abs(a-b);mean+=b;squares+=b*b;++n;}
        }
        if(!n||squares/n-(mean/n)*(mean/n)<25)continue; // Reject blank / low-information regions.
        double error=diff/n;scores[shift]=error;
        if(error<best){best=error;bestShift=shift;}
    }
    if(bestShift<0||best>8.0)return 0;
    // Repeated rows have multiple nearly equal offsets; do not silently drop content.
    for(int i=0;i<ch-48;++i)if(abs(i-bestShift)>4&&scores[i]<best+1.0)return 0;
    return ch-bestShift;
}

static Gdiplus::Bitmap* make_bitmap(const std::vector<BYTE>& px, int w, int h) {
    auto* bmp = new Gdiplus::Bitmap(w, h, PixelFormat32bppRGB);
    Gdiplus::Rect rc(0, 0, w, h);
    Gdiplus::BitmapData bd;
    if (bmp->LockBits(&rc, Gdiplus::ImageLockModeWrite, PixelFormat32bppRGB, &bd) != Gdiplus::Ok) {
        delete bmp;
        return nullptr;
    }
    for (int y = 0; y < h; ++y) {
        memcpy((BYTE*)bd.Scan0 + (size_t)y * bd.Stride,
               &px[(size_t)y * w * 4], (size_t)w * 4);
    }
    bmp->UnlockBits(&bd);
    return bmp;
}

bool longshot_capture(const Shot& shot, const RECT& sel,
                      Gdiplus::Bitmap*& out, std::wstring& err) {
    out = nullptr;
    err.clear();
    int x = shot.vx + sel.left;
    int y = shot.vy + sel.top;
    int w = sel.right - sel.left;
    int h = sel.bottom - sel.top;
    if (w < 16 || h < 48 || w > 10000 || (unsigned long long)w*h*4 > 200ull*1024*1024) { err = L"选区太小"; return false; }

    POINT center = { x + w / 2, y + h / 2 };
    HWND target = WindowFromPoint(center);
    HWND root = target ? GetAncestor(target,GA_ROOT) : nullptr;
    if(!root){err=L"无法定位滚动窗口";return false;}
    DWORD targetPid=0;GetWindowThreadProcessId(root,&targetPid);
    if(targetPid==GetCurrentProcessId()){err=L"请框选其他应用的可滚动内容";return false;}
    SetForegroundWindow(root);
    POINT oldCursor{};GetCursorPos(&oldCursor);
    struct Restore { POINT p; ~Restore(){SetCursorPos(p.x,p.y);} } restore{oldCursor};
    SetCursorPos(center.x, center.y);
    Sleep(120);

    LongFrame first;
    if (!grab_region(x, y, w, h, first)) {
        err = L"抓取滚动截图首帧失败";
        return false;
    }

    std::vector<BYTE> all = first.px;
    int totalH = first.h;
    const int MAX_H = std::min(20000, (int)(200ull*1024*1024/(w*4)));
    GetAsyncKeyState(VK_ESCAPE);
    for (int i = 0; i < 24 && totalH < MAX_H; ++i) {
        if((GetAsyncKeyState(VK_ESCAPE)&0x8001)||GetForegroundWindow()!=root){err=L"已取消滚动截图（Esc 或目标窗口切换）";return false;}
        mouse_event(MOUSEEVENTF_WHEEL, 0, 0, (DWORD)(-WHEEL_DELTA), 0);
        for(int tick=0;tick<8;++tick){Sleep(40);if(GetAsyncKeyState(VK_ESCAPE)&0x8001){err=L"已取消滚动截图";return false;}}
        LongFrame cur;
        if (!grab_region(x, y, w, h, cur)) break;
        int k = longshot_overlap(all, totalH, cur.px, cur.h, w);
        if (k <= 0) {
            // 没有重合规整就直接放弃，避免把无关画面硬拼在一起
            err = L"未检测到稳定的滚动重叠，已取消长截图";
            return false;
        }
        if (k >= cur.h - 2) break;   // 页面没有继续滚动
        int add = cur.h - k;
        if (totalH + add > MAX_H) add = MAX_H - totalH;
        if (add <= 0) break;
        size_t oldSize = all.size();
        all.resize(oldSize + (size_t)add * w * 4);
        memcpy(all.data() + oldSize, cur.px.data() + (size_t)k * w * 4,
               (size_t)add * w * 4);
        totalH += add;
    }

    if (totalH <= first.h + 2) {
        err = L"页面没有产生可拼接的滚动内容";
        return false;
    }
    logf(L"Long capture: %d x %d",w,totalH);
    out = make_bitmap(all, w, totalH);
    if (!out) {
        err = L"生成长截图位图失败";
        return false;
    }
    return true;
}

#include "output.h"
#include "settings.h"
#include "pin.h"
namespace {
std::vector<HWND> previewWindows;
struct Preview {
 Gdiplus::Bitmap* image; HWND hwnd{},save{},copy{},pin{};int offset=0;float scale=1;
 ~Preview(){delete image;}
};
void preview_layout(Preview*p){
 RECT r{};GetClientRect(p->hwnd,&r);
 MoveWindow(p->save,16,12,100,30,TRUE);MoveWindow(p->copy,128,12,100,30,TRUE);MoveWindow(p->pin,240,12,100,30,TRUE);
 p->scale=std::min(1.0f,(r.right-32)/(float)p->image->GetWidth());
 int total=(int)(p->image->GetHeight()*p->scale),view=std::max(1,(int)r.bottom-60);
 p->offset=std::clamp(p->offset,0,std::max(0,total-view));
 SCROLLINFO si{sizeof(si),SIF_RANGE|SIF_PAGE|SIF_POS,0,total-1,(UINT)view,p->offset,0};SetScrollInfo(p->hwnd,SB_VERT,&si,TRUE);
 InvalidateRect(p->hwnd,nullptr,TRUE);
}
LRESULT CALLBACK preview_proc(HWND h,UINT msg,WPARAM wp,LPARAM lp){
 auto*p=(Preview*)GetWindowLongPtrW(h,GWLP_USERDATA);
 if(msg==WM_CREATE){p=(Preview*)((CREATESTRUCTW*)lp)->lpCreateParams;p->hwnd=h;SetWindowLongPtrW(h,GWLP_USERDATA,(LONG_PTR)p);
  auto button=[&](const wchar_t*t,int id){auto c=CreateWindowW(L"BUTTON",t,WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,100,30,h,(HMENU)(INT_PTR)id,GetModuleHandleW(nullptr),nullptr);SendMessageW(c,WM_SETFONT,(WPARAM)GetStockObject(DEFAULT_GUI_FONT),TRUE);return c;};
   p->save=button(L"保存",1);p->copy=button(L"复制",2);p->pin=button(L"贴图",3);return 0;
 }
 if(!p)return DefWindowProcW(h,msg,wp,lp);
 switch(msg){
 case WM_SIZE:preview_layout(p);return 0;
 case WM_VSCROLL:{SCROLLINFO si{sizeof(si),SIF_ALL};GetScrollInfo(h,SB_VERT,&si);
  switch(LOWORD(wp)){case SB_THUMBTRACK:p->offset=si.nTrackPos;break;case SB_LINEUP:p->offset-=40;break;case SB_LINEDOWN:p->offset+=40;break;case SB_PAGEUP:p->offset-=si.nPage;break;case SB_PAGEDOWN:p->offset+=si.nPage;break;}
  preview_layout(p);return 0;}
 case WM_MOUSEWHEEL:p->offset-=GET_WHEEL_DELTA_WPARAM(wp);preview_layout(p);return 0;
 case WM_COMMAND:
   if(LOWORD(wp)==1){auto s=load_settings();std::wstring path;if(save_bitmap_direct(p->image,s.save_dir,s.image_format,s.jpeg_quality,path)&&s.auto_copy)copy_bitmap_to_clipboard(p->image);else if(path.empty())MessageBoxW(h,L"保存失败，请检查设置中的保存位置。",L"保存失败",MB_OK|MB_ICONERROR);}
  else if(LOWORD(wp)==2)copy_bitmap_to_clipboard(p->image);
  else if(LOWORD(wp)==3)pin_create(p->image->Clone(0,0,p->image->GetWidth(),p->image->GetHeight(),PixelFormat32bppRGB),100,100);
  return 0;
 case WM_PAINT:{PAINTSTRUCT ps{};HDC dc=BeginPaint(h,&ps);RECT r;GetClientRect(h,&r);Gdiplus::Graphics g(dc);g.Clear(Gdiplus::Color(255,245,245,245));g.SetClip(Gdiplus::Rect(0,56,r.right,r.bottom-56));g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);g.DrawImage(p->image,Gdiplus::RectF(16.0f,60.0f-p->offset,p->image->GetWidth()*p->scale,p->image->GetHeight()*p->scale));EndPaint(h,&ps);return 0;}
 case WM_CLOSE:DestroyWindow(h);return 0;
 case WM_DESTROY:previewWindows.erase(std::remove(previewWindows.begin(),previewWindows.end(),h),previewWindows.end());SetWindowLongPtrW(h,GWLP_USERDATA,0);delete p;return 0;
 }return DefWindowProcW(h,msg,wp,lp);
}
}
void longshot_preview(Gdiplus::Bitmap* image){
 if(!image)return;
 WNDCLASSW wc{};wc.lpfnWndProc=preview_proc;wc.hInstance=GetModuleHandleW(nullptr);wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.lpszClassName=L"weijieLongPreview";RegisterClassW(&wc);
 auto*p=new Preview{image};RECT work{};SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);
 HWND h=CreateWindowExW(0,wc.lpszClassName,L"滚动截图预览 · 滚轮查看 / 保存 / 复制",WS_OVERLAPPEDWINDOW|WS_VSCROLL,
  work.left+40,work.top+40,std::min(900,(int)(work.right-work.left)-80),std::min(760,(int)(work.bottom-work.top)-80),nullptr,nullptr,wc.hInstance,p);
 if(!h){delete p;return;}previewWindows.push_back(h);ShowWindow(h,SW_SHOW);SetForegroundWindow(h);
}


void longshot_close_previews(){while(!previewWindows.empty())DestroyWindow(previewWindows.back());}
