#include "ocr_setup.h"
#include "download.h"
#include "models.h"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <atomic>
#include <thread>
#include <stdexcept>

namespace {

struct Asset {
    const wchar_t* name;
    unsigned long long size;
    const wchar_t* hash;
};

const Asset kQ8{L"HunyuanOCR-Q8_0.gguf", 577949408ull,
                L"cdafc794cafeae377868d7a40a70e282a737e39abe77c0d8b73614447b364a21"};
const Asset kQ3{L"HunyuanOCR-Q3_K_S.gguf", 285220352ull,
                L"b44649df28d7c743e2ddecff23ae39fb1f7c4639da6796214ea23ee89574725b"};
const Asset kMmproj{L"mmproj-HunyuanOCR-Q8_0.gguf", 732938240ull,
                    L"b77913164ff73d4c0dc4d994e236ed72bacbbe5c5db1ec9b2828627b46c32804"};

const wchar_t* const kBaseUrl =
    L"https://github.com/CangShui/weijie/releases/download/models/";

std::thread worker;
std::atomic<bool> cancel{false};
std::mutex stateMutex;
OcrDownloadState state;

void update(std::wstring text, int percent = -1) {
    std::lock_guard<std::mutex> guard(stateMutex);
    state.text = std::move(text);
    if (percent >= 0) state.percent = percent;
}

std::wstring url_for(const wchar_t* name) {
    return std::wstring(kBaseUrl) + name;
}

std::wstring asset_path(const wchar_t* name) {
    return weights_dir() + L"\\" + name;
}

// 校验标记同时记录哈希与写入时间，文件被替换后会自动失效。
std::string stamp(const std::wstring& path, const Asset& asset) {
    WIN32_FILE_ATTRIBUTE_DATA fd{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fd)) return {};
    unsigned long long size = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    if (size != asset.size) return {};
    std::string text = w_to_u8(asset.hash);
    text += ":" + std::to_string(fd.ftLastWriteTime.dwHighDateTime) + ":" +
            std::to_string(fd.ftLastWriteTime.dwLowDateTime);
    return text;
}

bool marked(const Asset& asset) {
    const std::wstring path = asset_path(asset.name);
    std::ifstream marker(std::filesystem::path(path + L".verified"));
    std::string line;
    std::getline(marker, line);
    const std::string expected = stamp(path, asset);
    return !expected.empty() && line == expected;
}

void write_marker(const Asset& asset) {
    const std::wstring path = asset_path(asset.name);
    std::ofstream marker(std::filesystem::path(path + L".verified"),
                         std::ios::trunc);
    marker << stamp(path, asset);
}

}  // namespace

std::wstring hunyuan_ocr_dir() {
    return weights_dir();
}

std::wstring hunyuan_runtime_dir(bool /*gpu*/) {
    return llama_runtime_ready() ? llama_runtime_dir() : std::wstring();
}

bool hunyuan_runtime_ready() {
    return llama_runtime_ready();
}

bool hunyuan_model_ready() {
    return hunyuan_runtime_ready() && marked(kQ8) && marked(kMmproj);
}

bool hunyuan_fast_model_ready() {
    return hunyuan_runtime_ready() && marked(kQ3) && marked(kMmproj);
}

std::wstring hunyuan_model_status_text() {
    if (!hunyuan_runtime_ready()) return L"缺少模型运行时，点击下方按钮下载（约 37 MB）。";
    if (hunyuan_model_ready()) return L"权重 SHA-256 校验通过，可离线识别。";
    return L"未安装或未校验。下载约 1.31 GB，支持断点续传和多线程。";
}

std::wstring hunyuan_fast_model_status_text() {
    if (!hunyuan_runtime_ready()) return L"缺少模型运行时，点击下方按钮下载（约 37 MB）。";
    if (hunyuan_fast_model_ready()) return L"权重 SHA-256 校验通过，可离线识别。";
    return L"未安装或未校验。下载约 1.02 GB，支持断点续传和多线程。";
}

unsigned long long hunyuan_download_size(bool fast) {
    unsigned long long total = llama_runtime_size();
    const Asset& main = fast ? kQ3 : kQ8;
    if (!marked(main)) total += main.size;
    if (!marked(kMmproj)) total += kMmproj.size;
    return total;
}

OcrDownloadState hunyuan_download_state() {
    std::lock_guard<std::mutex> guard(stateMutex);
    return state;
}

void hunyuan_cancel_download() {
    cancel = true;
}

void hunyuan_download_shutdown() {
    cancel = true;
    if (worker.joinable()) worker.join();
}

void hunyuan_download_models_async(bool fast) {
    if (hunyuan_download_state().busy) return;
    if (worker.joinable()) worker.join();
    cancel = false;
    {
        std::lock_guard<std::mutex> guard(stateMutex);
        state = {true, 0, L"准备下载 / 校验…"};
    }
    worker = std::thread([fast] {
        LogTrace trace;
        logf(L"混元 OCR 下载请求到达：引擎=%s", fast ? L"轻量 Q3" : L"高精度 Q8");
        std::atomic<unsigned long long> done{0};
        unsigned long long total = hunyuan_download_size(fast);
        auto report = [&](const std::wstring& label) {
            unsigned long long now = done.load();
            int percent = total ? (int)(now * 100 / total) : 0;
            update(label + L"  " + human_size(now) + L" / " + human_size(total), percent);
        };
        auto on_bytes = [&](unsigned long long bytes) { done += bytes; report(L"正在下载"); };
        try {
            std::filesystem::create_directories(weights_dir());
            std::wstring error;
            if (!install_llama_runtime(cancel, on_bytes, error)) {
                if (cancel) throw std::runtime_error("cancel");
                logf(L"混元 OCR 运行时安装失败：%s", error.c_str());
                throw std::runtime_error("runtime");
            }
            const Asset selected[] = {fast ? kQ3 : kQ8, kMmproj};
            for (const Asset& asset : selected) {
                if (cancel) throw std::runtime_error("cancel");
                if (marked(asset)) continue;
                const std::wstring dest = asset_path(asset.name);
                if (disk_file_size(dest) == asset.size && verify_sha256(dest, asset.hash)) {
                    write_marker(asset);
                    continue;
                }
                bool ok = false;
                for (int attempt = 0; attempt < 2 && !cancel; ++attempt) {
                    report(L"正在下载 " + std::wstring(asset.name));
                    if (!download_file(url_for(asset.name), dest, asset.size, cancel,
                                       on_bytes, error)) {
                        if (cancel) throw std::runtime_error("cancel");
                        logf(L"混元 OCR 下载失败：文件=%s 尝试=%d 原因=%s", asset.name,
                             attempt + 1, error.c_str());
                        continue;
                    }
                    update(L"正在校验 SHA-256…", total ? (int)(done.load() * 100 / total) : 0);
                    if (verify_sha256(dest, asset.hash)) {
                        ok = true;
                        break;
                    }
                    logf(L"混元 OCR 校验失败：文件=%s 尝试=%d，删除后重试", asset.name,
                         attempt + 1);
                    DeleteFileW(dest.c_str());
                }
                if (!ok) throw std::runtime_error("checksum");
                write_marker(asset);
            }
            update(L"模型校验完成。勾选启用并保存后显示识别按钮。", 100);
            logf(L"混元 OCR 下载完成：引擎=%s", fast ? L"轻量 Q3" : L"高精度 Q8");
        } catch (const std::exception& error) {
            const std::wstring reason = u8_to_w(error.what());
            if (reason == L"cancel") {
                update(L"已取消，已下载分片会保留，可继续下载。");
                logf(L"混元 OCR 下载已取消，保留断点");
            } else if (reason == L"runtime") {
                update(L"运行时下载或解压失败，请检查网络与磁盘后重试。");
            } else {
                update(L"模型下载或校验失败，请检查网络、磁盘空间后重试。");
            }
        }
        std::lock_guard<std::mutex> guard(stateMutex);
        state.busy = false;
    });
}
