#include "longshot.h"
#include "animated_image.h"

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

LongshotAppend LongshotStitcher::append(const std::vector<BYTE>& frame,int frameHeight){
    int overlap=longshot_overlap(pixels,height,frame,frameHeight,width);
    if(overlap<=0)return LongshotAppend::Unmatched;
    if(overlap>=frameHeight)return LongshotAppend::Unchanged;
    int wanted=frameHeight-overlap;
    size_t rowBytes=(size_t)width*4;
    int add=(int)std::min<size_t>(wanted,maxBytes>pixels.size()?(maxBytes-pixels.size())/rowBytes:0);
    add=std::min(add,LONGSHOT_MAX_ENCODED_HEIGHT-height);
    size_t oldSize=pixels.size();
    pixels.resize(oldSize+(size_t)add*rowBytes);
    if(add)memcpy(pixels.data()+oldSize,frame.data()+(size_t)overlap*rowBytes,(size_t)add*rowBytes);
    height+=add;
    if(height==LONGSHOT_MAX_ENCODED_HEIGHT)return LongshotAppend::EncoderLimit;
    return add<wanted||maxBytes-pixels.size()<rowBytes?LongshotAppend::MemoryLimit:LongshotAppend::Added;
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
    LogTrace trace;
    out = nullptr;
    err.clear();
    int x = shot.vx + sel.left;
    int y = shot.vy + sel.top;
    int w = sel.right - sel.left;
    int h = sel.bottom - sel.top;
    logf(L"滚动截图请求到达：选区=(%d,%d) %dx%d，无24次/2万像素截断，像素缓存上限=%zu字节",
         x,y,w,h,LONGSHOT_MAX_PIXEL_BYTES);
    if (w < 16 || h < 48 || w > 10000 || (unsigned long long)w*h*4 > LONGSHOT_MAX_PIXEL_BYTES) {
        err = L"选区尺寸不支持，请选择至少 16×48 像素且不超过内存上限的区域";
        logf(L"滚动截图被拒绝：%s",err.c_str());return false;
    }

    POINT center = { x + w / 2, y + h / 2 };
    HWND target = WindowFromPoint(center);
    HWND root = target ? GetAncestor(target,GA_ROOT) : nullptr;
    if(!root){err=L"无法定位滚动窗口";logf(L"滚动截图被拒绝：%s",err.c_str());return false;}
    DWORD targetPid=0;GetWindowThreadProcessId(root,&targetPid);
    if(targetPid==GetCurrentProcessId()){err=L"请框选其他应用的可滚动内容";logf(L"滚动截图被拒绝：%s",err.c_str());return false;}
    SetForegroundWindow(root);
    POINT oldCursor{};GetCursorPos(&oldCursor);
    struct Restore { POINT p; ~Restore(){SetCursorPos(p.x,p.y);} } restore{oldCursor};
    SetCursorPos(center.x, center.y);
    Sleep(120);

    LongFrame first;
    if (!grab_region(x, y, w, h, first)) {
        err = L"抓取滚动截图首帧失败";
        logf(L"滚动截图返回：失败 原因=%s",err.c_str());
        return false;
    }

    LongshotStitcher stitched(w,h,std::move(first.px));
    size_t steps=0;int unchanged=0;
    GetAsyncKeyState(VK_ESCAPE);
    for (;;) {
        if(GetAsyncKeyState(VK_ESCAPE)&0x8001){err=L"已按 Esc 停止，保留已拼接内容";break;}
        if(GetForegroundWindow()!=root){err=L"目标窗口已切换，保留已拼接内容";break;}
        mouse_event(MOUSEEVENTF_WHEEL, 0, 0, (DWORD)(-WHEEL_DELTA), 0);
        ++steps;
        bool stopped=false;
        for(int tick=0;tick<8;++tick){
            Sleep(40);
            if(GetAsyncKeyState(VK_ESCAPE)&0x8001){err=L"已按 Esc 停止，保留已拼接内容";stopped=true;break;}
            if(GetForegroundWindow()!=root){err=L"目标窗口已切换，保留已拼接内容";stopped=true;break;}
        }
        if(stopped)break;
        LongFrame cur;
        if (!grab_region(x, y, w, h, cur)) {err=L"后续画面抓取失败，保留已拼接内容";break;}
        auto result=stitched.append(cur.px,cur.h);
        const wchar_t* results[]={L"新增图像",L"画面未变",L"无法匹配",L"达到内存上限",L"达到编码器尺寸上限"};
        logf(L"滚动截图拼接：滚动次数=%zu 结果=%s 当前高度=%d 像素字节=%zu",
             steps,results[(int)result],stitched.height,stitched.pixels.size());
        if(result==LongshotAppend::Unmatched){
            err=L"无法匹配后续画面，已停止并保留已拼接内容";break;
        }
        if(result==LongshotAppend::MemoryLimit){err=L"达到 200 MiB 像素缓存上限，已停止并保留已拼接内容";break;}
        if(result==LongshotAppend::EncoderLimit){err=L"达到当前编码器 65,535 像素高度上限，已停止并保留可保存的长图";break;}
        if(result==LongshotAppend::Unchanged){
            if(++unchanged>=3){err=L"页面连续三次未继续滚动，已完成";break;}
        }else unchanged=0;
    }

    if (stitched.height <= h + 2) {
        err = L"页面没有产生可拼接的滚动内容。"+err;
        logf(L"滚动截图返回：失败 滚动次数=%zu 原因=%s",steps,err.c_str());
        return false;
    }
    out = make_bitmap(stitched.pixels,w,stitched.height);
    if (!out) {
        err = L"生成长截图位图失败";
        logf(L"滚动截图返回：失败 原因=%s",err.c_str());
        return false;
    }
    logf(L"滚动截图返回：成功 尺寸=%dx%d 滚动次数=%zu 结束原因=%s",w,stitched.height,steps,err.c_str());
    return true;
}

#include "output.h"
#include "settings.h"
#include "pin.h"
namespace {
std::vector<HWND> previewWindows;
struct Preview {
 Gdiplus::Bitmap* image; HWND hwnd{},save{},saveAs{},copy{},pin{},status{};int offset=0;float scale=1;
 std::shared_ptr<const AnimatedScreenshot> animation;ULONGLONG animationStart=GetTickCount64();
 uint64_t animationTime=0;
 unsigned long long traceId;
 HFONT font{};
 std::wstring statusText;
 Preview(Gdiplus::Bitmap* bitmap,unsigned long long trace,const std::wstring& reason)
     :image(bitmap),traceId(trace),statusText(reason){}
 ~Preview(){delete image;if(font)DeleteObject(font);}
};
void preview_layout(Preview*p){
 RECT r{};GetClientRect(p->hwnd,&r);
 MoveWindow(p->save,S(16),S(12),S(100),S(30),TRUE);MoveWindow(p->saveAs,S(128),S(12),S(100),S(30),TRUE);
 MoveWindow(p->copy,S(240),S(12),S(100),S(30),TRUE);MoveWindow(p->pin,S(352),S(12),S(100),S(30),TRUE);
 MoveWindow(p->status,S(16),S(50),std::max(1,(int)r.right-S(32)),S(24),TRUE);
 p->scale=std::min(1.0f,std::max(1,(int)r.right-S(32))/(float)p->image->GetWidth());
 int total=(int)(p->image->GetHeight()*p->scale),view=std::max(1,(int)r.bottom-S(84));
 p->offset=std::clamp(p->offset,0,std::max(0,total-view));
 SCROLLINFO si{sizeof(si),SIF_RANGE|SIF_PAGE|SIF_POS,0,total-1,(UINT)view,p->offset,0};SetScrollInfo(p->hwnd,SB_VERT,&si,TRUE);
 InvalidateRect(p->hwnd,nullptr,TRUE);
}
void preview_paint(Preview*p,HDC dc){
 RECT r{};GetClientRect(p->hwnd,&r);
 Gdiplus::Graphics g(dc);g.Clear(Gdiplus::Color(255,245,245,245));
 g.SetClip(Gdiplus::Rect(0,S(80),r.right,std::max(1,(int)r.bottom-S(80))));
 g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
 float sourceY=p->offset/p->scale;
 float sourceHeight=std::min(p->image->GetHeight()-sourceY,std::max(0,(int)r.bottom-S(84))/p->scale);
 RECT visible{0,(LONG)floor(sourceY),(LONG)p->image->GetWidth(),
     std::min((LONG)p->image->GetHeight(),(LONG)ceil(sourceY+sourceHeight))};
 auto frame=p->animation&&sourceHeight>0?p->animation->render(GetTickCount64()-p->animationStart,&visible):nullptr;
 auto* image=frame?frame.get():p->image;
 if(sourceHeight>0)g.DrawImage(image,
     Gdiplus::RectF((float)S(16),(float)S(84),p->image->GetWidth()*p->scale,sourceHeight*p->scale),
     0,sourceY-(frame?visible.top:0),(float)p->image->GetWidth(),sourceHeight,Gdiplus::UnitPixel);
}
LRESULT CALLBACK preview_proc(HWND h,UINT msg,WPARAM wp,LPARAM lp){
 auto*p=(Preview*)GetWindowLongPtrW(h,GWLP_USERDATA);
 if(msg==WM_CREATE){p=(Preview*)((CREATESTRUCTW*)lp)->lpCreateParams;p->hwnd=h;SetWindowLongPtrW(h,GWLP_USERDATA,(LONG_PTR)p);
  p->font=CreateFontW(-S(13),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,
      CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
  auto button=[&](const wchar_t*t,int id){auto c=CreateWindowW(L"BUTTON",t,WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,100,30,h,(HMENU)(INT_PTR)id,GetModuleHandleW(nullptr),nullptr);SendMessageW(c,WM_SETFONT,(WPARAM)p->font,TRUE);return c;};
   p->save=button(L"保存",1);p->saveAs=button(L"另存为",4);p->copy=button(L"复制",2);p->pin=button(L"贴图",3);
   p->status=CreateWindowW(L"STATIC",p->statusText.c_str(),WS_CHILD|WS_VISIBLE|SS_PATHELLIPSIS,
       0,0,0,0,h,nullptr,GetModuleHandleW(nullptr),nullptr);
   SendMessageW(p->status,WM_SETFONT,(WPARAM)p->font,TRUE);
   return 0;
 }
 if(!p)return DefWindowProcW(h,msg,wp,lp);
 switch(msg){
 case WM_TIMER:
  if(p->animation&&p->animation->duration()){
   uint64_t time=(GetTickCount64()-p->animationStart)%p->animation->duration();
   if(time<p->animationTime||p->animation->next_change(p->animationTime)<=time){
    p->animationTime=time;RECT r{};GetClientRect(h,&r);r.top=S(80);InvalidateRect(h,&r,FALSE);
   }
  }return 0;
 case WM_GETMINMAXINFO:{auto* info=(MINMAXINFO*)lp;info->ptMinTrackSize={S(500),S(260)};return 0;}
 case WM_SIZE:preview_layout(p);return 0;
 case WM_VSCROLL:{SCROLLINFO si{sizeof(si),SIF_ALL};GetScrollInfo(h,SB_VERT,&si);
  switch(LOWORD(wp)){case SB_THUMBTRACK:p->offset=si.nTrackPos;break;case SB_LINEUP:p->offset-=40;break;case SB_LINEDOWN:p->offset+=40;break;case SB_PAGEUP:p->offset-=si.nPage;break;case SB_PAGEDOWN:p->offset+=si.nPage;break;}
  preview_layout(p);return 0;}
 case WM_MOUSEWHEEL:p->offset-=GET_WHEEL_DELTA_WPARAM(wp);preview_layout(p);return 0;
 case WM_COMMAND:{
  LogTrace trace(p->traceId);
  int id=LOWORD(wp);
  if(id==1||id==4){
   auto s=load_settings();std::wstring path;
   logf(L"长截图预览保存请求：操作=%s 设置目录=%s 格式=%s 尺寸=%ux%u",
        id==1?L"保存":L"另存为",s.save_dir.c_str(),s.image_format.c_str(),p->image->GetWidth(),p->image->GetHeight());
   bool cancelled=false;
   bool ok=id==1?save_bitmap_direct(p->image,s.save_dir,s.image_format,s.jpeg_quality,path,h,p->animation,&cancelled):
                  save_bitmap_dialog(h,p->image,s.save_dir,s.image_format,s.jpeg_quality,&path,p->animation,&cancelled);
   if(cancelled){logf(L"长截图保存取消：预览保留");return 0;}
   if(ok){
    p->statusText=L"已保存："+path;SetWindowTextW(p->status,p->statusText.c_str());
    if(s.auto_copy&&!copy_bitmap_to_clipboard(p->image)){
     logf(L"长截图已保存，但自动复制失败：文件=%s",path.c_str());
     MessageBoxW(h,L"图片已保存，但复制到剪贴板失败。可点击“复制”重试。",L"已保存",MB_OK|MB_ICONWARNING);
    }
   }else if(id==1){
    bool tallJpeg=(s.image_format==L"jpg"||s.image_format==L"jpeg")&&
                 (p->image->GetWidth()>65500||p->image->GetHeight()>65500);
    p->statusText=tallJpeg?L"图片超过 JPEG 尺寸上限，请另存为 PNG 或 BMP":L"保存失败，请检查设置目录或使用“另存为”";
    SetWindowTextW(p->status,p->statusText.c_str());
    MessageBoxW(h,tallJpeg?L"图片尺寸超过 JPEG 格式上限，请点击“另存为”选择 PNG 或 BMP。":
        L"长截图保存失败，请检查设置中的保存目录，或点击“另存为”选择其他位置。",L"保存失败",MB_OK|MB_ICONERROR);
   }
   logf(L"长截图预览保存返回：成功=%d 文件=%s；预览保留",(int)ok,path.c_str());
  }else if(id==2){
   bool ok=copy_bitmap_to_clipboard(p->image);logf(L"长截图复制返回：成功=%d",(int)ok);
   if(!ok)MessageBoxW(h,L"复制失败，请重试。",L"复制失败",MB_OK|MB_ICONERROR);
  }else if(id==3)pin_create(p->image->Clone(0,0,p->image->GetWidth(),p->image->GetHeight(),PixelFormat32bppRGB),100,100,p->animation);
  return 0;
 }
 case WM_PAINT:{PAINTSTRUCT ps{};HDC dc=BeginPaint(h,&ps);preview_paint(p,dc);EndPaint(h,&ps);return 0;}
 case WM_PRINTCLIENT:preview_paint(p,(HDC)wp);return 0;
 case WM_CLOSE:DestroyWindow(h);return 0;
 case WM_DESTROY:KillTimer(h,1);previewWindows.erase(std::remove(previewWindows.begin(),previewWindows.end(),h),previewWindows.end());SetWindowLongPtrW(h,GWLP_USERDATA,0);delete p;return 0;
 }return DefWindowProcW(h,msg,wp,lp);
}
}
void longshot_preview(Gdiplus::Bitmap* image,const std::wstring& finishReason,
                      std::shared_ptr<const AnimatedScreenshot> animation){
 if(!image)return;
 LogTrace trace;
 logf(L"长截图预览请求：尺寸=%ux%u 结束原因=%s",image->GetWidth(),image->GetHeight(),finishReason.c_str());
 WNDCLASSW wc{};wc.lpfnWndProc=preview_proc;wc.hInstance=GetModuleHandleW(nullptr);wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.lpszClassName=L"weijieLongPreview";RegisterClassW(&wc);
 if(animation){
  auto first=animation->render(0);
  if(first){delete image;image=first.release();}
 }
 auto*p=new Preview(image,trace.id(),finishReason);p->animation=std::move(animation);
 RECT work{};SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);
 HWND h=CreateWindowExW(0,wc.lpszClassName,L"滚动截图预览 · 保存 / 另存为 / 复制",WS_OVERLAPPEDWINDOW|WS_VSCROLL,
  work.left+40,work.top+40,std::min(900,(int)(work.right-work.left)-80),std::min(760,(int)(work.bottom-work.top)-80),nullptr,nullptr,wc.hInstance,p);
 if(!h){logf(L"长截图预览创建失败：error=%lu",GetLastError());delete p;return;}
 if(p->animation)SetTimer(h,1,30,nullptr);
 previewWindows.push_back(h);ShowWindow(h,SW_SHOW);SetForegroundWindow(h);
 logf(L"长截图预览返回：窗口=%p，可保存或另存为",h);
}


void longshot_close_previews(){while(!previewWindows.empty())DestroyWindow(previewWindows.back());}
