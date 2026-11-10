#include "ppocr_setup.h"

#include <winhttp.h>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <atomic>
#include <stdexcept>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

namespace {

constexpr wchar_t kPackageUrl[] =
    L"https://raw.githubusercontent.com/CangShui/weijie/main/runtime/weijie-ocr-ncnn.zip";
// Filled with the SHA-256 of runtime/weijie-ocr-ncnn.zip.
constexpr wchar_t kPackageSha256[] =
    L"69925d18971adaf086ef7800589ef0b1bc65a158452f5056be93a0d082aa469c";

std::thread worker;
std::atomic<bool> cancel{false};
std::mutex stateMutex;
PpOcrDownloadState state;

void update(std::wstring text, int percent = -1) {
    std::lock_guard<std::mutex> guard(stateMutex);
    state.text = std::move(text);
    if (percent >= 0) state.percent = percent;
}

bool file_exists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool dir_ready(const std::wstring& dir) {
    if (dir.empty()) return false;
    for (const wchar_t* name : {
             L"ocr_ncnn.exe", L"ncnn.dll", L"opencv_world4110.dll",
             L"config-cpu.json", L"config-gpu.json",
             L"models\\PP_OCRv6_small_det.param", L"models\\PP_OCRv6_small_det.bin",
             L"models\\PP_OCRv6_small_rec.param", L"models\\PP_OCRv6_small_rec.bin",
             L"models\\PP_LCNet_x0_25_textline_ori.param",
             L"models\\PP_LCNet_x0_25_textline_ori.bin",
             L"models\\ppocr_keys_v6.txt"}) {
        if (!file_exists(dir + L"\\" + name)) return false;
    }
    return true;
}

std::wstring exe_dir() {
    std::wstring exe = exe_path();
    size_t pos = exe.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"." : exe.substr(0, pos);
}

bool sha256_file(const std::wstring& path, std::wstring& hex) {
    BCRYPT_ALG_HANDLE alg{};
    BCRYPT_HASH_HANDLE hash{};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    std::vector<char> buf(1024 * 1024);
    bool ok = bool(f);
    while (f && !cancel) {
        f.read(buf.data(), buf.size());
        auto n = f.gcount();
        if (n > 0 && BCryptHashData(hash, (PUCHAR)buf.data(), (ULONG)n, 0) < 0) {
            ok = false;
            break;
        }
    }
    unsigned char digest[32]{};
    ok = ok && f.eof() && !cancel && BCryptFinishHash(hash, digest, 32, 0) >= 0;
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return false;
    const char* digits = "0123456789abcdef";
    hex.clear();
    for (auto b : digest) {
        hex.push_back((wchar_t)digits[b >> 4]);
        hex.push_back((wchar_t)digits[b & 15]);
    }
    return true;
}

struct Http {
    HINTERNET h{};
    ~Http() { if (h) WinHttpCloseHandle(h); }
    operator HINTERNET() const { return h; }
};

bool download_package(const std::wstring& target) {
    URL_COMPONENTS u{};
    u.dwStructSize = sizeof(u);
    u.dwHostNameLength = (DWORD)-1;
    u.dwUrlPathLength = (DWORD)-1;
    if (!WinHttpCrackUrl(kPackageUrl, 0, 0, &u)) return false;
    std::wstring host(u.lpszHostName, u.dwHostNameLength);
    std::wstring resource(u.lpszUrlPath, u.dwUrlPathLength);

    Http session{WinHttpOpen(L"weijie/1.5", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.h) {
        session.h = WinHttpOpen(L"weijie/1.5", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session.h) return false;
    WinHttpSetTimeouts(session, 20000, 20000, 20000, 20000);
    Http conn{WinHttpConnect(session, host.c_str(), u.nPort, 0)};
    if (!conn.h) return false;
    Http req{WinHttpOpenRequest(conn, L"GET", resource.c_str(), nullptr, nullptr,
                                WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
    if (!req.h) return false;
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, nullptr)) {
        return false;
    }
    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) return false;

    unsigned long long total = 0;
    len = sizeof(total);
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &total, &len, WINHTTP_NO_HEADER_INDEX)) {
        total = 0;
    }

    std::ofstream f(std::filesystem::path(target), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    char buf[65536];
    unsigned long long offset = 0;
    DWORD got = 0;
    while (!cancel) {
        if (!WinHttpReadData(req, buf, sizeof(buf), &got)) break;
        if (!got) break;
        f.write(buf, (std::streamsize)got);
        if (!f) return false;
        offset += got;
        if (total) update(L"正在下载 OCR 组件  " + std::to_wstring(offset * 90 / total) + L"%",
                          (int)(offset * 90 / total));
    }
    f.close();
    return !cancel && offset > 0 && (!total || offset == total);
}

bool extract_package(const std::wstring& zip, const std::wstring& dest) {
    std::error_code ec;
    std::filesystem::remove_all(dest, ec);
    std::filesystem::create_directories(dest, ec);
    if (ec) return false;

    std::wstring cmd = L"tar.exe -xf \"" + zip + L"\" -C \"" + dest + L"\"";
    std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back(0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0 && dir_ready(dest);
}

}  // namespace

std::wstring ppocr_component_dir() {
    return localappdata_dir() + L"\\ocr-ncnn";
}

bool ppocr_component_ready() {
    return dir_ready(ppocr_component_dir()) || dir_ready(exe_dir() + L"\\ocr-ncnn");
}

std::wstring ppocr_component_status_text() {
    if (ppocr_component_ready()) return L"PP-OCRv6 基础模型已安装，可直接识别。";
    return L"OCR 组件未安装。点击下方按钮下载组件和 PP-OCRv6 模型。";
}

PpOcrDownloadState ppocr_download_state() {
    std::lock_guard<std::mutex> guard(stateMutex);
    return state;
}

void ppocr_cancel_download() {
    cancel = true;
}

void ppocr_download_shutdown() {
    cancel = true;
    if (worker.joinable()) worker.join();
}

void ppocr_download_async() {
    if (ppocr_download_state().busy) return;
    if (worker.joinable()) worker.join();
    if (ppocr_component_ready()) return;

    cancel = false;
    {
        std::lock_guard<std::mutex> guard(stateMutex);
        state = {true, 0, L"准备下载 OCR 组件…"};
    }
    worker = std::thread([] {
        try {
            std::wstring base = localappdata_dir() + L"\\downloads";
            std::filesystem::create_directories(base);
            std::wstring zip = base + L"\\weijie-ocr-ncnn.zip";
            if (!download_package(zip)) throw std::runtime_error("download failed");
            if (cancel) throw std::runtime_error("cancelled");
            update(L"正在校验 OCR 组件…", 92);
            std::wstring hash;
            if (!sha256_file(zip, hash) || _wcsicmp(hash.c_str(), kPackageSha256) != 0) {
                throw std::runtime_error("checksum failed");
            }
            update(L"正在安装 OCR 组件…", 96);
            if (!extract_package(zip, ppocr_component_dir())) {
                throw std::runtime_error("extract failed");
            }
            std::filesystem::remove(zip);
            update(L"OCR 组件下载完成。", 100);
        } catch (...) {
            update(cancel ? L"已取消下载。" : L"OCR 组件下载失败，请检查网络后重试。");
        }
        std::lock_guard<std::mutex> guard(stateMutex);
        state.busy = false;
    });
}
