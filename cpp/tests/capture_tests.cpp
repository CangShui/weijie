// 实屏回归：原始键盘包走实际分发入口，前台为隐藏并限位鼠标的 D3D11 窗口。
#define wWinMain weijieUnusedEntry
#include "../src/main.cpp"
#undef wWinMain
#include <d3d11.h>
#include <wrl/client.h>
#include <iostream>
#include <functional>

using Microsoft::WRL::ComPtr;
static int passed = 0, failed = 0;
static HWND test_overlay() {
    HWND found = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM value) -> BOOL {
        wchar_t name[64]{};
        GetClassNameW(window, name, 64);
        if (wcscmp(name, L"weijieOverlay") != 0) return TRUE;
        *(HWND*)value = window;
        return FALSE;
    }, (LPARAM)&found);
    return found;
}
static void check_capture(bool ok, const char* name) {
    std::cout << (ok ? "PASS " : "FAIL ") << name << std::endl;
    logf(L"实屏回归：%s 结果=%s", u8_to_w(name).c_str(), ok ? L"通过" : L"失败");
    ok ? ++passed : ++failed;
}
static bool pump_until(const std::function<bool()>& condition, DWORD timeout = 4000) {
    ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < timeout) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (condition()) return true;
        Sleep(10);
    }
    return false;
}

static LRESULT CALLBACK game_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wp, lp);
}

static int settingsPhase=0;
static ULONGLONG settingsStarted=0;
static void CALLBACK capture_settings(HWND,UINT,UINT_PTR,DWORD){
    if(!g_settingsWnd)return;
    auto* cfg=(CfgDlg*)GetWindowLongPtrW(g_settingsWnd,GWLP_USERDATA);
    if(settingsPhase==0){
        SetFocus(cfg->saveDir);
        SetWindowTextW(cfg->saveDir,L"C:\\尚未保存的截图目录");
        RedrawWindow(g_settingsWnd,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_UPDATENOW);
        g_rawKeys=RawHotkeyState{};g_lastHotkeyTick=0;
        RAWKEYBOARD alt{};alt.VKey=VK_MENU;
        RAWKEYBOARD key{};key.VKey=VK_F9;
        dispatch_raw_keyboard(alt);dispatch_raw_keyboard(key);
        key.Flags=RI_KEY_BREAK;dispatch_raw_keyboard(key);
        alt.Flags=RI_KEY_BREAK;dispatch_raw_keyboard(alt);
        settingsStarted=GetTickCount64();settingsPhase=1;
    }else if(settingsPhase==1&&overlay_is_open()){
        HWND overlay=test_overlay();
        auto* state=(Overlay*)GetWindowLongPtrW(overlay,GWLP_USERDATA);
        check_capture(state&&GetForegroundWindow()==overlay,
                      "capture hotkey opens real overlay while settings is open");
        check_capture(text(cfg->saveDir)==L"C:\\尚未保存的截图目录",
                      "capture does not discard unsaved settings");
        POINT logo{(LONG)lround(40*cfg->scale),(LONG)lround(42*cfg->scale)};
        ClientToScreen(g_settingsWnd,&logo);
        Gdiplus::Color pixel;
        bool inside=state&&logo.x>=state->shot.vx&&logo.y>=state->shot.vy&&
            logo.x<state->shot.vx+state->shot.vw&&logo.y<state->shot.vy+state->shot.vh;
        bool captured=inside&&state->shot.bmp->GetPixel(logo.x-state->shot.vx,
            logo.y-state->shot.vy,&pixel)==Gdiplus::Ok;
        check_capture(captured&&pixel.GetG()>140&&pixel.GetR()<70&&pixel.GetB()<140,
                      "frozen screenshot includes visible settings UI");
        PostMessageW(overlay,WM_KEYDOWN,VK_ESCAPE,0);
        settingsPhase=2;
    }else if(settingsPhase==2&&!overlay_is_open()){
        check_capture(IsWindow(g_settingsWnd)&&text(cfg->saveDir)==L"C:\\尚未保存的截图目录",
                      "Escape closes capture instead of closing settings");
        settingsPhase=3;
        DestroyWindow(g_settingsWnd);
    }else if(GetTickCount64()-settingsStarted>5000){
        check_capture(false,"settings screenshot completes without swallowing overlay input");
        overlay_destroy();
        DestroyWindow(g_settingsWnd);
    }
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    std::filesystem::create_directories(argv[1]);
    SetEnvironmentVariableW(L"APPDATA", argv[1]);
    SetEnvironmentVariableW(L"LOCALAPPDATA", argv[1]);
    LogTrace trace;
    g_hinst = GetModuleHandleW(nullptr);
    init_dpi();
    Gdiplus::GdiplusStartupInput startup;
    ULONG_PTR token;
    Gdiplus::GdiplusStartup(&token, &startup, nullptr);
    g_cfg.ocr_enabled = false;
    g_cfg.translate_enabled = false;
    g_cfg.auto_copy = false;
    g_cfg.image_format = L"png";
    g_cfg.save_dir = std::wstring(argv[1]) + L"\\保存测试-" + std::to_wstring(GetCurrentProcessId());
    save_settings(g_cfg);
    if(argc>2&&wcscmp(argv[2],L"--benchmark")==0){
        std::vector<ULONGLONG> timings;
        for(int i=0;i<6;++i){
            auto started=GetTickCount64();Shot shot;
            bool ok=capture_desktop(shot);
            auto elapsed=GetTickCount64()-started;free_shot(shot);
            check_capture(ok,"benchmark captures fresh desktop without opening UI or saving screen pixels");
            logf(L"抓屏性能样本：序号=%d 冷启动=%d 耗时=%llums",i+1,(int)(i==0),elapsed);
            std::cout<<"CAPTURE_SAMPLE "<<i+1<<" "<<elapsed<<"ms"<<std::endl;
            if(i)timings.push_back(elapsed);
        }
        std::sort(timings.begin(),timings.end());
        std::cout<<"CAPTURE_WARM_MEDIAN "<<timings[timings.size()/2]<<"ms"<<std::endl;
        Gdiplus::GdiplusShutdown(token);return failed?1:0;
    }
    if (capture_requires_elevation(GetForegroundWindow())) {
        check_capture(!overlay_open() && !overlay_is_open(),
                      "higher-integrity foreground requests elevation instead of an unusable overlay");
        Shot desktop;
        check_capture(capture_desktop(desktop), "capture current elevated D3D desktop without input handoff");
        if (desktop.bmp) {
            save_bitmap_file(desktop.bmp, std::wstring(argv[1]) + L"\\desktop.png", 92);
            free_shot(desktop);
        }
        std::cout << "SKIP foreground/input tests: elevated foreground requires an elevated test runner."
                  << "\nTOTAL " << passed << " passed, " << failed << " failed, 1 skipped" << std::endl;
        return failed ? 1 : 0;
    }

    RECT originalClip{};
    GetClipCursor(&originalClip);
    POINT originalCursor{};
    GetCursorPos(&originalCursor);
    HWND originalForeground = GetForegroundWindow();
    WNDCLASSW messages{};
    messages.lpfnWndProc = msg_proc;
    messages.hInstance = g_hinst;
    messages.lpszClassName = WND_CLASS;
    RegisterClassW(&messages);
    g_msgwnd = CreateWindowExW(0, WND_CLASS, L"capture test", 0, 0, 0, 0, 0,
                               HWND_MESSAGE, nullptr, g_hinst, nullptr);
    // 不安装系统热键、低级钩子或轮询定时器，成功触发只能来自 Raw Input。
    g_hotOk = true;
    g_hotMods = MOD_ALT;
    g_hotVk = VK_F9;
    g_hotName = L"Alt+F9";
    install_raw_keyboard();
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<HWND> gameWindow{nullptr};
    std::atomic<bool> d3dReady{false};
    std::atomic<bool> blueFrame{false};
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor);
    RECT gameRect = monitor.rcMonitor;
    RECT gameClip{80, 80, 400, 320};
    std::thread game([&] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = game_proc;
        wc.hInstance = g_hinst;
        wc.lpszClassName = L"weijieD3DTest";
        RegisterClassW(&wc);
        HWND window = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"D3D test",
            WS_POPUP, gameRect.left, gameRect.top,
            gameRect.right - gameRect.left, gameRect.bottom - gameRect.top,
            nullptr, nullptr, g_hinst, nullptr);
        gameWindow.store(window);
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferDesc.Width = gameRect.right - gameRect.left;
        desc.BufferDesc.Height = gameRect.bottom - gameRect.top;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.OutputWindow = window;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        ComPtr<IDXGISwapChain> swapchain;
        HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE,
            nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc, &swapchain,
            &device, nullptr, &context);
        ComPtr<ID3D11Texture2D> backbuffer;
        ComPtr<ID3D11RenderTargetView> target;
        OverlayInput foreground;
        int cursorHides = 0;
        if (SUCCEEDED(hr)) hr = swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
        if (SUCCEEDED(hr)) hr = device->CreateRenderTargetView(backbuffer.Get(), nullptr, &target);
        if (SUCCEEDED(hr)) {
            ShowWindow(window, SW_SHOW);
            if (!foreground.acquire(window)) hr = E_FAIL;
        }
        if (SUCCEEDED(hr)) {
            ClipCursor(&gameClip);
            int count;
            do { count = ShowCursor(FALSE); ++cursorHides; } while (count >= 0);
            d3dReady.store(true);
        }
        SetEvent(ready);
        if (SUCCEEDED(hr)) {
            MSG msg{};
            bool running = true;
            while (running) {
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    if (msg.message == WM_QUIT) { running = false; break; }
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                if (!running) break;
                const float color[]{blueFrame?0.1f:0.8f,0.2f,blueFrame?0.8f:0.1f,1.0f};
                context->ClearRenderTargetView(target.Get(), color);
                swapchain->Present(1, 0);
                Sleep(10);
            }
            while (cursorHides-- > 0) ShowCursor(TRUE);
            foreground.release();
            ClipCursor(nullptr);
        } else if (window) {
            DestroyWindow(window);
        }
    });
    WaitForSingleObject(ready, 5000);
    bool readyOk = d3dReady.load();
    check_capture(readyOk, "D3D11 foreground test window created");
    if (readyOk) {
        HWND window = gameWindow.load();
        check_capture(GetForegroundWindow() == window, "D3D window owns foreground before capture");
        // 让 DWM 显示至少一帧，再按实际组合键。
        ULONGLONG start = GetTickCount64();
        pump_until([&] { return GetTickCount64() - start > 300; });
        RECT clip{};
        GetClipCursor(&clip);
        check_capture(EqualRect(&clip, &gameClip), "game constrains cursor before capture");
        // 将原始键盘包送入实际分发入口，不向正在运行的其它应用注入按键。
        g_rawKeys = RawHotkeyState{};
        RAWKEYBOARD alt{};alt.VKey=VK_MENU;
        RAWKEYBOARD key{};key.VKey=VK_F9;
        dispatch_raw_keyboard(alt);
        dispatch_raw_keyboard(key);
        key.Flags=RI_KEY_BREAK;dispatch_raw_keyboard(key);
        alt.Flags=RI_KEY_BREAK;dispatch_raw_keyboard(alt);
        bool opened = pump_until([] { return overlay_is_open(); });
        check_capture(opened, "raw keyboard dispatch opens screenshot overlay");
        if (opened) {
            HWND overlay = test_overlay();
            auto* state = (Overlay*)GetWindowLongPtrW(overlay, GWLP_USERDATA);
            check_capture(state && GetForegroundWindow() == overlay, "overlay takes foreground from D3D thread");
            GetClipCursor(&clip);
            RECT desktop{state->shot.vx, state->shot.vy,
                         state->shot.vx + state->shot.vw, state->shot.vy + state->shot.vh};
            check_capture(EqualRect(&clip, &desktop), "overlay releases game cursor clipping");
            CURSORINFO cursor{sizeof(cursor)};
            check_capture(GetCursorInfo(&cursor) && (cursor.flags & CURSOR_SHOWING),
                          "overlay cursor is visible");
            int x = 240 - state->shot.vx, y = 200 - state->shot.vy;
            BYTE* pixel = (BYTE*)state->shot.bits + ((size_t)y * state->shot.vw + x) * 4;
            check_capture(pixel[2] > 150 && pixel[1] < 100 && pixel[0] < 80,
                          "frozen frame contains D3D color before focus change");
            unsigned long long first = state->traceId;
            hotkey_triggered(L"回归测试重复输入");
            check_capture(test_overlay() == overlay &&
                          state->traceId == first, "duplicate input keeps a single overlay");
            POINT from{160 - state->shot.vx, 100 - state->shot.vy};
            POINT to{360 - state->shot.vx, 260 - state->shot.vy};
            SendMessageW(overlay, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(from.x, from.y));
            SendMessageW(overlay, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(to.x, to.y));
            SendMessageW(overlay, WM_LBUTTONUP, 0, MAKELPARAM(to.x, to.y));
            check_capture(state->hasSel && state->tbVisible &&
                          state->sel.right - state->sel.left == 200 &&
                          state->sel.bottom - state->sel.top == 160,
                          "borderless D3D capture produces selected region and toolbar");
            bool clickedSave = false;
            for (const auto& item : state->items) {
                if (item.id != T_SAVE) continue;
                int sx = (int)(state->tbX + item.rect.X + item.rect.Width / 2);
                int sy = (int)(state->tbY + item.rect.Y + item.rect.Height / 2);
                SendMessageW(overlay, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(sx, sy));
                clickedSave = true;
                break;
            }
            check_capture(clickedSave, "click configured-directory save button");
            check_capture(pump_until([] { return !overlay_is_open(); }),
                          "successful save closes screenshot overlay");
            std::vector<std::filesystem::path> saved;
            if (std::filesystem::is_directory(g_cfg.save_dir)) {
                for (const auto& file : std::filesystem::directory_iterator(g_cfg.save_dir))
                    if (file.path().extension() == L".png") saved.push_back(file.path());
            }
            check_capture(saved.size() == 1, "toolbar saves one PNG in configured Unicode directory");
            if (saved.size() == 1) {
                Gdiplus::Bitmap image(saved.front().c_str());
                Gdiplus::Color color;
                bool decoded = image.GetLastStatus() == Gdiplus::Ok &&
                    image.GetWidth() == 200 && image.GetHeight() == 160 &&
                    image.GetPixel(100, 80, &color) == Gdiplus::Ok;
                check_capture(decoded && color.GetR() > 150 && color.GetG() < 100 &&
                              color.GetB() < 80, "saved PNG contains selected D3D pixels");
                std::cout << "Saved image: " << w_to_u8(saved.front().wstring()) << std::endl;
            }
            if (!overlay_is_open()) {
                blueFrame=true;
                auto changed=GetTickCount64();
                pump_until([&]{return GetTickCount64()-changed>100;});
                SetForegroundWindow(window);
                SendMessageW(g_msgwnd,WM_TRAY,1,WM_LBUTTONDBLCLK);
                check_capture(overlay_is_open(), "tray double click reopens screenshot after saving");
                HWND current=test_overlay();
                auto* fresh=(Overlay*)GetWindowLongPtrW(current,GWLP_USERDATA);
                Gdiplus::Color color;
                bool sampled=fresh&&fresh->shot.bmp->GetPixel(240-fresh->shot.vx,200-fresh->shot.vy,&color)==Gdiplus::Ok;
                check_capture(sampled&&color.GetB()>150&&color.GetR()<80,
                    "reused graphics device captures new D3D pixels instead of previous screenshot");
            }
            overlay = test_overlay();
            if (overlay) {
                PostMessageW(overlay, WM_KEYDOWN, VK_ESCAPE, 0);
                check_capture(pump_until([] { return !overlay_is_open(); }),
                              "Escape closes overlay after cursor handoff");
            }
            ShowWindow(window,SW_HIDE);
            settingsPhase=0;
            UINT_PTR timer=SetTimer(nullptr,0,50,capture_settings);
            if(timer){
                show_settings_dialog(g_msgwnd);
                KillTimer(nullptr,timer);
                check_capture(settingsPhase==3,"settings capture workflow completes");
            }else{
                check_capture(false,"start settings capture timer");
            }
        }
    }
    overlay_destroy();
    if (gameWindow.load() && IsWindow(gameWindow.load()))
        PostMessageW(gameWindow.load(), WM_CLOSE, 0, 0);
    game.join();
    CloseHandle(ready);
    RAWINPUTDEVICE remove{0x01, 0x06, RIDEV_REMOVE, nullptr};
    RegisterRawInputDevices(&remove, 1, sizeof(remove));
    DestroyWindow(g_msgwnd);
    ClipCursor(&originalClip);
    SetCursorPos(originalCursor.x, originalCursor.y);
    if (originalForeground) SetForegroundWindow(originalForeground);
    std::cout << "TOTAL " << passed << " passed, " << failed << " failed" << std::endl;
    return failed ? 1 : 0;
}
