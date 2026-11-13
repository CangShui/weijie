#include "pin.h"
#include "settings.h"
#include "output.h"
#include "animated_image.h"
#include <vector>

static const wchar_t* PIN_CLASS = L"weijiePin";
static const int MENU_COPY = 5001;
static const int MENU_SAVE = 5002;
static const int MENU_CLOSE = 5003;

struct PinWin {
    HWND hwnd = nullptr;
    Gdiplus::Bitmap* bmp = nullptr;
    std::shared_ptr<const AnimatedScreenshot> animation;
    ULONGLONG animationStart=GetTickCount64();
    uint64_t animationTime=0;
    std::unique_ptr<Gdiplus::Bitmap> animationFrame;
    unsigned long long traceId=0;
    float scale = 1.0f;
    bool dragging = false;
    POINT dragFrom = {0, 0};
    RECT dragRect = {0, 0, 0, 0};
};

static std::vector<PinWin*> g_pins;

static PinWin* pin_of(HWND h) {
    return (PinWin*)GetWindowLongPtrW(h, GWLP_USERDATA);
}

static void pin_apply_size(PinWin* p, bool keepCenter) {
    int w = (int)lround(p->bmp->GetWidth() * p->scale);
    int h = (int)lround(p->bmp->GetHeight() * p->scale);
    RECT r;
    GetWindowRect(p->hwnd, &r);
    if (keepCenter) {
        int cx = (r.left + r.right) / 2;
        int cy = (r.top + r.bottom) / 2;
        r.left = cx - w / 2;
        r.top = cy - h / 2;
    }
    r.right = r.left + w;
    r.bottom = r.top + h;
    SetWindowPos(p->hwnd, HWND_TOPMOST, r.left, r.top, w, h, SWP_NOACTIVATE);
}

static void pin_paint(PinWin* p) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(p->hwnd, &ps);
    RECT rc;
    GetClientRect(p->hwnd, &rc);
    Gdiplus::Graphics g(hdc);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    g.DrawImage(p->animationFrame?p->animationFrame.get():p->bmp, 0, 0, rc.right, rc.bottom);
    // 细边框
    Gdiplus::Pen pen(Gdiplus::Color(90, 0, 0, 0), 1.0f);
    g.DrawRectangle(&pen, 0.0f, 0.0f, (float)(rc.right - 1), (float)(rc.bottom - 1));
    EndPaint(p->hwnd, &ps);
}

static void pin_save_as(PinWin* p) {
    LogTrace trace(p->traceId);
    Settings s = load_settings();
    save_bitmap_dialog(p->hwnd, p->bmp, s.save_dir, s.image_format, s.jpeg_quality,nullptr,p->animation);
}

static void pin_remove(PinWin* p) {
    for (size_t i = 0; i < g_pins.size(); ++i) {
        if (g_pins[i] == p) {
            g_pins.erase(g_pins.begin() + (ptrdiff_t)i);
            break;
        }
    }
    DestroyWindow(p->hwnd);
}

static LRESULT CALLBACK pin_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    PinWin* p = pin_of(h);
    LogTrace trace(p?p->traceId:0);

    switch (msg) {
    case WM_PAINT:
        if (p) pin_paint(p);
        return 0;
    case WM_TIMER:
        if(p&&p->animation&&p->animation->duration()){
            uint64_t time=(GetTickCount64()-p->animationStart)%p->animation->duration();
            if(time<p->animationTime||p->animation->next_change(p->animationTime)<=time){
                p->animationFrame=p->animation->render(time);p->animationTime=time;
                InvalidateRect(h,nullptr,FALSE);
            }
        }
        return 0;

    case WM_LBUTTONDOWN:
        if (p) {
            p->dragging = true;
            GetCursorPos(&p->dragFrom);
            GetWindowRect(h, &p->dragRect);
            SetCapture(h);
        }
        return 0;

    case WM_MOUSEMOVE:
        if (p && p->dragging) {
            POINT pt;
            GetCursorPos(&pt);
            int dx = pt.x - p->dragFrom.x;
            int dy = pt.y - p->dragFrom.y;
            SetWindowPos(h, HWND_TOPMOST, p->dragRect.left + dx, p->dragRect.top + dy,
                         0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return 0;

    case WM_LBUTTONUP:
        if (p && p->dragging) {
            p->dragging = false;
            ReleaseCapture();
        }
        return 0;

    case WM_LBUTTONDBLCLK:
        if (p) pin_remove(p);
        return 0;

    case WM_MOUSEWHEEL: {
        if (!p) return 0;
        short delta = GET_WHEEL_DELTA_WPARAM(wp);
        float step = delta > 0 ? 1.1f : 1.0f / 1.1f;
        float ns = p->scale * step;
        if (ns < 0.15f) ns = 0.15f;
        if (ns > 6.0f) ns = 6.0f;
        p->scale = ns;
        pin_apply_size(p, true);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    case WM_RBUTTONUP: {
        if (!p) return 0;
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, MENU_COPY, L"复制图片");
        AppendMenuW(menu, MF_STRING, MENU_SAVE, L"另存为…");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, MENU_CLOSE, L"关闭贴图");
        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(h);
        int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                      pt.x, pt.y, 0, h, nullptr);
        DestroyMenu(menu);
        if (cmd == MENU_COPY) copy_bitmap_to_clipboard(p->bmp);
        else if (cmd == MENU_SAVE) pin_save_as(p);
        else if (cmd == MENU_CLOSE) pin_remove(p);
        return 0;
    }

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            if (p) pin_remove(p);
            return 0;
        }
        break;

    case WM_DESTROY:
        if (p) {
            logf(L"贴图窗口关闭：动态=%d，回收预览与动画引用",(int)(p->animation!=nullptr));
            KillTimer(h,1);
            g_pins.erase(std::remove(g_pins.begin(),g_pins.end(),p),g_pins.end());
            delete p->bmp;
            p->bmp = nullptr;
            p->animation.reset();
            SetWindowLongPtrW(h,GWLP_USERDATA,0);delete p;
        }
        return 0;

    case WM_NCHITTEST:
        return HTCLIENT;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void ensure_class() {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = pin_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = PIN_CLASS;
    RegisterClassExW(&wc);
    done = true;
}

void pin_create(Gdiplus::Bitmap* bmp, int screenX, int screenY,
                std::shared_ptr<const AnimatedScreenshot> animation) {
    if (!bmp) return;
    LogTrace trace(animation?animation->traceId:0);
    ensure_class();

    auto* p = new PinWin();
    p->bmp = bmp;
    p->animation=std::move(animation);
    p->traceId=trace.id();
    int w = (int)bmp->GetWidth();
    int h = (int)bmp->GetHeight();

    p->hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, PIN_CLASS, L"贴图",
                              WS_POPUP, screenX, screenY, w, h,
                              nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!p->hwnd) {
        logf(L"贴图窗口创建失败：error=%lu",GetLastError());
        delete p->bmp;
        delete p;
        return;
    }
    SetWindowLongPtrW(p->hwnd, GWLP_USERDATA, (LONG_PTR)p);
    if(p->animation)SetTimer(p->hwnd,1,30,nullptr);
    logf(L"贴图窗口创建完成：尺寸=%dx%d 动态=%d，保存保留原截图图层",w,h,(int)(p->animation!=nullptr));
    g_pins.push_back(p);
    ShowWindow(p->hwnd, SW_SHOW);
    SetForegroundWindow(p->hwnd);
    UpdateWindow(p->hwnd);
}

int pin_count() {
    return (int)g_pins.size();
}

void pin_close_all() {
    std::vector<PinWin*> copy = g_pins;
    for (auto* p : copy) {
        if (IsWindow(p->hwnd)) DestroyWindow(p->hwnd);
    }
    g_pins.clear();
}
