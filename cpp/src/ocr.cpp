#include "ocr.h"
#include "ocr_setup.h"
#include "settings.h"
#include "fast_ocr.h"

#include <vector>

// ---------------------------------------------------------------- PNG 编码
static bool get_encoder(const wchar_t* mime, CLSID& clsid) {
    UINT num = 0, size = 0;
    if (Gdiplus::GetImageEncodersSize(&num, &size) != Gdiplus::Ok || size == 0) return false;
    std::vector<BYTE> buf(size);
    auto* codecs = (Gdiplus::ImageCodecInfo*)buf.data();
    if (Gdiplus::GetImageEncoders(num, size, codecs) != Gdiplus::Ok) return false;
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(codecs[i].MimeType, mime) == 0) {
            clsid = codecs[i].Clsid;
            return true;
        }
    }
    return false;
}

static bool save_png(Gdiplus::Bitmap* bmp, const std::wstring& path) {
    CLSID clsid;
    if (!get_encoder(L"image/png", clsid)) return false;
    return bmp->Save(path.c_str(), &clsid, nullptr) == Gdiplus::Ok;
}

static std::string read_all_utf8(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    std::string out;
    if (sz.QuadPart > 0 && sz.QuadPart < (32 << 20)) {
        out.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr);
        out.resize(got);
    }
    CloseHandle(h);
    return out;
}

static std::wstring trim(std::wstring s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) ++a;
    while (b > a && iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

// 运行 llama-mtmd-cli，把 stdout/stderr 重定向到临时文件，避免管道阻塞。
static bool run_cli(const std::wstring& cmd, const std::wstring& workdir,
                    std::wstring& out, std::wstring& err, const std::atomic<bool>* cancel) {
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring base = std::wstring(tmp) + L"weijieOcr";
    CreateDirectoryW(base.c_str(), nullptr);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t tag[128];
    _snwprintf_s(tag, _countof(tag), _TRUNCATE, L"\\%04d%02d%02d_%02d%02d%02d_%lu",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                 (unsigned long)GetCurrentThreadId());
    std::wstring outPath = base + tag + L"_out.txt";
    std::wstring errPath = base + tag + L"_err.txt";

    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE hOut = CreateFileW(outPath.c_str(), GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ,
                              &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE hErr = CreateFileW(errPath.c_str(), GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ,
                              &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hOut == INVALID_HANDLE_VALUE || hErr == INVALID_HANDLE_VALUE) {
        if (hOut != INVALID_HANDLE_VALUE) CloseHandle(hOut);
        if (hErr != INVALID_HANDLE_VALUE) CloseHandle(hErr);
        err = L"无法创建 OCR 临时文件";
        return false;
    }

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hOut;
    si.hStdError = hErr;
    HANDLE hIn=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,nullptr);
    si.hStdInput = hIn;
    PROCESS_INFORMATION pi = {};

    HANDLE job=CreateJobObjectW(nullptr,nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    bool jobOk=job && SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits));
    std::wstring cmdline = cmd;
    BOOL ok = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                             workdir.empty() ? nullptr : workdir.c_str(), &si, &pi);
    CloseHandle(hOut);
    CloseHandle(hErr);
    if(hIn!=INVALID_HANDLE_VALUE)CloseHandle(hIn);
    if (!ok) {
        DeleteFileW(outPath.c_str());
        DeleteFileW(errPath.c_str());
        if(job)CloseHandle(job);
        err = L"无法启动 OCR 推理进程";
        return false;
    }

    if(!jobOk || !AssignProcessToJobObject(job,pi.hProcess)){
        TerminateProcess(pi.hProcess,2);WaitForSingleObject(pi.hProcess,5000);
        CloseHandle(pi.hThread);CloseHandle(pi.hProcess);if(job)CloseHandle(job);
        DeleteFileW(outPath.c_str());DeleteFileW(errPath.c_str());
        err=L"无法创建受控 OCR 进程";return false;
    }
    ResumeThread(pi.hThread);
    ULONGLONG start=GetTickCount64();bool timeout=false,aborted=false;
    while(WaitForSingleObject(pi.hProcess,100)==WAIT_TIMEOUT){
        aborted=cancel && cancel->load();timeout=GetTickCount64()-start>240000;
        if(aborted||timeout){TerminateJobObject(job,2);WaitForSingleObject(pi.hProcess,5000);break;}
    }
    DWORD code=0;GetExitCodeProcess(pi.hProcess,&code);
    CloseHandle(pi.hThread);CloseHandle(pi.hProcess);CloseHandle(job);

    std::string so = read_all_utf8(outPath);
    std::string se = read_all_utf8(errPath);
    DeleteFileW(outPath.c_str());
    DeleteFileW(errPath.c_str());

    if (aborted) { err=L"已取消识别";return false; }
    if (timeout) {
        err = L"OCR 推理超时";
        return false;
    }
    if (code != 0) {
        err = L"OCR 推理失败（退出码 " + std::to_wstring((unsigned long)code) + L"）";
        if (!se.empty()) {
            std::wstring w = u8_to_w(se);
            if (w.size() > 600) w = L"…" + w.substr(w.size() - 600);
            err += L"\n" + w;
        }
        return false;
    }

    out = u8_to_w(so);
    logf(L"OCR completed, elapsed=%llu ms",GetTickCount64()-start);
    return true;
}

bool ocr_recognize(Gdiplus::Bitmap* bmp, std::wstring& text, std::wstring& err, const std::atomic<bool>* cancel) {
    text.clear();
    err.clear();
    if (!bmp) { err = L"没有可识别的图像"; return false; }

    Settings s = load_settings();
    if (!s.ocr_enabled) {
        err = L"请先在设置中启用 OCR";
        return false;
    }
    if(s.ocr_engine==L"ppocr"){
        bool ok=fast_ocr_recognize(bmp,text,err,cancel);
        if(!ok&&err.empty())err=L"PP-OCRv6 识别失败";
        return ok;
    }
    if (s.ocr_engine==L"hunyuan_fast" ? !hunyuan_fast_model_ready() : !hunyuan_model_ready()) {
        err = L"混元 OCR 模型尚未下载完成，请在设置中下载模型";
        return false;
    }

    std::wstring dir = hunyuan_ocr_dir();
    const bool useGpu = s.ocr_backend==L"gpu" &&
                        (gpu_ocr_supported() || fast_ocr_gpu_available());
    std::wstring runtime = useGpu ? hunyuan_runtime_dir(true) : hunyuan_runtime_dir(false);
    std::wstring model = s.ocr_engine==L"hunyuan_fast" ? L"HunyuanOCR-Q3_K_S.gguf" : L"HunyuanOCR-Q8_0.gguf";
    wchar_t temp[MAX_PATH]{}, unique[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temp) ||
        !GetTempFileNameW(temp, L"jjo", 0, unique)) {
        err = L"无法创建 OCR 临时文件";
        return false;
    }
    std::wstring image = unique;
    if (!save_png(bmp, image)) {
        DeleteFileW(image.c_str());
        err = L"无法写出 OCR 输入图片";
        return false;
    }

    const std::wstring prompt = L"识别图片中的所有文字，保持原有的换行和阅读顺序，不要添加任何解释。";
    std::wstring cmd =
        L"\"" + runtime + L"\\llama-mtmd-cli.exe\""
        L" -m \"" + dir + L"\\" + model + L"\""
        L" --mmproj \"" + dir + L"\\mmproj-HunyuanOCR-Q8_0.gguf\""
        L" --image \"" + image + L"\""
        L" -p \"" + prompt + L"\""
        L" -n 4096 -c 8192 --temp 0 --top-k 1"+std::wstring(useGpu?L" --device Vulkan0 -ngl 99 --mmproj-device Vulkan0":L" -ngl 0 --no-mmproj-offload");

    std::wstring out;
    bool ok = run_cli(cmd, runtime, out, err, cancel);
    DeleteFileW(image.c_str());
    if (!ok) return false;

    text = trim(out);
    if (text.empty()) {
        err = L"没有识别到文字";
        return false;
    }
    return true;
}
