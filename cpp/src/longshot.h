#pragma once
#include "capture.h"

// 自动滚动到页底或按 Esc 停止；资源/编码边界停止时保留可保存的长图。
// 成功后 out 由调用方 delete，err 带结束说明；失败时 err 带原因。
bool longshot_capture(const Shot& shot, const RECT& sel,
                      Gdiplus::Bitmap*& out, std::wstring& err);

#include <vector>
#include <memory>
struct AnimatedScreenshot;
int longshot_overlap(const std::vector<BYTE>& prev,int ph,const std::vector<BYTE>& cur,int ch,int w);
inline constexpr size_t LONGSHOT_MAX_PIXEL_BYTES=200ull*1024*1024;
inline constexpr int LONGSHOT_MAX_ENCODED_HEIGHT=65535;
enum class LongshotAppend { Added, Unchanged, Unmatched, MemoryLimit, EncoderLimit };
struct LongshotStitcher {
    int width,height;
    std::vector<BYTE> pixels;
    size_t maxBytes;
    LongshotStitcher(int w,int h,std::vector<BYTE> first,size_t budget=LONGSHOT_MAX_PIXEL_BYTES)
        :width(w),height(h),pixels(std::move(first)),maxBytes(budget){}
    LongshotAppend append(const std::vector<BYTE>& frame,int frameHeight);
};
void longshot_preview(Gdiplus::Bitmap* ownedBitmap,const std::wstring& finishReason=L"",
                      std::shared_ptr<const AnimatedScreenshot> animation=nullptr);

void longshot_close_previews();
