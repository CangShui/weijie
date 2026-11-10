// 公共定义：Win32 / GDI+ / 字符串 / 日志 / 颜色
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commdlg.h>
#include <objidl.h>
#include <gdiplus.h>

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <algorithm>

// ---------------------------------------------------------------- DPI
extern double g_scale;              // 系统 DPI / 96
inline int   S(int v)   { return (int)lround(v * g_scale); }
inline float SF(float v) { return (float)(v * g_scale); }
inline int   SPt(int v) { return (int)lround(v * g_scale); }

// ---------------------------------------------------------------- 字符串
std::wstring u8_to_w(const std::string& s);
std::string  w_to_u8(const std::wstring& s);
std::string  url_encode(const std::string& s);
std::string  json_escape(const std::string& s);

// ---------------------------------------------------------------- 路径 / 日志
std::wstring appdata_dir();       // %APPDATA%\weijie
std::wstring localappdata_dir();  // %LOCALAPPDATA%\weijie
std::wstring exe_path();
void logf(const wchar_t* fmt, ...);

// ---------------------------------------------------------------- 颜色
inline Gdiplus::Color ARGB(BYTE a, BYTE r, BYTE g, BYTE b) {
    return Gdiplus::Color(a, r, g, b);
}
inline Gdiplus::Color RGBc(BYTE r, BYTE g, BYTE b) {
    return Gdiplus::Color(255, r, g, b);
}
inline Gdiplus::Color withAlpha(Gdiplus::Color c, BYTE a) {
    return Gdiplus::Color(a, c.GetR(), c.GetG(), c.GetB());
}

// 微信截图标注色板
extern const Gdiplus::Color PALETTE[6];
#define N_PALETTE 6

// 覆盖层工具条请求主程序打开设置窗口
#define WM_JJ_OPEN_SETTINGS (WM_APP + 24)

// 主题色
#define C_GREEN_R 7
#define C_GREEN_G 193
#define C_GREEN_B 96

// ---------------------------------------------------------------- 矩形工具
inline Gdiplus::RectF normRect(float x0, float y0, float x1, float y1) {
    float l = x0 < x1 ? x0 : x1;
    float t = y0 < y1 ? y0 : y1;
    float w = fabsf(x1 - x0);
    float h = fabsf(y1 - y0);
    return Gdiplus::RectF(l, t, w, h);
}
inline bool ptInRectF(const Gdiplus::RectF& r, float x, float y) {
    return x >= r.X && x <= r.X + r.Width && y >= r.Y && y <= r.Y + r.Height;
}
inline bool ptInRect(const RECT& r, int x, int y) {
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}
inline RECT normRECT(int x0, int y0, int x1, int y1) {
    RECT r;
    r.left = x0 < x1 ? x0 : x1;
    r.top = y0 < y1 ? y0 : y1;
    r.right = x0 < x1 ? x1 : x0;
    r.bottom = y0 < y1 ? y1 : y0;
    return r;
}

// ---------------------------------------------------------------- 字体
const wchar_t* FONT_UI();      // Microsoft YaHei UI
const wchar_t* FONT_MONO();    // Consolas
const wchar_t* FONT_NUM();     // Arial

// ---------------------------------------------------------------- 主程序入口
// 由覆盖层工具条的「设置」按钮调用：异步打开设置，避免在覆盖层
// 自己的消息处理函数里嵌套一个模态窗口。
void app_open_settings();
