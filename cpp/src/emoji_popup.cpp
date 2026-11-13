#include "emoji_popup.h"

#include "emoji_renderer.h"
#include "overlay.h"

#include <windowsx.h>

using namespace Gdiplus;

namespace {

const wchar_t* EMOJI_CLASS = L"weijieEmojiPopup";
constexpr int kColumns = 11;
constexpr int kCell = 34;
constexpr int kPad = 12;
constexpr int kTitleHeight = 30;
constexpr int kFooterHeight = 24;

struct EmojiRow {
    int group=0;
    bool header=false;
    std::vector<int> images;
};
struct EmojiPopup {
    Overlay* overlay = nullptr;
    HWND hwnd = nullptr;
    int hover = -1;
    int count = 0;
    int rows = 0;
    int visibleRows = 1,scrollRow = 0,wheelDelta = 0;
    ULONGLONG animationStart=GetTickCount64();
    size_t animationFrame=0;
    std::vector<EmojiRow> display;
    HANDLE change=INVALID_HANDLE_VALUE;
    ULONGLONG reloadAt=0,watchAt=0;
};

void build_rows(EmojiPopup* popup){
    popup->display.clear();
    const auto& groups=emoji_groups();
    for(size_t i=0;i<groups.size();++i){
        popup->display.push_back({(int)i,true,{}});
        const auto& images=groups[i].images;
        for(size_t start=0;start<images.size();start+=kColumns)
            popup->display.push_back({(int)i,false,std::vector<int>(images.begin()+start,
                images.begin()+std::min(images.size(),start+kColumns))});
    }
    popup->rows=(int)popup->display.size();
    popup->visibleRows=std::min(std::max(popup->rows,2),std::clamp(
        (popup->overlay->shot.vh-S(kTitleHeight+kPad+kFooterHeight+8))/S(kCell),1,9));
    popup->scrollRow=std::clamp(popup->scrollRow,0,std::max(0,popup->rows-popup->visibleRows));
}

int popup_width(const EmojiPopup* popup) {
    return S(kPad * 2 + kColumns * kCell)+
        (popup->rows>popup->visibleRows?GetSystemMetrics(SM_CXVSCROLL):0);
}

int popup_height(const EmojiPopup* popup) {
    return S(kTitleHeight + popup->visibleRows * kCell + kPad + kFooterHeight);
}

void draw_round_rect(Graphics& g, const RectF& r, float radius) {
    GraphicsPath path;
    path.AddArc(r.X, r.Y, radius * 2, radius * 2, 180, 90);
    path.AddArc(r.X + r.Width - radius * 2, r.Y, radius * 2, radius * 2, 270, 90);
    path.AddArc(r.X + r.Width - radius * 2, r.Y + r.Height - radius * 2,
                radius * 2, radius * 2, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - radius * 2, radius * 2, radius * 2, 90, 90);
    path.CloseFigure();
    SolidBrush fill(Color(255, 250, 250, 250));
    Pen border(Color(255, 226, 226, 226), 1.0f);
    g.FillPath(&fill, &path);
    g.DrawPath(&border, &path);
}

void paint(EmojiPopup* popup, HDC dc) {
    RECT client{};GetClientRect(popup->hwnd,&client);
    const int width = client.right;
    const int height = popup_height(popup);
    Bitmap back(width, height, PixelFormat32bppPARGB);
    back.SetResolution((REAL)(96 * g_scale), (REAL)(96 * g_scale));
    Graphics g(&back);
    g.SetPageUnit(UnitPixel);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.Clear(Color(0, 0, 0, 0));
    draw_round_rect(g, RectF(0.5f, 0.5f, (REAL)width - 1, (REAL)height - 1), SF(7.0f));

    Font title(FONT_UI(), SF(13.0f), FontStyleBold, UnitPixel);
    SolidBrush ink(Color(255, 35, 43, 39));
    StringFormat nameFormat;nameFormat.SetTrimming(StringTrimmingEllipsisCharacter);
    const wchar_t* name=popup->hover>=0?emoji_text(popup->hover):L"表情 · images 图片";
    g.DrawString(name?name:L"表情 · images 图片",-1,&title,
        RectF(SF(kPad),SF(6),(float)width-SF(2*kPad),SF(24)),&nameFormat,&ink);

    const float cell = SF((float)kCell);
    const float imageSize = SF(26.0f);
    int end=std::min(popup->rows,popup->scrollRow+popup->visibleRows);
    for(int line=popup->scrollRow;line<end;++line){
        const auto& display=popup->display[line];
        int row=line-popup->scrollRow;
        if(display.header){
            const auto& group=emoji_groups()[display.group];
            SolidBrush fill(Color(255,235,243,238)),label(Color(255,35,87,58));
            RectF area(SF(kPad),SF(kTitleHeight)+row*cell,(float)width-SF(2*kPad),cell);
            g.FillRectangle(&fill,area);
            Font font(FONT_UI(),SF(12),FontStyleBold,UnitPixel);
            std::wstring text=group.name+L"  ("+std::to_wstring(group.images.size())+L")";
            StringFormat format;format.SetLineAlignment(StringAlignmentCenter);
            format.SetTrimming(StringTrimmingEllipsisCharacter);
            area.X+=SF(8);area.Width-=SF(16);g.DrawString(text.c_str(),-1,&font,area,&format,&label);
            continue;
        }
        for(size_t col=0;col<display.images.size();++col){
        int i=display.images[col];
        const RectF cellRect(SF((float)kPad) + col * cell,
                             SF((float)kTitleHeight) + row * cell,
                             cell, cell);
        if (i == popup->hover) {
            SolidBrush hover(Color(255, 238, 243, 239));
            draw_round_rect(g, cellRect, SF(6.0f));
            g.FillRectangle(&hover, RectF(cellRect.X + SF(2), cellRect.Y + SF(2),
                                          cellRect.Width - SF(4), cellRect.Height - SF(4)));
        }
        auto animation=i==popup->hover?emoji_animation(i):nullptr;
        if(animation)popup->animationFrame=animation->frame_index(GetTickCount64()-popup->animationStart);
        auto* image=animation?animation->frames[popup->animationFrame].get():emoji_bitmap(i);
        draw_stamp_image(g,image,
            PointF(cellRect.X+cell/2,cellRect.Y+cell/2),imageSize);
        }
    }
    Font body(FONT_UI(),SF(11),FontStyleRegular,UnitPixel);
    if(popup->display.empty()){
        g.DrawString(L"images 中没有可用图片\n添加图片或子文件夹后自动刷新",-1,&body,
            RectF(SF(kPad),SF(kTitleHeight+3),(float)width-SF(2*kPad),SF(52)),nullptr,&ink);
    }else{
        std::wstring status=L"共 "+std::to_wstring(popup->count)+L" 张";
        status+=L"  ·  "+std::to_wstring(emoji_groups().size())+L" 个分组";
        if(popup->rows>popup->visibleRows)
            status+=L"  ·  滚轮翻页";
        g.DrawString(status.c_str(),-1,&body,
            PointF(SF(kPad),SF((float)(kTitleHeight+popup->visibleRows*kCell+4))),nullptr,&ink);
    }

    Graphics out(dc);
    out.SetPageUnit(UnitPixel);
    out.DrawImage(&back, 0, 0, width, height);
}

int emoji_at(EmojiPopup* popup, POINT pt) {
    const float cell = SF((float)kCell);
    const float x = (float)pt.x - SF((float)kPad);
    const float y = (float)pt.y - SF((float)kTitleHeight);
    if (x < 0 || y < 0) return -1;
    const int col = (int)(x / cell);
    const int row = (int)(y / cell);
    if (col < 0 || col >= kColumns || row < 0||row>=popup->visibleRows) return -1;
    int line=row+popup->scrollRow;
    if(line>=(int)popup->display.size())return -1;
    const auto& display=popup->display[line];
    return !display.header&&col<(int)display.images.size()?display.images[col]:-1;
}

void update_scrollbar(EmojiPopup* popup){
    bool scroll=popup->rows>popup->visibleRows;
    ShowScrollBar(popup->hwnd,SB_VERT,scroll);
    if(scroll){
        SCROLLINFO info{sizeof(info),SIF_RANGE|SIF_PAGE|SIF_POS,0,popup->rows-1,
            (UINT)popup->visibleRows,popup->scrollRow};
        SetScrollInfo(popup->hwnd,SB_VERT,&info,TRUE);
    }
}

void refresh_images(EmojiPopup* popup){
    popup->count=emoji_reload();popup->hover=-1;build_rows(popup);
    int width=popup_width(popup),height=popup_height(popup);
    RECT bounds{};GetWindowRect(popup->hwnd,&bounds);
    int x=std::clamp((int)bounds.left,popup->overlay->shot.vx,
        popup->overlay->shot.vx+std::max(0,popup->overlay->shot.vw-width));
    int y=std::clamp((int)bounds.top,popup->overlay->shot.vy,
        popup->overlay->shot.vy+std::max(0,popup->overlay->shot.vh-height));
    SetWindowPos(popup->hwnd,nullptr,x,y,width,height,SWP_NOACTIVATE|SWP_NOZORDER);
    update_scrollbar(popup);InvalidateRect(popup->hwnd,nullptr,FALSE);
    logf(L"图片窗口已刷新：图片=%d 分组=%zu 滚动行=%d，删除的文件与目录不再展示",
        popup->count,emoji_groups().size(),popup->scrollRow);
}

void watch_images(EmojiPopup* popup){
    ULONGLONG now=GetTickCount64();
    if(popup->change==INVALID_HANDLE_VALUE&&now>=popup->watchAt){
        bool retry=popup->watchAt!=0;
        popup->watchAt=now+1000;
        popup->change=FindFirstChangeNotificationW(emoji_directory().c_str(),TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME|FILE_NOTIFY_CHANGE_DIR_NAME|FILE_NOTIFY_CHANGE_SIZE|FILE_NOTIFY_CHANGE_LAST_WRITE);
        if(popup->change!=INVALID_HANDLE_VALUE){
            if(retry)popup->reloadAt=now+200;
            logf(L"图片目录变化监听已开启：包含所有分组子目录");
        }
    }
    if(popup->change!=INVALID_HANDLE_VALUE&&WaitForSingleObject(popup->change,0)==WAIT_OBJECT_0){
        popup->reloadAt=now+200;
        if(!FindNextChangeNotification(popup->change)){
            FindCloseChangeNotification(popup->change);popup->change=INVALID_HANDLE_VALUE;
            logf(L"图片目录监听已释放：目录不可用，等待重新创建");
        }
    }
    if(popup->reloadAt&&now>=popup->reloadAt){
        popup->reloadAt=0;refresh_images(popup);
    }
}

void close_popup(EmojiPopup* popup) {
    if (!popup) return;
    if (popup->overlay && popup->overlay->emojiPopup == popup->hwnd) {
        popup->overlay->emojiPopup = nullptr;
    }
    DestroyWindow(popup->hwnd);
}

void scroll_to(EmojiPopup* popup,int row){
    row=std::clamp(row,0,std::max(0,popup->rows-popup->visibleRows));
    if(row==popup->scrollRow)return;
    LogTrace trace(popup->overlay->traceId);
    logf(L"图片选择翻页：原行=%d 新行=%d 总图片=%d 可见行=%d",
        popup->scrollRow,row,popup->count,popup->visibleRows);
    popup->scrollRow=row;popup->hover=-1;
    SetScrollPos(popup->hwnd,SB_VERT,row,TRUE);
    InvalidateRect(popup->hwnd,nullptr,FALSE);
}

LRESULT CALLBACK popup_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* popup = (EmojiPopup*)GetWindowLongPtrW(h, GWLP_USERDATA);
    LogTrace trace(popup?popup->overlay->traceId:0);
    switch (msg) {
    case WM_CREATE:
        popup = (EmojiPopup*)((CREATESTRUCTW*)lp)->lpCreateParams;
        popup->hwnd = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)popup);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PRINTCLIENT:
        if(popup)paint(popup,(HDC)wp);
        return 0;
    case WM_TIMER:
        if(popup&&wp==1)watch_images(popup);
        if(popup&&wp==1&&popup->hover>=0){
            auto animation=emoji_animation(popup->hover);
            if(animation&&animation->frame_index(GetTickCount64()-popup->animationStart)!=popup->animationFrame)
                InvalidateRect(h,nullptr,FALSE);
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(h, &ps);
        if (popup) paint(popup, dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (popup) {
            TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,h,0};TrackMouseEvent(&track);
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int hover = emoji_at(popup, pt);
            if (hover != popup->hover) {
                popup->hover = hover;
                InvalidateRect(h, nullptr, FALSE);
            }
        }
        return 0;
    case WM_MOUSEWHEEL:
        if(popup){
            popup->wheelDelta+=GET_WHEEL_DELTA_WPARAM(wp);
            int steps=popup->wheelDelta/WHEEL_DELTA;popup->wheelDelta%=WHEEL_DELTA;
            scroll_to(popup,popup->scrollRow-steps*3);
        }
        return 0;
    case WM_VSCROLL:
        if(popup){
            int row=popup->scrollRow;
            switch(LOWORD(wp)){
            case SB_LINEUP:--row;break;
            case SB_LINEDOWN:++row;break;
            case SB_PAGEUP:row-=popup->visibleRows;break;
            case SB_PAGEDOWN:row+=popup->visibleRows;break;
            case SB_TOP:row=0;break;
            case SB_BOTTOM:row=popup->rows;break;
            case SB_THUMBTRACK:case SB_THUMBPOSITION:{
                SCROLLINFO info{sizeof(info),SIF_TRACKPOS};GetScrollInfo(h,SB_VERT,&info);
                row=info.nTrackPos;break;
            }
            }
            scroll_to(popup,row);
        }
        return 0;
    case WM_MOUSELEAVE:
        if (popup) {
            popup->hover = -1;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (popup) {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int index = emoji_at(popup, pt);
            Overlay* overlay=popup->overlay;
            LogTrace trace(overlay->traceId);
            logf(L"表情选择请求：抬起位置=(%ld,%ld) 命中索引=%d",pt.x,pt.y,index);
            if (index >= 0) {
                overlay->emojiIndex = index;
                overlay->emojiImage = emoji_image(index);
                overlay->emojiAnimation = emoji_animation(index);
                for(const auto& group:emoji_groups())if(std::find(group.images.begin(),group.images.end(),index)!=group.images.end()){
                    overlay->emojiGroup=group.key;
                    logf(L"图片分组选择：分组=%s 文件=%s",group.name.c_str(),emoji_text(index));break;
                }
                overlay->tool = T_EMOJI;
                overlay->stampPressed=false;
                close_popup(popup);
                SetForegroundWindow(overlay->hwnd);
                SetFocus(overlay->hwnd);
                overlay_update_brush_cursor(overlay);
                InvalidateRect(overlay->hwnd, nullptr, FALSE);
                logf(L"表情选择完成：索引=%d 大小=%dpx 已释放放置按键状态 前台返回覆盖层=%d",
                    index,overlay->toolSizes[T_EMOJI-T_RECT],(int)(GetForegroundWindow()==overlay->hwnd));
                if(overlay->emojiAnimation)logf(L"动态贴图选择完成：帧=%zu 时长=%llums；保存时询问动态或静态",
                    overlay->emojiAnimation->frames.size(),overlay->emojiAnimation->duration);
            }else{
                logf(L"表情选择未执行：位置不在表情格内，保留选择面板");
            }
        }
        return 0;
    case WM_KEYDOWN:
        if(popup&&(wp==VK_NEXT||wp==VK_PRIOR||wp==VK_HOME||wp==VK_END)){
            scroll_to(popup,wp==VK_HOME?0:wp==VK_END?popup->rows:
                popup->scrollRow+(wp==VK_NEXT?popup->visibleRows:-popup->visibleRows));
            return 0;
        }
        if (wp == VK_ESCAPE) {
            close_popup(popup);
            return 0;
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && popup) {
            close_popup(popup);
            return 0;
        }
        break;
    case WM_DESTROY:
        KillTimer(h,1);
        if (popup) {
            if(popup->change!=INVALID_HANDLE_VALUE){
                FindCloseChangeNotification(popup->change);
                logf(L"图片分组窗口关闭：目录监听已释放");
            }
            if (popup->overlay && popup->overlay->emojiPopup == h) {
                popup->overlay->emojiPopup = nullptr;
            }
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            delete popup;
        }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void ensure_class() {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = popup_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = EMOJI_CLASS;
    RegisterClassExW(&wc);
    done = true;
}

}  // namespace

void close_emoji_popup(Overlay* overlay) {
    if (!overlay || !overlay->emojiPopup) return;
    HWND h = overlay->emojiPopup;
    overlay->emojiPopup = nullptr;
    DestroyWindow(h);
}

void show_emoji_picker(Overlay* overlay) {
    if (!overlay || !overlay->tbVisible) return;
    LogTrace trace(overlay->traceId);
    ensure_class();
    close_emoji_popup(overlay);

    auto* popup = new EmojiPopup();
    popup->overlay = overlay;
    popup->count = emoji_reload();
    build_rows(popup);
    for(size_t i=0;i<popup->display.size();++i)if(popup->display[i].header&&
        emoji_groups()[popup->display[i].group].key==overlay->emojiGroup){
        popup->scrollRow=std::min((int)i,std::max(0,popup->rows-popup->visibleRows));break;
    }
    const int width = popup_width(popup);
    const int height = popup_height(popup);

    int x = overlay->shot.vx + (int)overlay->tbX;
    int y = overlay->shot.vy + (int)overlay->tbY - height - S(6);
    for (const auto& item : overlay->items) {
        if (item.id == T_EMOJI) {
            x = overlay->shot.vx + (int)(overlay->tbX + item.rect.X) -
                (width - (int)item.rect.Width) / 2;
            break;
        }
    }
    if (y < overlay->shot.vy + 4) y = overlay->shot.vy + (int)overlay->tbY + S(54);
    x = std::max(overlay->shot.vx + 4, std::min(x, overlay->shot.vx + overlay->shot.vw - width - 4));
    y = std::max(overlay->shot.vy + 4, std::min(y, overlay->shot.vy + overlay->shot.vh - height - 4));

    HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, EMOJI_CLASS, L"表情",
                             WS_POPUP|(popup->rows>popup->visibleRows?WS_VSCROLL:0), x, y, width, height,
                             overlay->hwnd, nullptr, GetModuleHandleW(nullptr), popup);
    if (!h) {
        logf(L"图片选择窗口创建失败：error=%lu，可用图片=%d",GetLastError(),popup->count);
        delete popup;
        return;
    }
    overlay->emojiPopup = h;
    SetTimer(h,1,30,nullptr);
    update_scrollbar(popup);watch_images(popup);
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
    InvalidateRect(h, nullptr, FALSE);
    logf(L"图片选择窗口已显示：目录=%s 图片=%d 可见行=%d 可滚动=%d 尺寸=%dx%d",
        emoji_directory().c_str(),popup->count,popup->visibleRows,
        (int)(popup->rows>popup->visibleRows),width,height);
}
