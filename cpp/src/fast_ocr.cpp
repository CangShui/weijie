#include "fast_ocr.h"
#include "settings.h"

#include <mutex>
#include <vector>
#include <filesystem>
#include <fstream>

// PP-OCRv6 sidecars. ocr-ncnn is the Vulkan-capable C++ engine; ocr-fast
// remains a legacy OpenVINO fallback. The worker stays alive between captures
// so model loading/pipeline compilation is paid only once.
namespace {

std::mutex g_lock;
HANDLE g_job = nullptr;
HANDLE g_process = nullptr;
HANDLE g_in = nullptr;   // parent -> child stdin
HANDLE g_out = nullptr;  // child stdout -> parent
std::wstring g_runtime_dir;
std::atomic<bool> g_initialized{false};
std::atomic<bool> g_use_ncnn{false};
bool g_sidecar_reused = false;

bool file_exists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

void write_diag(const char* event, bool reused = false) {
    wchar_t path[MAX_PATH * 2]{};
    if (!GetEnvironmentVariableW(L"JJ_OCR_DIAGNOSTICS", path, (DWORD)_countof(path))) return;
    std::ofstream out(std::filesystem::path(path), std::ios::app);
    if (!out) return;
    out << "{\"event\": \"" << event << "\"";
    if (event && std::string(event) == "request_end") {
        out << ", \"reused\": " << (reused ? "true" : "false");
    }
    out << "}\n";
}

std::wstring find_runtime_dir() {
    std::wstring exe = exe_path();
    size_t pos = exe.find_last_of(L"\\/");
    std::wstring root = pos == std::wstring::npos ? L"." : exe.substr(0, pos);
    const std::wstring ncnn_candidates[] = {
        root + L"\\ocr-ncnn",
        localappdata_dir() + L"\\ocr-ncnn",
    };
    for (const auto& dir : ncnn_candidates) {
        if (!file_exists(dir + L"\\ocr_ncnn.exe") ||
            !file_exists(dir + L"\\ncnn.dll") ||
            !file_exists(dir + L"\\opencv_world4110.dll") ||
            !file_exists(dir + L"\\config-gpu.json") ||
            !file_exists(dir + L"\\config-cpu.json")) {
            continue;
        }
        bool models_ok = true;
        for (const wchar_t* name : {
                 L"PP_OCRv6_small_det.param", L"PP_OCRv6_small_det.bin",
                 L"PP_OCRv6_small_rec.param", L"PP_OCRv6_small_rec.bin",
                 L"PP_LCNet_x0_25_textline_ori.param",
                 L"PP_LCNet_x0_25_textline_ori.bin", L"ppocr_keys_v6.txt"}) {
            if (!file_exists(dir + L"\\models\\" + name)) {
                models_ok = false;
                break;
            }
        }
        if (models_ok) {
            g_use_ncnn.store(true);
            return dir;
        }
    }
    const std::wstring candidates[] = {
        root + L"\\ocr-fast",
        localappdata_dir() + L"\\ocr-fast",
    };
    for (const auto& dir : candidates) {
        if (!file_exists(dir + L"\\python.exe") ||
            !file_exists(dir + L"\\ocr_sidecar.py") ||
            !file_exists(dir + L"\\Lib\\site-packages\\rapidocr\\__init__.py")) {
            continue;
        }
        bool models_ok = true;
        for (const wchar_t* name : {
                 L"PP-OCRv6_det_small.onnx",
                 L"PP-OCRv6_rec_small.onnx",
                 L"ch_ppocr_mobile_v2.0_cls_mobile.onnx"}) {
            if (!file_exists(dir + L"\\models\\" + name)) {
                models_ok = false;
                break;
            }
        }
        if (models_ok) {
            g_use_ncnn.store(false);
            return dir;
        }
    }
    return {};
}

void close_sidecar_locked() {
    if (g_job) TerminateJobObject(g_job, 0);
    if (g_process) {
        WaitForSingleObject(g_process, 3000);
        CloseHandle(g_process);
    }
    if (g_job) CloseHandle(g_job);
    if (g_in) CloseHandle(g_in);
    if (g_out) CloseHandle(g_out);
    g_job = nullptr;
    g_process = nullptr;
    g_in = nullptr;
    g_out = nullptr;
}

bool read_one(HANDLE h, char& c, DWORD timeout_ms, const std::atomic<bool>* cancel) {
    ULONGLONG start = GetTickCount64();
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) return false;
        if (avail > 0) {
            DWORD got = 0;
            if (!ReadFile(h, &c, 1, &got, nullptr) || got != 1) return false;
            return true;
        }
        if (cancel && cancel->load()) return false;
        if (GetTickCount64() - start > timeout_ms) return false;
        Sleep(20);
    }
}

bool read_line(HANDLE h, std::string& line, DWORD timeout_ms,
               const std::atomic<bool>* cancel) {
    line.clear();
    for (;;) {
        char c = 0;
        if (!read_one(h, c, timeout_ms, cancel)) return false;
        if (c == '\n') {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        }
        line.push_back(c);
        if (line.size() > 4096) return false;
    }
}

bool read_exact(HANDLE h, std::string& out, size_t count, DWORD timeout_ms,
                const std::atomic<bool>* cancel) {
    out.clear();
    out.reserve(count);
    ULONGLONG start = GetTickCount64();
    while (out.size() < count) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) return false;
        if (avail == 0) {
            if (cancel && cancel->load()) return false;
            if (GetTickCount64() - start > timeout_ms) return false;
            Sleep(10);
            continue;
        }
        char buf[4096];
        DWORD want = (DWORD)std::min<size_t>(sizeof(buf), count - out.size());
        DWORD got = 0;
        if (!ReadFile(h, buf, want, &got, nullptr) || got == 0) return false;
        out.append(buf, got);
    }
    return true;
}

bool write_all(HANDLE h, const char* data, size_t count) {
    while (count > 0) {
        DWORD wrote = 0;
        DWORD chunk = (DWORD)std::min<size_t>(count, 65536);
        if (!WriteFile(h, data, chunk, &wrote, nullptr) || wrote == 0) return false;
        data += wrote;
        count -= wrote;
    }
    return true;
}

bool start_sidecar_locked(std::wstring& err) {
    // Keep the compiled models and dynamic-shape kernels alive across captures.
    // Previously this unconditionally killed the worker before every request.
    if (g_process && g_in && g_out &&
        WaitForSingleObject(g_process, 0) == WAIT_TIMEOUT) {
        g_sidecar_reused = true;
        logf(L"PP-OCR sidecar reuse pid=%lu", GetProcessId(g_process));
        return true;
    }
    g_sidecar_reused = false;
    close_sidecar_locked();
    g_runtime_dir = find_runtime_dir();
    if (g_runtime_dir.empty()) {
        err = L"缺少 PP-OCRv6 组件（ocr-ncnn / ocr-fast），请使用完整程序包";
        return false;
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE child_in_read = nullptr, parent_write = nullptr;
    HANDLE parent_read = nullptr, child_out_write = nullptr;
    if (!CreatePipe(&child_in_read, &parent_write, &sa, 0) ||
        !CreatePipe(&parent_read, &child_out_write, &sa, 0)) {
        if (child_in_read) CloseHandle(child_in_read);
        if (parent_write) CloseHandle(parent_write);
        if (parent_read) CloseHandle(parent_read);
        if (child_out_write) CloseHandle(child_out_write);
        err = L"无法创建 OCR 管道";
        return false;
    }
    SetHandleInformation(parent_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(parent_read, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = child_in_read;
    si.hStdOutput = child_out_write;
    si.hStdError = nul != INVALID_HANDLE_VALUE ? nul : child_out_write;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = g_use_ncnn.load()
        ? L"\"" + g_runtime_dir + L"\\ocr_ncnn.exe\""
        : L"\"" + g_runtime_dir + L"\\python.exe\" \"" +
          g_runtime_dir + L"\\ocr_sidecar.py\"";
    std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back(0);

    BOOL ok = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                             g_runtime_dir.c_str(), &si, &pi);
    CloseHandle(child_in_read);
    CloseHandle(child_out_write);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(parent_write);
        CloseHandle(parent_read);
        err = L"无法启动 OCR 引擎进程";
        return false;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    bool job_ok = job && SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                                 &limits, sizeof(limits)) &&
                  AssignProcessToJobObject(job, pi.hProcess);
    if (!job_ok) {
        TerminateProcess(pi.hProcess, 2);
        WaitForSingleObject(pi.hProcess, 3000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (job) CloseHandle(job);
        CloseHandle(parent_write);
        CloseHandle(parent_read);
        err = L"无法创建受控 OCR 进程";
        return false;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    g_job = job;
    g_process = pi.hProcess;
    g_in = parent_write;
    g_out = parent_read;

    std::string ready;
    if (!read_line(g_out, ready, 60000, nullptr) || ready.rfind("READY", 0) != 0) {
        close_sidecar_locked();
        err = L"OCR 引擎启动超时或初始化失败";
        return false;
    }
    logf(L"PP-OCR sidecar ready pid=%lu: %s", GetProcessId(g_process), g_runtime_dir.c_str());
    if (g_use_ncnn.load()) write_diag("worker_ready");
    return true;
}

bool get_encoder(const wchar_t* mime, CLSID& clsid) {
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

bool save_png(Gdiplus::Bitmap* bmp, const std::wstring& path) {
    CLSID clsid;
    if (!get_encoder(L"image/png", clsid)) return false;
    return bmp->Save(path.c_str(), &clsid, nullptr) == Gdiplus::Ok;
}

bool is_wide(wchar_t c) {
    unsigned u = (unsigned)c;
    return (u >= 0x4E00 && u <= 0x9FFF) ||
           (u >= 0x3400 && u <= 0x4DBF) ||
           (u >= 0x3000 && u <= 0x303F) ||
           (u >= 0xFF00 && u <= 0xFFEF) ||
           (u >= 0x2018 && u <= 0x201D);
}

std::wstring tidy(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        wchar_t c = in[i];
        if (c == L'\uFF0E') c = L'\u00B7';
        if (c == L' ') {
            wchar_t prev = out.empty() ? 0 : out.back();
            wchar_t next = 0;
            for (size_t j = i + 1; j < in.size(); ++j) {
                if (in[j] != L' ') { next = in[j]; break; }
            }
            if (is_wide(prev) && is_wide(next)) continue;
            if (prev == 0) continue;
        }
        out.push_back(c);
    }
    return out;
}

}  // namespace

bool fast_ocr_available() {
    return !find_runtime_dir().empty();
}

bool fast_ocr_gpu_available() {
    if (find_runtime_dir().empty()) return false;
    return g_use_ncnn.load();
}

bool fast_ocr_initializing() {
    return !g_initialized.load();
}

std::wstring fast_ocr_status_text() {
    if (find_runtime_dir().empty()) {
        return L"缺少 PP-OCRv6 组件（ocr-ncnn / ocr-fast），请使用完整程序包";
    }
    if (g_use_ncnn.load()) {
        return L"PP-OCRv6 · ncnn Vulkan 就绪；Intel / AMD / NVIDIA；无 Vulkan 回退 CPU。";
    }
    std::wstring text = L"OpenVINO 已就绪；";
    text += gpu_ocr_supported()
        ? L"CPU / Intel 核显可用；"
        : L"CPU 可用；检测到 AMD / NVIDIA 独显，GPU 自动关闭；";
    text += L"首次编译可能较慢，连续识别复用进程。";
    return text;
}

bool fast_ocr_recognize(Gdiplus::Bitmap* bmp, std::wstring& text, std::wstring& err,
                        const std::atomic<bool>* cancel) {
    text.clear();
    err.clear();
    if (!bmp) { err = L"没有可识别的图像"; return false; }
    if (cancel && cancel->load()) { err = L"已取消识别"; return false; }

    std::lock_guard<std::mutex> guard(g_lock);
    if (!start_sidecar_locked(err)) return false;

    wchar_t temp[MAX_PATH]{}, unique[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temp) || !GetTempFileNameW(temp, L"jjo", 0, unique)) {
        err = L"无法创建 OCR 临时文件";
        close_sidecar_locked();
        return false;
    }
    std::wstring image = unique;
    if (!save_png(bmp, image)) {
        DeleteFileW(image.c_str());
        err = L"无法写出 OCR 输入图片";
        close_sidecar_locked();
        return false;
    }

    Settings s = load_settings();
    std::string device = (s.ocr_backend == L"gpu" && (g_use_ncnn.load() || gpu_ocr_supported())) ? "GPU" :
                         s.ocr_backend == L"cpu" ? "CPU" : "auto";
    logf(L"PP-OCR request pid=%lu device=%s image=%ux%u",
         GetProcessId(g_process), u8_to_w(device).c_str(), bmp->GetWidth(), bmp->GetHeight());
    std::string request = device + "\n" + w_to_u8(image) + "\n";
    if (!write_all(g_in, request.data(), request.size())) {
        DeleteFileW(image.c_str());
        close_sidecar_locked();
        err = L"OCR 引擎写入失败";
        return false;
    }

    std::string header;
    ULONGLONG start = GetTickCount64();
    if (!read_line(g_out, header, 300000, cancel)) {
        bool cancelled = cancel && cancel->load();
        DeleteFileW(image.c_str());
        close_sidecar_locked();
        err = cancelled ? L"已取消识别" : L"OCR 引擎响应超时";
        return false;
    }
    size_t space = header.find(' ');
    if (space == std::string::npos) {
        DeleteFileW(image.c_str());
        close_sidecar_locked();
        err = L"OCR 引擎返回格式错误";
        return false;
    }
    std::string status = header.substr(0, space);
    int count = 0;
    try {
        count = std::stoi(header.substr(space + 1));
    } catch (...) {
        count = -1;
    }
    if (count < 0 || count > (16 << 20)) {
        DeleteFileW(image.c_str());
        close_sidecar_locked();
        err = L"OCR 引擎返回长度无效";
        return false;
    }

    std::string payload;
    if (!read_exact(g_out, payload, (size_t)count, 300000, cancel)) {
        bool cancelled = cancel && cancel->load();
        DeleteFileW(image.c_str());
        close_sidecar_locked();
        err = cancelled ? L"已取消识别" : L"OCR 引擎结果读取失败";
        return false;
    }
    char tail = 0;
    if (!read_one(g_out, tail, 5000, cancel)) {
        DeleteFileW(image.c_str());
        close_sidecar_locked();
        err = L"OCR 引擎结果不完整";
        return false;
    }
    DeleteFileW(image.c_str());

    if (status != "OK") {
        err = u8_to_w(payload);
        if (err.empty()) err = L"OCR 识别失败";
        close_sidecar_locked();
        return false;
    }

    g_initialized.store(true);
    text = tidy(u8_to_w(payload));
    if (g_use_ncnn.load()) write_diag("request_end", g_sidecar_reused);
    if (text.empty()) {
        err = L"没有识别到文字";
        return false;
    }
    logf(L"PP-OCR completed, elapsed=%llu ms, chars=%zu",
         GetTickCount64() - start, text.size());
    return true;
}

void fast_ocr_shutdown() {
    std::lock_guard<std::mutex> guard(g_lock);
    close_sidecar_locked();
}
