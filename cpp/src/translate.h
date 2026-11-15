#pragma once
#include "common.h"
#include "settings.h"

struct TranslateResult {
    std::wstring text;
    std::wstring src;
    std::wstring dst;
    std::wstring err;
    bool ok() const { return err.empty(); }
};

struct TranslateModelInfo {
    std::wstring file;
    unsigned long long size = 0;
    std::wstring url;
    std::wstring sha256;
};

// 内置翻译模型清单：文件名、权重字节数、下载地址与 SHA-256。
const std::vector<TranslateModelInfo>& translate_catalog();

struct TranslateDownloadState {
    bool busy = false;
    int percent = 0;
    std::wstring text;
};

TranslateDownloadState translate_download_state();
void translate_download_async(const std::wstring& file);
void translate_cancel_download();
void translate_download_shutdown();

// 完全本地推理：读取 models\weights 下的 GGUF，不发起任何联网请求。
TranslateResult translate_text(const std::wstring& text, const Settings& cfg);
