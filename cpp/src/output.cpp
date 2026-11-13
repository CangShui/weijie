#include "output.h"
#include "animated_image.h"
#include <shlobj.h>
#include <objidl.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

std::wstring default_save_dir() {
    std::wstring dir;
    PWSTR pics = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pics)) && pics) {
        dir = pics;
        CoTaskMemFree(pics);
    }
    if (dir.empty()) {
        wchar_t buf[MAX_PATH] = {0};
        GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
        dir = std::wstring(buf) + L"\\Pictures";
    }
    dir += L"\\微截";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

// ---------------------------------------------------------------- 剪贴板
static HGLOBAL bitmap_to_dib(Gdiplus::Bitmap* bmp, BITMAPINFOHEADER& outHdr) {
    int w = (int)bmp->GetWidth();
    int h = (int)bmp->GetHeight();

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = h;      // 正数：自下而上，符合 CF_DIB 约定
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bi.bmiHeader.biSizeImage = (DWORD)w * h * 4;

    HDC dc = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(dc);
    void* bits = nullptr;
    HBITMAP hbm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbm) {
        DeleteDC(mem);
        ReleaseDC(nullptr, dc);
        return nullptr;
    }
    HGDIOBJ old = SelectObject(mem, hbm);

    Gdiplus::Graphics g(mem);
    g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    g.DrawImage(bmp, 0, 0, w, h);
    g.Flush();

    size_t bytes = (size_t)w * h * 4;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + bytes);
    if (hMem) {
        void* dst = GlobalLock(hMem);
        outHdr = bi.bmiHeader;
        memcpy(dst, &bi.bmiHeader, sizeof(BITMAPINFOHEADER));
        memcpy((BYTE*)dst + sizeof(BITMAPINFOHEADER), bits, bytes);
        GlobalUnlock(hMem);
    }

    SelectObject(mem, old);
    DeleteObject(hbm);
    DeleteDC(mem);
    ReleaseDC(nullptr, dc);
    return hMem;
}

static bool encoder_clsid(const wchar_t* mime, CLSID& clsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return false;
    std::vector<BYTE> buf(size);
    auto* codecs = (Gdiplus::ImageCodecInfo*)buf.data();
    Gdiplus::GetImageEncoders(num, size, codecs);
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(codecs[i].MimeType, mime) == 0) {
            clsid = codecs[i].Clsid;
            return true;
        }
    }
    return false;
}

static HGLOBAL bitmap_to_png(Gdiplus::Bitmap* bmp, DWORD& size) {
    CLSID clsid;
    if (!encoder_clsid(L"image/png", clsid)) return nullptr;
    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(nullptr, FALSE, &stream) != S_OK) return nullptr;
    if (bmp->Save(stream, &clsid, nullptr) != Gdiplus::Ok) {
        stream->Release();
        return nullptr;
    }
    HGLOBAL h = nullptr;
    GetHGlobalFromStream(stream, &h);
    size = (DWORD)GlobalSize(h);
    stream->Release();
    return h;
}

bool copy_bitmap_to_clipboard(Gdiplus::Bitmap* bmp) {
    if (!bmp) return false;
    BITMAPINFOHEADER hdr = {};
    HGLOBAL dib = bitmap_to_dib(bmp, hdr);
    if (!dib) return false;

    HWND owner = GetActiveWindow();
    if (!owner || !OpenClipboard(owner)) {
        GlobalFree(dib);
        return false;
    }
    if(!EmptyClipboard() || !SetClipboardData(CF_DIB,dib)){GlobalFree(dib);CloseClipboard();return false;}

    DWORD pngSize = 0;
    HGLOBAL png = bitmap_to_png(bmp, pngSize);
    if (png) {
        UINT fmt = RegisterClipboardFormatW(L"PNG");
        if (!fmt || !SetClipboardData(fmt,png))GlobalFree(png);
    }
    CloseClipboard();
    return true;
}

// ---------------------------------------------------------------- 保存
bool save_bitmap_file(Gdiplus::Bitmap* bmp, const std::wstring& path, int quality) {
    LogTrace trace;
    if(!bmp){logf(L"图片写盘被拒绝：图像为空 文件=%s",path.c_str());return false;}
    // GetLastStatus 是历史操作状态；当前编码返回值才决定本次保存是否成功。
    auto previous=bmp->GetLastStatus();
    logf(L"图片编码请求：文件=%s 尺寸=%ux%u 质量=%d 历史状态=%d",
         path.c_str(),bmp->GetWidth(),bmp->GetHeight(),quality,(int)previous);
    auto completed=[&](Gdiplus::Status status){
        logf(L"图片编码返回：成功=%d GDI状态=%d 文件=%s",(int)(status==Gdiplus::Ok),(int)status,path.c_str());
        return status==Gdiplus::Ok;
    };
    std::wstring ext;
    size_t dot = path.find_last_of(L'.');
    if (dot != std::wstring::npos) ext = path.substr(dot + 1);
    for (auto& c : ext) c = towlower(c);

    CLSID clsid;
    if (ext == L"jpg" || ext == L"jpeg") {
        if (!encoder_clsid(L"image/jpeg", clsid)) {logf(L"图片编码失败：JPEG 编码器不可用");return false;}

        // 白底，避免透明区域变黑
        Gdiplus::Bitmap flat((INT)bmp->GetWidth(), (INT)bmp->GetHeight(), PixelFormat24bppRGB);
        {
            Gdiplus::Graphics g(&flat);
            Gdiplus::SolidBrush white(Gdiplus::Color(255, 255, 255, 255));
            g.FillRectangle(&white, 0, 0, (INT)flat.GetWidth(), (INT)flat.GetHeight());
            g.DrawImage(bmp, 0, 0, (INT)flat.GetWidth(), (INT)flat.GetHeight());
        }
        ULONG q = (ULONG)(quality < 1 ? 1 : (quality > 100 ? 100 : quality));
        Gdiplus::EncoderParameters params;
        params.Count = 1;
        params.Parameter[0].Guid = Gdiplus::EncoderQuality;
        params.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
        params.Parameter[0].NumberOfValues = 1;
        params.Parameter[0].Value = &q;
        return completed(flat.Save(path.c_str(), &clsid, &params));
    }

    if (ext == L"bmp") {
        if (!encoder_clsid(L"image/bmp", clsid)) {logf(L"图片编码失败：BMP 编码器不可用");return false;}
        return completed(bmp->Save(path.c_str(), &clsid, nullptr));
    }

    if (ext != L"png" || !encoder_clsid(L"image/png", clsid)) {
        logf(L"图片编码被拒绝：扩展名不支持或 PNG 编码器不可用 扩展名=%s",ext.c_str());return false;
    }
    return completed(bmp->Save(path.c_str(), &clsid, nullptr));
}

static std::wstring timestamp_name(const std::wstring& format) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[128];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"微截_%04d%02d%02d_%02d%02d%02d.%s",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                 format.c_str());
    return buf;
}

static int animation_save_choice(HWND owner,const std::shared_ptr<const AnimatedScreenshot>& animation){
    if(!animation||!animation->duration())return IDNO;
    logf(L"动态图片保存询问：最长贴图时长=%llums，等待用户选择动态 GIF 或首帧静态图片",animation->duration());
    int choice=MessageBoxW(owner,
        L"截图中包含动态 GIF 贴图，是否保存为动态图片？\n\n"
        L"是：保存为 GIF，时长采用最长贴图，较短贴图循环播放。\n"
        L"否：按静态图片格式保存，所有 GIF 贴图使用首帧。\n"
        L"取消：返回编辑，不保存。",
        L"保存动态图片",MB_YESNOCANCEL|MB_ICONQUESTION);
    logf(L"动态图片保存选择返回：%s",choice==IDYES?L"动态 GIF":choice==IDNO?L"静态首帧":L"取消保存");
    return choice;
}

bool save_bitmap_direct(Gdiplus::Bitmap* bmp, const std::wstring& preferDir,
                        const std::wstring& format, int quality, std::wstring& savedPath,HWND owner,
                        std::shared_ptr<const AnimatedScreenshot> animation,bool* cancelled) {
    LogTrace trace(animation?animation->traceId:0);
    if(cancelled)*cancelled=false;
    savedPath.clear();
    if (!bmp) {logf(L"直接保存被拒绝：没有图片");return false;}
    int choice=animation_save_choice(owner,animation);
    if(choice!=IDYES&&choice!=IDNO){if(cancelled)*cancelled=true;return false;}
    std::wstring dir = preferDir.empty() ? default_save_dir() : preferDir;
    logf(L"直接保存请求：设置目录=%s 实际目录=%s 格式=%s",preferDir.c_str(),dir.c_str(),format.c_str());
    int made = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    if (made != ERROR_SUCCESS && made != ERROR_ALREADY_EXISTS && made != ERROR_FILE_EXISTS) {
        logf(L"直接保存失败：目录无法创建或访问 error=%d 目录=%s",made,dir.c_str());
        return false;
    }

    std::wstring fmt = format;
    for (auto& c : fmt) c = towlower(c);
    if (fmt != L"jpg" && fmt != L"jpeg" && fmt != L"bmp") fmt = L"png";
    if(choice==IDYES)fmt=L"gif";
    std::wstring ext = (fmt == L"jpeg") ? L"jpg" : fmt;
    std::wstring base = dir + L"\\" + timestamp_name(ext);
    std::wstring path = base;

    // 同一秒内多次保存时自动加序号，不覆盖已有截图。
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        size_t dot = base.find_last_of(L'.');
        std::wstring stem = dot == std::wstring::npos ? base : base.substr(0, dot);
        std::wstring suffix = dot == std::wstring::npos ? L"" : base.substr(dot);
        for (int i = 2; i < 1000; ++i) {
            path = stem + L"_" + std::to_wstring(i) + suffix;
            if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) break;
        }
    }

    auto first=choice==IDNO&&animation?animation->render(0):nullptr;
    if(choice==IDNO&&animation&&!first){logf(L"静态保存失败：无法合成 GIF 首帧，未写入当前预览代替");return false;}
    bool ok = choice==IDYES?save_animated_gif(*animation,path):
        save_bitmap_file(first?first.get():bmp,path,quality);
    if (ok) savedPath = path;
    logf(L"直接保存返回：成功=%d 文件=%s",(int)ok,path.c_str());
    return ok;
}

bool save_bitmap_dialog(HWND owner, Gdiplus::Bitmap* bmp, const std::wstring& preferDir,
                        const std::wstring& format, int quality, std::wstring* savedPath,
                        std::shared_ptr<const AnimatedScreenshot> animation,bool* cancelled) {
    LogTrace trace(animation?animation->traceId:0);
    if(cancelled)*cancelled=false;
    if(savedPath)savedPath->clear();
    if (!bmp) {logf(L"另存为被拒绝：没有图片");return false;}
    int choice=animation_save_choice(owner,animation);
    if(choice!=IDYES&&choice!=IDNO){if(cancelled)*cancelled=true;return false;}
    std::wstring dir = preferDir.empty() ? default_save_dir() : preferDir;
    logf(L"另存为请求到达：初始目录=%s 格式=%s",dir.c_str(),format.c_str());
    int made=SHCreateDirectoryExW(nullptr,dir.c_str(),nullptr);
    if(made!=ERROR_SUCCESS&&made!=ERROR_ALREADY_EXISTS&&made!=ERROR_FILE_EXISTS){
        // 另存为必须允许从不可写的设置目录切换到其他位置。
        logf(L"另存为初始目录不可用：error=%d 原目录=%s，允许用户在对话框选择其他目录",made,dir.c_str());
        dir=default_save_dir();
    }
    std::wstring fmt = format;
    for (auto& c : fmt) c = towlower(c);
    if (fmt != L"jpg" && fmt != L"jpeg" && fmt != L"bmp") fmt = L"png";
    if(choice==IDYES)fmt=L"gif";
    std::wstring ext = (fmt == L"jpeg") ? L"jpg" : fmt;
    std::wstring full = dir + L"\\" + timestamp_name(ext);

    wchar_t file[MAX_PATH * 2] = {0};
    wcsncpy_s(file, full.c_str(), _TRUNCATE);

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    const wchar_t* filter =
        fmt == L"gif" ? L"GIF 动态图片 (*.gif)\0*.gif\0" :
        fmt == L"bmp" ? L"BMP 图片 (*.bmp)\0*.bmp\0PNG 图片 (*.png)\0*.png\0JPEG 图片 (*.jpg)\0*.jpg\0所有文件\0*.*\0" :
        fmt == L"png" ? L"PNG 图片 (*.png)\0*.png\0JPEG 图片 (*.jpg)\0*.jpg\0BMP 图片 (*.bmp)\0*.bmp\0所有文件\0*.*\0" :
                        L"JPEG 图片 (*.jpg)\0*.jpg\0PNG 图片 (*.png)\0*.png\0BMP 图片 (*.bmp)\0*.bmp\0所有文件\0*.*\0";
    ofn.lpstrFilter = filter;
    ofn.lpstrInitialDir = dir.c_str();
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.lpstrTitle = L"保存截图";
    ofn.lpstrDefExt = ext.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetSaveFileNameW(&ofn)) {
        DWORD e = CommDlgExtendedError();
        if(!e&&cancelled)*cancelled=true;
        logf(L"另存为返回：对话框%s error=%lu，未保存文件",e?L"失败":L"已取消",(unsigned long)e);
        if(e)MessageBoxW(owner,L"无法打开保存对话框，请重试。",L"另存为失败",MB_OK|MB_ICONERROR);
        return false;
    }
    std::wstring chosen=file;
    // File-type selector is authoritative for known image formats.
    std::wstring selected;
    if(fmt==L"gif"||(ofn.nFilterIndex>=1&&ofn.nFilterIndex<=3)){
        const wchar_t* pngOrder[]={L"png",L"jpg",L"bmp"};
        const wchar_t* jpgOrder[]={L"jpg",L"png",L"bmp"};
        const wchar_t* bmpOrder[]={L"bmp",L"png",L"jpg"};
        selected=fmt==L"gif"?L"gif":(fmt==L"png"?pngOrder:fmt==L"bmp"?bmpOrder:jpgOrder)[ofn.nFilterIndex-1];
        size_t dot=chosen.find_last_of(L'.'),slash=chosen.find_last_of(L"\\/");
        if(dot!=std::wstring::npos&&(slash==std::wstring::npos||dot>slash))chosen.resize(dot);
        chosen+=L"."+selected;
        if(chosen!=file&&GetFileAttributesW(chosen.c_str())!=INVALID_FILE_ATTRIBUTES &&
           MessageBoxW(owner,L"目标格式的文件已存在，是否覆盖？",L"保存截图",MB_YESNO|MB_ICONWARNING)!=IDYES){
            if(cancelled)*cancelled=true;
            logf(L"另存为返回：用户取消覆盖 文件=%s",chosen.c_str());return false;
        }
    }
    auto first=choice==IDNO&&animation?animation->render(0):nullptr;
    if(choice==IDNO&&animation&&!first){logf(L"静态另存为失败：无法合成 GIF 首帧");return false;}
    bool ok=choice==IDYES?save_animated_gif(*animation,chosen):
        save_bitmap_file(first?first.get():bmp,chosen,quality);
    if(ok&&savedPath)*savedPath=chosen;
    if(!ok){
        bool tallJpeg=(bmp->GetWidth()>65500||bmp->GetHeight()>65500)&&
            (chosen.ends_with(L".jpg")||chosen.ends_with(L".jpeg"));
        MessageBoxW(owner,tallJpeg?L"图片尺寸超过 JPEG 格式上限，请重新另存为 PNG 或 BMP。":
                    L"保存失败，请检查目录权限、图片尺寸及所选格式。",L"保存截图",MB_OK|MB_ICONERROR);
    }
    logf(L"另存为返回：成功=%d 文件=%s",(int)ok,chosen.c_str());
    return ok;
}
