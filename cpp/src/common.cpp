#include "common.h"
#include <shlobj.h>
#include <mutex>

double g_scale = 1.0;

const Gdiplus::Color PALETTE[6] = {
    Gdiplus::Color(255, 250, 81, 81),    // #FA5151 红
    Gdiplus::Color(255, 255, 195, 0),    // #FFC300 黄
    Gdiplus::Color(255, 7, 193, 96),     // #07C160 绿
    Gdiplus::Color(255, 30, 144, 255),   // #1E90FF 蓝
    Gdiplus::Color(255, 255, 255, 255),  // #FFFFFF 白
    Gdiplus::Color(255, 0, 0, 0),        // #000000 黑
};

const wchar_t* FONT_UI()   { return L"Microsoft YaHei UI"; }
const wchar_t* FONT_MONO() { return L"Consolas"; }
const wchar_t* FONT_NUM()  { return L"Arial"; }

std::wstring u8_to_w(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n);
    return out;
}

std::string w_to_u8(const std::wstring& s) {
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n, nullptr, nullptr);
    return out;
}

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back((char)c);
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (c < 0x20) {
                char buf[8];
                sprintf_s(buf, "\\u%04X", c);
                out += buf;
            } else {
                out.push_back((char)c);
            }
        }
    }
    return out;
}

static std::wstring env_dir(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2] = {0};
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH * 2);
    if (n == 0) return std::wstring();
    return std::wstring(buf, n);
}

std::wstring appdata_dir() {
    std::wstring d = env_dir(L"APPDATA");
    if (d.empty()) return L".";
    d += L"\\weijie";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

std::wstring localappdata_dir() {
    std::wstring d = env_dir(L"LOCALAPPDATA");
    if (d.empty()) return L".";
    d += L"\\weijie";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

std::wstring exe_path() {
    wchar_t buf[MAX_PATH * 2] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    return buf;
}

bool process_is_elevated(HANDLE process) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    bool elevated = GetTokenInformation(token, TokenElevation, &elevation,
                                        sizeof(elevation), &size) && elevation.TokenIsElevated;
    CloseHandle(token);
    return elevated;
}

bool capture_requires_elevation(HWND foreground) {
    if (!foreground || process_is_elevated(GetCurrentProcess())) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        logf(L"截图权限检查：无法查询前台进程 pid=%lu error=%lu，继续尝试普通截图", pid, GetLastError());
        return false;
    }
    bool elevated = process_is_elevated(process);
    CloseHandle(process);
    logf(L"截图权限检查：微截=普通权限 前台pid=%lu 前台管理员=%d", pid, (int)elevated);
    return elevated;
}

static thread_local unsigned long long currentTrace = 0;
static std::atomic<unsigned long long> nextTrace{0};

LogTrace::LogTrace(unsigned long long id) : previous_(currentTrace) {
    id_ = id ? id : (currentTrace ? currentTrace : ++nextTrace);
    currentTrace = id_;
}

LogTrace::~LogTrace() { currentTrace = previous_; }

void logf(const wchar_t* fmt, ...) {
    static std::mutex mtx;
    std::lock_guard<std::mutex> lk(mtx);
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    std::wstring line;
    wchar_t head[64];
    _snwprintf_s(head, _countof(head), _TRUNCATE, L"[%02d:%02d:%02d.%03d] ",
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    line = head;
    if (currentTrace)
        line += L"traceId=shot-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(currentTrace) + L" ";
    line += buf;
    line += L"\r\n";

    std::wstring dir = localappdata_dir() + L"\\logs";
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t name[64];
    swprintf_s(name, L"\\audit-dev-%04d-%02d-%02d.log", st.wYear, st.wMonth, st.wDay);
    std::string u8 = w_to_u8(line);
    // app.log 也是仓库现有 UI 回归脚本的输入。
    for (const auto& path : {dir + name, localappdata_dir() + L"\\app.log"}) {
        HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        DWORD wrote = 0;
        WriteFile(h, u8.data(), (DWORD)u8.size(), &wrote, nullptr);
        CloseHandle(h);
    }
}
