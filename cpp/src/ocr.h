#pragma once
#include "common.h"

// Local HunyuanOCR via the bundled llama.cpp runtime.
bool ocr_recognize(Gdiplus::Bitmap* bmp, std::wstring& text, std::wstring& err, const std::atomic<bool>* cancel = nullptr);

// 主开关关闭时终止本程序持有的模型进程；重新开启只恢复按需加载。
void ocr_set_enabled(bool enabled);
bool ocr_is_enabled();
bool ocr_track_process(HANDLE job, HANDLE process);
void ocr_forget_process(HANDLE job);

// 覆盖排队、模型加载和推理；最后一个调用结束后重新计算空闲时间。
class OcrActivity {
public:
    OcrActivity();
    ~OcrActivity();
    OcrActivity(const OcrActivity&) = delete;
    OcrActivity& operator=(const OcrActivity&) = delete;
};
void ocr_set_idle_timeout(int minutes);
// now 使用 GetTickCount64 的单调时间；回收不改变识别主开关。
bool ocr_reclaim_idle(ULONGLONG now);
// 由持有快引擎锁的回收路径调用，避免与模型启动交错。
bool ocr_idle_expired(ULONGLONG now);
