#pragma once
#include "common.h"
#include <imm.h>
#include "annotate.h"
#include "capture.h"
#include "settings.h"
#include <memory>
#include <mutex>

enum ToolId {
    T_NONE = 0,
    T_RECT, T_ELLIPSE, T_ARROW, T_PEN, T_MOSAIC, T_TEXT, T_NUMBER, T_EMOJI, T_LONG,
    T_OCR, T_TRANS, T_COLOR,
    T_UNDO, T_SAVE, T_SETTINGS, T_PIN, T_CANCEL, T_CONFIRM,
    T_SEP
};

enum PanelMode { PM_HIDDEN = 0, PM_LOADING, PM_TEXT, PM_ERROR };

struct ToolItem {
    int id = T_NONE;
    const wchar_t* tip = nullptr;
    Gdiplus::RectF rect;
};

struct Overlay {
    HWND hwnd = nullptr;
    Shot shot;
    bool alive = true;
    std::atomic<bool> cancelOcr{false};
    std::atomic<bool> ocrRunning{false};
    std::mutex jobMutex;
    int strokeWidth=3, textSize=18;
    int toolSizes[8]={3,3,3,3,24,18,24,40};
    Gdiplus::Color ink=PALETTE[0];
    std::vector<DWORD> recentColors;
    HWND stylePopup=nullptr;
    HWND emojiPopup=nullptr;
    bool ocrEnabled = false;
    bool translateEnabled = false;
    int emojiIndex = 0;

    // 选区
    bool hasSel = false;
    RECT sel = {0, 0, 0, 0};
    int mode = 0;               // 0 空闲 1 拖选 2 缩放 3 移动 4 绘制
    POINT dragFrom = {0, 0};
    RECT selOrigin = {0, 0, 0, 0};
    int edge = 0;               // 1 左 2 右 4 上 8 下
    RECT hoverWin = {0, 0, 0, 0};
    bool hasHover = false;
    bool windowSnapPending = false;
    RECT windowSnapRect = {0, 0, 0, 0};

    // 标注
    std::vector<Ann> anns;
    std::vector<Ann> redos;
    Ann cur;
    bool hasCur = false;
    int tool = T_NONE;
    int colorIdx = 0;
    int numberSeq = 0;

    Gdiplus::Bitmap* mosaic = nullptr;
    RECT mosaicKey = {0, 0, 0, 0};

    // 文字输入（内联编辑，不用子窗口）
    bool editing = false;
    std::wstring editText;
    std::wstring composing;
    Gdiplus::PointF editPos;
    HIMC himc = nullptr;

    // 工具条
    std::vector<ToolItem> items;
    int hoverItem = -1;
    float tbX = 0, tbY = 0;
    bool tbVisible = false;

    // 结果面板
    HWND resultEdit=nullptr;
    HFONT resultFont=nullptr;
    std::wstring resultDisplayed;
    int panel = PM_HIDDEN;
    std::wstring panelTitle, panelBody, panelHint, panelOriginal;
    bool showOriginal = false;
    Gdiplus::RectF panelRect;
    float panelScroll = 0;
    int panelBtnHover = -1;
    std::vector<std::wstring> panelLines;
    float panelLinesWidth = 0;

    ~Overlay();
};

// 抓屏并弹出覆盖层；已在截图中返回 false
bool overlay_open();
bool overlay_is_open();
void overlay_destroy();

// 供托盘/主程序调用
void overlay_request_capture();

