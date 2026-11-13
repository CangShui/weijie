#pragma once
#include "common.h"
#include <memory>
struct StampAnimation;

enum AnnKind { AK_RECT, AK_ELLIPSE, AK_ARROW, AK_PEN, AK_MOSAIC, AK_TEXT, AK_NUMBER, AK_EMOJI };

inline Gdiplus::RectF brush_footprint(Gdiplus::PointF center,float diameter){
    return {center.X-diameter/2,center.Y-diameter/2,diameter,diameter};
}

// 所有几何坐标都是「选区局部坐标」（物理像素）
struct Ann {
    AnnKind kind = AK_RECT;
    Gdiplus::Color color = PALETTE[0];
    float width = 3.0f;
    float fontSize = 18.0f;
    float diameter = 24.0f;
    float textScaleX=1,textScaleY=1,textAngle=0;

    Gdiplus::RectF rect;                    // 矩形 / 椭圆
    Gdiplus::PointF a, b;                   // 箭头
    std::vector<Gdiplus::PointF> pts;       // 画笔 / 马赛克
    std::wstring text;                      // 文字
    Gdiplus::PointF pos;                    // 文字 / 序号
    int index = 0;                          // 序号
    int emojiIndex = -1;                    // 表情目录索引
    std::shared_ptr<Gdiplus::Bitmap> emojiImage;
    std::shared_ptr<StampAnimation> emojiAnimation;

    void draw(Gdiplus::Graphics& g, Gdiplus::Bitmap* mosaic,unsigned long long time=0) const;
    Gdiplus::RectF bounds() const;
    bool hit_test(Gdiplus::PointF point,float tolerance) const;
    void translate(float dx,float dy);
    bool isStroke() const { return kind == AK_PEN || kind == AK_MOSAIC; }
    Gdiplus::SizeF text_natural_size() const;
    Gdiplus::SizeF text_frame_size() const;
    Gdiplus::PointF text_point(Gdiplus::PointF local) const;
    Gdiplus::PointF text_local(Gdiplus::PointF world) const;
    bool text_contains(Gdiplus::PointF world) const;
    void apply_text_transform(Gdiplus::Graphics& graphics) const;
private:
    mutable std::wstring measuredText;
    mutable float measuredFont=-1;
    mutable double measuredScale=-1;
    mutable Gdiplus::SizeF measuredSize;
};

// 生成像素化底图（输入为选区裁剪图）
Gdiplus::Bitmap* make_mosaic(Gdiplus::Bitmap* crop);
