// Native UI regression renderer: real controls and WndProcs, no mouse injection.
#include "../src/common.h"
#include "../src/overlay.h"
#include "../src/ocr.h"
#include "../src/translate.h"
#include "../src/output.h"
static std::wstring imageTestExe;
static std::wstring preview_images_exe_path(){return imageTestExe.empty()?exe_path():imageTestExe;}
#define exe_path preview_images_exe_path
#include "../src/emoji_renderer.cpp"
#undef exe_path
// 组件状态和设置提示由测试控制，不修改本机模型或等待人工确认。
static int readinessChecks[3]{};
static bool componentReady=false;
static int ocrWarnings=0,unexpectedWarnings=0;
static int captureRequests=0;
static int animationSaveDecision=IDYES,animationSavePrompts=0;
static bool trayMock=false,trayShellReady=false,trayShellIcon=false;
static unsigned trayAdds=0,trayModifies=0,trayDeletes=0,trayTimers=0,trayTimerKills=0;
static BOOL WINAPI preview_notify_icon(DWORD action,PNOTIFYICONDATAW data){
    if(!trayMock)return Shell_NotifyIconW(action,data);
    if(action==NIM_ADD){++trayAdds;if(!trayShellReady||trayShellIcon)return FALSE;trayShellIcon=true;return TRUE;}
    if(action==NIM_MODIFY){++trayModifies;return trayShellReady&&trayShellIcon;}
    if(action==NIM_DELETE){++trayDeletes;trayShellIcon=false;return TRUE;}
    return FALSE;
}
static UINT_PTR WINAPI preview_tray_set_timer(HWND window,UINT_PTR id,UINT delay,TIMERPROC callback){
    if(trayMock)++trayTimers;
    return SetTimer(window,id,delay,callback);
}
static BOOL WINAPI preview_tray_kill_timer(HWND window,UINT_PTR id){
    if(trayMock)++trayTimerKills;
    return KillTimer(window,id);
}
static bool preview_capture_request(){++captureRequests;logf(L"设置回归：截图请求已进入覆盖层入口");return true;}
static int WINAPI preview_settings_message_box(HWND,LPCWSTR message,LPCWSTR title,UINT){
    if(title&&!wcscmp(title,L"保存动态图片")){
        ++animationSavePrompts;logf(L"动画保存回归选择：结果=%d",animationSaveDecision);return animationSaveDecision;
    }
    if(title&&!wcscmp(title,L"保存截图")&&message&&wcsstr(message,L"是否覆盖"))
        return IDYES;
    if(title&&!wcscmp(title,L"OCR 设置"))++ocrWarnings;
    else ++unexpectedWarnings;
    logf(L"设置回归提示：标题=%s 内容=%s",title?title:L"",message?message:L"");
    return IDOK;
}
#define hunyuan_model_ready preview_hunyuan_model_ready
#define hunyuan_fast_model_ready preview_hunyuan_fast_model_ready
#define fast_ocr_available preview_fast_ocr_available
#define MessageBoxW preview_settings_message_box
#define overlay_open preview_capture_request
#define wWinMain weijieUnusedEntry
#define Shell_NotifyIconW preview_notify_icon
#define SetTimer preview_tray_set_timer
#define KillTimer preview_tray_kill_timer
#include "../src/main.cpp"
#undef KillTimer
#undef SetTimer
#undef Shell_NotifyIconW
#undef wWinMain
#undef hunyuan_model_ready
#undef hunyuan_fast_model_ready
#undef fast_ocr_available
#undef MessageBoxW
#undef overlay_open
static BOOL WINAPI preview_style_show_window(HWND,int){return TRUE;}
#define ShowWindow preview_style_show_window
#include "../src/style_popup.cpp"
#undef ShowWindow
#define ocr_recognize preview_ocr_recognize
#define translate_text preview_translate_text
static bool preview_ocr_recognize(Gdiplus::Bitmap*,std::wstring& text,std::wstring& error,const std::atomic<bool>*);
static TranslateResult preview_translate_text(const std::wstring&,const Settings&);
static HWND stampTestForeground=nullptr;
static HWND WINAPI preview_foreground_window(){
    return stampTestForeground?stampTestForeground:GetForegroundWindow();
}
static int overlayCopies=0;
static bool preview_overlay_copy(Gdiplus::Bitmap*){++overlayCopies;return true;}
#define GetForegroundWindow preview_foreground_window
#define MessageBoxW preview_settings_message_box
#define copy_bitmap_to_clipboard preview_overlay_copy
#include "../src/overlay.cpp"
#undef copy_bitmap_to_clipboard
#undef MessageBoxW
#undef GetForegroundWindow
#undef ocr_recognize
#undef translate_text
#include <iostream>
#include <fstream>

static int fileDialogCalls=0,longshotCopies=0;
static bool fileDialogCancelled=false,longshotCopyOk=true;
static DWORD fileDialogFilter=1;
static std::wstring fileDialogTarget,fileDialogInitial;
static BOOL WINAPI preview_file_dialog(OPENFILENAMEW* dialog){
    ++fileDialogCalls;
    fileDialogInitial=dialog->lpstrInitialDir?dialog->lpstrInitialDir:L"";
    if(fileDialogCancelled)return FALSE;
    wcsncpy_s(dialog->lpstrFile,dialog->nMaxFile,fileDialogTarget.c_str(),_TRUNCATE);
    dialog->nFilterIndex=fileDialogFilter;
    return TRUE;
}
static DWORD WINAPI preview_file_dialog_error(){return 0;}
#define GetSaveFileNameW preview_file_dialog
#define CommDlgExtendedError preview_file_dialog_error
#define MessageBoxW preview_settings_message_box
#include "../src/output.cpp"
#undef GetSaveFileNameW
#undef CommDlgExtendedError
#undef MessageBoxW
#define ensure_class ensure_pin_class
#include "../src/pin.cpp"
#undef ensure_class
#define ensure_class ensure_emoji_class
#include "../src/emoji_popup.cpp"
#undef ensure_class
static bool preview_longshot_copy(Gdiplus::Bitmap*){++longshotCopies;return longshotCopyOk;}
#define copy_bitmap_to_clipboard preview_longshot_copy
#define MessageBoxW preview_settings_message_box
#include "../src/longshot.cpp"
#undef copy_bitmap_to_clipboard
#undef MessageBoxW

bool preview_hunyuan_model_ready(){++readinessChecks[0];return componentReady;}
bool preview_hunyuan_fast_model_ready(){++readinessChecks[1];return componentReady;}
bool preview_fast_ocr_available(){++readinessChecks[2];return componentReady;}
static std::atomic<int> recognized{0},translated{0};
static bool preview_ocr_recognize(Gdiplus::Bitmap*,std::wstring& text,std::wstring& error,const std::atomic<bool>*){
    ++recognized;
    if(!componentReady){error=L"OCR 组件未安装";return false;}
    text=L"Hello";return true;
}
static TranslateResult preview_translate_text(const std::wstring&,const Settings&){
    ++translated;TranslateResult result;result.text=L"翻译结果";return result;
}

static void render_children(HWND root, HWND parent, HDC dc) {
    for(HWND child=GetWindow(parent,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)){
        if(!(GetWindowLongW(child,GWL_STYLE)&WS_VISIBLE))continue;
        RECT cr{};GetClientRect(child,&cr);
        POINT pos{};MapWindowPoints(child,root,&pos,1);
        int saved=SaveDC(dc);
        SetViewportOrgEx(dc,pos.x,pos.y,nullptr);
        IntersectClipRect(dc,0,0,cr.right,cr.bottom);
        SendMessageW(child,WM_PRINTCLIENT,(WPARAM)dc,PRF_CLIENT|PRF_ERASEBKGND);
        RestoreDC(dc,saved);
        render_children(root,child,dc);
    }
}
static bool png(HWND window,const std::wstring& path) {
    RECT r{};GetClientRect(window,&r);
    HDC dc=CreateCompatibleDC(nullptr);BITMAPINFO bi{};
    bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=r.right;
    bi.bmiHeader.biHeight=-r.bottom;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;
    void* bits=nullptr;HBITMAP bitmap=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,&bits,nullptr,0);
    auto old=SelectObject(dc,bitmap);
    SendMessageW(window,WM_PRINTCLIENT,(WPARAM)dc,PRF_CLIENT);
    render_children(window,window,dc);
    GdiFlush();
    Gdiplus::Bitmap image(r.right,r.bottom,r.right*4,PixelFormat32bppRGB,(BYTE*)bits);
    CLSID encoder{};UINT n=0,size=0;Gdiplus::GetImageEncodersSize(&n,&size);
    std::vector<BYTE> codecs(size);
    auto* list=(Gdiplus::ImageCodecInfo*)codecs.data();Gdiplus::GetImageEncoders(n,size,list);
    for(UINT i=0;i<n;++i)if(!wcscmp(list[i].MimeType,L"image/png"))encoder=list[i].Clsid;
    bool ok=image.Save(path.c_str(),&encoder)==Gdiplus::Ok;
    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);return ok;
}

static bool test_settings_save(const std::filesystem::path& output,const wchar_t* windowClass){
    Settings original=g_cfg;
    const wchar_t* engines[]={L"hunyuan",L"hunyuan_fast",L"ppocr"};
    bool allPassed=true;
    for(int engine=0;engine<3;++engine)for(int scenario=0;scenario<3;++scenario){
        LogTrace trace;
        bool enabled=scenario!=0;
        componentReady=scenario==2;
        logf(L"设置保存回归开始：引擎=%s OCR启用=%d 组件可用=%d",
             engines[engine],(int)enabled,(int)componentReady);
        Settings baseline;
        baseline.hotkey_enabled=false;
        baseline.ocr_engine=engines[engine];
        baseline.ocr_enabled=!enabled;
        baseline.save_dir=(output/L"initial-save").wstring();
        g_cfg=baseline;
        if(!save_settings(baseline)){g_cfg=original;return false;}
        CfgDlg dlg{};
        HWND form=CreateWindowW(windowClass,L"settings-save-test",WS_POPUP|WS_CLIPCHILDREN,
            0,0,S(840),S(648),nullptr,nullptr,g_hinst,&dlg);
        if(!form){g_cfg=original;return false;}
        auto nextDir=output/(L"保存目录-"+std::to_wstring(engine)+L"-"+std::to_wstring(scenario));
        SetWindowTextW(dlg.saveDir,nextDir.c_str());
        SendMessageW(dlg.ocrEnabled,BM_SETCHECK,enabled?BST_CHECKED:BST_UNCHECKED,0);
        SendMessageW(dlg.ocrEngine,CB_SETCURSEL,engine,0);
        dlg.page=1;choose_page(&dlg);
        std::fill_n(readinessChecks,3,0);ocrWarnings=0;unexpectedWarnings=0;
        SendMessageW(form,WM_COMMAND,MAKEWPARAM(1,BN_CLICKED),0);
        Settings stored=load_settings();
        int totalChecks=readinessChecks[0]+readinessChecks[1]+readinessChecks[2];
        bool ok=unexpectedWarnings==0&&totalChecks==0&&ocrWarnings==0&&dlg.done&&
            stored.ocr_enabled==enabled&&stored.ocr_engine==engines[engine]&&
            stored.save_dir==nextDir.wstring()&&g_cfg.save_dir==stored.save_dir;
        logf(L"设置保存回归返回：结果=%s 引擎=%s OCR启用=%d 组件可用=%d 组件查询次数=%d 弹窗=%d",
             ok?L"通过":L"失败",engines[engine],(int)enabled,(int)componentReady,totalChecks,ocrWarnings);
        std::cout<<(ok?"PASS ":"FAIL ")<<"settings save engine="<<w_to_u8(engines[engine])
                 <<" OCR="<<enabled<<" component="<<componentReady<<std::endl;
        allPassed=allPassed&&ok;
        if(IsWindow(form))DestroyWindow(form);
        DeleteObject(dlg.font);
    }
    g_cfg=original;
    return allPassed;
}

static bool settingsCapturePassed=false;
static bool test_idle_timeout_settings(const std::filesystem::path& output,const wchar_t* windowClass){
    Settings original=g_cfg;
    const wchar_t* labels[]={L"off",L"1分钟",L"10分钟",L"1小时",L"12小时",L"24小时"};
    bool allPassed=true;
    for(int i=0;i<6;++i){
        LogTrace trace;
        g_cfg.hotkey_enabled=false;g_cfg.save_dir=(output/L"idle-save").wstring();
        g_cfg.ocr_idle_minutes=OCR_IDLE_TIMEOUT_MINUTES[i];
        CfgDlg dlg{};
        HWND form=CreateWindowW(windowClass,L"idle-timeout-test",WS_POPUP|WS_CLIPCHILDREN,
            0,0,S(840),S(648),nullptr,nullptr,g_hinst,&dlg);
        if(!form){g_cfg=original;return false;}
        bool ok=SendMessageW(dlg.ocrIdle,CB_GETCOUNT,0,0)==6&&
                SendMessageW(dlg.ocrIdle,CB_GETCURSEL,0,0)==i;
        for(int j=0;j<6;++j){
            wchar_t label[64]{};
            SendMessageW(dlg.ocrIdle,CB_GETLBTEXT,j,(LPARAM)label);
            ok=ok&&!wcscmp(label,labels[j])&&
               SendMessageW(dlg.ocrIdle,CB_GETITEMDATA,j,0)==OCR_IDLE_TIMEOUT_MINUTES[j];
        }
        int next=(i+1)%6;
        SendMessageW(dlg.ocrIdle,CB_SETCURSEL,next,0);
        SendMessageW(form,WM_COMMAND,MAKEWPARAM(1,BN_CLICKED),0);
        ok=ok&&dlg.done&&load_settings().ocr_idle_minutes==OCR_IDLE_TIMEOUT_MINUTES[next]&&
           g_cfg.ocr_idle_minutes==OCR_IDLE_TIMEOUT_MINUTES[next];
        logf(L"空闲回收下拉回归：初始索引=%d 保存索引=%d 成功=%d",i,next,(int)ok);
        std::cout<<(ok?"PASS ":"FAIL ")<<"idle timeout dropdown option "<<i<<std::endl;
        allPassed=allPassed&&ok;
        if(IsWindow(form))DestroyWindow(form);
        DeleteObject(dlg.font);
    }
    g_cfg=original;save_settings(original);
    return allPassed;
}
static void CALLBACK capture_while_settings_open(HWND,UINT,UINT_PTR,DWORD){
    if(!g_settingsWnd)return;
    auto* dlg=(CfgDlg*)GetWindowLongPtrW(g_settingsWnd,GWLP_USERDATA);
    SetFocus(dlg->saveDir);
    SetWindowTextW(dlg->saveDir,L"C:\\尚未保存的目录");
    captureRequests=0;g_lastHotkeyTick=0;
    SendMessageW(g_msgwnd,WM_HOTKEY,HOTKEY_ID,0);
    bool systemCaptured=captureRequests==1;
    g_lastHotkeyTick=0;g_rawKeys=RawHotkeyState{};
    RAWKEYBOARD key{};key.VKey=VK_F24;
    dispatch_raw_keyboard(key);
    MSG message;
    while(PeekMessageW(&message,g_msgwnd,WM_JJ_HOTKEY,WM_JJ_HOTKEY,PM_REMOVE))DispatchMessageW(&message);
    settingsCapturePassed=systemCaptured&&captureRequests==2&&g_hotOk&&
        IsWindow(g_settingsWnd)&&!dlg->done&&text(dlg->saveDir)==L"C:\\尚未保存的目录";
    logf(L"设置打开时截图回归返回：成功=%d 系统热键=%d 总请求=%d 草稿保留=%d",
         (int)settingsCapturePassed,(int)systemCaptured,captureRequests,(int)!dlg->done);
    DestroyWindow(g_settingsWnd);
}

static bool test_settings_capture(){
    LogTrace trace;
    Settings original=g_cfg;
    HWND originalMessage=g_msgwnd;
    bool originalOk=g_hotOk;UINT originalMods=g_hotMods,originalVk=g_hotVk;
    std::wstring originalName=g_hotName;
    g_cfg.hotkey_enabled=true;g_cfg.hotkey=L"F24";
    if(!save_settings(g_cfg))return false;
    WNDCLASSW wc{};wc.lpfnWndProc=msg_proc;wc.hInstance=g_hinst;wc.lpszClassName=L"weijiePreviewMessages";
    RegisterClassW(&wc);
    g_msgwnd=CreateWindowW(wc.lpszClassName,L"preview",0,0,0,0,0,HWND_MESSAGE,nullptr,g_hinst,nullptr);
    g_hotOk=true;g_hotMods=0;g_hotVk=VK_F24;g_hotName=L"F24";
    double scale=g_scale;
    UINT_PTR timer=SetTimer(nullptr,0,25,capture_while_settings_open);
    if(!g_msgwnd||!timer)return false;
    show_settings_dialog(g_msgwnd);
    KillTimer(nullptr,timer);
    bool ok=settingsCapturePassed&&g_scale==scale;
    UnregisterHotKey(g_msgwnd,HOTKEY_ID);
    DestroyWindow(g_msgwnd);
    MSG quit;while(PeekMessageW(&quit,nullptr,WM_QUIT,WM_QUIT,PM_REMOVE)){}
    g_msgwnd=originalMessage;g_hotOk=originalOk;g_hotMods=originalMods;g_hotVk=originalVk;g_hotName=originalName;
    g_cfg=original;
    std::cout<<(ok?"PASS ":"FAIL ")<<"capture with settings open preserves hotkeys, draft and desktop scale"<<std::endl;
    return ok;
}

static bool wait_for_text_job(Overlay* overlay){
    ULONGLONG start=GetTickCount64();
    while(GetTickCount64()-start<3000){
        MSG message;
        while(PeekMessageW(&message,overlay->hwnd,WM_JJ_OCR,WM_JJ_TRANS,PM_REMOVE))DispatchMessageW(&message);
        if(overlay->panel!=PM_LOADING&&!overlay->ocrRunning)return true;
        Sleep(5);
    }
    return false;
}

static bool test_toolbar_switches(){
    ensure_class();
    bool allPassed=true;
    for(bool ocr:{false,true})for(bool translation:{false,true}){
        LogTrace trace;
        auto overlay=std::make_shared<Overlay>();
        overlay->traceId=trace.id();
        Settings cfg;cfg.ocr_enabled=ocr;cfg.translate_enabled=translation;
        componentReady=false;
        ocr_set_enabled(ocr);
        apply_overlay_settings(overlay.get(),cfg);
        overlay->shot.vw=800;overlay->shot.vh=600;
        overlay->shot.bmp=new Gdiplus::Bitmap(800,600,PixelFormat32bppRGB);
        overlay->hasSel=true;overlay->sel={80,80,240,180};
        g_ov=overlay;
        HWND window=CreateWindowW(OV_CLASS,L"toolbar-test",WS_POPUP,0,0,800,600,nullptr,nullptr,g_hinst,overlay.get());
        if(!window)return false;
        int ocrButtons=0,translationButtons=0;
        for(const auto& item:overlay->items){
            if(item.id==T_OCR)++ocrButtons;
            if(item.id==T_TRANS)++translationButtons;
        }
        Gdiplus::RectF buttons[3];const wchar_t* labels[3];int actions[3];
        int panelCount=panel_buttons(overlay.get(),buttons,labels,actions);
        bool ok=ocrButtons==(ocr?1:0)&&translationButtons==(translation?1:0)&&panelCount==(translation?3:2);
        if(!ocr&&translation){
            componentReady=true;recognized=0;translated=0;
            start_translate(overlay.get());
            ok=ok&&recognized==0&&translated==0&&overlay->panel==PM_ERROR&&
               overlay->panelBody.find(L"主开关已关闭")!=std::wstring::npos;
            overlay->panel=PM_TEXT;overlay->panelBody=L"已有文字";
            start_translate(overlay.get());
            ok=ok&&wait_for_text_job(overlay.get())&&recognized==0&&translated==1&&
               overlay->panel==PM_TEXT&&overlay->panelBody==L"翻译结果";
        }
        cfg.ocr_enabled=!ocr;cfg.translate_enabled=!translation;
        ocr_set_enabled(cfg.ocr_enabled);
        overlay_apply_display_settings(cfg);
        int updatedOcr=0,updatedTranslation=0;
        for(const auto& item:overlay->items){
            if(item.id==T_OCR)++updatedOcr;
            if(item.id==T_TRANS)++updatedTranslation;
        }
        ok=ok&&updatedOcr==(!ocr?1:0)&&updatedTranslation==(!translation?1:0);
        logf(L"独立按钮回归返回：识别=%d 翻译=%d 成功=%d",(int)ocr,(int)translation,(int)ok);
        std::cout<<(ok?"PASS ":"FAIL ")<<"independent toolbar OCR="<<ocr<<" translate="<<translation<<std::endl;
        allPassed=allPassed&&ok;
        DestroyWindow(window);
    }
    return allPassed;
}

static bool test_immediate_ocr_disable(const wchar_t* windowClass){
    LogTrace trace;
    Settings original=g_cfg;
    g_cfg.ocr_enabled=true;g_cfg.hotkey_enabled=false;
    save_settings(g_cfg);ocr_set_enabled(true);
    CfgDlg dlg{};
    HWND form=CreateWindowW(windowClass,L"ocr-master-test",WS_POPUP|WS_CLIPCHILDREN,
        0,0,S(840),S(648),nullptr,nullptr,g_hinst,&dlg);
    if(!form)return false;
    SendMessageW(dlg.ocrEnabled,BM_SETCHECK,BST_UNCHECKED,0);
    SendMessageW(form,WM_COMMAND,MAKEWPARAM(14,BN_CLICKED),(LPARAM)dlg.ocrEnabled);
    bool stopped=!ocr_is_enabled()&&load_settings().ocr_enabled;
    Settings other=g_cfg;other.jpeg_quality=91;save_settings(other);
    stopped=stopped&&!ocr_is_enabled();
    DestroyWindow(form);
    bool restored=ocr_is_enabled();
    DeleteObject(dlg.font);
    g_cfg=original;save_settings(original);ocr_set_enabled(original.ocr_enabled);
    std::cout<<(stopped&&restored?"PASS ":"FAIL ")
             <<"OCR switch disables immediately before save; cancel restores lazy permission"<<std::endl;
    return stopped&&restored;
}

static HWND create_longshot_preview(Gdiplus::Bitmap* owned,const std::wstring& reason){
    if(!owned)return nullptr;
    WNDCLASSW wc{};wc.lpfnWndProc=preview_proc;wc.hInstance=g_hinst;
    wc.lpszClassName=L"weijieLongPreviewRegression";RegisterClassW(&wc);
    LogTrace trace;
    auto* preview=new Preview(owned,trace.id(),reason);
    HWND form=CreateWindowW(wc.lpszClassName,L"longshot-preview-test",WS_POPUP|WS_CLIPCHILDREN|WS_VSCROLL,
        0,0,S(840),S(648),nullptr,nullptr,g_hinst,preview);
    if(!form)delete preview;
    return form;
}
static bool test_longshot_preview(const std::filesystem::path& output){
    Settings original=g_cfg;bool allPassed=true;
    auto report=[&](bool ok,const char* name){
        std::cout<<(ok?"PASS ":"FAIL ")<<name<<std::endl;
        logf(L"长截图预览回归：%s 成功=%d",u8_to_w(name).c_str(),(int)ok);
        allPassed=allPassed&&ok;
    };
    const int width=240,height=24000;
    auto alternative=output/L"长截图 另存目录"/(L"session-"+std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(alternative);
    std::vector<BYTE> pixels((size_t)width*height*4,255);
    for(int y=0;y<height;++y)for(int x=0;x<width;++x){
        size_t i=((size_t)y*width+x)*4;
        pixels[i]=(BYTE)(180+y/32%70);pixels[i+1]=(BYTE)(130+x/8%100);pixels[i+2]=(BYTE)(80+y/64%100);
    }
    for(const wchar_t* format:{L"png",L"jpg",L"bmp"}){
        LogTrace trace;
        Settings settings=original;settings.save_dir=(output/L"长截图 设置目录"/format).wstring();
        settings.image_format=format;settings.auto_copy=false;
        if(!save_settings(settings))return false;
        HWND form=create_longshot_preview(make_bitmap(pixels,width,height),L"已按 Esc 停止，保留已拼接内容");
        if(!form)return false;
        auto* preview=(Preview*)GetWindowLongPtrW(form,GWLP_USERDATA);
        fileDialogCalls=0;longshotCopies=0;unexpectedWarnings=0;
        report(text(preview->saveAs)==L"另存为","longshot preview has a separate Save As button");
        SendMessageW(preview->save,BM_CLICK,0,0);
        std::wstring first=preview->statusText.size()>4?preview->statusText.substr(4):L"";
        bool saved=std::filesystem::exists(first);
        if(saved){
            Gdiplus::Bitmap image(first.c_str());
            saved=image.GetWidth()==width&&image.GetHeight()==height;
        }
        report(saved&&std::filesystem::path(first).parent_path()==std::filesystem::path(settings.save_dir)&&
               fileDialogCalls==0&&longshotCopies==0&&unexpectedWarnings==0&&IsWindow(form),
               "longshot Save writes full 24000-pixel image to configured directory without dialog");
        SendMessageW(preview->save,BM_CLICK,0,0);
        std::wstring second=preview->statusText.substr(4);
        report(second!=first&&std::filesystem::exists(first)&&std::filesystem::exists(second),
               "repeated longshot Save uses a new filename without overwriting");
        fileDialogTarget=(alternative/(std::wstring(L"selected-")+format+L".tmp")).wstring();
        fileDialogCancelled=false;fileDialogFilter=2;
        SendMessageW(preview->saveAs,BM_CLICK,0,0);
        auto expected=std::filesystem::path(fileDialogTarget);
        expected.replace_extension(std::wstring(format)==L"png"?L".jpg":L".png");
        report(fileDialogCalls==1&&fileDialogInitial==settings.save_dir&&
               preview->statusText==L"已保存："+expected.wstring()&&std::filesystem::exists(expected)&&
               load_settings().save_dir==settings.save_dir&&IsWindow(form),
               "Save As honors chosen path and format without changing configured directory");
        std::wstring status=preview->statusText;
        fileDialogCancelled=true;
        fileDialogTarget=(alternative/L"cancelled.png").wstring();
        SendMessageW(preview->saveAs,BM_CLICK,0,0);
        report(preview->statusText==status&&!std::filesystem::exists(fileDialogTarget)&&IsWindow(form),
               "cancel Save As preserves preview and last saved status");
        if(std::wstring(format)==L"png"){
            settings.auto_copy=true;save_settings(settings);
            longshotCopyOk=false;unexpectedWarnings=0;
            SendMessageW(preview->save,BM_CLICK,0,0);
            report(longshotCopies==1&&unexpectedWarnings==1&&
                   preview->statusText.find(L"已保存：")==0&&
                   std::filesystem::exists(preview->statusText.substr(4)),
                   "clipboard failure does not turn a successful file save into a save failure");
            longshotCopyOk=true;
            settings.auto_copy=false;
            auto blocked=output/L"not-a-directory";
            {std::ofstream file(blocked);file<<"file";}
            settings.save_dir=(blocked/L"nested").wstring();save_settings(settings);
            unexpectedWarnings=0;
            SendMessageW(preview->save,BM_CLICK,0,0);
            report(unexpectedWarnings==1&&preview->statusText.find(L"保存失败")==0&&IsWindow(form),
                   "unwritable configured directory reports save failure and retains preview");
            fileDialogCancelled=false;fileDialogFilter=1;
            fileDialogTarget=(alternative/L"recovered.png").wstring();
            SendMessageW(preview->saveAs,BM_CLICK,0,0);
            report(std::filesystem::exists(fileDialogTarget)&&preview->statusText==L"已保存："+fileDialogTarget,
                   "Save As can recover from an unavailable configured directory");
        }
        DestroyWindow(form);
    }
    {
        LogTrace trace;
        Settings settings=original;settings.image_format=L"png";settings.auto_copy=false;
        settings.save_dir=(output/L"编码边界 长图目录").wstring();save_settings(settings);
        auto* image=new Gdiplus::Bitmap(64,LONGSHOT_MAX_ENCODED_HEIGHT,PixelFormat32bppRGB);
        {Gdiplus::Graphics graphics(image);graphics.Clear(Gdiplus::Color::White);}
        HWND form=create_longshot_preview(image,L"已按 Esc 停止，保留已拼接内容");
        if(!form)return false;
        auto* preview=(Preview*)GetWindowLongPtrW(form,GWLP_USERDATA);
        unexpectedWarnings=0;
        SendMessageW(preview->save,BM_CLICK,0,0);
        report(unexpectedWarnings==0&&preview->statusText.find(L"已保存：")==0&&
               std::filesystem::exists(preview->statusText.substr(4))&&IsWindow(form),
               "encoder-boundary longshot saves as PNG from the real preview button");
        fileDialogCancelled=false;fileDialogFilter=3;
        fileDialogTarget=(alternative/L"very-tall.tmp").wstring();
        SendMessageW(preview->saveAs,BM_CLICK,0,0);
        auto expected=std::filesystem::path(fileDialogTarget);expected.replace_extension(L".bmp");
        report(std::filesystem::exists(expected)&&preview->statusText==L"已保存："+expected.wstring(),
               "encoder-boundary longshot can be saved as full-height BMP");
        DestroyWindow(form);
    }
    fileDialogCancelled=false;g_cfg=original;save_settings(original);
    return allPassed;
}

static bool cursor_diameter_matches(HCURSOR cursor,int diameter){
    ICONINFO info{};
    if(!cursor||!GetIconInfo(cursor,&info))return false;
    BITMAP bitmap{};
    bool ok=GetObjectW(info.hbmColor,sizeof(bitmap),&bitmap)&&!info.fIcon&&
        bitmap.bmWidth==diameter+4&&bitmap.bmHeight==diameter+4&&
        info.xHotspot==(UINT)(2+diameter/2)&&info.yHotspot==(UINT)(2+diameter/2);
    if(info.hbmColor)DeleteObject(info.hbmColor);if(info.hbmMask)DeleteObject(info.hbmMask);
    return ok;
}

static bool mouse_drag_slider(HWND slider,int& finalPosition,bool edge=false){
    SendMessageW(slider,WM_KEYDOWN,VK_HOME,0);SendMessageW(slider,WM_KEYUP,VK_HOME,0);
    auto geometry=ui::trackbar_geometry(slider);RECT client{};GetClientRect(slider,&client);
    POINT start{(LONG)lround(geometry.cx)+(edge?(LONG)floor(geometry.radius)-1:0),(LONG)lround(geometry.cy)};
    POINT oldCenter{start.x,client.bottom/2};
    logf(L"滑条鼠标回归几何：控件=%p 高度=%ld 原生滑块=(%ld,%ld)-(%ld,%ld) 绘制中心=(%ld,%ld) 窗口中线=%ld 中线是否可命中滑块=%d",
         slider,client.bottom,geometry.thumb.left,geometry.thumb.top,geometry.thumb.right,geometry.thumb.bottom,
         start.x,start.y,oldCenter.y,(int)PtInRect(&geometry.thumb,oldCenter));
    SendMessageW(slider,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(start.x,start.y));
    bool captured=GetCapture()==slider;
    SendMessageW(slider,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(client.right+20,start.y));
    SendMessageW(slider,WM_LBUTTONUP,0,MAKELPARAM(client.right+20,client.bottom+20));
    finalPosition=(int)SendMessageW(slider,TBM_GETPOS,0,0);
    return captured&&GetCapture()!=slider&&finalPosition==(int)SendMessageW(slider,TBM_GETRANGEMAX,0,0);
}

static bool mouse_click_slider_track(HWND slider){
    SendMessageW(slider,WM_KEYDOWN,VK_HOME,0);SendMessageW(slider,WM_KEYUP,VK_HOME,0);
    auto geometry=ui::trackbar_geometry(slider);
    int before=(int)SendMessageW(slider,TBM_GETPOS,0,0);
    int x=geometry.channel.left+(geometry.channel.right-geometry.channel.left)*3/4;
    int y=(int)lround(geometry.cy);
    int maximum=(int)SendMessageW(slider,TBM_GETRANGEMAX,0,0);
    int expected=before+(int)lround((x-geometry.channel.left)/(double)(geometry.channel.right-geometry.channel.left)*(maximum-before));
    SendMessageW(slider,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
    SendMessageW(slider,WM_LBUTTONUP,0,MAKELPARAM(x,y));
    return (int)SendMessageW(slider,TBM_GETPOS,0,0)==expected&&expected>before&&GetCapture()!=slider;
}

static bool slider_follows_mouse(HWND slider,HWND canvas=nullptr){
    SendMessageW(slider,WM_KEYDOWN,VK_HOME,0);SendMessageW(slider,WM_KEYUP,VK_HOME,0);
    auto geometry=ui::trackbar_geometry(slider);
    int left=geometry.channel.left,right=geometry.channel.right,y=(int)lround(geometry.cy);
    int minimum=(int)SendMessageW(slider,TBM_GETRANGEMIN,0,0);
    int maximum=(int)SendMessageW(slider,TBM_GETRANGEMAX,0,0);
    SendMessageW(slider,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM((int)lround(geometry.cx),y));
    if(canvas)ValidateRect(canvas,nullptr);
    bool ok=true,sameValueMoved=false;int lastPosition=minimum,lastX=left;
    auto move=[&](int x){
        SendMessageW(slider,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(x,y));
        int value=(int)SendMessageW(slider,TBM_GETPOS,0,0);
        float display=ui::trackbar_geometry(slider).cx;
        int expected=minimum+(int)lround((x-left)/(double)(right-left)*(maximum-minimum));
        ok=ok&&fabs(display-x)<.01&&value==expected;
        if(value==lastPosition&&x!=lastX)sameValueMoved=true;
        logf(L"滑块逐点跟随回归：鼠标x=%d 绘制中心=%.2f 当前值=%d 期望值=%d 成功=%d",
             x,display,value,expected,(int)(fabs(display-x)<.01&&value==expected));
        lastPosition=value;lastX=x;
    };
    for(int percent:{13,37,61,83})move(left+(right-left)*percent/100);
    int fine=left+(right-left)*35/100;
    for(int dx=0;dx<5;++dx)move(fine+dx);
    if(right-left>2*(maximum-minimum))ok=ok&&sameValueMoved;
    Sleep(40);
    MSG message{};
    while(PeekMessageW(&message,GetParent(slider),WM_TIMER,WM_TIMER,PM_REMOVE))DispatchMessageW(&message);
    if(canvas)ok=ok&&!GetUpdateRect(canvas,nullptr,FALSE);
    SendMessageW(slider,WM_LBUTTONUP,0,MAKELPARAM(lastX,y));
    ok=ok&&GetCapture()!=slider&&fabs(ui::trackbar_geometry(slider).cx-lastX)<.01;
    SendMessageW(slider,WM_KEYDOWN,VK_RIGHT,0);SendMessageW(slider,WM_KEYUP,VK_RIGHT,0);
    int value=(int)SendMessageW(slider,TBM_GETPOS,0,0);
    float expected=left+(right-left)*(value-minimum)/(float)(maximum-minimum);
    ok=ok&&fabs(ui::trackbar_geometry(slider).cx-expected)<.01;
    SendMessageW(slider,TBM_SETPOS,TRUE,minimum);
    SendMessageW(GetParent(slider),WM_HSCROLL,TB_THUMBPOSITION,(LPARAM)slider);
    return ok&&fabs(ui::trackbar_geometry(slider).cx-left)<.01;
}

#include "size_drag_tests.inc"

static bool test_slider_mouse_input(const wchar_t* settingsClass){
    double originalScale=g_scale;bool allPassed=true;
    auto report=[&](bool ok,const char* name,int tool){
        std::cout<<(ok?"PASS ":"FAIL ")<<name<<" tool="<<tool<<" scale="<<g_scale<<std::endl;
        logf(L"滑条鼠标回归：%s 工具=%d DPI=%.2f 成功=%d",u8_to_w(name).c_str(),tool,g_scale,(int)ok);
        allPassed=allPassed&&ok;
    };
    for(double scale:{1.0,1.75}){
        g_scale=scale;
        {
            LogTrace trace;Overlay overlay;overlay.traceId=trace.id();overlay.ink=Gdiplus::Color(255,255,0,0);
            overlay.hwnd=CreateWindowW(L"STATIC",L"color-slider-owner",WS_POPUP,0,0,S(640),S(480),nullptr,nullptr,g_hinst,nullptr);
            auto* popup=new Popup{&overlay};
            HWND form=CreateWindowW(CLASS,L"color-slider-mouse-test",WS_POPUP|WS_CLIPCHILDREN,
                0,0,S(360),S(546),overlay.hwnd,nullptr,g_hinst,popup);
            if(!form){delete popup;DestroyWindow(overlay.hwnd);allPassed=false;continue;}
            int finalPosition=0;
            report(mouse_drag_slider(popup->slider,finalPosition)&&popup->hue==359,
                   "hue slider responds to dragging its visible thumb",T_COLOR);
            report(mouse_drag_slider(popup->alpha,finalPosition)&&overlay.ink.GetA()==0,
                   "transparency slider responds to dragging its visible thumb",T_COLOR);
            report(slider_follows_mouse(popup->slider)&&slider_follows_mouse(popup->alpha),
                   "hue and transparency thumb centers track intermediate mouse positions",T_COLOR);
            popup->changed=false;
            DestroyWindow(form);DestroyWindow(overlay.hwnd);overlay.hwnd=nullptr;
        }
        {
            LogTrace trace;CfgDlg dialog{};
            HWND form=CreateWindowW(settingsClass,L"quality-slider-mouse-test",WS_POPUP|WS_CLIPCHILDREN,
                0,0,S(840),S(648),nullptr,nullptr,g_hinst,&dialog);
            if(!form){allPassed=false;continue;}
            int finalPosition=0;
            report(mouse_drag_slider(dialog.quality,finalPosition)&&text(dialog.qualityVal)==L"100",
                   "JPEG quality slider mouse drag updates its displayed value",T_SAVE);
            report(mouse_click_slider_track(dialog.quality)&&
                   text(dialog.qualityVal)==std::to_wstring(SendMessageW(dialog.quality,TBM_GETPOS,0,0)),
                   "JPEG quality slider track click remains available",T_SAVE);
            report(slider_follows_mouse(dialog.quality),
                   "JPEG quality thumb center tracks intermediate mouse positions",T_SAVE);
            DestroyWindow(form);DeleteObject(dialog.font);
        }
    }
    g_scale=originalScale;return allPassed;
}

static bool test_emoji_size_drag(){
    double originalScale=g_scale;bool allPassed=true;ensure_class();
    auto report=[&](bool ok,const char* name){
        std::cout<<(ok?"PASS ":"FAIL ")<<name<<" scale="<<g_scale<<std::endl;
        logf(L"表情右键尺寸回归：%s DPI=%.2f 成功=%d",u8_to_w(name).c_str(),g_scale,(int)ok);
        allPassed=allPassed&&ok;
    };
    for(double scale:{1.0,1.75}){
        g_scale=scale;LogTrace trace;Overlay overlay;overlay.traceId=trace.id();
        overlay.hasSel=true;overlay.sel={100,100,1300,900};overlay.tbVisible=true;
        overlay.tbX=40;overlay.tbY=950;overlay.shot.vw=1600;overlay.shot.vh=1200;
        overlay.shot.bmp=new Bitmap(1600,1200,PixelFormat32bppRGB);
        {Graphics graphics(overlay.shot.bmp);graphics.Clear(Color::White);}
        HWND form=CreateWindowW(OV_CLASS,L"emoji-size-drag-test",WS_POPUP,
            -4000,-3000,1600,1200,nullptr,nullptr,g_hinst,&overlay);
        if(!form){allPassed=false;continue;}
        POINT point{};
        for(const auto& item:overlay.items)if(item.id==T_EMOJI)
            point={(LONG)lround(overlay.tbX+item.rect.X+item.rect.Width/2),
                   (LONG)lround(overlay.tbY+item.rect.Y+item.rect.Height/2)};
        SendMessageW(form,WM_RBUTTONDOWN,MK_RBUTTON,MAKELPARAM(point.x,point.y));
        report(overlay.sizeDrag.active&&overlay.tool==T_EMOJI&&!overlay.stylePopup&&GetCapture()==form,
               "toolbar Emoji right hold starts inline size slider without a popup");
        Ann old;old.kind=AK_EMOJI;old.fontSize=40;old.emojiIndex=overlay.emojiIndex;old.pos={150,150};
        overlay.anns.push_back(old);
        POINT moved{point.x+S(80),point.y};
        SendMessageW(form,WM_MOUSEMOVE,MK_RBUTTON,MAKELPARAM(moved.x,moved.y));
        SendMessageW(form,WM_RBUTTONUP,0,MAKELPARAM(moved.x,moved.y));
        int selected=overlay.toolSizes[T_EMOJI-T_RECT];
        report(selected>40&&overlay.anns[0].fontSize==40&&!overlay.sizeDrag.active&&GetCapture()!=form,
               "right drag changes future Emoji size and release confirms without resizing existing Emoji");
        SendMessageW(form,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(650,500));
        std::unique_ptr<Bitmap> result(render_result(&overlay));
        report(overlay.anns.size()==2&&overlay.anns.back().fontSize==selected&&result&&
               overlay.anns.back().bounds().Width==SF((float)selected),
               "Emoji insertion and export use right-drag selected size");
        SendMessageW(form,WM_LBUTTONUP,0,MAKELPARAM(650,500));
        SendMessageW(form,WM_RBUTTONDOWN,MK_RBUTTON,MAKELPARAM(1100,750));
        report(overlay.sizeDrag.active&&overlay.sizeDrag.tool==T_EMOJI&&!overlay.stylePopup,
               "empty canvas right hold adjusts future Emoji size without a window");
        SendMessageW(form,WM_RBUTTONUP,0,MAKELPARAM(1100,750));DestroyWindow(form);
    }
    g_scale=originalScale;return allPassed;
}

static POINT image_picker_point(HWND picker,int index){
    SCROLLINFO scroll{sizeof(scroll),SIF_POS};GetScrollInfo(picker,SB_VERT,&scroll);
    int row=0;
    for(const auto& group:emoji_groups()){
        ++row;
        auto found=std::find(group.images.begin(),group.images.end(),index);
        if(found!=group.images.end()){
            int offset=(int)(found-group.images.begin());
            return {S(12+(offset%11)*34+17),S(30+(row+offset/11-scroll.nPos)*34+17)};
        }
        row+=(int)(group.images.size()+10)/11;
    }
    return {-1,-1};
}

static bool test_emoji_picker_preview(){
    double originalScale=g_scale;bool passed=true;int checks=0;ensure_class();
    auto report=[&](bool ok,const char* name){
        ++checks;passed=passed&&ok;
        std::cout<<(ok?"PASS ":"FAIL ")<<name<<" scale="<<g_scale<<std::endl;
        logf(L"表情选择到虚影回归：%s DPI=%.2f 成功=%d",u8_to_w(name).c_str(),g_scale,(int)ok);
    };
    for(double scale:{1.0,1.75}){
        g_scale=scale;LogTrace trace;Overlay overlay;overlay.traceId=trace.id();
        overlay.hasSel=true;overlay.sel={50,50,1550,1100};overlay.tbVisible=true;
        overlay.tbX=50;overlay.tbY=1150;overlay.shot.vx=-4000;overlay.shot.vy=-3000;
        overlay.shot.vw=1600;overlay.shot.vh=1300;
        HWND form=CreateWindowW(OV_CLASS,L"emoji-picker-preview-test",WS_POPUP|WS_CLIPCHILDREN,
            -4000,-3000,1600,1300,nullptr,nullptr,g_hinst,&overlay);
        if(!form){passed=false;continue;}
        stampTestForeground=form;
        for(int selected:{48,49,81}){
            POINT button{};
            for(const auto& item:overlay.items)if(item.id==T_EMOJI)
                button={(LONG)lround(overlay.tbX+item.rect.X+item.rect.Width/2),
                        (LONG)lround(overlay.tbY+item.rect.Y+item.rect.Height/2)};
            SendMessageW(form,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(button.x,button.y));
            HWND picker=overlay.emojiPopup;
            report(picker&&IsWindow(picker)&&!overlay.stampPressed,
                   "toolbar opens Emoji picker without latching placement button");
            if(!picker)continue;
            POINT target=image_picker_point(picker,selected);int x=target.x,y=target.y;
            SendMessageW(picker,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
            report(IsWindow(picker)&&overlay.anns.empty(),
                   "Emoji picker consumes mouse down without closing or placing a stamp");
            SendMessageW(picker,WM_LBUTTONUP,0,MAKELPARAM(-1,-1));
            report(IsWindow(picker),"release outside Emoji cells leaves picker open");
            SendMessageW(picker,WM_LBUTTONUP,0,MAKELPARAM(x,y));
            report(!overlay.emojiPopup&&overlay.tool==T_EMOJI&&overlay.emojiIndex==selected&&
                   !overlay.stampPressed&&overlay.anns.empty(),
                   "picker commits selected face on release and clears placement state");
            SendMessageW(form,WM_MOUSEMOVE,0,MAKELPARAM(400,350));
            report(overlay.stampPreviewVisible&&overlay.stampPreviewKey.emojiIndex==selected&&overlay.anns.empty(),
                   "selected Emoji follows pointer before the first canvas click");
            SendMessageW(form,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(400,350));
            report(overlay.stampPressed&&!overlay.stampPreviewVisible&&overlay.anns.size()==1&&
                   overlay.anns.back().emojiIndex==selected,
                   "canvas click commits the selected Emoji and hides its ghost");
            SendMessageW(form,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(420,350));
            report(!overlay.stampPreviewVisible,"holding placement button keeps ghost hidden");
            // 在其他窗口松开鼠标后，覆盖层不会收到对应 WM_LBUTTONUP。
            SendMessageW(form,WM_MOUSEMOVE,0,MAKELPARAM(440,350));
            report(!overlay.stampPressed&&overlay.stampPreviewVisible&&overlay.anns.size()==1,
                   "return after mouse release outside canvas restores ghost without another click");
            overlay.anns.clear();overlay.undoStack.clear();overlay.redoStack.clear();
        }
        for(int tool=T_RECT;tool<=T_EMOJI;++tool){
            POINT point{};
            for(const auto& item:overlay.items)if(item.id==tool)
                point={(LONG)lround(overlay.tbX+item.rect.X+item.rect.Width/2),
                       (LONG)lround(overlay.tbY+item.rect.Y+item.rect.Height/2)};
            SendMessageW(form,WM_RBUTTONDOWN,MK_RBUTTON,MAKELPARAM(point.x,point.y));
            report(overlay.sizeDrag.active&&!overlay.stylePopup&&GetCapture()==form,
                   "all eight tools use inline right-hold sizing instead of popup windows");
            SendMessageW(form,WM_RBUTTONUP,0,MAKELPARAM(point.x,point.y));
        }
        DestroyWindow(form);stampTestForeground=nullptr;
    }
    g_scale=originalScale;std::cout<<"Emoji picker preview checks: "<<checks<<std::endl;return passed;
}

static bool same_brush_pixels(Gdiplus::Bitmap* actual,Gdiplus::Bitmap* expected){
    Gdiplus::Rect rect(0,0,(INT)actual->GetWidth(),(INT)actual->GetHeight());
    Gdiplus::BitmapData a{},b{};
    if(actual->LockBits(&rect,Gdiplus::ImageLockModeRead,PixelFormat32bppRGB,&a)!=Gdiplus::Ok)return false;
    if(expected->LockBits(&rect,Gdiplus::ImageLockModeRead,PixelFormat32bppRGB,&b)!=Gdiplus::Ok){
        actual->UnlockBits(&a);return false;
    }
    bool ok=true;
    for(int y=0;y<rect.Height&&ok;++y)for(int x=0;x<rect.Width*4;x+=4)
        if(abs(((BYTE*)a.Scan0)[y*a.Stride+x]-((BYTE*)b.Scan0)[y*b.Stride+x])>2){ok=false;break;}
    actual->UnlockBits(&a);expected->UnlockBits(&b);return ok;
}

static bool test_brush_cursors(){
    double originalScale=g_scale;HCURSOR originalCursor=GetCursor();bool allPassed=true;
    ensure_class();
    auto report=[&](bool ok,const char* name,int tool,int diameter){
        std::cout<<(ok?"PASS ":"FAIL ")<<name<<" tool="<<tool<<" px="<<diameter<<" scale="<<g_scale<<std::endl;
        logf(L"笔刷光标回归：%s 工具=%d 直径=%d DPI=%.2f 成功=%d",
             u8_to_w(name).c_str(),tool,diameter,g_scale,(int)ok);
        allPassed=allPassed&&ok;
    };
    for(double scale:{1.0,1.75})for(int tool:{T_PEN,T_MOSAIC}){
        g_scale=scale;
        const std::vector<int> sizes=tool==T_PEN?std::vector<int>{1,3,12,30}:std::vector<int>{4,24,60,120};
        for(int diameter:sizes){
            LogTrace trace;
            Overlay overlay;overlay.traceId=trace.id();overlay.hasSel=true;overlay.sel={0,0,320,240};
            overlay.tool=tool;overlay.toolSizes[tool-T_RECT]=diameter;overlay.ink=Gdiplus::Color::Black;
            overlay.shot.vw=320;overlay.shot.vh=240;
            overlay.shot.bmp=new Gdiplus::Bitmap(320,240,PixelFormat32bppRGB);
            {Gdiplus::Graphics graphics(overlay.shot.bmp);graphics.Clear(Gdiplus::Color::White);}
            if(tool==T_MOSAIC){
                overlay.mosaic=new Gdiplus::Bitmap(320,240,PixelFormat32bppRGB);overlay.mosaicKey=overlay.sel;
                Gdiplus::Graphics graphics(overlay.mosaic);graphics.Clear(Gdiplus::Color::Black);
            }
            HWND form=CreateWindowW(OV_CLASS,L"brush-cursor-test",WS_POPUP|WS_CLIPCHILDREN,
                0,0,320,240,nullptr,nullptr,g_hinst,&overlay);
            if(!form){allPassed=false;continue;}
            POINT point{160,120};
            set_cursor_for(&overlay,point);HCURSOR first=overlay.brushCursor;
            set_cursor_for(&overlay,point);
            report(cursor_diameter_matches(first,diameter)&&overlay.brushCursor==first&&GetCursor()==first,
                   "native circular cursor uses exact physical-pixel diameter and cached handle",tool,diameter);
            SendMessageW(form,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(point.x,point.y));
            report(overlay.mode==4&&overlay.hasCur&&overlay.cur.width==diameter&&
                   overlay.brushCursorDiameter==diameter,
                   "actual brush width matches cursor at stroke start",tool,diameter);
            SendMessageW(form,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(point.x,point.y));
            report(overlay.cur.pts.size()==1,"duplicate mouse positions keep a visible circular stamp",tool,diameter);
            overlay.toolSizes[tool-T_RECT]=diameter==sizes.back()?sizes.front():sizes.back();
            set_cursor_for(&overlay,point);
            report(overlay.brushCursor==first&&overlay.brushCursorDiameter==diameter,
                   "active stroke keeps its original footprint",tool,diameter);
            overlay.toolSizes[tool-T_RECT]=diameter;
            SendMessageW(form,WM_LBUTTONUP,0,MAKELPARAM(point.x,point.y));
            std::unique_ptr<Gdiplus::Bitmap> result(render_result(&overlay));
            Gdiplus::Bitmap expected(320,240,PixelFormat32bppRGB);
            {
                Gdiplus::Graphics graphics(&expected);graphics.Clear(Gdiplus::Color::White);
                graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
                Gdiplus::SolidBrush ink(Gdiplus::Color::Black);
                graphics.FillEllipse(&ink,brush_footprint({160,120},(float)diameter));
            }
            Gdiplus::Color source;
            report(result&&overlay.anns.size()==1&&overlay.anns[0].width==diameter&&
                   same_brush_pixels(result.get(),&expected)&&
                   overlay.shot.bmp->GetPixel(160,120,&source)==Gdiplus::Ok&&source.GetValue()==0xffffffff,
                   "saved stamp matches circular footprint without burning cursor into source",tool,diameter);
            if(diameter==sizes.back()){
                Ann original=overlay.anns[0];
                overlay.anns[0].color=Gdiplus::Color(128,0,0,0);
                std::unique_ptr<Gdiplus::Bitmap> alpha(render_result(&overlay));
                Gdiplus::Color middle;
                report(alpha&&alpha->GetPixel(160,120,&middle)==Gdiplus::Ok&&abs((int)middle.GetR()-127)<=1,
                       "brush opacity is preserved",tool,diameter);
                overlay.anns.clear();overlay.tbVisible=false;
                SendMessageW(form,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(80,120));
                SendMessageW(form,WM_LBUTTONUP,0,MAKELPARAM(240,120));
                std::unique_ptr<Gdiplus::Bitmap> line(render_result(&overlay));
                bool correct=line&&overlay.anns.size()==1&&overlay.anns[0].pts.size()==2&&
                             overlay.anns[0].pts.back().X==240;
                for(int y=0;y<240&&correct;++y){
                    Gdiplus::Color pixel;line->GetPixel(160,y,&pixel);
                    if(abs(y-120)<diameter/2-2)correct=pixel.GetR()<3;
                    if(abs(y-120)>diameter/2+2)correct=pixel.GetR()>252;
                }
                report(correct,"continuous stroke keeps the same diameter as its circular cursor",tool,diameter);
                overlay.anns={original};
            }
            int next=diameter==sizes.back()?sizes.front():sizes.back();
            overlay.tbVisible=false;
            POINT sizePoint{30,30};
            SendMessageW(form,WM_RBUTTONDOWN,MK_RBUTTON,MAKELPARAM(sizePoint.x,sizePoint.y));
            int delta=(int)lround((next-diameter)*overlay.sizeDrag.length/
                (tool_size_maximum(tool)-tool_size_minimum(tool)));
            SendMessageW(form,WM_MOUSEMOVE,MK_RBUTTON,MAKELPARAM(sizePoint.x+delta,sizePoint.y));
            SendMessageW(form,WM_RBUTTONUP,0,MAKELPARAM(sizePoint.x+delta,sizePoint.y));
            set_cursor_for(&overlay,point);
            report(cursor_diameter_matches(overlay.brushCursor,next)&&
                   overlay.annotation_width(tool)==next&&overlay.anns[0].width==diameter,
                   "right drag changes cursor and future strokes without resizing existing strokes",tool,diameter);
            overlay.ink=Gdiplus::Color(255,235,40,70);set_cursor_for(&overlay,point);
            report(overlay.brushCursorColor==overlay.ink.GetValue()&&cursor_diameter_matches(overlay.brushCursor,next),
                   "cursor color refresh preserves exact footprint",tool,diameter);
            overlay.tbVisible=true;overlay.tbX=80;overlay.tbY=80;
            bool hiddenOnToolbar=!brush_cursor_at(&overlay,point);
            overlay.tbVisible=false;overlay.panel=PM_ERROR;overlay.panelRect={100,80,100,100};
            bool hiddenOnPanel=!brush_cursor_at(&overlay,point);overlay.panel=PM_HIDDEN;
            overlay.mode=2;bool hiddenWhileResizing=!brush_cursor_at(&overlay,point);overlay.mode=0;
            report(hiddenOnToolbar&&hiddenOnPanel&&hiddenWhileResizing&&!brush_cursor_at(&overlay,{-1,-1}),
                   "brush preview stays out of UI controls and outside selection",tool,diameter);
            set_tool(&overlay,T_RECT);set_cursor_for(&overlay,point);
            report(!brush_cursor_at(&overlay,point)&&GetCursor()!=overlay.brushCursor,
                   "switching tools restores normal pointer",tool,diameter);
            DestroyWindow(form);
            report(!overlay.alive&&overlay.brushCursor==nullptr,
                   "closing overlay releases its custom cursor",tool,diameter);
        }
    }
    SetCursor(originalCursor);g_scale=originalScale;return allPassed;
}

static bool same_stamp_pixels(Bitmap* actual,Bitmap* expected){
    if(actual->GetWidth()!=expected->GetWidth()||actual->GetHeight()!=expected->GetHeight())return false;
    Rect rect(0,0,(INT)actual->GetWidth(),(INT)actual->GetHeight());BitmapData a{},b{};
    if(actual->LockBits(&rect,ImageLockModeRead,PixelFormat32bppPARGB,&a)!=Ok)return false;
    if(expected->LockBits(&rect,ImageLockModeRead,PixelFormat32bppPARGB,&b)!=Ok){
        actual->UnlockBits(&a);return false;
    }
    bool ok=true;
    for(int y=0;y<rect.Height&&ok;++y)for(int x=0;x<rect.Width*4;++x)
        if(abs(((BYTE*)a.Scan0)[y*a.Stride+x]-((BYTE*)b.Scan0)[y*b.Stride+x])>2){ok=false;break;}
    actual->UnlockBits(&a);expected->UnlockBits(&b);return ok;
}

static bool test_stamp_previews(const std::filesystem::path& output){
    double originalScale=g_scale;bool passed=true;int checks=0;ensure_class();
    auto report=[&](bool ok,const char* name){
        ++checks;passed=passed&&ok;
        std::cout<<(ok?"PASS ":"FAIL ")<<name<<" scale="<<g_scale<<std::endl;
        logf(L"落地虚影回归：%s DPI=%.2f 成功=%d",u8_to_w(name).c_str(),g_scale,(int)ok);
    };
    for(double scale:{1.0,1.75})for(int tool:{T_NUMBER,T_EMOJI}){
        g_scale=scale;LogTrace trace;Overlay overlay;overlay.traceId=trace.id();
        overlay.hasSel=true;overlay.sel={50,50,850,650};overlay.tool=tool;
        overlay.shot.vx=-4000;overlay.shot.vy=-3000;overlay.shot.vw=900;overlay.shot.vh=700;
        overlay.shot.bmp=new Bitmap(900,700,PixelFormat32bppRGB);
        {Graphics g(overlay.shot.bmp);g.Clear(Color::White);}
        HWND form=CreateWindowW(OV_CLASS,L"stamp-preview-test",WS_POPUP|WS_CLIPCHILDREN,
            -4000,-3000,900,700,nullptr,nullptr,g_hinst,&overlay);
        if(!form){passed=false;continue;}
        HWND foreground=GetForegroundWindow();stampTestForeground=form;
        POINT pt{400,300};
        for(int size:tool==T_NUMBER?std::vector<int>{12,24,96}:std::vector<int>{12,40,256}){
            overlay.toolSizes[tool-T_RECT]=size;
            update_stamp_preview(&overlay,pt);
            Ann expected=stamp_annotation(&overlay,{});
            report(overlay.stampPreviewVisible&&overlay.stampPreviewKey.kind==expected.kind&&
                   overlay.stampPreviewSide==(int)ceil(expected.bounds().Width)+4&&
                   overlay.anns.empty()&&overlay.numberSeq==0,"hover uses actual stamp size without placing");
            RECT rect{};GetWindowRect(overlay.stampPreview,&rect);
            report(rect.left==overlay.shot.vx+pt.x-overlay.stampPreviewSide/2&&
                   rect.top==overlay.shot.vy+pt.y-overlay.stampPreviewSide/2,
                   "preview center follows pointer on negative-origin desktop");
            // 对比预览位图与同一尺寸真实标注的每个像素，透明度只由分层窗口统一降低。
            HBITMAP bitmap=stamp_preview_bitmap(expected,overlay.stampPreviewSide);
            DIBSECTION dib{};bool pixels=bitmap&&GetObjectW(bitmap,sizeof(dib),&dib);
            if(pixels){
                Bitmap actual(dib.dsBm.bmWidth,dib.dsBm.bmHeight,dib.dsBm.bmWidthBytes,
                              PixelFormat32bppPARGB,(BYTE*)dib.dsBm.bmBits);
                Bitmap reference(overlay.stampPreviewSide,overlay.stampPreviewSide,PixelFormat32bppPARGB);
                {
                    Graphics g(&reference);g.Clear(Color(0,0,0,0));
                    g.SetSmoothingMode(SmoothingModeAntiAlias);g.SetPixelOffsetMode(PixelOffsetModeHalf);
                    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
                    expected.pos={(float)(overlay.stampPreviewSide/2),(float)(overlay.stampPreviewSide/2)};
                    expected.draw(g,nullptr);
                }
                pixels=same_stamp_pixels(&actual,&reference);
            }
            report(pixels&&STAMP_PREVIEW_ALPHA==128,"ghost matches committed stamp pixels at half opacity");
            if(bitmap)DeleteObject(bitmap);
        }
        LONG_PTR flags=GetWindowLongPtrW(overlay.stampPreview,GWL_EXSTYLE);
        report((flags&(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE))==
               (WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE)&&
               SendMessageW(overlay.stampPreview,WM_NCHITTEST,0,0)==HTTRANSPARENT&&
               SendMessageW(overlay.stampPreview,WM_MOUSEACTIVATE,0,0)==MA_NOACTIVATE&&
               GetForegroundWindow()==foreground,"ghost is click-through and never activates");
        ValidateRect(form,nullptr);HWND preview=overlay.stampPreview;
        Ann key=overlay.stampPreviewKey;
        SendMessageW(form,WM_MOUSEMOVE,0,MAKELPARAM(430,330));
        report(overlay.stampPreview==preview&&overlay.stampPreviewKey.index==key.index&&
               overlay.stampPreviewKey.emojiIndex==key.emojiIndex&&!GetUpdateRect(form,nullptr,FALSE),
               "pointer motion reuses preview without repainting screenshot");
        pt={430,330};
        overlay.ink=Color(110,45,160,90);overlay.emojiIndex=1;overlay.numberSeq=8;
        update_stamp_preview(&overlay,pt);
        report(overlay.stampPreviewKey.color.GetValue()==overlay.ink.GetValue()&&
               (tool==T_NUMBER?overlay.stampPreviewKey.index==9:overlay.stampPreviewKey.emojiIndex==1),
               "color next number and selected Emoji refresh preview");
        update_stamp_preview(&overlay,{55,300});
        HRGN region=CreateRectRgn(0,0,0,0);GetWindowRgn(preview,region);
        report(!PtInRegion(region,0,overlay.stampPreviewSide/2)&&
               PtInRegion(region,overlay.stampPreviewSide/2,overlay.stampPreviewSide/2),
               "ghost is clipped at selection edge");DeleteObject(region);
        overlay.tbVisible=true;overlay.tbX=400;overlay.tbY=310;
        update_stamp_preview(&overlay,pt);
        bool toolbarHidden=!overlay.stampPreviewVisible;overlay.tbVisible=false;
        overlay.panel=PM_TEXT;overlay.panelRect=RectF(400,280,100,100);
        update_stamp_preview(&overlay,pt);bool panelHidden=!overlay.stampPreviewVisible;
        overlay.panel=PM_HIDDEN;overlay.mode=2;update_stamp_preview(&overlay,pt);
        bool dragHidden=!overlay.stampPreviewVisible;overlay.mode=0;
        report(toolbarHidden&&panelHidden&&dragHidden,"ghost stays out of toolbar panel and resize operations");
        stampTestForeground=GetDesktopWindow();update_stamp_preview(&overlay,pt);
        report(!overlay.stampPreviewVisible,"inactive screenshot hides ghost");stampTestForeground=form;
        update_stamp_preview(&overlay,pt);SendMessageW(form,WM_MOUSELEAVE,0,0);
        report(!overlay.stampPreviewVisible&&!overlay.stampPreviewTracking,"mouse leave hides ghost");
        update_stamp_preview(&overlay,pt);
        std::unique_ptr<Bitmap> before(render_result(&overlay));
        Bitmap blank(800,600,PixelFormat32bppRGB);
        {Graphics g(&blank);g.Clear(Color::White);}
        report(before&&same_stamp_pixels(before.get(),&blank),"export excludes hovering ghost");
        Ann placement=stamp_annotation(&overlay,{(float)(pt.x-50),(float)(pt.y-50)});
        SendMessageW(form,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(pt.x,pt.y));
        report(overlay.anns.size()==1&&!overlay.stampPreviewVisible&&
               overlay.anns[0].bounds().Width==placement.bounds().Width&&
               overlay.anns[0].pos.X==placement.pos.X&&
               (tool==T_NUMBER?overlay.anns[0].index==9:overlay.anns[0].emojiIndex==1),
               "click commits exactly the previewed stamp and hides ghost");
        SendMessageW(form,WM_LBUTTONUP,0,MAKELPARAM(pt.x,pt.y));
        std::unique_ptr<Bitmap> after(render_result(&overlay));
        Bitmap reference(800,600,PixelFormat32bppRGB);
        {
            Graphics g(&reference);g.Clear(Color::White);
            g.SetSmoothingMode(SmoothingModeAntiAlias);g.SetPixelOffsetMode(PixelOffsetModeHalf);
            g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
            placement.draw(g,nullptr);
        }
        report(after&&same_stamp_pixels(after.get(),&reference),"export includes only committed stamp");
        if(tool==T_NUMBER){
            undo(&overlay);update_stamp_preview(&overlay,pt);bool undone=overlay.stampPreviewKey.index==1;
            redo(&overlay);update_stamp_preview(&overlay,pt);
            report(undone&&overlay.stampPreviewKey.index==10,"undo redo refresh next number without consuming it");
        }
        DestroyWindow(form);stampTestForeground=nullptr;
        report(!overlay.stampPreview&&!overlay.stampPreviewVisible,"overlay close releases preview window");
    }
    // 同页展示真实标注和半透明落地虚影，检查 100%/175% DPI 的可视大小。
    for(double scale:{1.0,1.75}){
        g_scale=scale;Bitmap sheet(1100,540,PixelFormat32bppRGB);
        {
            Graphics g(&sheet);g.Clear(Color(255,245,247,246));
            g.SetSmoothingMode(SmoothingModeAntiAlias);g.SetPixelOffsetMode(PixelOffsetModeHalf);
            g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
            Font font(FONT_UI(),20,FontStyleRegular,UnitPixel);SolidBrush ink(Color(255,45,55,50));
            g.DrawString(L"Committed stamp                         Hover preview (50%)",-1,&font,PointF(65,20),&ink);
            int row=0;
            for(int tool:{T_NUMBER,T_EMOJI}){
                Overlay o;o.tool=tool;o.numberSeq=11;o.emojiIndex=1;
                o.toolSizes[tool-T_RECT]=tool==T_NUMBER?72:180;
                Ann stamp=stamp_annotation(&o,{180.0f,150.0f+row*230});
                stamp.draw(g,nullptr);
                int side=(int)ceil(stamp.bounds().Width)+4;HBITMAP bitmap=stamp_preview_bitmap(stamp,side);
                DIBSECTION dib{};
                if(bitmap&&GetObjectW(bitmap,sizeof(dib),&dib)){
                    Bitmap image(side,side,side*4,PixelFormat32bppPARGB,(BYTE*)dib.dsBm.bmBits);
                    ColorMatrix matrix={1,0,0,0,0,0,1,0,0,0,0,0,1,0,0,0,0,0,STAMP_PREVIEW_ALPHA/255.0f,0,0,0,0,0,1};
                    ImageAttributes attr;attr.SetColorMatrix(&matrix);
                    g.DrawImage(&image,Rect(740-side/2,150+row*230-side/2,side,side),0,0,side,side,UnitPixel,&attr);
                }
                if(bitmap)DeleteObject(bitmap);++row;
            }
        }
        if(!save_bitmap_file(&sheet,(output/(scale==1?L"stamp-previews-100.png":L"stamp-previews-175.png")).wstring(),92))passed=false;
    }
    g_scale=originalScale;std::cout<<"Stamp preview checks: "<<checks<<std::endl;return passed;
}

static bool test_images_directory(const std::filesystem::path& output){
    double originalScale=g_scale;bool passed=true;int checks=0;LogTrace trace;
    auto report=[&](bool ok,const char* name){
        ++checks;passed=passed&&ok;
        std::cout<<(ok?"PASS ":"FAIL ")<<name<<" scale="<<g_scale<<std::endl;
        logf(L"images 自定义图片回归：%s 成功=%d",u8_to_w(name).c_str(),(int)ok);
    };
    auto root=output/L"isolated"/(L"images-"+std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(root/L"emoji");
    auto save=[&](const std::filesystem::path& path,int w,int h,const wchar_t* mime){
        Bitmap image(w,h,PixelFormat32bppARGB);
        {Graphics g(&image);g.Clear(Color(255,240,45,30));}
        UINT count=0,length=0;GetImageEncodersSize(&count,&length);std::vector<BYTE> buffer(length);
        auto* encoders=(ImageCodecInfo*)buffer.data();GetImageEncoders(count,length,encoders);
        for(UINT i=0;i<count;++i)if(!wcscmp(encoders[i].MimeType,mime))
            return image.Save(path.c_str(),&encoders[i].Clsid)==Ok;
        return false;
    };
    imageTestExe=(root/L"fixture.exe").wstring();
    report(save(root/L"emoji"/L"00.png",8,8,L"image/png")&&emoji_reload()==0,
           "only executable-adjacent images is scanned; legacy emoji is ignored");
    std::filesystem::create_directories(root/L"images");
    report(emoji_reload()==0&&emoji_count()==0,"empty images directory has no built-in fallback");
    bool fixtures=save(root/L"images"/L"001-wide.JPG",3200,400,L"image/jpeg")&&
        save(root/L"images"/L"002-portrait.png",320,2400,L"image/png");
    // 两帧 GIF：首帧红色、第二帧蓝色。
    const BYTE gif[]={
        'G','I','F','8','9','a',2,0,1,0,0x80,0,0,255,0,0,0,0,255,
        0x21,0xf9,4,0,10,0,0,0,0x2c,0,0,0,0,2,0,1,0,0,2,2,4,10,0,
        0x21,0xf9,4,0,10,0,0,0,0x2c,0,0,0,0,2,0,1,0,0,2,2,76,10,0,0x3b
    };
    {std::ofstream file(root/L"images"/L"003-animation.GIF",std::ios::binary);
     file.write((const char*)gif,sizeof(gif));fixtures=fixtures&&file.good();}
    {std::ofstream bad(root/L"images"/L"004-broken.png");bad<<"not an image";}
    {std::ofstream note(root/L"images"/L"005-notes.txt");note<<"not a stamp";}
    std::filesystem::create_directories(root/L"images"/L"nested");
    fixtures=fixtures&&save(root/L"images"/L"nested"/L"grouped.png",8,8,L"image/png");
    for(int i=0;i<100;++i)
        fixtures=save(root/L"images"/(L"tile-"+std::to_wstring(100+i)+L".png"),8,8,L"image/png")&&fixtures;
    report(fixtures,"mixed-format large images animated GIF and 100 extra pictures created");
    for(double scale:{1.0,1.75}){
        g_scale=scale;
        report(emoji_reload()==104&&std::wstring(emoji_text(0))==L"001-wide.JPG"&&
               std::wstring(emoji_text(2))==L"003-animation.GIF",
               "supported files and folder groups are discovered; corrupt files are skipped");
        bool thumbnailsBounded=true;
        for(int i=0;i<emoji_count();++i){
            auto* image=emoji_bitmap(i);
            thumbnailsBounded=thumbnailsBounded&&image&&
                std::max(image->GetWidth(),image->GetHeight())<=(UINT)ceil(std::max(26.0f,SF(26)));
        }
        report(thumbnailsBounded,"catalog keeps small thumbnails and loads placement-sized images on selection");
        Bitmap animated((root/L"images"/L"003-animation.GIF").c_str());
        GUID time=FrameDimensionTime;Color firstFrame;auto gifImage=emoji_image(2);
        if(gifImage)gifImage->GetPixel(0,0,&firstFrame);
        report(animated.GetFrameCount(&time)==2&&gifImage&&firstFrame.GetR()>200&&firstFrame.GetB()<20,
               "animated GIF uses first frame for static screenshot");
        bool bounded=true;
        for(int i=0;i<emoji_count();++i){
            auto image=emoji_image(i);
            bounded=bounded&&image&&std::max(image->GetWidth(),image->GetHeight())<=(UINT)ceil(SF(256));
        }
        report(bounded,"large source pictures use bounded caches without retaining file locks");
        Overlay overlay;overlay.traceId=trace.id();overlay.hasSel=true;overlay.sel={50,50,650,650};
        overlay.shot.vx=-4000;overlay.shot.vy=-3000;overlay.shot.vw=700;overlay.shot.vh=800;
        overlay.shot.bmp=new Bitmap(700,800,PixelFormat32bppRGB);
        {Graphics g(overlay.shot.bmp);g.Clear(Color::White);}
        HWND form=CreateWindowW(OV_CLASS,L"custom-images-test",WS_POPUP|WS_CLIPCHILDREN,
            -4000,-3000,700,800,nullptr,nullptr,g_hinst,&overlay);
        if(!form){passed=false;continue;}
        stampTestForeground=form;overlay.tool=T_EMOJI;
        for(int index:{0,1,2})for(int size:{12,40,256}){
            overlay.emojiIndex=index;overlay.emojiImage=emoji_image(index);
            overlay.toolSizes[T_EMOJI-T_RECT]=size;overlay.tbVisible=false;
            auto image=overlay.emojiImage;
            RectF footprint=stamp_image_rect(image.get(),{300,300},SF((float)size));
            float sourceRatio=(float)image->GetWidth()/image->GetHeight();
            report(abs(std::max(footprint.Width,footprint.Height)-SF((float)size))<.01f&&
                   abs(footprint.Width/footprint.Height-sourceRatio)<.01f,
                   "JPG PNG GIF preserve aspect ratio within configured size at every DPI");
            update_stamp_preview(&overlay,{350,350});
            report(overlay.stampPreviewVisible&&overlay.stampPreviewKey.emojiImage==image&&
                   overlay.stampPreviewSide==(int)ceil(SF((float)size))+4,
                   "custom picture ghost uses selected image and software size");
            SendMessageW(form,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(350,350));
            Ann stamp=overlay.anns.back();
            report(stamp.emojiImage==image&&stamp.fontSize==size,
                   "placement keeps selected image independent of catalog indices");
            std::unique_ptr<Bitmap> result(render_result(&overlay));
            Bitmap expected(600,600,PixelFormat32bppRGB);
            {
                Graphics g(&expected);g.Clear(Color::White);
                g.SetSmoothingMode(SmoothingModeAntiAlias);g.SetPixelOffsetMode(PixelOffsetModeHalf);
                g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
                stamp.draw(g,nullptr);
            }
            report(result&&same_stamp_pixels(result.get(),&expected),"custom picture export contains exact scaled placement only");
            overlay.anns.clear();SendMessageW(form,WM_LBUTTONUP,0,MAKELPARAM(350,350));
        }
        overlay.tbVisible=true;overlay.tbX=0;overlay.tbY=680;
        show_emoji_picker(&overlay);HWND picker=overlay.emojiPopup;
        SCROLLINFO scroll{sizeof(scroll),SIF_ALL};if(picker)GetScrollInfo(picker,SB_VERT,&scroll);
        report(picker&&scroll.nMax==12&&scroll.nPage>0&&scroll.nPage<=9,
               "more than 88 images have a bounded scrollable picker");
        SendMessageW(picker,WM_VSCROLL,SB_BOTTOM,0);GetScrollInfo(picker,SB_VERT,&scroll);
        POINT target=image_picker_point(picker,102);
        SendMessageW(picker,WM_LBUTTONUP,0,MAKELPARAM(target.x,target.y));
        report(!overlay.emojiPopup&&overlay.emojiIndex==102&&overlay.emojiImage,
               "last image is selectable after scrolling beyond first page");
        overlay.tbVisible=true;show_emoji_picker(&overlay);picker=overlay.emojiPopup;
        std::wstring suffix=scale==1?L"-100":L"-175";
        report(picker&&png(picker,(output/(L"images-picker"+suffix+L".png")).wstring()),
               "custom images picker snapshot rendered");
        close_emoji_popup(&overlay);DestroyWindow(form);stampTestForeground=nullptr;
    }
    Ann retained;retained.kind=AK_EMOJI;retained.emojiImage=emoji_image(0);retained.fontSize=40;
    auto oldImage=retained.emojiImage;
    std::filesystem::remove(root/L"images"/L"001-wide.JPG");
    report(emoji_reload()==103&&retained.emojiImage==oldImage&&oldImage->GetWidth()>oldImage->GetHeight()&&
           emoji_image(0)->GetWidth()<emoji_image(0)->GetHeight(),
           "directory refresh detects removal without changing already placed pictures");
    report(save(root/L"images"/L"000-added.jpeg",6,12,L"image/jpeg")&&emoji_reload()==104&&
           std::wstring(emoji_text(0))==L"000-added.jpeg",
           "reopening detects new JPEG files without restarting app");
    imageTestExe.clear();emoji_reload();g_scale=originalScale;
    std::cout<<"Images directory checks: "<<checks<<std::endl;return passed;
}

#include "gif_export_tests.inc"
#include "image_groups_tests.inc"
#include "text_cursor_tests.inc"
#include "text_transform_tests.inc"
#include "annotation_moving_tests.inc"
#include "tray_tests.inc"

static bool brush_cursor_snapshot(const std::filesystem::path& output,const std::wstring& suffix){
    Gdiplus::Bitmap image(S(720),S(420),PixelFormat32bppRGB);
    Gdiplus::Bitmap mosaic(S(720),S(420),PixelFormat32bppRGB);
    {Gdiplus::Graphics graphics(&mosaic);
     for(int y=0;y<S(420);y+=9)for(int x=0;x<S(720);x+=9){
        Gdiplus::SolidBrush tile(Gdiplus::Color(255,(BYTE)(80+x/9%5*30),(BYTE)(90+y/9%5*25),140));
        graphics.FillRectangle(&tile,x,y,9,9);
     }}
    {
        Gdiplus::Graphics graphics(&image);graphics.Clear(Gdiplus::Color(255,245,248,246));
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        Gdiplus::Font font(L"Microsoft YaHei UI",SF(15),Gdiplus::FontStyleRegular,Gdiplus::UnitPixel);
        Gdiplus::SolidBrush textColor(Gdiplus::Color(255,35,45,42));
        graphics.DrawString(L"鼠标尺寸预览",-1,&font,Gdiplus::PointF(SF(80),SF(24)),&textColor);
        graphics.DrawString(L"实际落笔范围",-1,&font,Gdiplus::PointF(SF(440),SF(24)),&textColor);
        for(int row=0;row<2;++row){
            int tool=row?T_MOSAIC:T_PEN,diameter=row?72:24;
            auto text=std::wstring(row?L"马赛克  ":L"画笔  ")+std::to_wstring(diameter)+L" px";
            float y=SF(row?290.0f:130.0f);
            graphics.DrawString(text.c_str(),-1,&font,Gdiplus::PointF(SF(24),y+65),&textColor);
            Gdiplus::Color ink(255,235,40,70);
            HCURSOR cursor=make_brush_cursor(tool,diameter,ink);
            if(!cursor)return false;
            int side=diameter+4,hot=2+diameter/2;
            HDC dc=CreateCompatibleDC(nullptr);BITMAPINFO info{};
            info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=side;info.bmiHeader.biHeight=-side;
            info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
            void* pixels=nullptr;HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
            if(!dc||!bitmap||!pixels){if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);DestroyCursor(cursor);return false;}
            std::fill_n((DWORD*)pixels,side*side,0xfff5f8f6);
            auto previous=SelectObject(dc,bitmap);
            DrawIconEx(dc,0,0,cursor,side,side,0,nullptr,DI_NORMAL);GdiFlush();
            {
                Gdiplus::Bitmap preview(side,side,side*4,PixelFormat32bppRGB,(BYTE*)pixels);
                graphics.DrawImage(&preview,Gdiplus::Rect(S(150)-hot,(int)y-hot,side,side),
                                   0,0,side,side,Gdiplus::UnitPixel);
            }
            SelectObject(dc,previous);DeleteObject(bitmap);DeleteDC(dc);DestroyCursor(cursor);
            Ann stamp;stamp.kind=row?AK_MOSAIC:AK_PEN;stamp.width=(float)diameter;
            stamp.color=ink;stamp.pts.push_back({SF(510),y});
            stamp.draw(graphics,&mosaic);
        }
    }
    return save_bitmap_file(&image,(output/(L"brush-cursors"+suffix+L".png")).wstring(),92);
}

int wmain(int argc,wchar_t**argv){
    if(argc<2)return 2;
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    CoInitializeSecurity(nullptr,-1,nullptr,nullptr,RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
        RPC_C_IMP_LEVEL_IMPERSONATE,nullptr,EOAC_NONE,nullptr);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_hinst=GetModuleHandleW(nullptr);
    Gdiplus::GdiplusStartupInput input;ULONG_PTR token=0;Gdiplus::GdiplusStartup(&token,&input,nullptr);
    INITCOMMONCONTROLSEX init{sizeof(init),ICC_STANDARD_CLASSES|ICC_BAR_CLASSES|ICC_HOTKEY_CLASS|ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&init);g_cfg=load_settings();
    std::filesystem::path output=argv[1];std::filesystem::create_directories(output);
    // Interactions with the color picker must not overwrite the user's history.
    auto isolated=output/L"isolated"/(L"session-"+std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(isolated);
    SetEnvironmentVariableW(L"APPDATA",isolated.c_str());
    SetEnvironmentVariableW(L"LOCALAPPDATA",isolated.c_str());
    WNDCLASSW wc{};wc.lpfnWndProc=cfg_proc;wc.hInstance=g_hinst;wc.lpszClassName=L"weijieSettingsPreview";RegisterClassW(&wc);
    WNDCLASSW pc{};pc.lpfnWndProc=proc;pc.hInstance=g_hinst;pc.lpszClassName=CLASS;RegisterClassW(&pc);
    if(!test_tray_lifecycle())return 32;
    if(!test_settings_save(output,wc.lpszClassName))return 13;
    if(!test_idle_timeout_settings(output,wc.lpszClassName))return 17;
    if(!test_settings_capture())return 14;
    if(!test_toolbar_switches())return 15;
    if(!test_immediate_ocr_disable(wc.lpszClassName))return 16;
    if(!test_longshot_preview(output))return 18;
    if(!test_slider_mouse_input(wc.lpszClassName))return 22;
    if(!test_emoji_size_drag())return 23;
    if(!test_size_drag())return 29;
    if(!test_emoji_picker_preview())return 25;
    if(!test_brush_cursors())return 20;
    if(!test_stamp_previews(output))return 24;
    if(!test_images_directory(output))return 26;
    if(!test_gif_export(output))return 27;
    if(!test_image_groups(output))return 28;
    if(!test_text_cursors(output))return 30;
    if(!test_text_transforms(output))return 31;
    if(!test_annotation_moving(output))return 33;
    int files=10;
    for(double scale:{1.0,1.75}){
        g_scale=scale;
        std::wstring suffix=scale==1.0?L"-100":L"-175";
        if(!brush_cursor_snapshot(output,suffix))return 21;
        ++files;
        CfgDlg dlg{};
        HWND form=CreateWindowW(wc.lpszClassName,L"preview",WS_POPUP|WS_CLIPCHILDREN,
            0,0,S(840),S(648),nullptr,nullptr,g_hinst,&dlg);
        if(!form)return 3;
        for(int i=0;i<3;++i){dlg.page=i;choose_page(&dlg);
            for(int page=0;page<3;++page)for(HWND child:dlg.pages[page])
                if(bool(GetWindowLongW(child,GWL_STYLE)&WS_VISIBLE)!=(page==i))return 7;
            if(!png(form,(output/(L"settings-"+std::to_wstring(i)+suffix+L".png")).wstring()))return 4;
            ++files;
        }
        SendMessageW(form,WM_COMMAND,201,0);
        if(dlg.page!=0)return 8;
        SendMessageW(dlg.quality,TBM_SETPOS,TRUE,67);
        SendMessageW(form,WM_HSCROLL,TB_THUMBPOSITION,(LPARAM)dlg.quality);
        if(text(dlg.qualityVal)!=L"67")return 9;
        DestroyWindow(form);DeleteObject(dlg.font);
        auto* longImage=new Gdiplus::Bitmap(760,2400,PixelFormat32bppRGB);
        {
            Gdiplus::Graphics graphics(longImage);graphics.Clear(Gdiplus::Color::White);
            Gdiplus::Font title(L"Microsoft YaHei UI",24),body(L"Microsoft YaHei UI",16);
            Gdiplus::SolidBrush ink(Gdiplus::Color(255,35,45,42)),green(Gdiplus::Color(255,7,193,96));
            graphics.DrawString(L"滚动截图 · 连续内容预览",-1,&title,Gdiplus::PointF(28,24),&green);
            for(int row=0;row<38;++row){
                auto line=std::to_wstring(row+1)+L"  自动滚动拼接，保留完整内容。";
                graphics.DrawString(line.c_str(),-1,&body,Gdiplus::PointF(28.0f,88.0f+row*56),&ink);
            }
        }
        HWND longForm=create_longshot_preview(longImage,L"页面连续三次未继续滚动，已完成");
        if(!longForm||!png(longForm,(output/(L"longshot"+suffix+L".png")).wstring()))return 19;
        ++files;DestroyWindow(longForm);
        Overlay ov;ov.hwnd=CreateWindowW(L"STATIC",L"preview",WS_POPUP,0,0,S(400),S(600),nullptr,nullptr,g_hinst,nullptr);
        ov.recentColors={0xfff25656,0xfff4b843,0xff07c160,0xff3983ef,0xff9a63df,0xff29332d,0xffffffff};
        for(int tool=T_RECT;tool<=T_EMOJI;++tool){
            if(!size_drag_snapshot(&ov,tool,(output/(L"tool-"+std::to_wstring(tool)+suffix+L".png")).wstring()))return 6;
            ++files;
        }
        auto* data=new Popup{&ov};
        HWND pop=CreateWindowW(CLASS,L"preview",WS_POPUP|WS_CLIPCHILDREN,0,0,
            S(360),S(546),ov.hwnd,nullptr,g_hinst,data);
        if(!pop||!png(pop,(output/(L"color"+suffix+L".png")).wstring()))return 6;
        SetWindowTextW(data->hex,L"#36A875");
        SendMessageW(pop,WM_COMMAND,MAKEWPARAM(3,EN_KILLFOCUS),(LPARAM)data->hex);
        if((ov.ink.GetValue()&0xffffff)!=0x36a875)return 11;
        SendMessageW(data->alpha,TBM_SETPOS,TRUE,50);
        SendMessageW(pop,WM_HSCROLL,TB_THUMBPOSITION,(LPARAM)data->alpha);
        if(abs((int)ov.ink.GetA()-128)>1)return 12;
        ++files;DestroyWindow(pop);
        DestroyWindow(ov.hwnd);ov.hwnd=nullptr;
    }
    std::cout<<"PASS rendered "<<files<<" UI snapshots at 100% and 175% DPI\n"
             <<"PASS 3-page visibility / navigation / JPEG quality slider\n"
             <<"PASS all 8 right-drag sliders / HEX color / transparency\n";
    return 0;
}
