#pragma once
#include "common.h"

int emoji_count();
const wchar_t* emoji_text(int index);
Gdiplus::Bitmap* emoji_bitmap(int index);
void draw_emoji(Gdiplus::Graphics& g, int index, const Gdiplus::PointF& center, float size);
