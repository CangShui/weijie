#pragma once
#include "common.h"

// Local HunyuanOCR via the bundled llama.cpp runtime.
bool ocr_recognize(Gdiplus::Bitmap* bmp, std::wstring& text, std::wstring& err, const std::atomic<bool>* cancel = nullptr);
