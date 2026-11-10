// Native UI regression renderer: real controls and WndProcs, no mouse injection.
#define wWinMain weijieUnusedEntry
#include "../src/main.cpp"
#undef wWinMain
#include "../src/style_popup.cpp"
#include <iostream>

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
int wmain(int argc,wchar_t**argv){
    if(argc<2)return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_hinst=GetModuleHandleW(nullptr);
    Gdiplus::GdiplusStartupInput input;ULONG_PTR token=0;Gdiplus::GdiplusStartup(&token,&input,nullptr);
    INITCOMMONCONTROLSEX init{sizeof(init),ICC_STANDARD_CLASSES|ICC_BAR_CLASSES|ICC_HOTKEY_CLASS|ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&init);g_cfg=load_settings();
    std::filesystem::path output=argv[1];std::filesystem::create_directories(output);
    // Interactions with the color picker must not overwrite the user's history.
    std::filesystem::create_directories(output/L"isolated");
    SetEnvironmentVariableW(L"APPDATA",(output/L"isolated").c_str());
    WNDCLASSW wc{};wc.lpfnWndProc=cfg_proc;wc.hInstance=g_hinst;wc.lpszClassName=L"weijieSettingsPreview";RegisterClassW(&wc);
    WNDCLASSW pc{};pc.lpfnWndProc=proc;pc.hInstance=g_hinst;pc.lpszClassName=CLASS;RegisterClassW(&pc);
    int files=0;
    for(double scale:{1.0,1.75}){
        g_scale=scale;
        std::wstring suffix=scale==1.0?L"-100":L"-175";
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
        Overlay ov;ov.hwnd=CreateWindowW(L"STATIC",L"preview",WS_POPUP,0,0,S(400),S(600),nullptr,nullptr,g_hinst,nullptr);
        ov.recentColors={0xfff25656,0xfff4b843,0xff07c160,0xff3983ef,0xff9a63df,0xff29332d,0xffffffff};
        for(int tool=T_RECT;tool<=T_NUMBER+1;++tool){
            bool color=tool==T_NUMBER+1;
            auto* data=new Popup{&ov,color,color?T_RECT:tool};
            HWND pop=CreateWindowW(CLASS,L"preview",WS_POPUP|WS_CLIPCHILDREN,0,0,
                                  S(color?360:336),S(color?546:244),ov.hwnd,nullptr,g_hinst,data);
            if(!pop)return 5;
            std::wstring name=color?L"color":L"tool-"+std::to_wstring(tool);
            if(!png(pop,(output/(name+suffix+L".png")).wstring()))return 6;
            if(!color){
                auto* data=(Popup*)GetWindowLongPtrW(pop,GWLP_USERDATA);
                SendMessageW(data->slider,TBM_SETPOS,TRUE,high(data));
                SendMessageW(pop,WM_HSCROLL,TB_THUMBPOSITION,(LPARAM)data->slider);
                if(ov.toolSizes[tool-T_RECT]!=high(data))return 10;
            }else{
                auto* data=(Popup*)GetWindowLongPtrW(pop,GWLP_USERDATA);
                SetWindowTextW(data->hex,L"#36A875");
                SendMessageW(pop,WM_COMMAND,MAKEWPARAM(3,EN_KILLFOCUS),(LPARAM)data->hex);
                if((ov.ink.GetValue()&0xffffff)!=0x36a875)return 11;
                SendMessageW(data->alpha,TBM_SETPOS,TRUE,50);
                SendMessageW(pop,WM_HSCROLL,TB_THUMBPOSITION,(LPARAM)data->alpha);
                if(abs((int)ov.ink.GetA()-128)>1)return 12;
            }
            ++files;DestroyWindow(pop);
        }
        DestroyWindow(ov.hwnd);ov.hwnd=nullptr;
    }
    std::cout<<"PASS rendered "<<files<<" UI snapshots at 100% and 175% DPI\n"
             <<"PASS 3-page visibility / navigation / JPEG quality slider\n"
             <<"PASS all 7 tool sliders / HEX color / transparency\n";
    return 0;
}
