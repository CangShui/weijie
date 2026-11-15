#pragma once
#include "common.h"
#include <functional>

struct GgufFile {
    std::wstring name;
    unsigned long long size = 0;
};

// models\weights 下的全部 GGUF，按文件名排序。
std::vector<GgufFile> list_gguf_models();
std::wstring human_size(unsigned long long bytes);

// 主程序目录 models\runtime\llama：OCR 与翻译共用的 llama.cpp 运行时。
std::wstring llama_runtime_dir();
bool llama_runtime_ready();
bool install_llama_runtime(std::atomic<bool>& cancel,
                           const std::function<void(unsigned long long)>& on_bytes,
                           std::wstring& err);
unsigned long long llama_runtime_size();

// 把旧版 %LOCALAPPDATA% 模型与运行时迁移到程序目录，幂等，不覆盖已有文件。
void migrate_legacy_models();
