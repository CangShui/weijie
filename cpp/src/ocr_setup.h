#pragma once
#include "common.h"
struct OcrDownloadState { bool busy = false; int percent = 0; std::wstring text; };
std::wstring hunyuan_ocr_dir();
std::wstring hunyuan_runtime_dir(bool gpu=false);
bool hunyuan_model_ready();
bool hunyuan_fast_model_ready();
bool hunyuan_runtime_ready();
std::wstring hunyuan_model_status_text();
void hunyuan_download_models_async();
OcrDownloadState hunyuan_download_state();
void hunyuan_cancel_download();
void hunyuan_download_shutdown();
