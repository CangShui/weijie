#pragma once
#include "capture.h"

// 在选区上方滚动鼠标滚轮，抓取多帧并做简单重叠拼接。
// 成功后 out 由调用方 delete；失败时 err 带原因。
bool longshot_capture(const Shot& shot, const RECT& sel,
                      Gdiplus::Bitmap*& out, std::wstring& err);

#include <vector>
int longshot_overlap(const std::vector<BYTE>& prev,int ph,const std::vector<BYTE>& cur,int ch,int w);
void longshot_preview(Gdiplus::Bitmap* ownedBitmap);

void longshot_close_previews();
