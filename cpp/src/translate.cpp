#include "translate.h"
#include "download.h"
#include "models.h"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include <iterator>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

namespace {

const TranslateModelInfo kCatalog[] = {
    {L"Haidass-Translate-143M.Q8_0.gguf", 153655008ull,
     L"https://github.com/CangShui/weijie/releases/download/models/Haidass-Translate-143M.Q8_0.gguf",
     L"2f96047bc633752aefe0462df270f9f8f39bd3fb484a90dc4e08314f78507d13"},
    {L"Qwen3.5-0.8B-Q8_0.gguf", 833592096ull,
     L"https://github.com/CangShui/weijie/releases/download/models/Qwen3.5-0.8B-Q8_0.gguf",
     L"37ae482d336108d23516fa35e8e0c4126688d81018b87178a18d752a1357814f"},
    {L"Index-Translate-2B.Q4_K_M.gguf", 1312164352ull,
     L"https://github.com/CangShui/weijie/releases/download/models/Index-Translate-2B.Q4_K_M.gguf",
     L"044b313d29342bd3b2c77cbb64023ca0d209bd9b3247763f9d162767ef2d746a"},
    {L"Index-Translate-2B.Q8_0.gguf", 2076674560ull,
     L"https://github.com/CangShui/weijie/releases/download/models/Index-Translate-2B.Q8_0.gguf",
     L"a09d287f41d297206947a2e51acc29dd03c8b22356111fdb1e289c0c97c173f3"},
};

std::thread worker;
std::atomic<bool> cancel{false};
std::mutex stateMutex;
TranslateDownloadState state;

void update(std::wstring text, int percent = -1) {
    std::lock_guard<std::mutex> guard(stateMutex);
    state.text = std::move(text);
    if (percent >= 0) state.percent = percent;
}

void trim_inplace(std::wstring& text) {
    size_t begin = 0, end = text.size();
    while (begin < end && iswspace(text[begin])) ++begin;
    while (end > begin && iswspace(text[end - 1])) --end;
    text = text.substr(begin, end - begin);
}

std::string read_all_utf8(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    std::string out;
    if (size.QuadPart > 0 && size.QuadPart < (32 << 20)) {
        out.resize((size_t)size.QuadPart);
        DWORD got = 0;
        ReadFile(file, out.data(), (DWORD)out.size(), &got, nullptr);
        out.resize(got);
    }
    CloseHandle(file);
    return out;
}

void detect(const std::wstring& text, std::wstring& src, std::wstring& dst) {
    int cjk = 0, latin = 0;
    for (wchar_t c : text) {
        unsigned value = (unsigned)c;
        if ((value >= 0x4E00 && value <= 0x9FFF) || (value >= 0x3400 && value <= 0x4DBF)) ++cjk;
        else if ((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z')) ++latin;
    }
    if (cjk >= latin) { src = L"zh-CN"; dst = L"英文"; }
    else { src = L"en"; dst = L"简体中文"; }
}

std::vector<std::wstring> split_chunks(const std::wstring& text, size_t limit) {
    std::vector<std::wstring> parts;
    std::wstring current;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t newline = text.find(L'\n', pos);
        std::wstring line = text.substr(pos, newline == std::wstring::npos
                                                 ? std::wstring::npos : newline - pos);
        pos = (newline == std::wstring::npos) ? text.size() : newline + 1;
        while (line.size() > limit) {
            if (!current.empty()) { parts.push_back(current); current.clear(); }
            parts.push_back(line.substr(0, limit));
            line = line.substr(limit);
        }
        if (!current.empty() && current.size() + line.size() + 1 > limit) {
            parts.push_back(current);
            current.clear();
        }
        if (!current.empty()) current += L'\n';
        current += line;
    }
    if (!current.empty()) parts.push_back(current);
    std::vector<std::wstring> result;
    for (auto& part : parts) {
        trim_inplace(part);
        if (!part.empty()) result.push_back(part);
    }
    if (result.empty()) result.push_back(text);
    return result;
}

bool contains(const std::wstring& text, const wchar_t* needle) {
    return text.find(needle) != std::wstring::npos;
}

std::wstring build_prompt(const std::wstring& text, const std::wstring& dst,
                          const std::wstring& model) {
    if (contains(model, L"Haidass")) {
        if (dst == L"英文") {
            return L"请将以下简体中文翻译成英文。\n" + text;
        }
        return L"Translate the following text from English to Simplified Chinese.\n" + text;
    }
    return L"请将以下文本翻译为" + dst + L"，直接输出译文，不要解释。\n\n" + text;
}

}  // namespace

const std::vector<TranslateModelInfo>& translate_catalog() {
    static const std::vector<TranslateModelInfo> catalog(std::begin(kCatalog), std::end(kCatalog));
    return catalog;
}

namespace {

bool run_llama(const std::wstring& prompt, const std::wstring& model, bool gpu, int maxTokens,
               std::wstring& output, std::wstring& error) {
    wchar_t temp[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temp)) { error = L"无法创建翻译临时目录"; return false; }
    const std::wstring base = std::wstring(temp) + L"weijieTranslate";
    CreateDirectoryW(base.c_str(), nullptr);
    wchar_t unique[MAX_PATH]{};
    if (!GetTempFileNameW(base.c_str(), L"tr", 0, unique)) {
        error = L"无法创建翻译临时文件";
        return false;
    }
    const std::wstring promptPath = std::wstring(unique) + L".prompt";
    const std::wstring outPath = std::wstring(unique) + L".out";
    const std::wstring errPath = std::wstring(unique) + L".err";
    DeleteFileW(unique);

    {
        std::ofstream promptFile(std::filesystem::path(promptPath), std::ios::binary | std::ios::trunc);
        if (!promptFile) { error = L"无法写入翻译临时文件"; return false; }
        promptFile << w_to_u8(prompt);
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE outHandle = CreateFileW(outPath.c_str(), GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ,
                                   &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE errHandle = CreateFileW(errPath.c_str(), GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ,
                                   &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (outHandle == INVALID_HANDLE_VALUE || errHandle == INVALID_HANDLE_VALUE) {
        if (outHandle != INVALID_HANDLE_VALUE) CloseHandle(outHandle);
        if (errHandle != INVALID_HANDLE_VALUE) CloseHandle(errHandle);
        DeleteFileW(promptPath.c_str());
        error = L"无法创建翻译临时文件";
        return false;
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = outHandle;
    si.hStdError = errHandle;
    HANDLE inHandle = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  &sa, OPEN_EXISTING, 0, nullptr);
    si.hStdInput = inHandle;

    const std::wstring command =
        L"\"" + llama_runtime_dir() + L"\\llama-cli.exe\" -m \"" + model +
        L"\" -f \"" + promptPath + L"\" -st -n " + std::to_wstring(maxTokens) +
        L" --temp 0 --no-display-prompt -rea off -ngl " + (gpu ? L"99" : L"0");
    std::vector<wchar_t> line(command.begin(), command.end());
    line.push_back(0);
    PROCESS_INFORMATION pi{};
    BOOL started = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE,
                                  CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                                  llama_runtime_dir().c_str(), &si, &pi);
    CloseHandle(outHandle);
    CloseHandle(errHandle);
    if (inHandle != INVALID_HANDLE_VALUE) CloseHandle(inHandle);
    if (!started) {
        DeleteFileW(promptPath.c_str());
        DeleteFileW(outPath.c_str());
        DeleteFileW(errPath.c_str());
        error = L"无法启动本地翻译进程";
        return false;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job) {
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        AssignProcessToJobObject(job, pi.hProcess);
    }
    ResumeThread(pi.hThread);
    bool timeout = WaitForSingleObject(pi.hProcess, 300000) == WAIT_TIMEOUT;
    if (timeout) {
        if (job) TerminateJobObject(job, 2);
        WaitForSingleObject(pi.hProcess, 3000);
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);

    const std::string raw = read_all_utf8(outPath);
    const std::string tail = read_all_utf8(errPath);
    DeleteFileW(promptPath.c_str());
    DeleteFileW(outPath.c_str());
    DeleteFileW(errPath.c_str());

    if (timeout) { error = L"本地翻译超时"; return false; }
    output = u8_to_w(raw);
    if (code != 0 && output.empty()) {
        error = L"本地翻译失败（退出码 " + std::to_wstring((unsigned long)code) + L"）";
        if (!tail.empty()) {
            std::wstring detail = u8_to_w(tail);
            if (detail.size() > 400) detail = detail.substr(detail.size() - 400);
            error += L"\n" + detail;
        }
        return false;
    }
    output = u8_to_w(raw);
    return true;
}

void normalize_newlines(std::wstring& text) {
    std::wstring out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\r') {
            if (i + 1 < text.size() && text[i + 1] == L'\n') ++i;
            out += L'\n';
        } else {
            out += text[i];
        }
    }
    text.swap(out);
}

// 去掉终端配色等 CSI 序列，避免它们混进译文。
void strip_ansi(std::wstring& text) {
    std::wstring out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == 0x1B && i + 1 < text.size() && text[i + 1] == L'[') {
            i += 2;
            while (i < text.size() && !(text[i] >= 0x40 && text[i] <= 0x7E)) ++i;
            continue;
        }
        out += text[i];
    }
    text.swap(out);
}

std::wstring extract_answer(std::wstring raw, const std::wstring& prompt) {
    strip_ansi(raw);
    normalize_newlines(raw);
    size_t stats = raw.find(L"[ Prompt:");
    if (stats != std::wstring::npos) raw.resize(stats);
    size_t echo = raw.rfind(prompt);
    if (echo != std::wstring::npos) raw = raw.substr(echo + prompt.size());
    size_t exiting = raw.rfind(L"Exiting...");
    if (exiting != std::wstring::npos) raw.resize(exiting);
    trim_inplace(raw);
    if (raw.rfind(L"<think>", 0) == 0) {
        size_t close = raw.find(L"</think>");
        if (close != std::wstring::npos) {
            raw = raw.substr(close + 8);
            trim_inplace(raw);
        } else {
            raw.clear();
        }
    }
    while (raw.rfind(L"- - ", 0) == 0) {
        raw = raw.substr(4);
        trim_inplace(raw);
    }
    while (raw.rfind(L"> ", 0) == 0) {
        raw = raw.substr(2);
        trim_inplace(raw);
    }
    if (raw.size() > 2 && raw.front() == L'"' && raw.back() == L'"') {
        raw = raw.substr(1, raw.size() - 2);
        trim_inplace(raw);
    }
    return raw;
}

}  // namespace

TranslateResult translate_text(const std::wstring& text, const Settings& cfg) {
    TranslateResult result;
    LogTrace trace;
    std::wstring body = text;
    trim_inplace(body);
    if (body.empty()) {
        result.err = L"没有可翻译的文字";
        logf(L"本地翻译请求被拒绝：没有可翻译的文字");
        return result;
    }
    detect(body, result.src, result.dst);
    const std::wstring model = weights_dir() + L"\\" + cfg.translate_model;
    logf(L"本地翻译请求到达：模型=%s 字符数=%zu 方向=%s->%s",
         cfg.translate_model.c_str(), body.size(), result.src.c_str(), result.dst.c_str());
    if (cfg.translate_model.empty()) {
        result.err = L"未选择翻译模型，请在设置中下载或导入 GGUF 模型";
        logf(L"本地翻译被拒绝：未选择模型");
        return result;
    }
    if (!llama_runtime_ready()) {
        result.err = L"翻译运行时未安装，请在设置中下载翻译模型";
        logf(L"本地翻译被拒绝：运行时缺失 path=%s", llama_runtime_dir().c_str());
        return result;
    }
    if (disk_file_size(model) == 0) {
        result.err = L"翻译模型文件不存在：" + cfg.translate_model;
        logf(L"本地翻译被拒绝：模型文件缺失 path=%s", model.c_str());
        return result;
    }

    auto chunks = split_chunks(body, 900);
    if (chunks.size() > 6) {
        chunks.resize(6);
        logf(L"本地翻译文本过长，仅处理前 6 段");
    }
    const bool gpu = cfg.ocr_backend == L"gpu";
    std::wstring joined;
    for (size_t index = 0; index < chunks.size(); ++index) {
        const std::wstring prompt = build_prompt(chunks[index], result.dst, cfg.translate_model);
        int maxTokens = (int)std::min<size_t>(2048, std::max<size_t>(128, chunks[index].size() * 3 + 64));
        std::wstring raw, error;
        if (!run_llama(prompt, model, gpu, maxTokens, raw, error)) {
            result.err = error.empty() ? L"本地翻译失败" : error;
            logf(L"本地翻译失败：第=%zu 段 原因=%s", index + 1, result.err.c_str());
            return result;
        }
        std::wstring answer = extract_answer(raw, prompt);
        if (answer.empty()) {
            result.err = L"模型没有返回译文";
            logf(L"本地翻译失败：第=%zu 段 输出为空 raw=%s", index + 1,
                 raw.substr(0, 200).c_str());
            return result;
        }
        if (!joined.empty()) joined += L'\n';
        joined += answer;
    }
    result.text = joined;
    logf(L"本地翻译返回：成功 字符数=%zu 段数=%zu", result.text.size(), chunks.size());
    return result;
}

TranslateDownloadState translate_download_state() {
    std::lock_guard<std::mutex> guard(stateMutex);
    return state;
}

void translate_cancel_download() {
    cancel = true;
}

void translate_download_shutdown() {
    cancel = true;
    if (worker.joinable()) worker.join();
}

void translate_download_async(const std::wstring& file) {
    if (translate_download_state().busy) return;
    if (worker.joinable()) worker.join();
    const TranslateModelInfo* info = nullptr;
    for (const auto& item : translate_catalog()) {
        if (item.file == file) { info = &item; break; }
    }
    if (!info) {
        std::lock_guard<std::mutex> guard(stateMutex);
        state = {false, 0, L"自定义模型不支持在线下载"};
        return;
    }
    cancel = false;
    {
        std::lock_guard<std::mutex> guard(stateMutex);
        state = {true, 0, L"准备下载翻译模型…"};
    }
    worker = std::thread([info] {
        LogTrace trace;
        logf(L"翻译模型下载请求到达：%s", info->file.c_str());
        std::atomic<unsigned long long> done{0};
        const unsigned long long total = info->size + llama_runtime_size();
        auto on_bytes = [&](unsigned long long bytes) {
            done += bytes;
            int percent = total ? (int)(done.load() * 100 / total) : 0;
            update(L"正在下载  " + human_size(done.load()) + L" / " + human_size(total), percent);
        };
        bool ready = false;
        std::wstring error;
        try {
            std::filesystem::create_directories(weights_dir());
            if (!install_llama_runtime(cancel, on_bytes, error)) {
                if (!cancel) logf(L"翻译运行时安装失败：%s", error.c_str());
                throw std::runtime_error("runtime");
            }
            const std::wstring dest = weights_dir() + L"\\" + info->file;
            if (disk_file_size(dest) == info->size && verify_sha256(dest, info->sha256)) {
                ready = true;
            } else {
                if (!download_file(info->url, dest, info->size, cancel, on_bytes, error)) {
                    if (!cancel) logf(L"翻译模型下载失败：%s 原因=%s",
                                      info->file.c_str(), error.c_str());
                    throw std::runtime_error("download");
                }
                update(L"正在校验 SHA-256…", 98);
                if (!verify_sha256(dest, info->sha256)) {
                    DeleteFileW(dest.c_str());
                    logf(L"翻译模型校验失败，已删除：%s", info->file.c_str());
                    throw std::runtime_error("checksum");
                }
                ready = true;
            }
        } catch (const std::exception&) {
        }
        if (cancel) {
            update(L"已取消，已下载分片会保留，可继续下载。");
            logf(L"翻译模型下载已取消，保留断点");
        } else if (ready) {
            update(L"翻译模型已就绪。", 100);
            logf(L"翻译模型下载完成：%s", info->file.c_str());
        } else {
            update(L"翻译模型下载或校验失败，请检查网络与磁盘后重试。");
        }
        std::lock_guard<std::mutex> guard(stateMutex);
        state.busy = false;
    });
}
