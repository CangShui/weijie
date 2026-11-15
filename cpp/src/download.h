#pragma once
#include "common.h"
#include <functional>

// 分片多线程下载：每片独立 .part 文件，进程重启后按已有字节续传。
// on_bytes 在任意线程回调，调用方负责线程安全。
bool download_file(const std::wstring& url, const std::wstring& dest,
                   unsigned long long expected_size, std::atomic<bool>& cancel,
                   const std::function<void(unsigned long long)>& on_bytes,
                   std::wstring& err);

bool sha256_file(const std::wstring& path, std::wstring& hex_lower);
bool verify_sha256(const std::wstring& path, const std::wstring& expected_lower);
unsigned long long disk_file_size(const std::wstring& path);
