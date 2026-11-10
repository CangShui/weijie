#include "emoji_renderer.h"

#include <memory>
#include <unordered_map>

#include "emoji_catalog.inc"

using namespace Gdiplus;

namespace {

std::unordered_map<int, std::unique_ptr<Bitmap>> g_cache;

std::wstring emoji_path(int index) {
    wchar_t name[32]{};
    swprintf_s(name, L"%02d.png", index);

    std::wstring exe = exe_path();
    size_t pos = exe.find_last_of(L"\\/");
    std::wstring root = pos == std::wstring::npos ? L"." : exe.substr(0, pos);
    const std::wstring candidates[] = {
        root + L"\\emoji\\" + name,
        localappdata_dir() + L"\\emoji\\" + name,
    };
    for (const auto& path : candidates) {
        DWORD attr = GetFileAttributesW(path.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) return path;
    }
    return {};
}

}  // namespace

int emoji_count() {
    return EMOJI_COUNT;
}

const wchar_t* emoji_text(int index) {
    if (index < 0 || index >= EMOJI_COUNT) return nullptr;
    return EMOJI_CATALOG[index];
}

Bitmap* emoji_bitmap(int index) {
    if (index < 0 || index >= EMOJI_COUNT) return nullptr;
    auto it = g_cache.find(index);
    if (it != g_cache.end()) return it->second.get();

    std::wstring path = emoji_path(index);
    if (path.empty()) return nullptr;
    auto* bitmap = Bitmap::FromFile(path.c_str(), FALSE);
    if (!bitmap || bitmap->GetLastStatus() != Ok) {
        delete bitmap;
        return nullptr;
    }
    auto inserted = g_cache.emplace(index, std::unique_ptr<Bitmap>(bitmap));
    return inserted.first->second.get();
}

void draw_emoji(Graphics& g, int index, const PointF& center, float size) {
    Bitmap* bitmap = emoji_bitmap(index);
    if (!bitmap) {
        // If a packaged emoji asset is missing, keep text visible rather than
        // leaving an unexplained blank annotation.
        Font font(L"Segoe UI Emoji", size * 0.82f, FontStyleRegular, UnitPixel);
        SolidBrush ink(Color(255, 35, 43, 39));
        StringFormat format;
        format.SetAlignment(StringAlignmentCenter);
        format.SetLineAlignment(StringAlignmentCenter);
        const wchar_t* text = emoji_text(index);
        if (text) {
            g.DrawString(text, -1, &font,
                         RectF(center.X - size / 2, center.Y - size / 2, size, size),
                         &format, &ink);
        }
        return;
    }

    RectF destination(center.X - size / 2, center.Y - size / 2, size, size);
    g.DrawImage(bitmap, destination, 0, 0, (REAL)bitmap->GetWidth(),
                (REAL)bitmap->GetHeight(), UnitPixel);
}
