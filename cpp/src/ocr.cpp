#include "ocr.h"
#include "ocr_setup.h"
#include "settings.h"
#include "fast_ocr.h"

#include <vector>
#include <mutex>

namespace {
struct ModelProcess { HANDLE sourceJob,job,process; };
std::mutex modelMutex;
std::vector<ModelProcess> modelProcesses;
std::atomic<bool> modelEnabled{false};
int idleMinutes=0;
unsigned activeRequests=0;
ULONGLONG lastActivity=GetTickCount64();
}

bool ocr_is_enabled(){return modelEnabled.load();}

OcrActivity::OcrActivity(){
    std::lock_guard<std::mutex> lock(modelMutex);
    ++activeRequests;
    logf(L"OCR 调用开始：活动调用=%u，排队、加载和推理期间不自动回收",activeRequests);
}
OcrActivity::~OcrActivity(){
    std::lock_guard<std::mutex> lock(modelMutex);
    --activeRequests;
    lastActivity=GetTickCount64();
    logf(L"OCR 调用结束：剩余调用=%u 空闲计时起点=%llu 超时分钟=%d",
         activeRequests,lastActivity,idleMinutes);
}
void ocr_set_idle_timeout(int minutes){
    std::lock_guard<std::mutex> lock(modelMutex);
    if(idleMinutes==minutes)return;
    logf(L"OCR 空闲回收设置生效：原分钟=%d 新分钟=%d（0=off），不改变主开关或预加载模型",
         idleMinutes,minutes);
    idleMinutes=minutes;
}
bool ocr_idle_expired(ULONGLONG now){
    std::lock_guard<std::mutex> lock(modelMutex);
    if(!modelEnabled||!idleMinutes||activeRequests||now<lastActivity)return false;
    ULONGLONG elapsed=now-lastActivity,limit=(ULONGLONG)idleMinutes*60000;
    if(elapsed<limit)return false;
    logf(L"OCR 空闲超时：当前=%llu 上次调用结束=%llu 空闲毫秒=%llu 阈值=%d分钟×60000=%llu，活动调用=0，允许回收",
         now,lastActivity,elapsed,idleMinutes,limit);
    return true;
}

bool ocr_track_process(HANDLE job,HANDLE process){
    std::lock_guard<std::mutex> lock(modelMutex);
    if(!modelEnabled){
        logf(L"模型进程注册被拒绝：识别主开关已关闭，不允许加载模型");
        return false;
    }
    ModelProcess item{job,nullptr,nullptr};
    HANDLE self=GetCurrentProcess();
    if(!DuplicateHandle(self,job,self,&item.job,0,FALSE,DUPLICATE_SAME_ACCESS)||
       !DuplicateHandle(self,process,self,&item.process,0,FALSE,DUPLICATE_SAME_ACCESS)){
        DWORD error=GetLastError();
        if(item.job)CloseHandle(item.job);
        if(item.process)CloseHandle(item.process);
        logf(L"模型进程注册失败：无法保留停止句柄 error=%lu",error);
        return false;
    }
    modelProcesses.push_back(item);
    logf(L"模型进程已纳入主开关管理：pid=%lu",GetProcessId(process));
    return true;
}

void ocr_forget_process(HANDLE job){
    std::lock_guard<std::mutex> lock(modelMutex);
    auto found=std::find_if(modelProcesses.begin(),modelProcesses.end(),
        [job](const ModelProcess& item){return item.sourceJob==job;});
    if(found==modelProcesses.end())return;
    CloseHandle(found->job);CloseHandle(found->process);
    modelProcesses.erase(found);
}

void ocr_set_enabled(bool enabled){
    LogTrace trace;
    logf(L"识别主开关请求到达：启用=%d",(int)enabled);
    bool unloaded=true;
    {
        std::lock_guard<std::mutex> lock(modelMutex);
        modelEnabled.store(enabled);
        if(!enabled){
            for(const auto& item:modelProcesses){
                DWORD pid=GetProcessId(item.process);
                if(WaitForSingleObject(item.process,0)!=WAIT_OBJECT_0){
                    BOOL stopped=TerminateJobObject(item.job,ERROR_CANCELLED);
                    DWORD error=stopped?ERROR_SUCCESS:GetLastError();
                    DWORD waited=WaitForSingleObject(item.process,3000);
                    unloaded=unloaded&&waited==WAIT_OBJECT_0;
                    logf(L"识别主开关卸载模型：pid=%lu 终止成功=%d error=%lu 进程已退出=%d",
                         pid,(int)stopped,error,(int)(waited==WAIT_OBJECT_0));
                }
            }
        }
    }
    // 不等待正在推理的线程持有的互斥锁；模型进程已由上面的独立句柄终止。
    if(!enabled)fast_ocr_unload();
    logf(L"识别主开关返回：启用=%d 模型进程已停止=%d；开启仅允许按需加载",
         (int)enabled,(int)(!enabled&&unloaded));
}

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

    if(!jobOk || !AssignProcessToJobObject(job,pi.hProcess)||!ocr_track_process(job,pi.hProcess)){
        TerminateProcess(pi.hProcess,2);WaitForSingleObject(pi.hProcess,5000);
        CloseHandle(pi.hThread);CloseHandle(pi.hProcess);if(job)CloseHandle(job);
        DeleteFileW(outPath.c_str());DeleteFileW(errPath.c_str());
        err=ocr_is_enabled()?L"无法创建受控 OCR 进程":L"识别主开关已关闭";return false;
    }
    ResumeThread(pi.hThread);
    ULONGLONG start=GetTickCount64();bool timeout=false,aborted=false;
    while(WaitForSingleObject(pi.hProcess,100)==WAIT_TIMEOUT){
        aborted=!ocr_is_enabled()||(cancel && cancel->load());timeout=GetTickCount64()-start>240000;
        if(aborted||timeout){TerminateJobObject(job,2);WaitForSingleObject(pi.hProcess,5000);break;}
    }
    DWORD code=0;GetExitCodeProcess(pi.hProcess,&code);
    aborted=aborted||!ocr_is_enabled()||(cancel&&cancel->load());
    ocr_forget_process(job);
    CloseHandle(pi.hThread);CloseHandle(pi.hProcess);CloseHandle(job);

    std::string so = read_all_utf8(outPath);
    std::string se = read_all_utf8(errPath);
    DeleteFileW(outPath.c_str());
    DeleteFileW(errPath.c_str());

    if (aborted) { err=ocr_is_enabled()?L"已取消识别":L"识别主开关已关闭，模型已卸载";return false; }
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
    LogTrace trace;
    OcrActivity activity;
    text.clear();
    err.clear();
    if (!bmp) { err = L"没有可识别的图像";logf(L"识别请求被拒绝：输入图像为空"); return false; }

    Settings s = load_settings();
    if(!s.ocr_enabled||!ocr_is_enabled()){
        err=L"识别主开关已关闭，请开启后再识别图片";
        logf(L"识别请求被拒绝：主开关已关闭，不启动或重新加载模型");
        return false;
    }
    logf(L"识别引擎请求到达：引擎=%s 图像=%ux%u 识别按钮显示=%d",
         s.ocr_engine.c_str(),bmp->GetWidth(),bmp->GetHeight(),(int)s.ocr_enabled);
    if(s.ocr_engine==L"ppocr"){
        bool ok=fast_ocr_recognize(bmp,text,err,cancel);
        if(!ok&&err.empty())err=L"PP-OCRv6 识别失败";
        logf(L"识别引擎返回：成功=%d 字符数=%zu 错误=%s",(int)ok,text.size(),err.c_str());
        return ok;
    }
    if (s.ocr_engine==L"hunyuan_fast" ? !hunyuan_fast_model_ready() : !hunyuan_model_ready()) {
        err = L"混元 OCR 模型尚未下载完成，请在设置中下载模型";
        logf(L"识别请求未执行：%s",err.c_str());
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
