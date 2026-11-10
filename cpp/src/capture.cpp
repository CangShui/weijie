#include "capture.h"
#include "settings.h"
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

// 截图时忽略的窗口类（桌面壳层等）
static const wchar_t* SKIP_CLASSES[] = {
    L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
    L"Windows.UI.Core.CoreWindow", L"ApplicationManager_DesktopShellWindow",
    L"MultitaskingViewFrame", L"ForegroundStaging", L"TaskListThumbnailWnd",
    L"Windows.Internal.Shell.TabProxyWindow", L"MsgrIMEWindowClass",
    L"SysShadow", L"ConsoleWindowClass", L"weijieOverlay", L"weijiePin",
    L"weijieSettings", L"weijieTip",
};

static bool class_is_skipped(const wchar_t* cls) {
    for (auto* c : SKIP_CLASSES) {
        if (wcscmp(cls, c) == 0) return true;
    }
    return false;
}

static bool is_transparent_overlay(HWND h) {
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (ex & WS_EX_TRANSPARENT) return true;
    if ((ex & WS_EX_LAYERED) == 0) return false;

    BYTE alpha = 255;
    DWORD flags = 0;
    return GetLayeredWindowAttributes(h, nullptr, &alpha, &flags) &&
           (flags & LWA_ALPHA) && alpha == 0;
}

static bool window_bounds(HWND h, RECT& r) {
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))) &&
        !GetWindowRect(h, &r)) {
        return false;
    }
    return true;
}

static bool is_window_candidate(HWND h, POINT pt, RECT& r) {
    if (!IsWindowVisible(h) || IsIconic(h)) return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return false;

    wchar_t cls[256] = {0};
    GetClassNameW(h, cls, 256);
    if (class_is_skipped(cls)) return false;

    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return false;
    if (is_transparent_overlay(h)) return false;

    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) {
        return false;
    }

    if (!window_bounds(h, r)) return false;
    const int w = r.right - r.left;
    const int hh = r.bottom - r.top;
    if (w < 40 || hh < 30 || !PtInRect(&r, pt)) return false;
    return true;
}

bool capture_desktop(Shot& s) {
    free_shot(s);

    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0 || vh <= 0) return false;

    HDC hdcScreen = GetDC(nullptr);
    if (!hdcScreen) return false;
    HDC hdcMem = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = vw;
    bi.bmiHeader.biHeight = -vh;     // 自上而下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP hbm = CreateDIBSection(hdcScreen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbm) {
        DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcScreen);
        return false;
    }
    HGDIOBJ old = SelectObject(hdcMem, hbm);
    BOOL ok = BitBlt(hdcMem, 0, 0, vw, vh, hdcScreen, vx, vy, SRCCOPY | CAPTUREBLT);
    Settings cfg = load_settings();
    if (ok && cfg.include_cursor) {
        CURSORINFO ci = {};
        ci.cbSize = sizeof(ci);
        if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) {
            int cx = ci.ptScreenPos.x - vx;
            int cy = ci.ptScreenPos.y - vy;
            if (cx >= 0 && cy >= 0 && cx < vw && cy < vh) {
                ICONINFO ii{};
                if(GetIconInfo(ci.hCursor,&ii)){
                    DrawIconEx(hdcMem,cx-(int)ii.xHotspot,cy-(int)ii.yHotspot,ci.hCursor,0,0,0,nullptr,DI_NORMAL);
                    if(ii.hbmMask)DeleteObject(ii.hbmMask);if(ii.hbmColor)DeleteObject(ii.hbmColor);
                }
            }
        }
    }
    SelectObject(hdcMem, old);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
    if (!ok) {
        DeleteObject(hbm);
        return false;
    }

    // 压暗副本：等价于盖一层 alpha=122 的黑色
    void* dimBits = nullptr;
    HBITMAP hbmDim = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &dimBits, nullptr, 0);
    if (!hbmDim) {
        DeleteObject(hbm);
        return false;
    }
    {
        const BYTE* src = (const BYTE*)bits;
        BYTE* dst = (BYTE*)dimBits;
        size_t n = (size_t)vw * vh;
        for (size_t i = 0; i < n; ++i) {
            // px * (1 - 122/255)
            dst[i * 4 + 0] = (BYTE)(src[i * 4 + 0] * 133 / 255);
            dst[i * 4 + 1] = (BYTE)(src[i * 4 + 1] * 133 / 255);
            dst[i * 4 + 2] = (BYTE)(src[i * 4 + 2] * 133 / 255);
            dst[i * 4 + 3] = 255;
        }
    }

    s.vx = vx; s.vy = vy; s.vw = vw; s.vh = vh;
    s.hbm = hbm; s.hbmDim = hbmDim; s.bits = bits;
    // 直接包裹 DIB 内存，零拷贝
    s.bmp = new Gdiplus::Bitmap(vw, vh, vw * 4, PixelFormat32bppRGB, (BYTE*)bits);
    s.bmpDim = new Gdiplus::Bitmap(vw, vh, vw * 4, PixelFormat32bppRGB, (BYTE*)dimBits);
    return true;
}

void free_shot(Shot& s) {
    delete s.bmp;      s.bmp = nullptr;
    delete s.bmpDim;   s.bmpDim = nullptr;
    if (s.hbm) { DeleteObject(s.hbm); s.hbm = nullptr; }
    if (s.hbmDim) { DeleteObject(s.hbmDim); s.hbmDim = nullptr; }
    s.bits = nullptr;
    s.vw = s.vh = 0;
}

// ------------------------------------------------------------------ 窗口探测
struct WinHit {
    HWND hwnd;
    RECT rect;
    long long area;
    POINT point;
};

static BOOL CALLBACK enum_proc(HWND h, LPARAM lp) {
    auto* best = (WinHit*)lp;
    RECT r = {};
    if (!is_window_candidate(h, best->point, r)) return TRUE;
    best->hwnd = h;
    best->rect = r;
    return FALSE; // EnumWindows is front-to-back: first eligible containing window wins.
}

bool window_rect_at(int sx, int sy, RECT& out) {
    WinHit best = {nullptr, {}, LLONG_MAX, {sx,sy}};
    POINT pt = {sx, sy};
    HWND hit = WindowFromPoint(pt);
    if (hit) {
        HWND root = GetAncestor(hit, GA_ROOT);
        RECT candidate{};
        if (root && is_window_candidate(root, pt, candidate)) {
            out = candidate;
            return true;
        }
    }
    EnumWindows(enum_proc, (LPARAM)&best);
    if (best.hwnd && ptInRect(best.rect, sx, sy)) {
        out = best.rect;
        return true;
    }
    return false;
}
