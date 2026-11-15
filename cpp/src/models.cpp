#include "models.h"
#include "download.h"

#include <filesystem>
#include <fstream>
#include <vector>

namespace {

constexpr wchar_t kRuntimeZip[] = L"weijie-llama-runtime.zip";
constexpr unsigned long long kRuntimeSize = 38120301ull;
constexpr wchar_t kRuntimeSha256[] =
    L"89402e1b11f3be31a9f1e09eb4d7ba766a9cf942f573917a401f10d6af663d31";
constexpr wchar_t kRuntimeUrl[] =
    L"https://github.com/CangShui/weijie/releases/download/models/weijie-llama-runtime.zip";

bool regular_file(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fd{};
    return GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fd) &&
           !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool extract_zip(const std::wstring& zip, const std::wstring& dest) {
    std::error_code ec;
    std::filesystem::create_directories(dest, ec);
    if (ec) return false;
    std::wstring command = L"tar.exe -xf \"" + zip + L"\" -C \"" + dest + L"\"";
    std::vector<wchar_t> line(command.begin(), command.end());
    line.push_back(0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        return false;
    }
    WaitForSingleObject(pi.hProcess, 180000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

bool endswith(const std::wstring& text, const wchar_t* suffix) {
    size_t length = wcslen(suffix);
    return text.size() >= length &&
           _wcsicmp(text.c_str() + text.size() - length, suffix) == 0;
}

// 同一卷直接改名；跨卷复制后保留时间戳，旧目录的 .verified 标记继续有效。
bool move_or_copy(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code ec;
    if (std::filesystem::exists(to, ec)) return true;
    std::filesystem::rename(from, to, ec);
    if (!ec) return true;
    ec.clear();
    std::filesystem::copy_file(from, to, std::filesystem::copy_options::none, ec);
    if (ec) return false;
    auto stamp = std::filesystem::last_write_time(from, ec);
    if (!ec) std::filesystem::last_write_time(to, stamp, ec);
    ec.clear();
    std::filesystem::remove(from, ec);
    return true;
}

void migrate_tree(const std::wstring& from, const std::wstring& to, const wchar_t* label) {
    std::error_code ec;
    if (!std::filesystem::is_directory(from, ec)) return;
    int moved = 0, kept = 0, failed = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(
             from, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        const auto& source = it->path();
        auto relative = std::filesystem::relative(source, from, ec);
        if (ec) break;
        const auto target = std::filesystem::path(to) / relative;
        if (it->is_directory(ec)) {
            std::filesystem::create_directories(target, ec);
            ec.clear();
            continue;
        }
        std::filesystem::create_directories(target.parent_path(), ec);
        ec.clear();
        if (std::filesystem::exists(target, ec)) {
            ++kept;
            continue;
        }
        if (move_or_copy(source, target)) ++moved;
        else ++failed;
    }
    logf(L"模型迁移：%s 来源=%s 目标=%s 移动=%d 已存在=%d 失败=%d",
         label, from.c_str(), to.c_str(), moved, kept, failed);
}

}  // namespace

std::vector<GgufFile> list_gguf_models() {
    std::vector<GgufFile> models;
    std::error_code ec;
    const std::wstring dir = weights_dir();
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const std::wstring name = entry.path().filename().wstring();
        if (!endswith(name, L".gguf")) continue;
        std::error_code size_ec;
        const auto size = entry.file_size(size_ec);
        models.push_back({name, size_ec ? 0ull : (unsigned long long)size});
    }
    std::sort(models.begin(), models.end(),
              [](const GgufFile& a, const GgufFile& b) { return a.name < b.name; });
    return models;
}

std::wstring human_size(unsigned long long bytes) {
    wchar_t text[64]{};
    if (bytes >= (1ull << 30)) {
        _snwprintf_s(text, _countof(text), _TRUNCATE, L"%.1f GB", bytes / 1073741824.0);
    } else if (bytes >= (1ull << 20)) {
        _snwprintf_s(text, _countof(text), _TRUNCATE, L"%.1f MB", bytes / 1048576.0);
    } else if (bytes >= 1024) {
        _snwprintf_s(text, _countof(text), _TRUNCATE, L"%.0f KB", bytes / 1024.0);
    } else {
        _snwprintf_s(text, _countof(text), _TRUNCATE, L"%llu B", bytes);
    }
    return text;
}

std::wstring llama_runtime_dir() {
    return runtime_root_dir() + L"\\llama";
}

bool llama_runtime_ready() {
    const std::wstring dir = llama_runtime_dir();
    for (const wchar_t* name : {L"llama-cli.exe", L"llama-mtmd-cli.exe", L"llama.dll",
                                L"mtmd.dll", L"llama-common.dll", L"ggml.dll",
                                L"ggml-base.dll"}) {
        if (!regular_file(dir + L"\\" + name)) return false;
    }
    return true;
}

unsigned long long llama_runtime_size() {
    return llama_runtime_ready() ? 0ull : kRuntimeSize;
}

bool install_llama_runtime(std::atomic<bool>& cancel,
                           const std::function<void(unsigned long long)>& on_bytes,
                           std::wstring& err) {
    if (llama_runtime_ready()) return true;
    const std::wstring zip = models_dir() + L"\\" + kRuntimeZip;
    bool verified = disk_file_size(zip) == kRuntimeSize && verify_sha256(zip, kRuntimeSha256);
    if (!verified) {
        DeleteFileW(zip.c_str());
        if (!download_file(kRuntimeUrl, zip, kRuntimeSize, cancel, on_bytes, err)) return false;
        if (!verify_sha256(zip, kRuntimeSha256)) {
            DeleteFileW(zip.c_str());
            err = L"运行时 SHA-256 校验失败";
            return false;
        }
    }
    if (!extract_zip(zip, llama_runtime_dir()) || !llama_runtime_ready()) {
        err = L"运行时解压失败";
        return false;
    }
    DeleteFileW(zip.c_str());
    return true;
}

void migrate_legacy_models() {
    const std::wstring weights = weights_dir();
    const std::wstring runtime = runtime_root_dir();
    migrate_tree(localappdata_dir() + L"\\hunyuan-ocr", weights, L"混元 OCR 权重");
    migrate_tree(localappdata_dir() + L"\\ocr-ncnn", runtime + L"\\ppocr-ncnn",
                 L"PP-OCR 运行时");
    migrate_tree(exe_dir() + L"\\ocr-ncnn", runtime + L"\\ppocr-ncnn", L"PP-OCR 运行时");
    for (const wchar_t* legacy : {L"ocr-runtime", L"ocr-runtime-gpu"}) {
        const std::wstring old = exe_dir() + L"\\" + legacy;
        std::error_code ec;
        if (!std::filesystem::is_directory(old, ec)) continue;
        const std::wstring target = llama_runtime_dir();
        std::filesystem::create_directories(target, ec);
        int moved = 0;
        for (const auto& entry : std::filesystem::directory_iterator(old, ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec)) continue;
            const std::wstring name = entry.path().filename().wstring();
            if (!endswith(name, L".dll") && !endswith(name, L".exe")) continue;
            if (move_or_copy(entry.path(), std::filesystem::path(target) / name)) ++moved;
        }
        logf(L"模型迁移：%s 运行时移动到 %s，数量=%d", legacy, target.c_str(), moved);
    }
}
