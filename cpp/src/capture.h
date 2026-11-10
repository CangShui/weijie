#pragma once
#include "common.h"

// 冻结帧：整块虚拟桌面（物理像素）
struct Shot {
    int vx = 0, vy = 0, vw = 0, vh = 0;
    HBITMAP hbm = nullptr;      // 原始画面
    HBITMAP hbmDim = nullptr;   // 压暗后的画面（选区外遮罩）
    void* bits = nullptr;
    Gdiplus::Bitmap* bmp = nullptr;
    Gdiplus::Bitmap* bmpDim = nullptr;
};

bool capture_desktop(Shot& s);
void free_shot(Shot& s);

// 屏幕坐标 -> 该点所在的顶层窗口矩形（屏幕坐标）
bool window_rect_at(int sx, int sy, RECT& out);
