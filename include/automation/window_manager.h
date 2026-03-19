#pragma once
/**
 * @file window_manager.h
 * @brief Win32 window management, input simulation, and clipboard
 * 
 * Direct Win32 API calls for window find/focus/minimize/maximize/close/snap,
 * keyboard/mouse simulation via SendInput, clipboard, screenshots, multi-monitor.
 *
 * Includes Smart Input Wait: polls for edit-control readiness
 * before typing to prevent the "swallowed first letter" bug.
 */

#include <string>
#include <vector>
#include <optional>
#include <utility>
#include <unordered_map>

#include <windows.h>

namespace vision {

/// Information about a visible window
struct WindowInfo {
    HWND hwnd = nullptr;
    std::string title;
    std::string exe_name;
    DWORD pid = 0;
    RECT rect{};
    bool is_visible = false;
    bool is_minimized = false;
};

/// Information about a connected monitor
struct MonitorInfo {
    HMONITOR handle = nullptr;
    RECT rect{};
    RECT work_rect{};
    bool is_primary = false;
    int index = 0;
};

class WindowManager {
public:
    WindowManager();
    ~WindowManager();

    // Non-copyable (GDI+ token ownership)
    WindowManager(const WindowManager&) = delete;
    WindowManager& operator=(const WindowManager&) = delete;

    // ── Window queries ───────────────────────────────────────────
    std::string getActiveWindowTitle();
    std::optional<HWND> findWindow(const std::string& title_pattern);
    std::vector<WindowInfo> listWindows();

    // ── Window control ───────────────────────────────────────────
    bool minimizeWindow(const std::string& pattern);
    bool maximizeWindow(const std::string& pattern);
    bool closeWindow(const std::string& pattern);
    bool focusWindow(const std::string& pattern);
    void snapWindowLeft();
    void snapWindowRight();
    void showDesktop();

    // ── Screenshot ───────────────────────────────────────────────
    std::string takeScreenshot(const std::string& save_path = "");

    // ── Volume (Core Audio COM) ──────────────────────────────────
    void setVolume(int level);
    void volumeUp();
    void volumeDown();
    void muteToggle();
    int getVolume();

    // ── Input simulation (SendInput) ─────────────────────────────
    void scrollPage(const std::string& direction, int amount = 3);
    void pressKey(const std::string& key);
    void typeText(const std::string& text, float interval = 0.05f,
                  const std::string& target_window = "");
    void sendHotkey(WORD vk, bool ctrl = false, bool alt = false,
                    bool shift = false, bool win = false);

    // ── Window waiting ───────────────────────────────────────────
    std::pair<bool, HWND> waitForWindow(const std::string& pattern,
                                         float timeout = 10.0f);
    bool waitAndFocus(const std::string& app_name, float timeout = 8.0f);

    /// Wait until a window's edit control is ready for input.
    /// Polls GetGUIThreadInfo for caret, falls back to UIA keyboard focus.
    /// Returns true if input readiness was confirmed, false on timeout.
    bool waitForInputReady(HWND hwnd, float timeout = 5.0f);

    // ── Multi-window layout ──────────────────────────────────────
    bool arrangeWindowsSideBySide(const std::string& left, const std::string& right);
    void tileAllWindows();

    // ── Multi-monitor ────────────────────────────────────────────
    std::vector<MonitorInfo> getMonitors();
    bool moveToMonitor(const std::string& pattern, int monitor_num);

    // ── Clipboard ────────────────────────────────────────────────
    std::string getClipboard();
    bool setClipboard(const std::string& text);
    std::string copySelection();
    void pasteClipboard();

private:
    /// Map of known app title patterns for waitAndFocus
    static const std::unordered_map<std::string, std::vector<std::string>> APP_WINDOW_PATTERNS;

    /// Convert string key name to virtual key code
    WORD stringToVK(const std::string& key);

    /// Send a single key input event
    void sendKeyInput(WORD vk, bool key_up);

    /// GDI+ token for RAII screenshot support (init once, shutdown once)
    unsigned long long gdiplusToken_ = 0;
};

} // namespace vision
