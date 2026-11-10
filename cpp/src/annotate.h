#pragma once
#include "common.h"

enum AnnKind { AK_RECT, AK_ELLIPSE, AK_ARROW, AK_PEN, AK_MOSAIC, AK_TEXT, AK_NUMBER, AK_EMOJI };

// 所有几何坐标都是「选区局部坐标」（物理像素）
struct Ann {
    AnnKind kind = AK_RECT;
    Gdiplus::Color color = PALETTE[0];
    float width = 3.0f;
    float fontSize = 18.0f;
    float diameter = 24.0f;

    Gdiplus::RectF rect;                    // 矩形 / 椭圆
    Gdiplus::PointF a, b;                   // 箭头
    std::vector<Gdiplus::PointF> pts;       // 画笔 / 马赛克
    std::wstring text;                      // 文字
    Gdiplus::PointF pos;                    // 文字 / 序号
    int index = 0;                          // 序号
    int emojiIndex = -1;                    // 表情目录索引

    void draw(Gdiplus::Graphics& g, Gdiplus::Bitmap* mosaic) const;
    Gdiplus::RectF bounds() const;
    bool isStroke() const { return kind == AK_PEN || kind == AK_MOSAIC; }
};

// 生成像素化底图（输入为选区裁剪图）
Gdiplus::Bitmap* make_mosaic(Gdiplus::Bitmap* crop);
