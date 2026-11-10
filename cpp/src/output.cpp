#include "output.h"
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
    if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) return false;
    std::wstring ext;
    size_t dot = path.find_last_of(L'.');
    if (dot != std::wstring::npos) ext = path.substr(dot + 1);
    for (auto& c : ext) c = towlower(c);

    CLSID clsid;
    if (ext == L"jpg" || ext == L"jpeg") {
        if (!encoder_clsid(L"image/jpeg", clsid)) return false;

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
        return flat.Save(path.c_str(), &clsid, &params) == Gdiplus::Ok;
    }

    if (ext == L"bmp") {
        if (!encoder_clsid(L"image/bmp", clsid)) return false;
        return bmp->Save(path.c_str(), &clsid, nullptr) == Gdiplus::Ok;
    }

    if (ext != L"png" || !encoder_clsid(L"image/png", clsid)) return false;
    return bmp->Save(path.c_str(), &clsid, nullptr) == Gdiplus::Ok;
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

bool save_bitmap_direct(Gdiplus::Bitmap* bmp, const std::wstring& preferDir,
                        const std::wstring& format, int quality, std::wstring& savedPath) {
    savedPath.clear();
    if (!bmp) return false;
    std::wstring dir = preferDir.empty() ? default_save_dir() : preferDir;
    int made = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    if (made != ERROR_SUCCESS && made != ERROR_ALREADY_EXISTS && made != ERROR_FILE_EXISTS) {
        return false;
    }

    std::wstring fmt = format;
    for (auto& c : fmt) c = towlower(c);
    if (fmt != L"jpg" && fmt != L"jpeg" && fmt != L"bmp") fmt = L"png";
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

    bool ok = save_bitmap_file(bmp, path, quality);
    if (ok) savedPath = path;
    return ok;
}

bool save_bitmap_dialog(HWND owner, Gdiplus::Bitmap* bmp, const std::wstring& preferDir,
                        const std::wstring& format, int quality) {
    if (!bmp) return false;
    std::wstring dir = preferDir.empty() ? default_save_dir() : preferDir;
    int made=SHCreateDirectoryExW(owner,dir.c_str(),nullptr);
    if(made!=ERROR_SUCCESS&&made!=ERROR_ALREADY_EXISTS&&made!=ERROR_FILE_EXISTS){MessageBoxW(owner,L"无法创建截图保存目录",L"保存失败",MB_OK|MB_ICONERROR);return false;}
    std::wstring fmt = format;
    for (auto& c : fmt) c = towlower(c);
    if (fmt != L"jpg" && fmt != L"jpeg" && fmt != L"bmp") fmt = L"png";
    std::wstring ext = (fmt == L"jpeg") ? L"jpg" : fmt;
    std::wstring full = dir + L"\\" + timestamp_name(ext);

    wchar_t file[MAX_PATH * 2] = {0};
    wcsncpy_s(file, full.c_str(), _TRUNCATE);

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    const wchar_t* filter =
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
        logf(L"保存对话框取消/失败 err=%lu", (unsigned long)e);
        return false;
    }
    std::wstring chosen=file;
    // File-type selector is authoritative for known image formats.
    std::wstring selected;
    if(ofn.nFilterIndex<=3){
        const wchar_t* pngOrder[]={L"png",L"jpg",L"bmp"};
        const wchar_t* jpgOrder[]={L"jpg",L"png",L"bmp"};
        const wchar_t* bmpOrder[]={L"bmp",L"png",L"jpg"};
        selected=(fmt==L"png"?pngOrder:fmt==L"bmp"?bmpOrder:jpgOrder)[ofn.nFilterIndex-1];
        size_t dot=chosen.find_last_of(L'.'),slash=chosen.find_last_of(L"\\/");
        if(dot!=std::wstring::npos&&(slash==std::wstring::npos||dot>slash))chosen.resize(dot);
        chosen+=L"."+selected;
        if(chosen!=file&&GetFileAttributesW(chosen.c_str())!=INVALID_FILE_ATTRIBUTES &&
           MessageBoxW(owner,L"目标格式的文件已存在，是否覆盖？",L"保存截图",MB_YESNO|MB_ICONWARNING)!=IDYES)return false;
    }
    bool ok = save_bitmap_file(bmp, chosen, quality);
    if(!ok)MessageBoxW(owner,L"保存失败，请检查目录权限及扩展名（PNG / JPG / BMP）。",L"保存截图",MB_OK|MB_ICONERROR);
    logf(L"写盘 %ls -> %d", file, (int)ok);
    return ok;
}
