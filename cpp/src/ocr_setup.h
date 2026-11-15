#pragma once
#include "common.h"
struct OcrDownloadState { bool busy = false; int percent = 0; std::wstring text; };
std::wstring hunyuan_ocr_dir();          // 权重目录：<exe>\models\weights
std::wstring hunyuan_runtime_dir(bool gpu=false);
bool hunyuan_model_ready();
bool hunyuan_fast_model_ready();
bool hunyuan_runtime_ready();
std::wstring hunyuan_model_status_text();
std::wstring hunyuan_fast_model_status_text();
unsigned long long hunyuan_download_size(bool fast);
void hunyuan_download_models_async(bool fast=false);
OcrDownloadState hunyuan_download_state();
void hunyuan_cancel_download();
void hunyuan_download_shutdown();
