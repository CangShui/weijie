static bool test_tray_lifecycle(){
    LogTrace trace;bool passed=true;int checks=0;
    auto report=[&](bool ok,const char* name){
        ++checks;passed=passed&&ok;
        std::cout<<(ok?"PASS ":"FAIL ")<<name<<std::endl;
        logf(L"托盘生命周期回归：%s 成功=%d",u8_to_w(name).c_str(),(int)ok);
    };
    HWND previousWindow=g_msgwnd;HICON previousIcon=g_icon;
    auto previousClass=WND_CLASS;WND_CLASS=L"weijieTrayPreview";
    WNDCLASSW windowClass{};windowClass.lpfnWndProc=msg_proc;
    windowClass.hInstance=g_hinst;windowClass.lpszClassName=WND_CLASS;
    RegisterClassW(&windowClass);
    g_msgwnd=create_message_window(g_hinst);
    if(!g_msgwnd)return false;
    g_icon=LoadIconW(nullptr,IDI_APPLICATION);
    trayMock=true;trayShellReady=false;trayShellIcon=false;
    trayAdds=trayModifies=trayDeletes=trayTimers=trayTimerKills=0;
    g_trayStopping=g_trayRegistered=false;g_trayTrace=0;
    bool enumerated=false;
    EnumWindows([](HWND window,LPARAM param)->BOOL{
        if(window==g_msgwnd)*reinterpret_cast<bool*>(param)=true;
        return TRUE;
    },reinterpret_cast<LPARAM>(&enumerated));
    report(enumerated&&!IsWindowVisible(g_msgwnd)&&
           (GetWindowLongW(g_msgwnd,GWL_EXSTYLE)&WS_EX_TOOLWINDOW)&&
           FindWindowExW(nullptr,nullptr,WND_CLASS,nullptr)==g_msgwnd,
           "tray owner is a hidden top-level tool window discoverable by singleton quit");
    report(g_taskbarCreated!=0,"TaskbarCreated registered for shell broadcasts");
    tray_add();
    auto registrationTrace=g_trayTrace;
    report(!g_trayRegistered&&trayAdds==1&&trayModifies==1&&trayTimers==1,
           "startup before Explorer schedules retry without claiming success");
    SendMessageW(g_msgwnd,WM_TIMER,TIMER_TRAY_RETRY,0);
    report(!g_trayRegistered&&g_trayAttempts==2&&g_trayTrace==registrationTrace,
           "failed retry keeps waiting with the same operation trace");
    trayShellReady=true;
    SendMessageW(g_msgwnd,WM_TIMER,TIMER_TRAY_RETRY,0);
    report(g_trayRegistered&&trayShellIcon&&trayAdds==3&&trayTimerKills==1,
           "late Explorer registration succeeds and cancels retry timer");
    unsigned adds=trayAdds,timers=trayTimers;
    tray_add();SendMessageW(g_msgwnd,WM_TIMER,TIMER_TRAY_RETRY,0);
    report(trayAdds==adds&&trayTimers==timers,"registered icon ignores repeated add and stale timer");
    int captures=captureRequests;
    SendMessageW(g_msgwnd,WM_TRAY,1,WM_LBUTTONDBLCLK);
    report(captureRequests==captures+1,"legacy tray double-click still starts capture");
    trayShellIcon=false;
    SendMessageW(g_msgwnd,g_taskbarCreated,0,0);
    report(g_trayRegistered&&trayShellIcon&&trayAdds==adds+1,
           "TaskbarCreated restores icon after simulated Explorer recreation");
    unsigned modifies=trayModifies;
    SendMessageW(g_msgwnd,g_taskbarCreated,0,0);
    report(g_trayRegistered&&trayShellIcon&&trayModifies==modifies+1,
           "repeated shell message updates existing icon instead of duplicating it");
    trayShellReady=false;trayShellIcon=false;
    SendMessageW(g_msgwnd,g_taskbarCreated,0,0);
    report(!g_trayRegistered,"shell restoration failure resumes retry");
    unsigned kills=trayTimerKills;
    tray_remove();adds=trayAdds;
    SendMessageW(g_msgwnd,WM_TIMER,TIMER_TRAY_RETRY,0);
    SendMessageW(g_msgwnd,g_taskbarCreated,0,0);
    report(g_trayStopping&&trayTimerKills==kills+1&&trayAdds==adds,
           "shutdown cancels pending retry and ignores late shell and timer messages");
    // 再次启动并销毁真实窗口，验证 WM_DESTROY 在窗口仍有效时移除图标。
    g_trayStopping=false;trayShellReady=true;
    tray_add();unsigned deletes=trayDeletes;
    DestroyWindow(g_msgwnd);
    report(trayDeletes==deletes+1&&!trayShellIcon&&g_trayStopping,
           "owner destruction removes registered icon exactly once");
    tray_remove();
    report(trayDeletes==deletes+1,"final cleanup does not remove icon twice");
    MSG quit{};PeekMessageW(&quit,nullptr,WM_QUIT,WM_QUIT,PM_REMOVE);
    trayMock=false;g_msgwnd=previousWindow;g_icon=previousIcon;
    g_trayRegistered=g_trayStopping=false;g_trayTrace=0;g_nid={};
    UnregisterClassW(WND_CLASS,g_hinst);
    WND_CLASS=previousClass;
    std::cout<<"Tray lifecycle checks: "<<checks<<std::endl;
    return passed;
}
