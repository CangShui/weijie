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

struct EmojiPopup {
    Overlay* overlay = nullptr;
    HWND hwnd = nullptr;
    int hover = -1;
    int count = 0;
    int rows = 0;
};

int popup_width() {
    return S(kPad * 2 + kColumns * kCell);
}

int popup_height(const EmojiPopup* popup) {
    return S(kTitleHeight + popup->rows * kCell + kPad);
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
    const int width = popup_width();
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
    g.DrawString(L"表情", -1, &title, PointF(SF((float)kPad), SF(8.0f)), nullptr, &ink);

    const float cell = SF((float)kCell);
    const float imageSize = SF(26.0f);
    for (int i = 0; i < popup->count; ++i) {
        const int col = i % kColumns;
        const int row = i / kColumns;
        const RectF cellRect(SF((float)kPad) + col * cell,
                             SF((float)kTitleHeight) + row * cell,
                             cell, cell);
        if (i == popup->hover) {
            SolidBrush hover(Color(255, 238, 243, 239));
            draw_round_rect(g, cellRect, SF(6.0f));
            g.FillRectangle(&hover, RectF(cellRect.X + SF(2), cellRect.Y + SF(2),
                                          cellRect.Width - SF(4), cellRect.Height - SF(4)));
        }
        Bitmap* image = emoji_bitmap(i);
        if (image) {
            g.DrawImage(image,
                        RectF(cellRect.X + (cell - imageSize) / 2,
                              cellRect.Y + (cell - imageSize) / 2,
                              imageSize, imageSize),
                        0, 0, (REAL)image->GetWidth(), (REAL)image->GetHeight(), UnitPixel);
        } else {
            Font fallback(FONT_UI(), SF(12.0f), FontStyleRegular, UnitPixel);
            StringFormat fmt;
            fmt.SetAlignment(StringAlignmentCenter);
            fmt.SetLineAlignment(StringAlignmentCenter);
            const wchar_t* text = emoji_text(i);
            if (text) g.DrawString(text, -1, &fallback, cellRect, &fmt, &ink);
        }
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
    if (col < 0 || col >= kColumns || row < 0) return -1;
    const int index = row * kColumns + col;
    return index < popup->count ? index : -1;
}

void close_popup(EmojiPopup* popup) {
    if (!popup) return;
    if (popup->overlay && popup->overlay->emojiPopup == popup->hwnd) {
        popup->overlay->emojiPopup = nullptr;
    }
    DestroyWindow(popup->hwnd);
}

LRESULT CALLBACK popup_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* popup = (EmojiPopup*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE:
        popup = (EmojiPopup*)((CREATESTRUCTW*)lp)->lpCreateParams;
        popup->hwnd = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)popup);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(h, &ps);
        if (popup) paint(popup, dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (popup) {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int hover = emoji_at(popup, pt);
            if (hover != popup->hover) {
                popup->hover = hover;
                InvalidateRect(h, nullptr, FALSE);
            }
        }
        return 0;
    case WM_MOUSELEAVE:
        if (popup) {
            popup->hover = -1;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (popup) {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int index = emoji_at(popup, pt);
            if (index >= 0) {
                popup->overlay->emojiIndex = index;
                popup->overlay->tool = T_EMOJI;
                InvalidateRect(popup->overlay->hwnd, nullptr, FALSE);
                close_popup(popup);
            }
        }
        return 0;
    case WM_KEYDOWN:
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
        if (popup) {
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
    ensure_class();
    close_emoji_popup(overlay);

    auto* popup = new EmojiPopup();
    popup->overlay = overlay;
    popup->count = emoji_count();
    popup->rows = (popup->count + kColumns - 1) / kColumns;
    const int width = popup_width();
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
                             WS_POPUP, x, y, width, height,
                             overlay->hwnd, nullptr, GetModuleHandleW(nullptr), popup);
    if (!h) {
        delete popup;
        return;
    }
    overlay->emojiPopup = h;
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
    InvalidateRect(h, nullptr, FALSE);
}
