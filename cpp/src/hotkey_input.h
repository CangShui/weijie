#pragma once
#include "common.h"
#include <array>

// Raw Input 独立维护物理键状态，不依赖低级钩子或游戏窗口的消息队列。
class RawHotkeyState {
public:
    void reset() {
        for (UINT key = 0; key < down_.size(); ++key)
            down_[key] = (GetAsyncKeyState(key) & 0x8000) != 0;
    }

    bool update(const RAWKEYBOARD& input, UINT mods, UINT vk) {
        UINT key = input.VKey;
        if (key == VK_SHIFT)
            key = MapVirtualKeyW(input.MakeCode, MAPVK_VSC_TO_VK_EX);
        else if (key == VK_CONTROL)
            key = (input.Flags & RI_KEY_E0) ? VK_RCONTROL : VK_LCONTROL;
        else if (key == VK_MENU)
            key = (input.Flags & RI_KEY_E0) ? VK_RMENU : VK_LMENU;
        if (!key || key >= down_.size()) return false;
        const bool down = (input.Flags & RI_KEY_BREAK) == 0;
        const bool newlyDown = down && !down_[key];
        down_[key] = down;
        UINT actual = 0;
        if (down_[VK_LCONTROL] || down_[VK_RCONTROL]) actual |= MOD_CONTROL;
        if (down_[VK_LMENU] || down_[VK_RMENU]) actual |= MOD_ALT;
        if (down_[VK_LSHIFT] || down_[VK_RSHIFT]) actual |= MOD_SHIFT;
        // Windows 键不是受支持的修饰键，按住它时不触发另一组快捷键。
        if (down_[VK_LWIN] || down_[VK_RWIN]) actual |= MOD_WIN;
        return newlyDown && key == vk && actual == mods;
    }
private:
    std::array<bool, 256> down_{};
};
