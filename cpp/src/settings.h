#pragma once
#include "common.h"

inline constexpr int OCR_IDLE_TIMEOUT_MINUTES[] = {0, 1, 10, 60, 720, 1440};

struct Settings {
    std::wstring hotkey    = L"Ctrl+Shift+A";
    bool hotkey_enabled = true;
    std::wstring save_dir;
    std::wstring image_format = L"png";   // png / jpg / bmp
    int jpeg_quality = 92;                // 1..100
    bool auto_copy = true;
    bool include_cursor = false;
    int stroke_width = 3;
    int text_size = 18;
    std::wstring recent_colors;
    std::wstring ocr_backend = L"auto";
    std::wstring ocr_engine = L"ppocr";
    std::wstring ocr_benchmark;
    bool ocr_enabled = false;
    int ocr_idle_minutes = 0;             // 0 = off
    bool translate_enabled = true;
    std::wstring ocr_model_dir;           // empty = %LOCALAPPDATA%\weijie\hunyuan-ocr
    std::wstring llm_base  = L"https://api.deepseek.com/v1";
    std::wstring llm_key;
    std::wstring llm_model = L"deepseek-chat";
};

Settings load_settings();
bool save_settings(const Settings& s);
std::wstring settings_path();

struct HotkeyPreset {
    const wchar_t* name;
    UINT mods;
    UINT vk;
};
extern const HotkeyPreset HOTKEY_PRESETS[9];
extern const int N_HOTKEY_PRESETS;

bool resolve_hotkey(const std::wstring& name, UINT& mods, UINT& vk);

// GPU OCR 目前只支持纯 Intel 核显环境；出现 AMD / NVIDIA 独显时走 CPU。
bool gpu_ocr_supported();
