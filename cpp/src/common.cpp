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
    line += buf;
    line += L"\r\n";

    std::wstring path = localappdata_dir() + L"\\app.log";
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    std::string u8 = w_to_u8(line);
    SetFilePointer(h, 0, nullptr, FILE_END);
    WriteFile(h, u8.data(), (DWORD)u8.size(), &wrote, nullptr);
    CloseHandle(h);
}
