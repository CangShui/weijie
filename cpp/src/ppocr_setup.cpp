#include "ppocr_setup.h"
#include "download.h"
#include "models.h"

#include <filesystem>
#include <mutex>
#include <thread>
#include <atomic>
#include <stdexcept>
#include <vector>

namespace {

constexpr wchar_t kPackageUrl[] =
    L"https://github.com/CangShui/weijie/releases/download/models/weijie-ocr-ncnn.zip";
constexpr unsigned long long kPackageSize = 146397538ull;
constexpr wchar_t kPackageSha256[] =
    L"d9c9a0c098fde203f3078baee02d68850e1d60f70ada51642b606c6e4f658ad7";

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
    // ncnn is linked into the sidecar; only the OpenCV runtime is external.
    for (const wchar_t* name : {
             L"ocr_ncnn.exe", L"opencv_world4110.dll",
             L"config-cpu.json", L"config-gpu.json",
             L"models\\PP_OCRv6_small_det.param", L"models\\PP_OCRv6_small_det.bin",
             L"models\\PP_OCRv6_small_rec.param", L"models\\PP_OCRv6_small_rec.bin",
             L"models\\PP_LCNet_x0_25_textline_ori.param",
             L"models\\PP_LCNet_x0_25_textline_ori.bin",
             L"models\\ppocr_keys_v6.txt",
             L"models\\PP-OCRv6_tiny_det.param", L"models\\PP-OCRv6_tiny_rec.param",
             L"models\\PP-OCRv6_vocab_tiny.txt",
             L"models\\PP-OCRv6_medium_det.param", L"models\\PP-OCRv6_medium_rec.param",
             L"models\\PP-OCRv6_vocab.txt"}) {
        if (!file_exists(dir + L"\\" + name)) return false;
    }
    return true;
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
    return runtime_root_dir() + L"\\ppocr-ncnn";
}

bool ppocr_component_ready() {
    return dir_ready(ppocr_component_dir()) || dir_ready(exe_dir() + L"\\ocr-ncnn");
}

std::wstring ppocr_component_status_text() {
    if (ppocr_component_ready()) return L"PP-OCRv6 已安装：tiny / small / medium 三档可选。";
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
        LogTrace trace;
        logf(L"PP-OCR 组件下载请求到达");
        try {
            std::filesystem::create_directories(models_dir());
            const std::wstring zip = models_dir() + L"\\weijie-ocr-ncnn.zip";
            std::atomic<unsigned long long> done{0};
            std::wstring error;
            bool ok = download_file(kPackageUrl, zip, kPackageSize, cancel,
                                    [&](unsigned long long bytes) {
                                        done += bytes;
                                        int percent = (int)(done.load() * 90 / kPackageSize);
                                        update(L"正在下载 OCR 组件  " + human_size(done.load()) +
                                                   L" / " + human_size(kPackageSize),
                                               percent);
                                    },
                                    error);
            if (!ok) {
                if (cancel) throw std::runtime_error("cancel");
                logf(L"PP-OCR 组件下载失败：%s", error.c_str());
                throw std::runtime_error("download");
            }
            if (cancel) throw std::runtime_error("cancel");
            update(L"正在校验 OCR 组件…", 92);
            if (!verify_sha256(zip, kPackageSha256)) {
                DeleteFileW(zip.c_str());
                logf(L"PP-OCR 组件 SHA-256 校验失败，已删除下载文件");
                throw std::runtime_error("checksum");
            }
            update(L"正在安装 OCR 组件…", 96);
            if (!extract_package(zip, ppocr_component_dir())) {
                logf(L"PP-OCR 组件解压失败：目标=%s", ppocr_component_dir().c_str());
                throw std::runtime_error("extract");
            }
            std::filesystem::remove(zip);
            update(L"OCR 组件下载完成。", 100);
            logf(L"PP-OCR 组件安装完成：%s", ppocr_component_dir().c_str());
        } catch (const std::exception&) {
            update(cancel ? L"已取消下载。" : L"OCR 组件下载失败，请检查网络后重试。");
        }
        std::lock_guard<std::mutex> guard(stateMutex);
        state.busy = false;
    });
}
