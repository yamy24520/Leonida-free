#pragma once

// The window a GDKX title shows its frames in, for PresentX.
//
// On the console PresentX puts a frame on the display: it names no window. A GDKX title still makes its own top-level
// Win32 window (CreateWindowExW with its own class, a message loop on its own thread) before it presents, so on the PC
// that window is where the frames belong. Measured on a retail executable: by its first PresentX the title has one
// visible, unowned top-level window on a thread of its own; the other windows of the process at that point are hidden
// (the IME windows Windows adds, a driver's dummy window).
//
// The lookup sends no message to any window. The present thread is not the title's window thread, and that thread can
// be busy (or waiting on the present thread) when the first PresentX comes: GetWindowTextW, or any call that sends a
// message across threads, would block the present thread there. Measured: a lookup that read the window titles with
// GetWindowTextW stopped the retail title at its first PresentX. EnumWindows, GetWindowThreadProcessId, IsWindowVisible,
// GetWindow(GW_OWNER), GetWindowLongPtrW and GetClientRect read the window manager's own state and send nothing.

#include <windows.h>

namespace rt::d3d12 {

// The visible, unowned, non-tool top-level window of `process_id` with the largest client area, `exclude` aside (the
// runtime's own fallback window); null when the process has none. Ties keep the first one in Z order (the topmost).
inline HWND find_title_window(DWORD process_id, HWND exclude = nullptr) {
    struct Search {
        DWORD process_id;
        HWND exclude;
        HWND best;
        LONGLONG best_area;
    } search{process_id, exclude, nullptr, -1};
    EnumWindows(
        [](HWND window, LPARAM context) -> BOOL {
            auto& s = *reinterpret_cast<Search*>(context);
            DWORD owner_process = 0;
            if (window == s.exclude || !GetWindowThreadProcessId(window, &owner_process) ||
                owner_process != s.process_id)
                return TRUE;
            if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER)) return TRUE;
            if (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
            RECT client{};
            const LONGLONG area =
                GetClientRect(window, &client) ? LONGLONG(client.right - client.left) * (client.bottom - client.top) : 0;
            if (area > s.best_area) {
                s.best = window;
                s.best_area = area;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.best;
}

}  // namespace rt::d3d12
