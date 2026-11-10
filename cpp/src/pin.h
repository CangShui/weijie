#pragma once
#include "common.h"

// 把位图贴到屏幕上（贴图窗口），位置为屏幕坐标
void pin_create(Gdiplus::Bitmap* bmp, int screenX, int screenY);
int  pin_count();
void pin_close_all();
