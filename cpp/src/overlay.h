#pragma once
#include "common.h"
#include <imm.h>
#include "annotate.h"
#include "capture.h"
#include "settings.h"
#include "overlay_input.h"
#include <memory>
#include <mutex>
#include <optional>

enum ToolId {
    T_NONE = 0,
    T_RECT, T_ELLIPSE, T_ARROW, T_PEN, T_MOSAIC, T_TEXT, T_NUMBER, T_EMOJI, T_LONG,
    T_OCR, T_TRANS, T_COLOR,
    T_UNDO, T_SAVE, T_SETTINGS, T_PIN, T_CANCEL, T_CONFIRM,
    T_SEP
};

enum PanelMode { PM_HIDDEN = 0, PM_LOADING, PM_TEXT, PM_ERROR };

inline int tool_size_minimum(int tool){
    return tool==T_TEXT?10:tool==T_NUMBER||tool==T_EMOJI?12:tool==T_MOSAIC?4:1;
}
inline int tool_size_maximum(int tool){
    return tool==T_EMOJI?256:tool==T_TEXT||tool==T_NUMBER?96:tool==T_MOSAIC?120:30;
}
inline const wchar_t* tool_size_name(int tool){
    const wchar_t* names[]={L"矩形",L"椭圆",L"箭头",L"画笔",L"马赛克",L"文字",L"序号",L"表情"};
    return tool>=T_RECT&&tool<=T_EMOJI?names[tool-T_RECT]:L"";
}
struct SizeDrag {
    bool active=false;
    int tool=T_NONE,original=0,changes=0;
    POINT origin{};
    RECT bounds{};
    float length=0,initial=0,thumb=0;
    ULONGLONG started=0;
};

struct ToolItem {
    int id = T_NONE;
    const wchar_t* tip = nullptr;
    Gdiplus::RectF rect;
};

struct AnnotationChange {
    int index=0;
    std::optional<Ann> before,after;
};
struct TextDrag {
    int kind=0,handle=-1,index=-1;
    bool editing=false,resumeEditing=false;
    Gdiplus::PointF origin,center;
    float angle=0;
    ULONGLONG lastLog=0;
    Ann before;
};
struct AnnotationDrag {
    bool active=false,moved=false,cycle=false;
    int index=-1;
    POINT origin{};
    float dx=0,dy=0;
    ULONGLONG lastLog=0;
    std::vector<int> hits;
    Ann before;
};

struct Overlay {
    HWND hwnd = nullptr;
    unsigned long long traceId = 0;
    OverlayInput input;
    Shot shot;
    bool alive = true;
    std::atomic<bool> cancelOcr{false};
    std::atomic<bool> ocrRunning{false};
    std::mutex jobMutex;
    int strokeWidth=3, textSize=18;
    int toolSizes[8]={3,3,3,3,24,18,24,40};
    float annotation_width(int selectedTool) const {
        float pixels=(float)toolSizes[selectedTool-T_RECT];
        return selectedTool==T_PEN||selectedTool==T_MOSAIC?pixels:SF(pixels);
    }
    HCURSOR brushCursor=nullptr;
    int brushCursorTool=T_NONE,brushCursorDiameter=0;
    DWORD brushCursorColor=0;
    HWND stampPreview=nullptr;
    Ann stampPreviewKey;
    double stampPreviewScale=0;
    int stampPreviewSide=0;
    size_t stampPreviewFrame=0;
    ULONGLONG animationStart=0,animationLast=0;
    bool stampPreviewVisible=false,stampPreviewTracking=false,stampPressed=false;
    Gdiplus::Color ink=PALETTE[0];
    std::vector<DWORD> recentColors;
    HWND stylePopup=nullptr;
    HWND emojiPopup=nullptr;
    SizeDrag sizeDrag;
    bool sizeRightConsumed=false;
    bool ocrEnabled = false;
    bool translateEnabled = false;
    int emojiIndex = 0;
    std::wstring emojiGroup=L"default";
    std::shared_ptr<Gdiplus::Bitmap> emojiImage;
    std::shared_ptr<StampAnimation> emojiAnimation;

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
    std::vector<AnnotationChange> undoStack,redoStack;
    Ann cur;
    bool hasCur = false;
    int tool = T_NONE;
    int colorIdx = 0;
    int numberSeq = 0;
    int selectedAnnotation=-1;
    AnnotationDrag annotationDrag;
    std::vector<int> annotationCycleHits;
    POINT annotationCyclePoint{};
    bool annotationCycleValid=false;

    Gdiplus::Bitmap* mosaic = nullptr;
    RECT mosaicKey = {0, 0, 0, 0};

    // 文字输入（内联编辑，不用子窗口）
    bool editing = false;
    std::wstring editText;
    std::wstring composing;
    Gdiplus::PointF editPos;
    Ann textDraft;
    int editingIndex=-1,selectedText=-1;
    TextDrag textDrag;
    bool ignoreTextEscape=false;
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
void overlay_apply_display_settings(const Settings& settings);
void overlay_destroy();
inline void overlay_update_brush_cursor(Overlay* overlay){
    if(!overlay||!overlay->hwnd||!overlay->alive)return;
    POINT screen{};
    if(GetCursorPos(&screen)&&(WindowFromPoint(screen)==overlay->hwnd||
                              WindowFromPoint(screen)==overlay->stampPreview))
        SendMessageW(overlay->hwnd,WM_SETCURSOR,(WPARAM)overlay->hwnd,MAKELPARAM(HTCLIENT,WM_MOUSEMOVE));
}

// 供托盘/主程序调用
void overlay_request_capture();

