#include "common.h"
#include "settings.h"
#include "overlay.h"
#include "pin.h"
#include "output.h"
#include "ocr_setup.h"
#include "ppocr_setup.h"
#include "longshot.h"
#include "fast_ocr.h"
#include "ocr.h"
#include "ui_theme.h"
#include "hotkey_input.h"
#include "autostart.h"

#include <memory>
#include <atomic>
#include <thread>
#include <commctrl.h>
#include <shlobj.h>
#include <filesystem>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "comctl32.lib")

#define APP_NAME    L"微截"
#define APP_VERSION L"1.6.0"

// 与 src/app.rc 里的 IDI_APP 保持一致
#define IDI_APP 101

static const UINT HOTKEY_ID       = 0xA1;
static const UINT WM_TRAY         = WM_APP + 1;
static const UINT WM_ACTIVATE_APP = WM_APP + 2;
static const wchar_t* WND_CLASS   = L"weijieMsg";

static HINSTANCE g_hinst = nullptr;
static HWND g_msgwnd = nullptr;
static HICON g_icon = nullptr;
static HICON g_iconBig = nullptr;   // 32x32，给窗口类
static HICON g_iconSm = nullptr;    // 16x16，给窗口类标题栏/任务栏
static NOTIFYICONDATAW g_nid = {};
static UINT g_taskbarCreated = 0;
static bool g_trayRegistered = false;
static bool g_trayStopping = false;
static unsigned g_trayAttempts = 0;
static unsigned long long g_trayTrace = 0;
static Settings g_cfg;
static UINT g_hotMods = MOD_ALT, g_hotVk = 0x41;
static bool g_hotOk = false;
static std::wstring g_hotName = L"Alt+A";
static HHOOK g_kbHook = nullptr;
static bool g_hotDown = false;
static bool g_pollKeyDown = false;
static DWORD g_lastHotkeyTick = 0;
static std::atomic<bool> g_watchStop{false};
static std::thread g_watch;
static HWND g_settingsWnd = nullptr;
static RawHotkeyState g_rawKeys;

static const UINT WM_JJ_HOTKEY = WM_APP + 6;
static const UINT_PTR TIMER_HOTKEY_POLL = 1;
static const UINT_PTR TIMER_OCR_IDLE = 2;
static const UINT_PTR TIMER_TRAY_RETRY = 3;

// ------------------------------------------------------------------ 图标
static HICON make_icon(int size) {
    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color) return nullptr;

    HDC mem = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(mem, color);

    {
        Gdiplus::Graphics g(mem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));

        float S = (float)size;
        float inset = S * 0.055f;
        float r = (S - inset * 2) * 0.235f;
        Gdiplus::GraphicsPath p;
        float x = inset, y = inset, w = S - inset * 2, h = S - inset * 2;
        p.AddArc(x, y, r * 2, r * 2, 180, 90);
        p.AddArc(x + w - r * 2, y, r * 2, r * 2, 270, 90);
        p.AddArc(x + w - r * 2, y + h - r * 2, r * 2, r * 2, 0, 90);
        p.AddArc(x, y + h - r * 2, r * 2, r * 2, 90, 90);
        p.CloseFigure();

        Gdiplus::SolidBrush green(Gdiplus::Color(255, 7, 193, 96));
        g.FillPath(&green, &p);

        // 白色取景框角标
        float sw = S * 0.075f;
        float m = S * 0.255f;
        float arm = S * 0.20f;
        Gdiplus::Pen pen(Gdiplus::Color(255, 255, 255, 255), sw);
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        float L = m, T = m, R = S - m, B = S - m;
        g.DrawLine(&pen, L, T + arm, L, T);
        g.DrawLine(&pen, L, T, L + arm, T);
        g.DrawLine(&pen, R - arm, T, R, T);
        g.DrawLine(&pen, R, T, R, T + arm);
        g.DrawLine(&pen, R, B - arm, R, B);
        g.DrawLine(&pen, R, B, R - arm, B);
        g.DrawLine(&pen, L + arm, B, L, B);
        g.DrawLine(&pen, L, B, L, B - arm);
    }

    SelectObject(mem, old);
    DeleteDC(mem);

    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return ic;
}

// 优先用 app.rc 里嵌入的 ico（与资源管理器显示的一致），失败再退回自绘
static HICON load_icon(int size) {
    HICON h = (HICON)LoadImageW(g_hinst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                               size, size, LR_DEFAULTCOLOR);
    if (!h) {
        logf(L"嵌入图标缺失，改用自绘图标 size=%d", size);
        h = make_icon(size);
    }
    return h;
}

// ------------------------------------------------------------------ 热键
// 系统热键、低级钩子、异步轮询和 Raw Input 共用入口并去重。
// Raw Input 不安装到游戏进程，也不改变游戏本身的输入注册。
static bool recording_hotkey() {
    if(!g_settingsWnd||GetForegroundWindow()!=g_settingsWnd)return false;
    HWND field=GetDlgItem(g_settingsWnd,100),focus=GetFocus();
    return field&&IsWindowVisible(field)&&(focus==field||IsChild(field,focus));
}

static LRESULT CALLBACK hotkey_record_proc(HWND h,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR){
    if(msg==WM_SETFOCUS){
        LogTrace trace;
        UnregisterHotKey(g_msgwnd,HOTKEY_ID);
        logf(L"快捷键录入开始：仅在录入框获得焦点时暂停全局快捷键");
    }else if(msg==WM_KILLFOCUS&&g_cfg.hotkey_enabled){
        LogTrace trace;
        UnregisterHotKey(g_msgwnd,HOTKEY_ID);
        g_hotOk=RegisterHotKey(g_msgwnd,HOTKEY_ID,g_hotMods|MOD_NOREPEAT,g_hotVk)!=FALSE;
        logf(L"快捷键录入结束：恢复截图快捷键 结果=%d error=%lu",
             (int)g_hotOk,g_hotOk?ERROR_SUCCESS:GetLastError());
    }
    if(msg==WM_NCDESTROY)RemoveWindowSubclass(h,hotkey_record_proc,732);
    return DefSubclassProc(h,msg,wp,lp);
}

static bool hotkey_modifiers_match(UINT mods) {
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) return false;
    return ctrl == ((mods & MOD_CONTROL) != 0) &&
           alt == ((mods & MOD_ALT) != 0) &&
           shift == ((mods & MOD_SHIFT) != 0);
}

// 钩子不可用时，主线程仍以 30ms 间隔检测按键边沿。
static void poll_hotkey() {
    const bool keyDown = (GetAsyncKeyState(g_hotVk) & 0x8000) != 0;
    const bool newlyDown = keyDown && !g_pollKeyDown;
    g_pollKeyDown = keyDown;
    if (!keyDown) {
        g_hotDown = false;
        return;
    }
    if (!newlyDown || !g_hotOk || !g_cfg.hotkey_enabled || recording_hotkey() ||
        g_hotDown || !hotkey_modifiers_match(g_hotMods)) return;
    g_hotDown = true;
    PostMessageW(g_msgwnd, WM_JJ_HOTKEY, 1, (LPARAM)GetTickCount64());
}

static LRESULT CALLBACK low_level_keyboard_proc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
        if (info && info->vkCode == g_hotVk) {
            if (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) {
                if (g_hotOk && g_cfg.hotkey_enabled && !recording_hotkey() &&
                    !g_hotDown && hotkey_modifiers_match(g_hotMods)) {
                    g_hotDown = true;
                    PostMessageW(g_msgwnd, WM_JJ_HOTKEY, 0, (LPARAM)GetTickCount64());
                }
            } else if (wp == WM_KEYUP || wp == WM_SYSKEYUP) {
                g_hotDown = false;
            }
        }
    }
    return CallNextHookEx(g_kbHook, code, wp, lp);
}

static bool install_keyboard_hook() {
    if (g_kbHook) return true;
    g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, low_level_keyboard_proc, g_hinst, 0);
    if (!g_kbHook) logf(L"低级键盘钩子安装失败 %lu", GetLastError());
    else logf(L"低级键盘钩子已安装");
    return g_kbHook != nullptr;
}

static void hotkey_triggered(const wchar_t* source,ULONGLONG queuedAt=0) {
    LogTrace trace;
    logf(L"截图请求到达：来源=%s，快捷键=%s 输入消息排队=%llums",
        source,g_hotName.c_str(),queuedAt?GetTickCount64()-queuedAt:0);
    if (!g_cfg.hotkey_enabled || recording_hotkey()) {
        logf(L"截图请求被拦截：快捷键已关闭或正在快捷键录入框中录入，未进入抓屏");
        return;
    }
    const DWORD now = GetTickCount();
    if (g_lastHotkeyTick && (DWORD)(now - g_lastHotkeyTick) < 400) {
        logf(L"截图请求被拦截：400ms 内的重复输入，未进入抓屏");
        return;
    }
    g_lastHotkeyTick = now;
    logf(L"收到热键（%s）", source);
    bool ok = overlay_open();
    logf(L"截图请求返回：覆盖层%s", ok ? L"已打开" : L"未打开");
}

static void install_raw_keyboard() {
    RAWINPUTDEVICE device{0x01, 0x06, RIDEV_INPUTSINK, g_msgwnd};
    g_rawKeys.reset();
    if (!RegisterRawInputDevices(&device, 1, sizeof(device)))
        logf(L"Raw Input 后台键盘注册失败 error=%lu，保留系统热键、钩子和轮询", GetLastError());
    else
        logf(L"Raw Input 后台键盘已注册：只监听截图快捷键，不拦截游戏输入");
}

static void dispatch_raw_keyboard(const RAWKEYBOARD& input) {
    bool matched = g_rawKeys.update(input, g_hotMods, g_hotVk);
    if (matched && g_hotOk && g_cfg.hotkey_enabled && !recording_hotkey())
        PostMessageW(g_msgwnd, WM_JJ_HOTKEY, 2, (LPARAM)GetTickCount64());
}

static bool register_preferred(const std::wstring& name) {
 UINT mods=0,vk=0;if(!resolve_hotkey(name,mods,vk))return false;
 bool oldOk=g_hotOk;UINT oldMods=g_hotMods,oldVk=g_hotVk;
 UnregisterHotKey(g_msgwnd,HOTKEY_ID);g_hotOk=false;
 bool ok=RegisterHotKey(g_msgwnd,HOTKEY_ID,mods|MOD_NOREPEAT,vk)!=FALSE;
 logf(L"尝试热键 %s -> %s",name.c_str(),ok?L"True":L"False");
 if(ok){
  g_hotMods=mods;g_hotVk=vk;g_hotOk=true;g_hotName=name;g_hotDown=false;g_pollKeyDown=false;
  g_rawKeys.reset();
  install_keyboard_hook();
  return true;
 }
 if(oldOk)g_hotOk=RegisterHotKey(g_msgwnd,HOTKEY_ID,oldMods|MOD_NOREPEAT,oldVk)!=FALSE;
 return false;
}

// ------------------------------------------------------------------ 托盘
static void tray_try_add() {
    if (g_trayStopping || g_trayRegistered) return;
    LogTrace trace(g_trayTrace);
    ++g_trayAttempts;
    logf(L"托盘注册执行：第%u次，窗口=%p，图标=%p", g_trayAttempts, g_msgwnd, g_icon);
    // 同一窗口和 uID 标识同一个图标；重复的任务栏通知只更新已有图标。
    if (Shell_NotifyIconW(NIM_ADD, &g_nid) ||
        Shell_NotifyIconW(NIM_MODIFY, &g_nid)) {
        g_trayRegistered = true;
        KillTimer(g_msgwnd, TIMER_TRAY_RETRY);
        logf(L"托盘注册完成：系统已接受图标，尝试次数=%u，已停止重试", g_trayAttempts);
        NOTIFYICONIDENTIFIER identifier{};
        identifier.cbSize = sizeof(identifier);
        identifier.hWnd = g_msgwnd;
        identifier.uID = g_nid.uID;
        RECT bounds{};
        HRESULT location = Shell_NotifyIconGetRect(&identifier, &bounds);
        logf(L"托盘位置查询：HRESULT=0x%08X，系统返回区域=(%ld,%ld)-(%ld,%ld)",
             (unsigned)location, bounds.left, bounds.top, bounds.right, bounds.bottom);
        return;
    }
    logf(L"托盘注册未完成：系统通知区域尚未接受图标，2秒后重试；截图快捷键继续可用");
    if (!SetTimer(g_msgwnd, TIMER_TRAY_RETRY, 2000, nullptr))
        logf(L"托盘重试定时器创建失败：error=%lu，等待任务栏恢复通知", GetLastError());
}

static void tray_add(const wchar_t* reason = L"程序启动") {
    if (g_trayStopping || g_trayRegistered) return;
    LogTrace trace;
    g_trayTrace = trace.id();
    g_trayAttempts = 0;
    logf(L"托盘注册请求到达：来源=%s", reason);
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_msgwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_icon;
    wcsncpy_s(g_nid.szTip, APP_NAME, _TRUNCATE);
    tray_try_add();
}

static void tray_remove() {
    if (g_trayStopping) return;
    LogTrace trace(g_trayTrace);
    g_trayStopping = true;
    KillTimer(g_msgwnd, TIMER_TRAY_RETRY);
    if (g_trayRegistered)
        logf(L"托盘退出清理：移除图标结果=%d", (int)Shell_NotifyIconW(NIM_DELETE, &g_nid));
    else
        logf(L"托盘退出清理：图标未注册，已取消等待中的重试");
    g_trayRegistered = false;
}

void app_game_permission_hint() {
    NOTIFYICONDATAW notice = g_nid;
    notice.uFlags = NIF_INFO;
    notice.dwInfoFlags = NIIF_INFO;
    wcscpy_s(notice.szInfoTitle, L"游戏截图需要管理员权限");
    wcscpy_s(notice.szInfo, L"前台程序以管理员权限运行。请右键微截托盘，选择“以管理员身份重启（游戏截图）”，确认后再回到游戏按截图快捷键。");
    BOOL shown = g_trayRegistered && Shell_NotifyIconW(NIM_MODIFY, &notice);
    logf(L"游戏截图权限提示：系统接受通知=%d，使用托盘中的管理员重启入口，需要用户确认 UAC", (int)shown);
}

static void restart_elevated() {
    LogTrace trace;
    logf(L"管理员重启请求到达：来源=托盘，等待用户确认 UAC");
    std::wstring path = exe_path();
    SHELLEXECUTEINFOW launch{sizeof(launch)};
    launch.hwnd = g_msgwnd;
    launch.lpVerb = L"runas";
    launch.lpFile = path.c_str();
    launch.lpParameters = L"--elevated-restart";
    launch.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&launch)) {
        DWORD error = GetLastError();
        logf(L"管理员重启返回：未启动 error=%lu，保留当前微截实例", error);
        if (error != ERROR_CANCELLED)
            MessageBoxW(nullptr, L"无法以管理员身份启动微截，请退出后右键程序选择“以管理员身份运行”。",
                        APP_NAME, MB_OK | MB_ICONWARNING);
        return;
    }
    logf(L"管理员重启返回：新进程已启动，由新实例接管单实例锁");
}

static void show_tray_menu() {
    HMENU menu = CreatePopupMenu();
    std::wstring cap = std::wstring(L"截图  ") + g_hotName;
    AppendMenuW(menu, MF_STRING, 1, cap.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    HMENU hk = CreatePopupMenu();
    for (int i = 0; i < N_HOTKEY_PRESETS; ++i) {
        UINT flags = MF_STRING;
        if (g_hotName == HOTKEY_PRESETS[i].name) flags |= MF_CHECKED;
        AppendMenuW(hk, flags, 100 + i, HOTKEY_PRESETS[i].name);
    }
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)hk, L"快捷键");

    AppendMenuW(menu, MF_STRING, 2, L"设置（截图/OCR/翻译）…");
    AppendMenuW(menu, MF_STRING | (autostart_enabled() ? MF_CHECKED : 0), 3, L"开机自动启动（管理员）");
    if (!process_is_elevated(GetCurrentProcess()))
        AppendMenuW(menu, MF_STRING, 6, L"以管理员身份重启（游戏截图）…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 4, L"关于");
    AppendMenuW(menu, MF_STRING, 5, L"退出");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_msgwnd);
    int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_msgwnd, nullptr);
    DestroyMenu(menu);

    if (cmd == 1) {
        overlay_open();
    } else if (cmd >= 100 && cmd < 100 + N_HOTKEY_PRESETS) {
        std::wstring name = HOTKEY_PRESETS[cmd - 100].name;
        if (register_preferred(name)) {
            g_cfg.hotkey = g_hotName;
            g_cfg.hotkey_enabled = true;
            save_settings(g_cfg);
        }
    } else if (cmd == 2) {
        extern void show_settings_dialog(HWND owner);
        show_settings_dialog(g_msgwnd);
    } else if (cmd == 3) {
        std::wstring error;
        if(!set_autostart(!autostart_enabled(),error))
            MessageBoxW(nullptr,error.c_str(),L"开机启动设置失败",MB_OK|MB_ICONWARNING);
    } else if (cmd == 4) {
        std::wstring msg = std::wstring(L"微截 ") + APP_VERSION + L"\n\n"
                           L"微信风格的独立截图工具（原生 C++ 版）。\n"
                           L"不依赖微信，占用小、启动快。\n\n"
                           L"当前快捷键：" + g_hotName;
        MessageBoxW(nullptr, msg.c_str(), L"关于微截", MB_OK | MB_ICONINFORMATION);
    } else if (cmd == 5) {
        PostMessageW(g_msgwnd, WM_CLOSE, 0, 0);
    } else if (cmd == 6) {
        restart_elevated();
    }
}

// ------------------------------------------------------------------ Settings: compact native tabbed UI.
struct CfgDlg {
 double scale=g_scale;
 HWND hwnd{}, tab{}, hotkey{}, monitor{}, saveDir{}, format{}, quality{}, qualityVal{};
 HWND autoCopy{}, includeCursor{}, autostart{}, stroke{}, fontSize{};
 HWND ocrEnabled{}, ocrStatus{}, ocrDownload{}, ocrProgress{}, ocrEngine{}, ocrBackend{}, ocrBench{}, ocrIdle{};
 HWND translateEnabled{};
 HWND base{}, key{}, model{};
 HWND nav[3]{};
 std::vector<HWND> pages[3];
 bool done=false; int page=0; HFONT font{};
};
static HWND control(CfgDlg* d,int page,const wchar_t* cls,const wchar_t* text,DWORD style,int x,int y,int w,int h,int id=0){
 if(!_wcsicmp(cls,L"COMBOBOX"))style|=CBS_OWNERDRAWFIXED|CBS_HASSTRINGS|CBS_NOINTEGRALHEIGHT;
 HWND c=CreateWindowExW(0,cls,text,
  WS_CHILD|WS_VISIBLE|style,S(x),S(y),S(w),S(h),d->hwnd,(HMENU)(INT_PTR)id,g_hinst,nullptr);
 SendMessageW(c,WM_SETFONT,(WPARAM)d->font,TRUE);
 ui::style(c,id==1);
 if(page<0)SetPropW(c,L"JjCanvas",(HANDLE)1);
 if(!_wcsicmp(cls,L"COMBOBOX")){SendMessageW(c,CB_SETITEMHEIGHT,(WPARAM)-1,S(28));SendMessageW(c,CB_SETITEMHEIGHT,0,S(32));}
 if(page>=0)d->pages[page].push_back(c);return c;
}
static void label(CfgDlg*d,int page,const wchar_t*t,int x,int y,int w,int h=25){control(d,page,L"STATIC",t,SS_LEFT,x,y,w,h);}
static HWND edit(CfgDlg*d,int page,int x,int y,int w,int id){return control(d,page,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,x,y,w,28,id);}
static HWND check(CfgDlg*d,int page,const wchar_t*t,int y,int id,bool checked){
 HWND c=control(d,page,L"BUTTON",t,BS_AUTOCHECKBOX|WS_TABSTOP,32,y,610,25,id);
 SendMessageW(c,BM_SETCHECK,checked?BST_CHECKED:BST_UNCHECKED,0);return c;
}
static std::wstring text(HWND h){int n=GetWindowTextLengthW(h);std::wstring s(n+1,L'\0');GetWindowTextW(h,s.data(),n+1);s.resize(n);return s;}
static bool checked(HWND h){return SendMessageW(h,BM_GETCHECK,0,0)==BST_CHECKED;}
static void choose_page(CfgDlg*d){
 // Hide the old page first and repaint the parent before showing the new
 // controls. With WS_CLIPCHILDREN, otherwise stale child/parent pixels can
 // remain under controls that happen to share the same rectangle.
 for(int p=0;p<3;++p)for(auto h:d->pages[p])ShowWindow(h,SW_HIDE);
 RedrawWindow(d->hwnd,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_UPDATENOW);
 for(auto h:d->pages[d->page])ShowWindow(h,SW_SHOW);
 for(int i=0;i<3;++i){if(i==d->page)SetPropW(d->nav[i],L"JjSelected",(HANDLE)1);else RemovePropW(d->nav[i],L"JjSelected");InvalidateRect(d->nav[i],nullptr,FALSE);}
 InvalidateRect(d->hwnd,nullptr,TRUE);
}
static int selected_ocr_engine(CfgDlg*d){
 return d&&d->ocrEngine?(int)SendMessageW(d->ocrEngine,CB_GETCURSEL,0,0):0;
}
static void refresh_download(CfgDlg*d){
 if(selected_ocr_engine(d)==2){
  auto state=ppocr_download_state();
  if(state.busy){
   SetWindowTextW(d->ocrStatus,state.text.c_str());
   SetWindowTextW(d->ocrDownload,L"取消下载");
   EnableWindow(d->ocrDownload,TRUE);
   SendMessageW(d->ocrProgress,PBM_SETPOS,state.percent,0);
  }else if(ppocr_component_ready()){
   SetWindowTextW(d->ocrStatus,ppocr_component_status_text().c_str());
   SetWindowTextW(d->ocrDownload,L"组件已安装");
   EnableWindow(d->ocrDownload,FALSE);
   SendMessageW(d->ocrProgress,PBM_SETPOS,100,0);
  }else{
   SetWindowTextW(d->ocrStatus,state.text.empty()
       ? ppocr_component_status_text().c_str() : state.text.c_str());
   SetWindowTextW(d->ocrDownload,L"下载 OCR 组件和 PP 模型");
   EnableWindow(d->ocrDownload,TRUE);
   SendMessageW(d->ocrProgress,PBM_SETPOS,0,0);
  }
  return;
 }
 EnableWindow(d->ocrDownload,TRUE);
 auto state=hunyuan_download_state();
 std::wstring t=state.text.empty()?hunyuan_model_status_text():state.text;
 SetWindowTextW(d->ocrStatus,t.c_str());
 SetWindowTextW(d->ocrDownload,state.busy?L"取消下载":L"下载 / 校验模型");
 SendMessageW(d->ocrProgress,PBM_SETPOS,state.percent,0);
}
#include "settings_layout.inc"
static std::wstring hotkey_name(HWND h){
 WORD key=(WORD)SendMessageW(h,HKM_GETHOTKEY,0,0);UINT vk=LOBYTE(key);BYTE m=HIBYTE(key);
 std::wstring s;
 if(m&HOTKEYF_CONTROL)s+=L"Ctrl+";if(m&HOTKEYF_ALT)s+=L"Alt+";if(m&HOTKEYF_SHIFT)s+=L"Shift+";
 if(vk>=VK_F1&&vk<=VK_F24)s+=L"F"+std::to_wstring(vk-VK_F1+1);
 else if(vk==VK_SNAPSHOT)s+=L"PrintScreen";
 else if((vk>='A'&&vk<='Z')||(vk>='0'&&vk<='9'))s+=(wchar_t)vk;
 else return {};return s;
}
static LRESULT CALLBACK cfg_proc(HWND h,UINT msg,WPARAM wp,LPARAM lp){
 auto*d=(CfgDlg*)GetWindowLongPtrW(h,GWLP_USERDATA);
 if(msg==WM_CREATE)d=(CfgDlg*)((CREATESTRUCTW*)lp)->lpCreateParams;
 bool scaled=msg==WM_CREATE||msg==WM_PAINT||msg==WM_PRINTCLIENT||
             msg==WM_MEASUREITEM||msg==WM_DRAWITEM;
 UiScaleScope scale(d&&scaled?d->scale:g_scale);
 switch(msg){
 case WM_CREATE:d->hwnd=h;SetWindowLongPtrW(h,GWLP_USERDATA,(LONG_PTR)d);SetPropW(h,L"JjScale",&d->scale);cfg_layout(d);return 0;
 case WM_ERASEBKGND:return 1;
 case WM_PRINTCLIENT:if(d)cfg_paint(d,(HDC)wp);return 0;
 case WM_PAINT:{PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);if(d)cfg_paint(d,dc);EndPaint(h,&ps);return 0;}
 case WM_CTLCOLORSTATIC:case WM_CTLCOLOREDIT:case WM_CTLCOLORBTN:case WM_CTLCOLORLISTBOX:
  return ui::colors(msg,wp,lp);
 case WM_MEASUREITEM:if(lp){auto*m=(MEASUREITEMSTRUCT*)lp;if(m->CtlType==ODT_COMBOBOX){m->itemHeight=S(32);return TRUE;}}break;
 case WM_DRAWITEM:if(ui::draw_item((DRAWITEMSTRUCT*)lp))return TRUE;break;
 case WM_TIMER:if(d)refresh_download(d);return 0;
 case WM_HSCROLL:if(d)SetWindowTextW(d->qualityVal,std::to_wstring(SendMessageW(d->quality,TBM_GETPOS,0,0)).c_str());return 0;
 case WM_COMMAND:{
  if(!d)return 0;int id=LOWORD(wp);
  if(id==2){DestroyWindow(h);return 0;}
  if(id>=201&&id<=203){d->page=id-201;choose_page(d);return 0;}
  if(id==14&&HIWORD(wp)==BN_CLICKED&&!checked(d->ocrEnabled)){
   LogTrace trace;
   ocr_set_enabled(false);
   Settings preview=g_cfg;preview.ocr_enabled=false;
   overlay_apply_display_settings(preview);
   logf(L"识别主开关已即时关闭并卸载模型；保存后保留关闭状态，取消则恢复原开关但不预加载模型");
   return 0;
  }
   if(id==109&&HIWORD(wp)==CBN_SELCHANGE){refresh_download(d);return 0;}
  if(id==10){
   BROWSEINFOW bi{};bi.hwndOwner=h;bi.lpszTitle=L"选择截图保存目录";bi.ulFlags=BIF_RETURNONLYFSDIRS|BIF_NEWDIALOGSTYLE;
   auto pidl=SHBrowseForFolderW(&bi);if(pidl){wchar_t path[MAX_PATH]{};if(SHGetPathFromIDListW(pidl,path))SetWindowTextW(d->saveDir,path);CoTaskMemFree(pidl);}return 0;
  }
  if(id==11){
   if(selected_ocr_engine(d)==2){
    auto state=ppocr_download_state();
    if(state.busy)ppocr_cancel_download();
    else ppocr_download_async();
    refresh_download(d);return 0;
   }
   if(hunyuan_download_state().busy)hunyuan_cancel_download();
   else if(MessageBoxW(h,L"将从 Hugging Face 下载约 1.31 GB 模型并校验 SHA-256。\n已完整下载的文件仅校验，不重复下载。\n模型遵循腾讯混元社区许可（见随附 LICENSE-Hunyuan）。\n是否继续？",L"混元 OCR",MB_YESNO|MB_ICONINFORMATION)==IDYES)hunyuan_download_models_async();
   refresh_download(d);return 0;
  }
  if(id!=1)return 0;
  LogTrace trace;
  logf(L"保存设置请求到达：页面=%d OCR启用=%d 所选引擎=%d",
       d->page, (int)checked(d->ocrEnabled), selected_ocr_engine(d));
  Settings next=g_cfg;next.hotkey_enabled=checked(d->monitor);
  auto hk=hotkey_name(d->hotkey);UINT mods=0,vk=0;
  if(next.hotkey_enabled&&!resolve_hotkey(hk,mods,vk)){logf(L"保存设置被拒绝：全局快捷键格式无效，未写入设置");MessageBoxW(h,L"请录入 Ctrl / Alt 组合键，或 F1–F24 / PrintScreen。",L"快捷键无效",MB_OK|MB_ICONWARNING);return 0;}
  if(!hk.empty())next.hotkey=hk;
  next.save_dir=text(d->saveDir);
  if(next.save_dir.empty())next.save_dir=default_save_dir();
  if(!std::filesystem::path(next.save_dir).is_absolute()){logf(L"保存设置被拒绝：保存目录不是绝对路径 path=%s，未写入设置",next.save_dir.c_str());MessageBoxW(h,L"保存位置必须是绝对路径。",L"保存位置",MB_OK);return 0;}
  std::error_code ec;std::filesystem::create_directories(next.save_dir,ec);
  if(ec){logf(L"保存设置被拒绝：无法创建保存目录 path=%s error=%d，未写入设置",next.save_dir.c_str(),ec.value());MessageBoxW(h,L"无法创建保存目录，请选择有写入权限的文件夹。",L"保存位置",MB_OK);return 0;}
  logf(L"保存目录校验通过：path=%s",next.save_dir.c_str());
  auto fi=SendMessageW(d->format,CB_GETCURSEL,0,0);next.image_format=fi==1?L"jpg":fi==2?L"bmp":L"png";
  next.jpeg_quality=(int)SendMessageW(d->quality,TBM_GETPOS,0,0);
  try{next.stroke_width=std::stoi(text(d->stroke));next.text_size=std::stoi(text(d->fontSize));}catch(...){next.stroke_width=0;}
  if(next.stroke_width<1||next.stroke_width>30||next.text_size<10||next.text_size>72){logf(L"保存设置被拒绝：标注参数无效 线宽=%d 字号=%d，未写入设置",next.stroke_width,next.text_size);MessageBoxW(h,L"线宽范围为 1–30，字号范围为 10–72。",L"标注设置",MB_OK);return 0;}
   next.auto_copy=checked(d->autoCopy);next.include_cursor=checked(d->includeCursor);next.ocr_enabled=checked(d->ocrEnabled);
   next.translate_enabled=checked(d->translateEnabled);
   int engineIndex=(int)SendMessageW(d->ocrEngine,CB_GETCURSEL,0,0);
   next.ocr_engine=engineIndex==2?L"ppocr":engineIndex==1?L"hunyuan_fast":L"hunyuan";
   int backendIndex=(int)SendMessageW(d->ocrBackend,CB_GETCURSEL,0,0);
   LRESULT backend=SendMessageW(d->ocrBackend,CB_GETITEMDATA,backendIndex,0);
   next.ocr_backend=backend==1?L"cpu":backend==2?L"gpu":L"auto";
   int idleIndex=(int)SendMessageW(d->ocrIdle,CB_GETCURSEL,0,0);
   next.ocr_idle_minutes=(int)SendMessageW(d->ocrIdle,CB_GETITEMDATA,idleIndex,0);
   logf(L"设置界面读取空闲回收选项：索引=%d 分钟=%d，保存成功后生效",idleIndex,next.ocr_idle_minutes);
  logf(L"功能设置：识别主开关=%d 翻译按钮=%d；组件在点击功能时检查",
       (int)next.ocr_enabled,(int)next.translate_enabled);
  next.llm_base=text(d->base);next.llm_key=text(d->key);next.llm_model=text(d->model);
  if(next.hotkey_enabled&&!register_preferred(next.hotkey)){logf(L"保存设置被拒绝：快捷键被占用 hotkey=%s，未写入设置",next.hotkey.c_str());MessageBoxW(h,L"此快捷键已被占用，未保存更改。请选择另一个组合键。",L"快捷键冲突",MB_OK|MB_ICONWARNING);return 0;}
  if(!save_settings(next)){logf(L"保存设置返回：设置文件写入失败，恢复原快捷键，未应用更改");UnregisterHotKey(g_msgwnd,HOTKEY_ID);g_hotOk=false;if(g_cfg.hotkey_enabled)register_preferred(g_cfg.hotkey);MessageBoxW(h,L"设置文件写入失败，未应用。",L"保存失败",MB_OK);return 0;}
  g_cfg=next;
  if(g_cfg.ocr_enabled&&!ocr_is_enabled())ocr_set_enabled(true);
  overlay_apply_display_settings(g_cfg);
  if(!g_cfg.hotkey_enabled){UnregisterHotKey(g_msgwnd,HOTKEY_ID);g_hotOk=false;}
  if(checked(d->autostart)!=autostart_enabled()){
   std::wstring error;
   if(!set_autostart(checked(d->autostart),error)){
    logf(L"保存设置返回：截图设置已写入，但管理员开机启动配置失败，保留设置窗口供重试");
    std::wstring message=L"其它设置已保存，但开机启动未配置成功。\n\n"+error;
    MessageBoxW(h,message.c_str(),L"开机启动设置失败",MB_OK|MB_ICONWARNING);return 0;
   }
  }
  logf(L"保存设置返回：成功并应用 保存目录=%s OCR启用=%d 引擎=%s",
       next.save_dir.c_str(),(int)next.ocr_enabled,next.ocr_engine.c_str());
  DestroyWindow(h);return 0;
 }
 case WM_CLOSE:DestroyWindow(h);return 0;
 case WM_DESTROY:if(d){KillTimer(h,60);ocr_set_enabled(g_cfg.ocr_enabled);overlay_apply_display_settings(g_cfg);d->done=true;}return 0;
 case WM_NCDESTROY:RemovePropW(h,L"JjScale");break;
 }return DefWindowProcW(h,msg,wp,lp);
}
void show_settings_dialog(HWND owner){
 if(g_settingsWnd){ShowWindow(g_settingsWnd,SW_SHOW);SetForegroundWindow(g_settingsWnd);return;}
 if(overlay_is_open())return;
 g_cfg=load_settings();
 WNDCLASSEXW wc{};wc.cbSize=sizeof(wc);wc.lpfnWndProc=cfg_proc;wc.hInstance=g_hinst;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
 wc.hbrBackground=ui::canvas_brush();wc.lpszClassName=L"weijieSettings";wc.hIcon=g_iconBig;wc.hIconSm=g_iconSm;RegisterClassExW(&wc);
 RECT work{};SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);
 CfgDlg d;d.scale=std::min(g_scale,std::min((work.bottom-work.top-56)/696.0,(work.right-work.left-40)/872.0));
 RECT rect{0,0,(LONG)lround(840*d.scale),(LONG)lround(648*d.scale)};AdjustWindowRectEx(&rect,WS_CAPTION|WS_SYSMENU|WS_POPUP|WS_CLIPCHILDREN,FALSE,0);
 int w=rect.right-rect.left,h=rect.bottom-rect.top;
 g_settingsWnd=CreateWindowExW(0,L"weijieSettings",L"微截设置",WS_CAPTION|WS_SYSMENU|WS_POPUP|WS_CLIPCHILDREN,
  work.left+(work.right-work.left-w)/2,work.top+(work.bottom-work.top-h)/2,w,h,nullptr,nullptr,g_hinst,&d);
 logf(L"settings create hwnd=%p error=%lu",g_settingsWnd,GetLastError()); if(g_settingsWnd){ShowWindow(g_settingsWnd,SW_SHOW);SetForegroundWindow(g_settingsWnd);
  MSG m;while(!d.done){int result=GetMessageW(&m,nullptr,0,0);if(result<=0){if(!result)PostQuitMessage((int)m.wParam);break;}
   bool settingsMessage=m.hwnd==g_settingsWnd||IsChild(g_settingsWnd,m.hwnd);
   if(!settingsMessage||!IsDialogMessageW(g_settingsWnd,&m)){TranslateMessage(&m);DispatchMessageW(&m);}}
  if(IsWindow(g_settingsWnd))DestroyWindow(g_settingsWnd);
 }
 if(d.font)DeleteObject(d.font);g_settingsWnd=nullptr;
 if(g_cfg.hotkey_enabled&&!g_hotOk)register_preferred(g_cfg.hotkey);
}

void app_open_settings() {
    if (g_msgwnd) PostMessageW(g_msgwnd, WM_JJ_OPEN_SETTINGS, 0, 0);
}

// ------------------------------------------------------------------ 单实例
static bool watch_activate(bool settings) {
    HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, settings ? L"weijie_Settings" : L"weijie_Activate");
    if (!ev) return false;
    SetEvent(ev);
    CloseHandle(ev);
    return true;
}

static void start_watcher() {
 HANDLE events[]={CreateEventW(nullptr,FALSE,FALSE,L"weijie_Activate"),CreateEventW(nullptr,FALSE,FALSE,L"weijie_Settings")};
 g_watch=std::thread([a=events[0],b=events[1]]{
  HANDLE ev[]={a,b};while(!g_watchStop){DWORD r=WaitForMultipleObjects(2,ev,FALSE,300);
   if(r==WAIT_OBJECT_0)PostMessageW(g_msgwnd,WM_ACTIVATE_APP,0,0);
   else if(r==WAIT_OBJECT_0+1)PostMessageW(g_msgwnd,WM_APP+3,0,0);
  }if(a)CloseHandle(a);if(b)CloseHandle(b);
 });
}

// ------------------------------------------------------------------ 消息窗口
static LRESULT CALLBACK msg_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_HOTKEY:
        if (wp == HOTKEY_ID) hotkey_triggered(L"RegisterHotKey");
        return 0;

    case WM_JJ_HOTKEY:
        hotkey_triggered(wp == 2 ? L"Raw Input" :
                         wp == 1 ? L"异步按键轮询" : L"低级键盘钩子",(ULONGLONG)lp);
        return 0;

    case WM_INPUT: {
        RAWINPUT input{};
        UINT size = sizeof(input);
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &input, &size,
                            sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            input.header.dwType == RIM_TYPEKEYBOARD) {
            dispatch_raw_keyboard(input.data.keyboard);
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    case WM_JJ_OPEN_SETTINGS:
        if (!overlay_is_open()) show_settings_dialog(g_msgwnd);
        return 0;

    case WM_TIMER:
        if (wp == TIMER_HOTKEY_POLL) poll_hotkey();
        if (wp == TIMER_OCR_IDLE) ocr_reclaim_idle(GetTickCount64());
        if (wp == TIMER_TRAY_RETRY) tray_try_add();
        return 0;

    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
            show_tray_menu();
        } else if (LOWORD(lp) == WM_LBUTTONDBLCLK) {
            LogTrace trace;
            logf(L"截图请求到达：来源=托盘双击");
            bool opened=overlay_open();
            logf(L"托盘双击截图返回：覆盖层已打开=%d",(int)opened);
        }
        return 0;

    case WM_APP + 4:
        PostMessageW(h,WM_CLOSE,0,0);return 0;

    case WM_APP + 3:
        show_settings_dialog(g_msgwnd);
        return 0;

    case WM_ACTIVATE_APP: {
        LogTrace trace;
        logf(L"截图请求到达：来源=快捷入口或单实例唤起");
        bool opened=overlay_open();
        logf(L"快捷入口截图返回：覆盖层已打开=%d",(int)opened);
        return 0;
    }

    case WM_CLOSE:
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        tray_remove();
        KillTimer(h, TIMER_HOTKEY_POLL);
        KillTimer(h, TIMER_OCR_IDLE);
        PostQuitMessage(0);
        return 0;
    }

    if (g_taskbarCreated && msg == g_taskbarCreated && !g_trayStopping) {
        g_trayRegistered = false;
        tray_add(L"任务栏创建或恢复通知");
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static HWND create_message_window(HINSTANCE hinst) {
    LogTrace trace;
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    // 隐藏顶层工具窗口接收 Shell 广播，不显示任务栏按钮，也不抢占焦点。
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, WND_CLASS, APP_NAME, WS_POPUP,
                                 0, 0, 0, 0, nullptr, nullptr, hinst, nullptr);
    if (!window) {
        logf(L"后台消息窗口创建失败：error=%lu", GetLastError());
        return nullptr;
    }
    if (!g_taskbarCreated)
        logf(L"任务栏恢复消息注册失败：error=%lu", GetLastError());
    else if (!ChangeWindowMessageFilterEx(window, g_taskbarCreated, MSGFLT_ALLOW, nullptr))
        logf(L"任务栏恢复消息权限配置失败：error=%lu", GetLastError());
    else
        logf(L"任务栏恢复接收已就绪：隐藏顶层窗口=%p，允许普通权限 Explorer 通知管理员实例", window);
    if (!ChangeWindowMessageFilterEx(window, WM_TRAY, MSGFLT_ALLOW, nullptr))
        logf(L"托盘鼠标消息权限配置失败：error=%lu", GetLastError());
    else
        logf(L"托盘鼠标消息接收已就绪：允许 Explorer 回传点击事件");
    return window;
}

// ------------------------------------------------------------------ 入口
static void init_dpi() {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        typedef BOOL(WINAPI * SetCtx)(void*);
        auto fn = (SetCtx)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        if (fn && fn((void*)-4)) {   // PER_MONITOR_AWARE_V2
            // ok
        } else {
            SetProcessDPIAware();
        }
    }
    HDC dc = GetDC(nullptr);
    g_scale = GetDeviceCaps(dc, LOGPIXELSX) / 96.0;
    ReleaseDC(nullptr, dc);
    if (g_scale < 1.0) g_scale = 1.0;
    logf(L"DPI scale = %.3f", g_scale);
}

// 兜底：真崩了至少把异常码和地址写进日志，方便下次定位
static LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep) {
    if (ep && ep->ExceptionRecord) {
        logf(L"程序异常 code=0x%08X addr=%p",
             (unsigned)ep->ExceptionRecord->ExceptionCode,
             ep->ExceptionRecord->ExceptionAddress);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, LPWSTR, int) {
    g_hinst = hinst;
    const bool auto_capture = wcsstr(GetCommandLineW(), L"--capture") != nullptr;
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES | ICC_TAB_CLASSES | ICC_HOTKEY_CLASS };
    InitCommonControlsEx(&icc);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HRESULT comSecurity=CoInitializeSecurity(nullptr,-1,nullptr,nullptr,
        RPC_C_AUTHN_LEVEL_PKT_PRIVACY,RPC_C_IMP_LEVEL_IMPERSONATE,nullptr,EOAC_NONE,nullptr);
    if(FAILED(comSecurity)&&comSecurity!=RPC_E_TOO_LATE)
        logf(L"初始化任务计划 COM 安全上下文失败 HRESULT=0x%08X",(unsigned)comSecurity);
    SetUnhandledExceptionFilter(crash_filter);

    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartup(&token, &gsi, nullptr);

    // 单实例
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"weijie_SingleInstance");
    DWORD mutexError = GetLastError();
    if (!mutex) {
        logf(L"启动被拒绝：无法取得单实例锁 error=%lu，未创建第二个微截实例", mutexError);
        MessageBoxW(nullptr, mutexError == ERROR_ACCESS_DENIED
            ? L"微截可能已以管理员身份运行，请从系统托盘使用或退出该实例。"
            : L"无法创建微截单实例锁，请稍后重试。",
            APP_NAME, MB_OK | MB_ICONWARNING);
        Gdiplus::GdiplusShutdown(token);
        return 1;
    }
    bool existingInstance = mutexError == ERROR_ALREADY_EXISTS;
    if (existingInstance && wcsstr(GetCommandLineW(), L"--elevated-restart") &&
        process_is_elevated(GetCurrentProcess())) {
        HWND existing = FindWindowExW(nullptr, nullptr, WND_CLASS, nullptr);
        logf(L"管理员实例接管开始：通知旧实例正常退出 hwnd=%p", existing);
        if (existing) PostMessageW(existing, WM_APP + 4, 0, 0);
        DWORD wait = WaitForSingleObject(mutex, 10000);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
            logf(L"管理员实例接管失败：旧实例未退出 wait=%lu，未强制终止旧进程", wait);
            MessageBoxW(nullptr, L"旧版微截尚未退出，请从托盘退出后重新以管理员身份运行。",
                        APP_NAME, MB_OK | MB_ICONWARNING);
            CloseHandle(mutex);
            Gdiplus::GdiplusShutdown(token);
            return 1;
        }
        existingInstance = false;
        logf(L"管理员实例接管完成：已取得单实例锁");
    }
    if (existingInstance) {
        if(wcsstr(GetCommandLineW(),L"--quit")){
            HWND existing=FindWindowExW(nullptr,nullptr,WND_CLASS,nullptr);
            if(existing)PostMessageW(existing,WM_APP+4,0,0);
            CloseHandle(mutex);Gdiplus::GdiplusShutdown(token);return 0;
        }
        if (watch_activate(wcsstr(GetCommandLineW(), L"--settings") != nullptr)) {
            CloseHandle(mutex);
            Gdiplus::GdiplusShutdown(token);
            return 0;
        }
        MessageBoxW(nullptr, L"微截已经在运行了，请查看系统托盘。", APP_NAME, MB_OK | MB_ICONINFORMATION);
        CloseHandle(mutex);
        Gdiplus::GdiplusShutdown(token);
        return 0;
    }

    if(wcsstr(GetCommandLineW(),L"--quit")){if(mutex)CloseHandle(mutex);Gdiplus::GdiplusShutdown(token);return 0;}
    init_dpi();
    g_cfg = load_settings();
    ocr_set_enabled(g_cfg.ocr_enabled);
    ocr_set_idle_timeout(g_cfg.ocr_idle_minutes);
    logf(L"启动 %s，exe=%s", APP_VERSION, exe_path().c_str());
    logf(L"运行权限：管理员=%d", (int)process_is_elevated(GetCurrentProcess()));
    logf(L"启动来源：%s",wcsstr(GetCommandLineW(),L"--autostart")?L"管理员登录任务":L"手动启动");
    std::wstring startupError;
    if(!migrate_legacy_autostart(startupError))
        MessageBoxW(nullptr,startupError.c_str(),L"开机启动升级失败",MB_OK|MB_ICONWARNING);

    // 注册消息窗口类
    g_iconBig = load_icon(GetSystemMetrics(SM_CXICON));
    g_iconSm = load_icon(GetSystemMetrics(SM_CXSMICON));

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = msg_proc;
    wc.hInstance = hinst;
    wc.lpszClassName = WND_CLASS;
    wc.hIcon = g_iconBig;
    wc.hIconSm = g_iconSm;
    RegisterClassExW(&wc);

    g_msgwnd = create_message_window(hinst);
    if (!g_msgwnd) return 1;

    g_icon = g_iconSm ? g_iconSm : make_icon(GetSystemMetrics(SM_CXSMICON));
    tray_add();
    if (capture_requires_elevation(GetForegroundWindow())) app_game_permission_hint();

    install_keyboard_hook();
    install_raw_keyboard();
    SetTimer(g_msgwnd, TIMER_HOTKEY_POLL, 30, nullptr);
    SetTimer(g_msgwnd, TIMER_OCR_IDLE, 1000, nullptr);
    if (g_cfg.hotkey_enabled && !register_preferred(g_cfg.hotkey)) {
        MessageBoxW(nullptr,L"截图快捷键被占用，请在设置中更换。",APP_NAME,MB_OK|MB_ICONWARNING);
    }
    start_watcher();

    if (auto_capture) {
        PostMessageW(g_msgwnd, WM_ACTIVATE_APP, 0, 0);
    }

    // 调试/快捷入口：微截.exe --settings 直接打开设置
    if (wcsstr(GetCommandLineW(), L"--settings")) {
        logf(L"settings requested");
        show_settings_dialog(g_msgwnd);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 收尾
    g_watchStop.store(true);
    if (g_watch.joinable()) g_watch.join();
    ppocr_download_shutdown();
    hunyuan_download_shutdown();
    ocr_set_enabled(false);
    fast_ocr_shutdown();
    if (g_kbHook) { UnhookWindowsHookEx(g_kbHook); g_kbHook = nullptr; }
    RAWINPUTDEVICE rawRemove{0x01, 0x06, RIDEV_REMOVE, nullptr};
    RegisterRawInputDevices(&rawRemove, 1, sizeof(rawRemove));
    tray_remove();
    longshot_close_previews();
    pin_close_all();
    overlay_destroy();
    if (g_icon) DestroyIcon(g_icon);
    if (mutex) CloseHandle(mutex);
    Gdiplus::GdiplusShutdown(token);
    return 0;
}









