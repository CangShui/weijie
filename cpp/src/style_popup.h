#pragma once
#include "common.h"
struct Overlay;
void show_color_picker(Overlay* overlay);
void close_style_popup(Overlay* overlay);
Gdiplus::Color hsv_color(double hue,double saturation,double value,BYTE alpha=255);
void color_hsv(Gdiplus::Color color,double& hue,double& saturation,double& value);
std::vector<DWORD> load_recent_colors(const std::wstring& encoded);
std::wstring encode_recent_colors(const std::vector<DWORD>& colors);
