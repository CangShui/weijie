#pragma once
#include "common.h"
bool fast_ocr_available();
bool fast_ocr_gpu_available();
std::wstring fast_ocr_status_text();
bool fast_ocr_initializing();
bool fast_ocr_recognize(Gdiplus::Bitmap*,std::wstring&,std::wstring&,const std::atomic<bool>* cancel=nullptr);
void fast_ocr_shutdown();
