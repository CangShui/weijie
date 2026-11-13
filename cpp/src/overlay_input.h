#pragma once
#include "common.h"

class OverlayInput {
public:
    bool acquire(HWND window) {
        HWND foreground = GetForegroundWindow();
        DWORD current = GetCurrentThreadId();
        DWORD other = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
        bool attached = other && other != current && AttachThreadInput(other, current, TRUE);
        DWORD attachError = attached ? ERROR_SUCCESS : GetLastError();
        SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(window);
        SetActiveWindow(window);
        SetFocus(window);
        if (attached) AttachThreadInput(other, current, FALSE);
        if (GetForegroundWindow() != window) {
            logf(L"覆盖层输入接管失败：前台=%p 目标=%p 附加输入队列=%d error=%lu，关闭不可操作的覆盖层",
                 GetForegroundWindow(), window, (int)attached, attachError);
            return false;
        }
        // 必须先使游戏失去前台，再解除鼠标限位；否则游戏会立即重新锁住鼠标。
        if (!ClipCursor(nullptr)) {
            logf(L"覆盖层输入接管失败：无法解除鼠标限位 error=%lu", GetLastError());
            return false;
        }
        if (!cursorShows_) {
            int count;
            do { count = ShowCursor(TRUE); ++cursorShows_; } while (count < 0);
        }
        logf(L"覆盖层输入接管完成：已取得前台、解除鼠标限位并显示鼠标，显示计数增量=%d",
             cursorShows_);
        return true;
    }

    void release() {
        int count = cursorShows_;
        while (cursorShows_ > 0) { ShowCursor(FALSE); --cursorShows_; }
        logf(L"覆盖层输入已释放：鼠标显示计数恢复，回收增量=%d", count);
        // 不恢复游戏旧的 ClipCursor 区域；游戏重新获得前台时自行设置。
    }
private:
    int cursorShows_ = 0;
};
