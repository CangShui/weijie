static HWND setting_check(CfgDlg*d,int page,const wchar_t*t,int x,int y,int w,int id,bool value){
    HWND h=control(d,page,L"BUTTON",t,BS_AUTOCHECKBOX|WS_TABSTOP,x,y,w,28,id);
    SendMessageW(h,BM_SETCHECK,value?BST_CHECKED:BST_UNCHECKED,0);return h;
}
static HWND setting_edit(CfgDlg*d,int page,int x,int y,int w,int id,bool password=false){
    // Parent paints the rounded field border; the EDIT remains a native,
    // borderless input with proper IME, selection and password semantics.
    return control(d,page,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL|(password?ES_PASSWORD:0),
                   x+11,y+8,w-22,23,id);
}
static void setting_hint(CfgDlg*d,int page,const wchar_t*t,int x,int y,int w,int height=25){
    HWND h=control(d,page,L"STATIC",t,SS_LEFT,x,y,w,height);
    SetPropW(h,L"JjMuted",(HANDLE)1);
}
static void cfg_paint(CfgDlg*d,HDC dc){
    using namespace Gdiplus;RECT rc{};GetClientRect(d->hwnd,&rc);
    Bitmap b(rc.right,rc.bottom,PixelFormat32bppARGB);
    b.SetResolution((REAL)(96*g_scale),(REAL)(96*g_scale));
    Graphics g(&b);g.SetPageUnit(UnitPixel);
    g.Clear(ui::canvas());g.SetSmoothingMode(SmoothingModeAntiAlias);g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    auto r=[](float x,float y,float w,float h){return RectF(SF(x),SF(y),SF(w),SF(h));};
    ui::box(g,r(24,27,30,30),ui::green(),8);
    Pen icon(Color::White,SF(1.5));g.DrawRectangle(&icon,SF(32),SF(35),SF(14),SF(12));
    ui::text(g,L"微截",r(66,23,112,32),20,ui::ink(),true);
    ui::text(g,L"让截图更简单",r(24,64,156,22),12,ui::muted());
    ui::text(g,L"1.5  ·  原生桌面版",r(24,524,156,25),11,ui::muted());
    const wchar_t* names[]={L"截图与快捷键",L"文字识别",L"翻译服务"};
    const wchar_t* desc[]={L"按你的习惯，设置每一次截图",L"本地识别，按需启用",L"为提取的文字选择翻译服务"};
    ui::text(g,names[d->page],r(228,21,560,34),21,ui::ink(),true);
    ui::text(g,desc[d->page],r(228,58,560,23),12,ui::muted());
    ui::box(g,r(204,100,612,458),Color::White,12,ui::line());
    if(d->page==0){
        ui::separator(g,226,218,568);
        ui::separator(g,226,352,568);
    }else if(d->page==1){
        ui::separator(g,226,216,568);
    }else{
        ui::box(g,r(226,190,568,62),ui::tint(),8);
        ui::text(g,L"仅点击“翻译”时发送识别文字，截图始终保留在本地。\nAPI Key 留空时使用 MyMemory 翻译服务。",
                 r(242,198,536,46),12,Color(255,33,116,72));
    }
    // Rounded borders behind native input controls.
    for(HWND h:d->pages[d->page]){
        wchar_t cls[64]{};GetClassNameW(h,cls,64);
        bool isEdit=!_wcsicmp(cls,L"EDIT"),isHot=!_wcsicmp(cls,HOTKEY_CLASSW);
        if(!isEdit&&!isHot)continue;
        RECT wr;GetWindowRect(h,&wr);MapWindowPoints(HWND_DESKTOP,d->hwnd,(POINT*)&wr,2);
        RectF field((REAL)wr.left-SF(10),(REAL)wr.top-SF(7),
                    (REAL)(wr.right-wr.left)+SF(20),(REAL)(wr.bottom-wr.top)+SF(14));
        if(isHot)field=RectF((REAL)wr.left-SF(5),(REAL)wr.top-SF(4),
                           (REAL)(wr.right-wr.left)+SF(10),(REAL)(wr.bottom-wr.top)+SF(8));
        ui::box(g,field,Color::White,7,GetFocus()==h?ui::green():ui::line());
    }
    ui::separator(g,24,576,792);
    ui::text(g,L"更改将在保存后生效",r(228,594,300,34),12,ui::muted());
    Graphics out(dc);out.SetPageUnit(UnitPixel);out.DrawImage(&b,0,0,rc.right,rc.bottom);
}
static void cfg_layout(CfgDlg*d){
    d->font=CreateFontW(-S(13),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
    const wchar_t* names[]={L"截图与快捷键",L"文字识别",L"翻译服务"};
    for(int i=0;i<3;++i){
        d->nav[i]=control(d,-1,L"BUTTON",names[i],WS_TABSTOP,16,112+i*52,164,42,201+i);
        SetPropW(d->nav[i],L"JjNav",(HANDLE)1);
    }
    d->monitor=setting_check(d,0,L"启用全局截图快捷键",226,115,530,16,g_cfg.hotkey_enabled);
    label(d,0,L"截图快捷键",226,161,132);
    d->hotkey=control(d,0,HOTKEY_CLASSW,L"",WS_TABSTOP,379,160,260,26,100);
    UINT mods=0,vk=0;resolve_hotkey(g_cfg.hotkey,mods,vk);
    BYTE flags=((mods&MOD_CONTROL)?HOTKEYF_CONTROL:0)|((mods&MOD_ALT)?HOTKEYF_ALT:0)|((mods&MOD_SHIFT)?HOTKEYF_SHIFT:0);
    SendMessageW(d->hotkey,HKM_SETHOTKEY,MAKEWORD(vk,flags),0);
    setting_hint(d,0,L"点击后按组合键 · Ctrl / Alt 或 F1–F24",374,190,410,22);
    label(d,0,L"保存位置",226,239,132);
    d->saveDir=setting_edit(d,0,374,231,322,102);
    SetWindowTextW(d->saveDir,(g_cfg.save_dir.empty()?default_save_dir():g_cfg.save_dir).c_str());
    control(d,0,L"BUTTON",L"浏览",WS_TABSTOP,708,231,84,36,10);
    label(d,0,L"保存格式",226,290,132);
    d->format=control(d,0,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP,374,283,190,180,101);
    for(auto t:{L"PNG · 无损",L"JPEG · 有损",L"BMP · 无损"})SendMessageW(d->format,CB_ADDSTRING,0,(LPARAM)t);
    SendMessageW(d->format,CB_SETCURSEL,g_cfg.image_format==L"bmp"?2:g_cfg.image_format==L"jpg"?1:0,0);
    label(d,0,L"JPEG 质量",226,328,132);
    d->quality=control(d,0,TRACKBAR_CLASSW,L"",WS_TABSTOP|TBS_HORZ,368,320,338,28,20);
    SendMessageW(d->quality,TBM_SETRANGE,TRUE,MAKELPARAM(1,100));SendMessageW(d->quality,TBM_SETPOS,TRUE,g_cfg.jpeg_quality);
    d->qualityVal=control(d,0,L"STATIC",std::to_wstring(g_cfg.jpeg_quality).c_str(),SS_RIGHT,726,324,64,24);
    label(d,0,L"线宽 / 字号",226,375,132);
    d->stroke=setting_edit(d,0,374,365,66,103);SetWindowTextW(d->stroke,std::to_wstring(g_cfg.stroke_width).c_str());
    d->fontSize=setting_edit(d,0,452,365,66,104);SetWindowTextW(d->fontSize,std::to_wstring(g_cfg.text_size).c_str());
    setting_hint(d,0,L"截图中也可右键调整",538,375,254,24);
    d->autoCopy=setting_check(d,0,L"保存后同时复制到剪贴板",226,415,550,12,g_cfg.auto_copy);
    d->includeCursor=setting_check(d,0,L"截图包含鼠标指针",226,456,550,13,g_cfg.include_cursor);
    d->autostart=setting_check(d,0,L"开机自动启动",226,497,550,15,autostart_enabled());

    d->ocrEnabled=setting_check(d,1,L"启用文字识别",226,115,550,14,g_cfg.ocr_enabled);
    setting_hint(d,1,L"关闭后隐藏工具条中的识别和翻译按钮。",226,154,566,25);
    label(d,1,L"识别引擎",226,239,132);
    d->ocrEngine=control(d,1,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP,374,231,418,200,109);
    for(auto t:{L"混元 OCR · 高精度 Q8",L"混元 OCR · 轻量 Q3",L"PP-OCRv6 · ncnn Vulkan · 极速"})SendMessageW(d->ocrEngine,CB_ADDSTRING,0,(LPARAM)t);
    SendMessageW(d->ocrEngine,CB_SETCURSEL,g_cfg.ocr_engine==L"ppocr"?2:g_cfg.ocr_engine==L"hunyuan_fast"?1:0,0);
    label(d,1,L"推理设备",226,287,132);
    d->ocrBackend=control(d,1,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP,374,279,418,200,110);
    SendMessageW(d->ocrBackend,CB_ADDSTRING,0,(LPARAM)L"自动 · CPU 低延迟优先");SendMessageW(d->ocrBackend,CB_SETITEMDATA,0,0);
    SendMessageW(d->ocrBackend,CB_ADDSTRING,0,(LPARAM)L"CPU");SendMessageW(d->ocrBackend,CB_SETITEMDATA,1,1);
    const bool gpuAvailable=fast_ocr_gpu_available()||gpu_ocr_supported();
    if(gpuAvailable){
        SendMessageW(d->ocrBackend,CB_ADDSTRING,0,(LPARAM)L"GPU · Vulkan（Intel / AMD / NVIDIA）");
        SendMessageW(d->ocrBackend,CB_SETITEMDATA,2,2);
    }
    SendMessageW(d->ocrBackend,CB_SETCURSEL,
                 g_cfg.ocr_backend==L"gpu"?(gpuAvailable?2:1):g_cfg.ocr_backend==L"cpu"?1:0,0);
    setting_hint(d,1,L"截图识别在本机完成，不上传图片。",226,330,566,22);
    setting_hint(d,1,L"GPU 使用 Vulkan：支持 Intel / AMD / NVIDIA；无 Vulkan 驱动时自动回退 CPU。",226,352,566,22);
    d->ocrBench=control(d,1,L"STATIC",L"PP-OCRv6 组件与模型按需下载；混元模型地址不变。",0,226,378,566,24);
    d->ocrStatus=control(d,1,L"STATIC",L"",0,226,404,566,40,105);
    d->ocrProgress=control(d,1,PROGRESS_CLASSW,L"",PBS_SMOOTH,226,450,566,6);
    SendMessageW(d->ocrProgress,PBM_SETRANGE32,0,100);
    SendMessageW(d->ocrProgress,PBM_SETBARCOLOR,0,RGB(7,193,96));SendMessageW(d->ocrProgress,PBM_SETBKCOLOR,0,RGB(232,239,234));
    d->ocrDownload=control(d,1,L"BUTTON",L"下载 / 校验模型",WS_TABSTOP,226,474,186,36,11);
    label(d,1,L"本地推理 · 保护隐私",484,480,308,24);
    setting_hint(d,1,L"下载前请阅读随程序提供的模型许可。",226,520,566,24);

    d->translateEnabled=setting_check(d,2,L"启用翻译服务",226,115,550,17,g_cfg.translate_enabled);
    setting_hint(d,2,L"关闭后隐藏截图工具条中的翻译按钮。",226,154,566,25);
    label(d,2,L"接口地址",226,282,132);
    d->base=setting_edit(d,2,374,274,418,106);
    label(d,2,L"API Key",226,342,132);
    d->key=setting_edit(d,2,374,334,418,107,true);
    label(d,2,L"模型名称",226,402,132);
    d->model=setting_edit(d,2,374,394,418,108);
    SetWindowTextW(d->base,g_cfg.llm_base.c_str());SetWindowTextW(d->key,g_cfg.llm_key.c_str());SetWindowTextW(d->model,g_cfg.llm_model.c_str());
    setting_hint(d,2,L"兼容 OpenAI 格式的翻译接口。",374,444,418,26);
    setting_hint(d,2,L"密钥保存在当前用户配置中，请勿分享设置文件。",226,492,566,44);
    control(d,-1,L"BUTTON",L"取消",WS_TABSTOP,596,594,94,34,2);
    control(d,-1,L"BUTTON",L"保存设置",WS_TABSTOP|BS_DEFPUSHBUTTON,702,594,114,34,1);
    choose_page(d);refresh_download(d);SetTimer(d->hwnd,60,400,nullptr);
}
