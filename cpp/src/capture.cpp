#include "capture.h"
#include "settings.h"
#include "desktop_duplication.h"
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
    return capture_desktop(s,load_settings());
}

bool capture_desktop(Shot& s,const Settings& cfg) {
    LogTrace trace;
    const auto started=GetTickCount64();
    free_shot(s);

    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    logf(L"抓屏开始：虚拟桌面=%dx%d 原点=(%d,%d)，先取游戏画面再切换焦点", vw, vh, vx, vy);
    if (vw <= 0 || vh <= 0) {
        logf(L"抓屏被拒绝：虚拟桌面尺寸无效");
        return false;
    }

    HDC hdcScreen = GetDC(nullptr);
    if (!hdcScreen) { logf(L"抓屏失败：无法取得桌面 DC error=%lu", GetLastError()); return false; }
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    if (!hdcMem) {
        logf(L"抓屏失败：无法创建内存 DC error=%lu", GetLastError());
        ReleaseDC(nullptr, hdcScreen);
        return false;
    }

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
        logf(L"抓屏失败：无法创建原始位图 error=%lu", GetLastError());
        DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcScreen);
        return false;
    }
    HGDIOBJ old = SelectObject(hdcMem, hbm);
    // 先初始化屏幕之间的空隙；DXGI 按各屏的物理位置填充画面。
    std::fill_n((DWORD*)bits, (size_t)vw * vh, 0xFF000000);
    RECT desktop{vx, vy, vx + vw, vy + vh};
    bool gdiOk = BitBlt(hdcMem, 0, 0, vw, vh, hdcScreen, vx, vy, SRCCOPY | CAPTUREBLT) != FALSE;
    DWORD gdiError = gdiOk ? ERROR_SUCCESS : GetLastError();
    GdiFlush();
    const auto gdiFinished=GetTickCount64();
    logf(L"GDI 抓屏基底：结果=%s error=%lu 耗时=%llums，随后按屏更新 DXGI 画面",
         gdiOk ? L"成功" : L"失败", gdiError,gdiFinished-started);
    bool dxgiOk = capture_desktop_dxgi((BYTE*)bits, desktop, gdiOk);
    const auto dxgiFinished=GetTickCount64();
    bool ok = dxgiOk || gdiOk;
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
    GdiFlush(); // 后续直接读取 DIB 内存，先提交 GDI 的光标绘制与回退抓屏。
    SelectObject(hdcMem, old);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
    if (!ok) {
        logf(L"抓屏返回：失败，未创建冻结帧");
        DeleteObject(hbm);
        return false;
    }

    // 压暗副本：等价于盖一层 alpha=122 的黑色
    void* dimBits = nullptr;
    HBITMAP hbmDim = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &dimBits, nullptr, 0);
    if (!hbmDim) {
        logf(L"抓屏失败：无法创建遮罩位图 error=%lu", GetLastError());
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
    logf(L"抓屏返回：成功，冻结帧与压暗副本=%dx%d 包含鼠标=%d GDI=%llums DXGI=%llums 光标遮罩=%llums 总计=%llums",
        vw,vh,(int)cfg.include_cursor,gdiFinished-started,dxgiFinished-gdiFinished,
        GetTickCount64()-dxgiFinished,GetTickCount64()-started);
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
