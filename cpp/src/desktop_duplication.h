#pragma once
#include "common.h"
#include <dxgi1_2.h>

// 将 DXGI 的未旋转 BGRA 表面放回虚拟桌面，兼容竖屏与负坐标副屏。
bool copy_duplication_pixels(const BYTE* src, UINT width, UINT height, UINT pitch,
                             DXGI_MODE_ROTATION rotation, const RECT& output,
                             BYTE* dst, const RECT& desktop);

// 用各屏的 DXGI 帧更新 GDI 基底，单屏失败不会覆盖其它屏的 DXGI 画面。
bool capture_desktop_dxgi(BYTE* bits, const RECT& desktop, bool gdiAvailable);
