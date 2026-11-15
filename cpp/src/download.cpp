#include "download.h"

#include <winhttp.h>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

namespace {

struct Http {
    HINTERNET h{};
    ~Http() { if (h) WinHttpCloseHandle(h); }
    operator HINTERNET() const { return h; }
};

struct Target {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 443;
    bool secure = true;
};

bool split_url(const std::wstring& url, Target& target) {
    URL_COMPONENTS u{};
    u.dwStructSize = sizeof(u);
    u.dwHostNameLength = (DWORD)-1;
    u.dwUrlPathLength = (DWORD)-1;
    u.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &u)) return false;
    target.host.assign(u.lpszHostName, u.dwHostNameLength);
    target.path.assign(u.lpszUrlPath, u.dwUrlPathLength);
    if (u.dwExtraInfoLength) target.path.append(u.lpszExtraInfo, u.dwExtraInfoLength);
    target.port = u.nPort;
    target.secure = u.nScheme == INTERNET_SCHEME_HTTPS;
    return !target.host.empty() && !target.path.empty();
}

// 单个分片：从 begin 开始追加到 part，服务器必须返回 206。
bool fetch_range(const Target& target, unsigned long long begin, unsigned long long end,
                 const std::wstring& part, std::atomic<bool>& cancel,
                 const std::function<void(unsigned long long)>& on_bytes,
                 std::wstring& err) {
    Http session{WinHttpOpen(L"weijie/1.7", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.h) {
        session.h = WinHttpOpen(L"weijie/1.7", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session.h) { err = L"网络初始化失败"; return false; }
    WinHttpSetTimeouts(session, 20000, 20000, 30000, 60000);
    Http conn{WinHttpConnect(session, target.host.c_str(), target.port, 0)};
    if (!conn.h) { err = L"无法连接下载服务器"; return false; }
    Http req{WinHttpOpenRequest(conn, L"GET", target.path.c_str(), nullptr, nullptr,
                                WINHTTP_DEFAULT_ACCEPT_TYPES,
                                target.secure ? WINHTTP_FLAG_SECURE : 0)};
    if (!req.h) { err = L"创建下载请求失败"; return false; }
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    std::wstring range = L"Range: bytes=" + std::to_wstring(begin) + L"-";
    if (end != ~0ull) range += std::to_wstring(end);
    range += L"\r\n";
    if (!WinHttpSendRequest(req, range.c_str(), (DWORD)-1, nullptr, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, nullptr)) {
        err = L"下载请求失败";
        return false;
    }
    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
    if (status != 206) {
        err = L"服务器返回 HTTP " + std::to_wstring(status);
        return false;
    }
    std::ofstream out(std::filesystem::path(part), std::ios::binary | std::ios::app);
    if (!out) { err = L"无法写入下载文件"; return false; }
    std::vector<char> buf(1 << 20);
    while (!cancel) {
        DWORD got = 0;
        if (!WinHttpReadData(req, buf.data(), (DWORD)buf.size(), &got)) {
            err = L"下载中断";
            return false;
        }
        if (!got) break;
        if (end != ~0ull && begin + got > end + 1) { err = L"下载数据超出预期"; return false; }
        out.write(buf.data(), got);
        if (!out) { err = L"写入磁盘失败"; return false; }
        begin += got;
        on_bytes(got);
    }
    out.close();
    if (cancel) { err = L"已取消"; return false; }
    if (end != ~0ull && begin != end + 1) { err = L"分片下载不完整"; return false; }
    return true;
}

}  // namespace

unsigned long long disk_file_size(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fd{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fd)) return 0;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return 0;
    return ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
}

bool sha256_file(const std::wstring& path, std::wstring& hex_lower) {
    BCRYPT_ALG_HANDLE alg{};
    BCRYPT_HASH_HANDLE hash{};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    std::vector<char> buf(1 << 20);
    bool ok = bool(f);
    while (f && ok) {
        f.read(buf.data(), (std::streamsize)buf.size());
        auto n = f.gcount();
        if (n > 0 && BCryptHashData(hash, (PUCHAR)buf.data(), (ULONG)n, 0) < 0) ok = false;
    }
    unsigned char digest[32]{};
    ok = ok && f.eof() && BCryptFinishHash(hash, digest, 32, 0) >= 0;
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return false;
    static const wchar_t* digits = L"0123456789abcdef";
    hex_lower.clear();
    for (auto b : digest) {
        hex_lower.push_back(digits[b >> 4]);
        hex_lower.push_back(digits[b & 15]);
    }
    return true;
}

bool verify_sha256(const std::wstring& path, const std::wstring& expected_lower) {
    std::wstring actual;
    return sha256_file(path, actual) && _wcsicmp(actual.c_str(), expected_lower.c_str()) == 0;
}

bool download_file(const std::wstring& url, const std::wstring& dest,
                   unsigned long long expected_size, std::atomic<bool>& cancel,
                   const std::function<void(unsigned long long)>& on_bytes,
                   std::wstring& err) {
    if (expected_size && disk_file_size(dest) == expected_size) return true;
    Target target;
    if (!split_url(url, target)) { err = L"下载地址无效"; return false; }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(dest).parent_path(), ec);

    if (expected_size == 0) {
        DeleteFileW(dest.c_str());
        return fetch_range(target, 0, ~0ull, dest, cancel, on_bytes, err);
    }

    const int count = expected_size < (16ull << 20) ? 1 : 4;
    std::vector<unsigned long long> starts(count), ends(count);
    const unsigned long long per = expected_size / count;
    for (int i = 0; i < count; ++i) {
        starts[i] = per * i;
        ends[i] = (i == count - 1) ? expected_size - 1 : per * (i + 1) - 1;
    }
    std::atomic<int> failed{0};
    std::mutex errorMutex;
    std::wstring firstError;
    std::vector<std::thread> workers;
    for (int i = 0; i < count; ++i) {
        workers.emplace_back([&, i] {
            const std::wstring part = dest + L".part" + std::to_wstring(i);
            const unsigned long long need = ends[i] - starts[i] + 1;
            for (int attempt = 0; attempt < 5 && !cancel; ++attempt) {
                unsigned long long have = disk_file_size(part);
                if (have > need) { DeleteFileW(part.c_str()); have = 0; }
                if (have == need) return;
                std::wstring e;
                if (fetch_range(target, starts[i] + have, ends[i], part, cancel, on_bytes, e) &&
                    disk_file_size(part) == need) {
                    return;
                }
                if (cancel) return;
                {
                    std::lock_guard<std::mutex> guard(errorMutex);
                    if (firstError.empty()) firstError = e;
                }
                Sleep(1000u * (unsigned)(attempt + 1));
            }
            ++failed;
        });
    }
    for (auto& worker : workers) worker.join();
    if (cancel) { err = L"已取消，已下载的分片会保留"; return false; }
    if (failed) { err = firstError.empty() ? L"下载失败" : firstError; return false; }

    const std::wstring merged = dest + L".merging";
    {
        std::ofstream out(std::filesystem::path(merged), std::ios::binary | std::ios::trunc);
        if (!out) { err = L"无法写入合并文件"; return false; }
        for (int i = 0; i < count; ++i) {
            std::ifstream in(std::filesystem::path(dest + L".part" + std::to_wstring(i)),
                             std::ios::binary);
            if (!in) { err = L"分片文件缺失"; return false; }
            out << in.rdbuf();
        }
    }
    if (disk_file_size(merged) != expected_size) {
        DeleteFileW(merged.c_str());
        err = L"下载数据不完整";
        return false;
    }
    if (!MoveFileExW(merged.c_str(), dest.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(merged.c_str());
        err = L"无法保存下载文件";
        return false;
    }
    for (int i = 0; i < count; ++i) DeleteFileW((dest + L".part" + std::to_wstring(i)).c_str());
    return true;
}
