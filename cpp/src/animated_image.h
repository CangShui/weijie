#pragma once
#include "annotate.h"
#include <cstdint>

struct AnimatedScreenshot {
    std::shared_ptr<Gdiplus::Bitmap> background,mosaic;
    std::vector<Ann> annotations;
    double scale=1;
    unsigned long long traceId=0;
    uint64_t duration() const;
    uint64_t next_change(uint64_t time) const;
    std::unique_ptr<Gdiplus::Bitmap> render(uint64_t time=0,const RECT* region=nullptr) const;
};

bool save_animated_gif(const AnimatedScreenshot& image,const std::wstring& path);
