#pragma once
#include "common.h"
#include <memory>
struct AnimatedScreenshot;

// 把位图放进剪贴板（CF_DIB + PNG 双格式）
bool copy_bitmap_to_clipboard(Gdiplus::Bitmap* bmp);

// 弹出保存对话框并写盘；成功返回 true
bool save_bitmap_dialog(HWND owner, Gdiplus::Bitmap* bmp, const std::wstring& preferDir,
                        const std::wstring& format, int quality, std::wstring* savedPath=nullptr,
                        std::shared_ptr<const AnimatedScreenshot> animation=nullptr,bool* cancelled=nullptr);

// 直接保存到设置中的目录（自动生成不冲突的文件名，不弹保存对话框）
bool save_bitmap_direct(Gdiplus::Bitmap* bmp, const std::wstring& preferDir,
                        const std::wstring& format, int quality, std::wstring& savedPath,HWND owner=nullptr,
                        std::shared_ptr<const AnimatedScreenshot> animation=nullptr,bool* cancelled=nullptr);

// 默认保存目录：图片\微截
std::wstring default_save_dir();

bool save_bitmap_file(Gdiplus::Bitmap* bmp,const std::wstring& path,int quality);
