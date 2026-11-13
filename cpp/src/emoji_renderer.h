#pragma once
#include "common.h"
#include <memory>
#include <cstdint>

struct StampAnimation {
    std::vector<std::shared_ptr<Gdiplus::Bitmap>> frames;
    std::vector<UINT> delays;
    uint64_t duration=0;
    size_t frame_index(uint64_t time) const;
    uint64_t next_change(uint64_t time) const;
};

struct StampGroup {
    std::wstring key,name;
    std::vector<int> images;
};

std::wstring emoji_directory();
int emoji_reload();
int emoji_count();
const std::vector<StampGroup>& emoji_groups();
const wchar_t* emoji_text(int index);
std::shared_ptr<Gdiplus::Bitmap> emoji_image(int index);
std::shared_ptr<StampAnimation> emoji_animation(int index);
Gdiplus::Bitmap* emoji_bitmap(int index);
Gdiplus::RectF stamp_image_rect(Gdiplus::Bitmap* image,const Gdiplus::PointF& center,float size);
void draw_stamp_image(Gdiplus::Graphics& g,Gdiplus::Bitmap* image,const Gdiplus::PointF& center,float size);
void draw_emoji(Gdiplus::Graphics& g, int index, const Gdiplus::PointF& center, float size);
