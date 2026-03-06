/**
 * @file window_manager.cpp
 * @brief Win32 window management, input simulation, volume control, clipboard, screenshots
 *
 * Implements the full WindowManager interface using direct Win32 API calls,
 * Core Audio COM for volume, SendInput for keyboard/mouse, GDI+ for PNG screenshots.
 */

#include "window_manager.h"
#include "ui_automation.h"
#include <algorithm>
#include <chrono>
#include <thread>
#include <sstream>
#include <filesystem>
#include <ctime>
#include <cctype>

// Win32 extended headers
#include <dwmapi.h>
#include <psapi.h>
#include <shlobj.h>
#include <shellapi.h>

// Core Audio COM for volume
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>

// GDI+ for PNG save
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")

#ifdef VISION_HAS_SPDLOG
#include <spdlog/spdlog.h>
#define LOG_INFO(...) spdlog::info(__VA_ARGS__)
#define LOG_WARN(...) spdlog::warn(__VA_ARGS__)
#define LOG_ERROR(...) spdlog::error(__VA_ARGS__)
#else
#define LOG_INFO(...) (void)0
#define LOG_WARN(...) (void)0
#define LOG_ERROR(...) (void)0
#endif

namespace fs = std::filesystem;

namespace vision {

// ═══════════════════ Helpers ═══════════════════

static std::string wideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(),
                                    nullptr, 0, nullptr, nullptr);
    std::string result(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(),
                        result.data(), size, nullptr, nullptr);
    return result;
}

static std::wstring utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(),
                                    nullptr, 0);
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(),
                        result.data(), size);
    return result;
}

static std::string toLowerStr(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}

static std::string getProcessName(DWORD pid) {
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProcess) return {};
    char name[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    if (QueryFullProcessImageNameA(hProcess, 0, name, &size)) {
        CloseHandle(hProcess);
        std::string full(name);
        auto pos = full.find_last_of("\\/");
        return pos != std::string::npos ? full.substr(pos + 1) : full;
    }
    CloseHandle(hProcess);
    return {};
}

// GDI+ CLSID helper
static int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    auto pImageCodecInfo = (Gdiplus::ImageCodecInfo*)(malloc(size));
    if (!pImageCodecInfo) return -1;
    Gdiplus::GetImageEncoders(num, size, pImageCodecInfo);
    for (UINT j = 0; j < num; ++j) {
        if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0) {
            *pClsid = pImageCodecInfo[j].Clsid;
            free(pImageCodecInfo);
            return j;
        }
    }
    free(pImageCodecInfo);
    return -1;
}

// ═══════════════════ Static Data ═══════════════════

const std::unordered_map<std::string, std::vector<std::string>> WindowManager::APP_WINDOW_PATTERNS = {
    {"notepad",    {"notepad", "untitled - notepad", "- notepad"}},
    {"chrome",     {"chrome", "google chrome"}},
    {"edge",       {"edge", "microsoft edge"}},
    {"firefox",    {"mozilla firefox", "firefox"}},
    {"brave",      {"brave"}},
    {"discord",    {"discord"}},
    {"spotify",    {"spotify"}},
    {"code",       {"visual studio code", "vs code", "- code"}},
    {"explorer",   {"file explorer", "explorer"}},
    {"cmd",        {"command prompt", "cmd.exe"}},
    {"powershell", {"powershell", "windows powershell"}},
    {"terminal",   {"windows terminal", "terminal"}},
    {"word",       {"word", "- microsoft word", "document"}},
    {"excel",      {"excel", "- microsoft excel"}},
    {"vlc",        {"vlc media player", "vlc"}},
    {"paint",      {"paint", "untitled - paint"}},
    {"calculator", {"calculator"}},
};

// ═══════════════════ Constructor ═══════════════════

WindowManager::WindowManager() {
    // Initialize GDI+ ONCE at construction (not per-screenshot)
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    Gdiplus::GdiplusStartup(reinterpret_cast<ULONG_PTR*>(&gdiplusToken_), &gdiplusStartupInput, nullptr);
    LOG_INFO("WindowManager initialized (GDI+ ready)");
}

WindowManager::~WindowManager() {
    // Shutdown GDI+ ONCE at destruction
    if (gdiplusToken_) {
        Gdiplus::GdiplusShutdown(static_cast<ULONG_PTR>(gdiplusToken_));
        gdiplusToken_ = 0;
    }
}

// ═══════════════════ Window Queries ═══════════════════

std::string WindowManager::getActiveWindowTitle() {
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return {};
    wchar_t title[512] = {};
    GetWindowTextW(hwnd, title, 512);
    return wideToUtf8(title);
}

std::optional<HWND> WindowManager::findWindow(const std::string& title_pattern) {
    std::string pattern_lower = toLowerStr(title_pattern);
    HWND found = nullptr;

    struct EnumData { std::string pattern; HWND result; };
    EnumData data{pattern_lower, nullptr};

    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* d = reinterpret_cast<EnumData*>(lParam);
        if (!IsWindowVisible(hwnd)) return TRUE;

        wchar_t title[512] = {};
        GetWindowTextW(hwnd, title, 512);
        std::string t = toLowerStr(wideToUtf8(title));
        if (t.empty()) return TRUE;

        // Check title contains pattern
        if (t.find(d->pattern) != std::string::npos) {
            d->result = hwnd;
            return FALSE; // stop enumeration
        }

        // Check exe name
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        std::string exe = toLowerStr(getProcessName(pid));
        if (exe.find(d->pattern) != std::string::npos) {
            d->result = hwnd;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&data));

    if (data.result) return data.result;
    return std::nullopt;
}

std::vector<WindowInfo> WindowManager::listWindows() {
    std::vector<WindowInfo> windows;

    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* list = reinterpret_cast<std::vector<WindowInfo>*>(lParam);
        if (!IsWindowVisible(hwnd)) return TRUE;

        wchar_t title[512] = {};
        GetWindowTextW(hwnd, title, 512);
        std::string t = wideToUtf8(title);
        if (t.empty()) return TRUE;

        WindowInfo wi;
        wi.hwnd = hwnd;
        wi.title = t;
        wi.is_visible = true;
        wi.is_minimized = IsIconic(hwnd) != 0;
        GetWindowRect(hwnd, &wi.rect);

        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        wi.pid = pid;
        wi.exe_name = getProcessName(pid);

        list->push_back(wi);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&windows));

    return windows;
}

// ═══════════════════ Window Control ═══════════════════

bool WindowManager::minimizeWindow(const std::string& pattern) {
    auto hwnd = findWindow(pattern);
    if (!hwnd) return false;
    ShowWindow(*hwnd, SW_MINIMIZE);
    return true;
}

bool WindowManager::maximizeWindow(const std::string& pattern) {
    auto hwnd = findWindow(pattern);
    if (!hwnd) return false;
    ShowWindow(*hwnd, SW_MAXIMIZE);
    return true;
}

bool WindowManager::closeWindow(const std::string& pattern) {
    auto hwnd = findWindow(pattern);
    if (!hwnd) return false;
    PostMessageW(*hwnd, WM_CLOSE, 0, 0);
    return true;
}

bool WindowManager::focusWindow(const std::string& pattern) {
    auto hwnd = findWindow(pattern);
    if (!hwnd) return false;

    // Restore if minimized
    if (IsIconic(*hwnd)) ShowWindow(*hwnd, SW_RESTORE);

    // Try to bring to foreground
    SetForegroundWindow(*hwnd);
    SetFocus(*hwnd);

    // Use Alt key trick if SetForegroundWindow fails
    DWORD fgThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    DWORD targetThread = GetWindowThreadProcessId(*hwnd, nullptr);
    if (fgThread != targetThread) {
        AttachThreadInput(fgThread, targetThread, TRUE);
        SetForegroundWindow(*hwnd);
        AttachThreadInput(fgThread, targetThread, FALSE);
    }
    return true;
}

void WindowManager::snapWindowLeft() {
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return;

    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);

    RECT work = mi.rcWork;
    int halfW = (work.right - work.left) / 2;
    SetWindowPos(hwnd, nullptr, work.left, work.top, halfW,
                 work.bottom - work.top, SWP_NOZORDER | SWP_SHOWWINDOW);
}

void WindowManager::snapWindowRight() {
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return;

    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);

    RECT work = mi.rcWork;
    int halfW = (work.right - work.left) / 2;
    SetWindowPos(hwnd, nullptr, work.left + halfW, work.top, halfW,
                 work.bottom - work.top, SWP_NOZORDER | SWP_SHOWWINDOW);
}

void WindowManager::showDesktop() {
    // Simulate Win+D
    sendHotkey('D', false, false, false, true);
}

// ═══════════════════ Screenshot (PNG via GDI+) ═══════════════════

std::string WindowManager::takeScreenshot(const std::string& save_path) {
    // GDI+ is already initialized in the constructor

    // Get FULL virtual screen (all monitors, DPI-aware)
    int screenX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int screenY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int screenW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int screenH = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP bmp = CreateCompatibleBitmap(screenDC, screenW, screenH);
    HGDIOBJ oldBmp = SelectObject(memDC, bmp);

    BitBlt(memDC, 0, 0, screenW, screenH, screenDC, screenX, screenY, SRCCOPY);

    SelectObject(memDC, oldBmp);  // Restore old bitmap

    // Determine save path — default to AI_Workspace/Screenshots (whitelisted, no CFA blocking)
    std::string finalPath = save_path;
    if (finalPath.empty()) {
        char profile[MAX_PATH] = {};
        if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr, 0, profile))) {
            finalPath = std::string(profile) + "\\AI_Workspace\\Screenshots";
        } else {
            finalPath = ".\\Screenshots";
        }

        // Ensure the directory exists
        fs::create_directories(finalPath);

        // Generate timestamped filename
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        struct tm tm_buf;
        localtime_s(&tm_buf, &time_t_now);
        char timestamp[64];
        strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", &tm_buf);
        finalPath += "\\VISION_Screenshot_" + std::string(timestamp) + ".png";
    }

    // Save as PNG using GDI+
    Gdiplus::Bitmap* gdiBitmap = Gdiplus::Bitmap::FromHBITMAP(bmp, nullptr);
    std::string result_msg;

    if (gdiBitmap) {
        CLSID pngClsid;
        if (GetEncoderClsid(L"image/png", &pngClsid) >= 0) {
            std::wstring widePath = utf8ToWide(finalPath);

            // Ensure parent directory exists
            fs::path p(finalPath);
            fs::create_directories(p.parent_path());

            Gdiplus::Status status = gdiBitmap->Save(widePath.c_str(), &pngClsid, nullptr);

            if (status == Gdiplus::Ok) {
                // STRICT VERIFICATION: confirm the file actually exists and has content
                std::error_code ec;
                if (fs::exists(finalPath, ec) && fs::file_size(finalPath, ec) > 0) {
                    LOG_INFO("Screenshot verified: {} ({} bytes)", finalPath,
                             fs::file_size(finalPath, ec));
                    result_msg = finalPath;

                    // Auto-open the screenshot in the default image viewer
                    ShellExecuteA(nullptr, "open", finalPath.c_str(),
                                  nullptr, nullptr, SW_SHOW);
                } else {
                    LOG_ERROR("Screenshot file missing or empty after save — "
                              "likely blocked by Controlled Folder Access or VirtualStore: {}",
                              finalPath);
                    result_msg = "ERROR: Screenshot save was reported as OK but the file "
                                 "does not exist at: " + finalPath +
                                 " — Windows Defender Controlled Folder Access may be blocking writes. "
                                 "Try adding AI_Workspace to the allowed folders list.";
                }
            } else {
                LOG_ERROR("GDI+ Save failed with status: {}", (int)status);
                result_msg = "ERROR: GDI+ Save failed (status " + std::to_string((int)status) + ")";
            }
        } else {
            result_msg = "ERROR: Failed to find PNG encoder";
        }
        delete gdiBitmap;
    } else {
        result_msg = "ERROR: Failed to create GDI+ Bitmap from HBITMAP";
    }

    // Cleanup GDI objects (but NOT GdiplusShutdown — that's in the destructor)
    DeleteObject(bmp);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);

    return result_msg;
}

// ═══════════════════ Volume (Core Audio COM) ═══════════════════

// FIX C5: RAII COM scope for volume operations (prevents CoInit/Uninit mismatch)
struct VolumeComScope {
    HRESULT hr;
    VolumeComScope() { hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~VolumeComScope() { if (SUCCEEDED(hr) || hr == S_FALSE) CoUninitialize(); }
    bool ok() const { return SUCCEEDED(hr) || hr == S_FALSE; }
};

static IAudioEndpointVolume* getEndpointVolume(VolumeComScope& com) {
    if (!com.ok()) return nullptr;

    IMMDeviceEnumerator* deviceEnumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  (void**)&deviceEnumerator);
    if (FAILED(hr) || !deviceEnumerator) return nullptr;

    IMMDevice* defaultDevice = nullptr;
    hr = deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultDevice);
    deviceEnumerator->Release();
    if (FAILED(hr) || !defaultDevice) return nullptr;

    IAudioEndpointVolume* endpointVolume = nullptr;
    hr = defaultDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
                                  nullptr, (void**)&endpointVolume);
    defaultDevice->Release();
    if (FAILED(hr)) return nullptr;

    return endpointVolume;
}

void WindowManager::setVolume(int level) {
    VolumeComScope com;  // RAII: CoUninitialize always called
    auto* vol = getEndpointVolume(com);
    if (!vol) return;
    float normalized = std::clamp(level, 0, 100) / 100.0f;
    vol->SetMasterVolumeLevelScalar(normalized, nullptr);
    vol->Release();
}

void WindowManager::volumeUp() {
    int cur = getVolume();
    setVolume(std::min(cur + 10, 100));
}

void WindowManager::volumeDown() {
    int cur = getVolume();
    setVolume(std::max(cur - 10, 0));
}

void WindowManager::muteToggle() {
    VolumeComScope com;
    auto* vol = getEndpointVolume(com);
    if (!vol) return;
    BOOL muted = FALSE;
    vol->GetMute(&muted);
    vol->SetMute(!muted, nullptr);
    vol->Release();
}

int WindowManager::getVolume() {
    VolumeComScope com;
    auto* vol = getEndpointVolume(com);
    if (!vol) return 50;
    float level = 0.0f;
    vol->GetMasterVolumeLevelScalar(&level);
    vol->Release();
    return (int)(level * 100.0f);
}

// ═══════════════════ Input Simulation ═══════════════════

void WindowManager::sendKeyInput(WORD vk, bool key_up) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    if (key_up) input.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(INPUT));
}

void WindowManager::sendHotkey(WORD vk, bool ctrl, bool alt, bool shift, bool win) {
    if (ctrl)  sendKeyInput(VK_CONTROL, false);
    if (alt)   sendKeyInput(VK_MENU, false);
    if (shift) sendKeyInput(VK_SHIFT, false);
    if (win)   sendKeyInput(VK_LWIN, false);

    sendKeyInput(vk, false);
    sendKeyInput(vk, true);

    if (win)   sendKeyInput(VK_LWIN, true);
    if (shift) sendKeyInput(VK_SHIFT, true);
    if (alt)   sendKeyInput(VK_MENU, true);
    if (ctrl)  sendKeyInput(VK_CONTROL, true);
}

WORD WindowManager::stringToVK(const std::string& key) {
    static const std::unordered_map<std::string, WORD> VK_MAP = {
        {"enter", VK_RETURN}, {"return", VK_RETURN}, {"tab", VK_TAB},
        {"escape", VK_ESCAPE}, {"esc", VK_ESCAPE}, {"space", VK_SPACE},
        {"backspace", VK_BACK}, {"delete", VK_DELETE}, {"del", VK_DELETE},
        {"insert", VK_INSERT}, {"home", VK_HOME}, {"end", VK_END},
        {"pageup", VK_PRIOR}, {"pagedown", VK_NEXT},
        {"up", VK_UP}, {"down", VK_DOWN}, {"left", VK_LEFT}, {"right", VK_RIGHT},
        {"f1", VK_F1}, {"f2", VK_F2}, {"f3", VK_F3}, {"f4", VK_F4},
        {"f5", VK_F5}, {"f6", VK_F6}, {"f7", VK_F7}, {"f8", VK_F8},
        {"f9", VK_F9}, {"f10", VK_F10}, {"f11", VK_F11}, {"f12", VK_F12},
        {"ctrl", VK_CONTROL}, {"alt", VK_MENU}, {"shift", VK_SHIFT},
        {"win", VK_LWIN}, {"windows", VK_LWIN}, {"caps", VK_CAPITAL},
        {"capslock", VK_CAPITAL}, {"numlock", VK_NUMLOCK},
        {"printscreen", VK_SNAPSHOT}, {"prtsc", VK_SNAPSHOT},
        {"scrolllock", VK_SCROLL}, {"pause", VK_PAUSE},
    };

    std::string lower = toLowerStr(key);
    auto it = VK_MAP.find(lower);
    if (it != VK_MAP.end()) return it->second;

    // Single character → VkKeyScanA
    if (lower.size() == 1) {
        SHORT vk = VkKeyScanA(lower[0]);
        if (vk != -1) return vk & 0xFF;
    }

    // Try as hex or decimal
    return 0;
}

void WindowManager::pressKey(const std::string& key) {
    std::string lower = toLowerStr(key);

    // Handle combo like "ctrl+l", "ctrl+shift+c"
    if (lower.find('+') != std::string::npos) {
        bool ctrl = false, alt = false, shift = false, win = false;
        std::string main_key;

        std::istringstream ss(lower);
        std::string part;
        std::vector<std::string> parts;
        while (std::getline(ss, part, '+')) parts.push_back(part);

        for (size_t i = 0; i < parts.size(); i++) {
            std::string p = parts[i];
            if (p == "ctrl" || p == "control") ctrl = true;
            else if (p == "alt") alt = true;
            else if (p == "shift") shift = true;
            else if (p == "win" || p == "windows") win = true;
            else main_key = p;
        }

        WORD vk = stringToVK(main_key);
        if (vk == 0 && main_key.size() == 1) {
            SHORT scan = VkKeyScanA(main_key[0]);
            vk = scan & 0xFF;
        }
        if (vk != 0) sendHotkey(vk, ctrl, alt, shift, win);
        return;
    }

    // Single key
    WORD vk = stringToVK(lower);
    if (vk != 0) {
        sendKeyInput(vk, false);
        sendKeyInput(vk, true);
    }
}

void WindowManager::typeText(const std::string& text, float interval,
                              const std::string& target_window) {
    if (text.empty()) return;

    // Focus target window if specified
    if (!target_window.empty()) {
        if (!waitAndFocus(target_window, 3.0f)) {
            LOG_WARN("Could not find/focus window '{}' for typing", target_window);
        }
        
        // Wait for the edit control to be ready (no hardcoded sleep)
        HWND fg = GetForegroundWindow();
        if (fg) {
            // Click the client area to activate child edit controls
            RECT rc;
            GetClientRect(fg, &rc);
            POINT center = { (rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2 };
            ClientToScreen(fg, &center);
            SetCursorPos(center.x, center.y);

            INPUT click[2] = {};
            click[0].type = INPUT_MOUSE;
            click[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            click[1].type = INPUT_MOUSE;
            click[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
            SendInput(2, click, sizeof(INPUT));

            // Smart wait: poll until the caret / keyboard focus is ready
            waitForInputReady(fg, 5.0f);
        }
    }

    std::wstring wtext = utf8ToWide(text);

    // Type using Unicode SendInput for full character support
    for (wchar_t wc : wtext) {
        INPUT inputs[2] = {};

        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wScan = wc;
        inputs[0].ki.dwFlags = KEYEVENTF_UNICODE;

        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wScan = wc;
        inputs[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;

        SendInput(2, inputs, sizeof(INPUT));

        if (interval > 0.0f) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds((int)(interval * 1000)));
        } else {
            // Tiny default delay for reliability in fast apps
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
}

void WindowManager::scrollPage(const std::string& direction, int amount) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_WHEEL;

    std::string dir = toLowerStr(direction);
    // FIX L5: Use signed arithmetic to avoid DWORD wrapping on negative values
    int delta = (dir == "up") ? (WHEEL_DELTA * amount) : (-WHEEL_DELTA * amount);
    input.mi.mouseData = static_cast<DWORD>(delta);
    SendInput(1, &input, sizeof(INPUT));
}

// ═══════════════════ Window Waiting ═══════════════════

std::pair<bool, HWND> WindowManager::waitForWindow(const std::string& pattern,
                                                     float timeout) {
    auto start = std::chrono::steady_clock::now();
    while (true) {
        auto elapsed = std::chrono::duration<float>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeout) break;

        auto hwnd = findWindow(pattern);
        if (hwnd) return {true, *hwnd};

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return {false, nullptr};
}

bool WindowManager::waitAndFocus(const std::string& app_name, float timeout) {
    std::string lower = toLowerStr(app_name);

    // Try known patterns first
    auto it = APP_WINDOW_PATTERNS.find(lower);
    if (it != APP_WINDOW_PATTERNS.end()) {
        for (const auto& pattern : it->second) {
            auto [found, hwnd] = waitForWindow(pattern, timeout / (float)it->second.size());
            if (found) {
                focusWindow(pattern);
                return true;
            }
        }
    }

    // Fall back to direct pattern match
    auto [found, hwnd] = waitForWindow(lower, timeout);
    if (found) {
        focusWindow(lower);
        return true;
    }
    return false;
}

// ═══════════════════ Smart Input Wait ═══════════════════

bool WindowManager::waitForInputReady(HWND hwnd, float timeout) {
    if (!hwnd) return false;

    auto start = std::chrono::steady_clock::now();

    while (true) {
        float elapsed = std::chrono::duration<float>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeout) break;

        // Method 1: Check if the GUI thread reports an active caret
        DWORD threadId = GetWindowThreadProcessId(hwnd, nullptr);
        GUITHREADINFO gti{};
        gti.cbSize = sizeof(gti);
        if (GetGUIThreadInfo(threadId, &gti)) {
            if (gti.hwndCaret != nullptr) {
                LOG_INFO("Input ready: caret detected after {:.0f}ms", elapsed * 1000);
                return true;
            }
            // Also check if a child has focus (e.g., edit control)
            if (gti.hwndFocus != nullptr && gti.hwndFocus != hwnd) {
                // A child control has keyboard focus — good enough
                LOG_INFO("Input ready: child focus detected after {:.0f}ms", elapsed * 1000);
                return true;
            }
        }

        // Method 2: Fallback to UI Automation keyboard focus check
        // FIX B2: Use function-scoped pointer instead of static to avoid
        // COM destruction after main() exit
        UIAutomation uia_checker;
        if (uia_checker.isAvailable()) {
            auto elem = uia_checker.findElement("");  // Any focused element
            // Even if findElement returns nothing, the fact that UIA is responsive
            // combined with the window being foreground is a good signal
            if (elapsed > 0.3f) {
                // After 300ms minimum, if the window is foreground, proceed
                HWND current_fg = GetForegroundWindow();
                if (current_fg == hwnd) {
                    LOG_INFO("Input ready: UIA fallback after {:.0f}ms", elapsed * 1000);
                    return true;
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    LOG_WARN("waitForInputReady timed out after {:.0f}ms", timeout * 1000);
    return false;
}

// ═══════════════════ Multi-Window Layout ═══════════════════

bool WindowManager::arrangeWindowsSideBySide(const std::string& left, const std::string& right) {
    auto hwndL = findWindow(left);
    auto hwndR = findWindow(right);
    if (!hwndL || !hwndR) return false;

    HMONITOR mon = MonitorFromWindow(*hwndL, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);

    RECT work = mi.rcWork;
    int halfW = (work.right - work.left) / 2;
    int h = work.bottom - work.top;

    SetWindowPos(*hwndL, nullptr, work.left, work.top, halfW, h, SWP_NOZORDER | SWP_SHOWWINDOW);
    SetWindowPos(*hwndR, nullptr, work.left + halfW, work.top, halfW, h, SWP_NOZORDER | SWP_SHOWWINDOW);

    return true;
}

void WindowManager::tileAllWindows() {
    auto windows = listWindows();

    // Filter to actual app windows (exclude tiny/invisible)
    std::vector<HWND> toTile;
    for (auto& w : windows) {
        int width = w.rect.right - w.rect.left;
        int height = w.rect.bottom - w.rect.top;
        if (width > 100 && height > 100 && !w.is_minimized) {
            toTile.push_back(w.hwnd);
        }
    }
    if (toTile.empty()) return;

    HMONITOR mon = MonitorFromWindow(toTile[0], MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    RECT work = mi.rcWork;

    int count = (int)toTile.size();
    int cols = (int)std::ceil(std::sqrt((double)count));
    int rows = (count + cols - 1) / cols;

    int cellW = (work.right - work.left) / cols;
    int cellH = (work.bottom - work.top) / rows;

    for (int i = 0; i < count; i++) {
        int col = i % cols;
        int row = i / cols;
        SetWindowPos(toTile[i], nullptr,
                     work.left + col * cellW, work.top + row * cellH,
                     cellW, cellH, SWP_NOZORDER | SWP_SHOWWINDOW);
    }
}

// ═══════════════════ Multi-Monitor ═══════════════════

std::vector<MonitorInfo> WindowManager::getMonitors() {
    struct MonitorEnum { std::vector<MonitorInfo>* monitors; int idx; };
    std::vector<MonitorInfo> monitors;
    MonitorEnum data{&monitors, 0};

    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hMon, HDC, LPRECT, LPARAM lParam) -> BOOL {
        auto* d = reinterpret_cast<MonitorEnum*>(lParam);
        MONITORINFO mi{}; mi.cbSize = sizeof(mi);
        GetMonitorInfoW(hMon, &mi);

        MonitorInfo info;
        info.handle = hMon;
        info.rect = mi.rcMonitor;
        info.work_rect = mi.rcWork;
        info.is_primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        info.index = d->idx++;
        d->monitors->push_back(info);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&data));

    return monitors;
}

bool WindowManager::moveToMonitor(const std::string& pattern, int monitor_num) {
    auto hwnd = findWindow(pattern);
    if (!hwnd) return false;

    auto monitors = getMonitors();
    if (monitor_num < 0 || monitor_num >= (int)monitors.size()) return false;

    RECT target = monitors[monitor_num].work_rect;
    RECT current;
    GetWindowRect(*hwnd, &current);

    int width = current.right - current.left;
    int height = current.bottom - current.top;

    SetWindowPos(*hwnd, nullptr, target.left, target.top,
                 std::min(width, (int)(target.right - target.left)),
                 std::min(height, (int)(target.bottom - target.top)),
                 SWP_NOZORDER | SWP_SHOWWINDOW);
    return true;
}

// ═══════════════════ Clipboard ═══════════════════

std::string WindowManager::getClipboard() {
    if (!OpenClipboard(nullptr)) return {};
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (!hData) { CloseClipboard(); return {}; }

    auto* pData = (wchar_t*)GlobalLock(hData);
    if (!pData) { CloseClipboard(); return {}; }

    std::string result = wideToUtf8(pData);
    GlobalUnlock(hData);
    CloseClipboard();
    return result;
}

bool WindowManager::setClipboard(const std::string& text) {
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();

    std::wstring wide = utf8ToWide(text);
    size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!hMem) { CloseClipboard(); return false; }

    memcpy(GlobalLock(hMem), wide.c_str(), bytes);
    GlobalUnlock(hMem);
    SetClipboardData(CF_UNICODETEXT, hMem);
    CloseClipboard();
    return true;
}

std::string WindowManager::copySelection() {
    // Simulate Ctrl+C, wait, read clipboard
    sendHotkey('C', true);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return getClipboard();
}

void WindowManager::pasteClipboard() {
    sendHotkey('V', true);
}

} // namespace vision
