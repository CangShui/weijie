#pragma once
#include "common.h"

struct PpOcrDownloadState { bool busy = false; int percent = 0; std::wstring text; };

bool ppocr_component_ready();
std::wstring ppocr_component_status_text();
std::wstring ppocr_component_dir();
PpOcrDownloadState ppocr_download_state();
void ppocr_download_async();
void ppocr_cancel_download();
void ppocr_download_shutdown();
