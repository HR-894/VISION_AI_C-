/**
 * @file system_commands.cpp
 * @brief Comprehensive Windows system controls — Part 1 (Brightness, Network, Settings, Power)
 */

#include "system_commands.h"
#include <algorithm>
#include <sstream>
#include <filesystem>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <comdef.h>
#include <wbemidl.h>
#include <tlhelp32.h>
#include <iphlpapi.h>
#include <psapi.h>
#include <powrprof.h>

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
using json = nlohmann::json;

namespace vision {

// ═══════════════════ Static maps ═══════════════════

const std::unordered_map<std::string, std::string> SystemCommands::BROWSER_EXE = {
    {"edge", "msedge.exe"}, {"microsoft edge", "msedge.exe"}, {"msedge", "msedge.exe"},
    {"chrome", "chrome.exe"}, {"google chrome", "chrome.exe"},
    {"firefox", "firefox.exe"}, {"mozilla firefox", "firefox.exe"},
    {"brave", "brave.exe"}, {"opera", "opera.exe"}, {"vivaldi", "vivaldi.exe"},
};

const std::unordered_map<std::string, std::string> SystemCommands::SETTINGS_MAP = {
    {"display", "ms-settings:display"}, {"sound", "ms-settings:sound"},
    {"notifications", "ms-settings:notifications"}, {"power", "ms-settings:powersleep"},
    {"battery", "ms-settings:batterysaver"}, {"storage", "ms-settings:storagesense"},
    {"bluetooth", "ms-settings:bluetooth"}, {"wifi", "ms-settings:network-wifi"},
    {"vpn", "ms-settings:network-vpn"}, {"privacy", "ms-settings:privacy"},
    {"update", "ms-settings:windowsupdate"}, {"about", "ms-settings:about"},
    {"personalization", "ms-settings:personalization"}, {"themes", "ms-settings:themes"},
    {"taskbar", "ms-settings:taskbar"}, {"apps", "ms-settings:appsfeatures"},
    {"default", "ms-settings:defaultapps"}, {"mouse", "ms-settings:mousetouchpad"},
    {"keyboard", "ms-settings:typing"}, {"network", "ms-settings:network-status"},
};

const std::vector<std::string> SystemCommands::PROTECTED_PROCESSES = {
    "explorer.exe", "svchost.exe", "csrss.exe", "wininit.exe",
    "winlogon.exe", "lsass.exe", "services.exe", "smss.exe",
    "dwm.exe", "System", "Registry", "taskmgr.exe",
};

SystemCommands::SystemCommands() = default;

SystemCommands::~SystemCommands() {
    stopFocusMode();
    stopStopwatch();
    for (auto& t : timer_threads_) {
        if (t.joinable()) t.detach();
    }
}

std::wstring SystemCommands::toWide(const std::string& str) {
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    std::wstring wide(len - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, wide.data(), len);
    return wide;
}

std::string SystemCommands::runCommand(const std::string& cmd) {
    FILE* pipe = _popen(cmd.c_str(), "r");
    if (!pipe) return "";
    char buf[256];
    std::string result;
    while (fgets(buf, sizeof(buf), pipe)) result += buf;
    _pclose(pipe);
    // Trim trailing whitespace
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' '))
        result.pop_back();
    return result;
}

// ═══════════════════ Brightness (WMI) ═══════════════════

std::pair<bool, std::string> SystemCommands::setBrightness(int level) {
    level = std::clamp(level, 0, 100);
    std::string cmd = "powershell -NoProfile -Command \"(Get-WmiObject -Namespace root/wmi "
                      "-Class WmiMonitorBrightnessMethods).WmiSetBrightness(1," +
                      std::to_string(level) + ")\" 2>nul";
    runCommand(cmd);
    LOG_INFO("Brightness set to {}%", level);
    return {true, "Brightness set to " + std::to_string(level) + "%"};
}

std::pair<bool, std::string> SystemCommands::brightnessUp(int amount) {
    int current = getCurrentBrightness();
    return setBrightness(std::min(100, current + amount));
}

std::pair<bool, std::string> SystemCommands::brightnessDown(int amount) {
    int current = getCurrentBrightness();
    return setBrightness(std::max(0, current - amount));
}

int SystemCommands::getCurrentBrightness() {
    std::string result = runCommand(
        "powershell -NoProfile -Command \"(Get-WmiObject -Namespace root/wmi "
        "-Class WmiMonitorBrightness).CurrentBrightness\" 2>nul");
    try { return std::stoi(result); }
    catch (...) { return 50; }
}

// ═══════════════════ Network ═══════════════════

std::pair<bool, std::string> SystemCommands::toggleWifi(bool enable) {
    std::string cmd = "netsh interface set interface \"Wi-Fi\" " +
                      std::string(enable ? "enable" : "disable");
    runCommand(cmd);
    return {true, std::string("WiFi ") + (enable ? "enabled" : "disabled")};
}

std::pair<bool, std::string> SystemCommands::toggleBluetooth(bool enable) {
    ShellExecuteW(nullptr, L"open", L"ms-settings:bluetooth", nullptr, nullptr, SW_SHOW);
    return {true, "Opened Bluetooth settings"};
}

void SystemCommands::toggleAirplaneMode() {
    ShellExecuteW(nullptr, L"open", L"ms-settings:network-airplanemode", nullptr, nullptr, SW_SHOW);
}

std::string SystemCommands::getWifiName() {
    return runCommand("netsh wlan show interfaces | findstr /R \"^....SSID\" | findstr /V BSSID");
}

std::string SystemCommands::getIPAddress() {
    std::string local = runCommand(
        "powershell -NoProfile -Command \"(Get-NetIPAddress -AddressFamily IPv4 "
        "| Where-Object {$_.InterfaceAlias -notlike '*Loopback*'}).IPAddress\" 2>nul");
    return local.empty() ? "Unknown" : local;
}

// ═══════════════════ Display / Accessibility ═══════════════════

std::pair<bool, std::string> SystemCommands::toggleNightLight(bool enable) {
    ShellExecuteW(nullptr, L"open", L"ms-settings:nightlight", nullptr, nullptr, SW_SHOW);
    return {true, "Opened Night Light settings"};
}

std::pair<bool, std::string> SystemCommands::toggleMagnifier(bool enable) {
    if (enable) ShellExecuteW(nullptr, L"open", L"magnify.exe", nullptr, nullptr, SW_SHOW);
    else runCommand("taskkill /IM magnify.exe /F 2>nul");
    return {true, std::string("Magnifier ") + (enable ? "opened" : "closed")};
}

std::pair<bool, std::string> SystemCommands::toggleNarrator(bool enable) {
    if (enable) ShellExecuteW(nullptr, L"open", L"narrator.exe", nullptr, nullptr, SW_SHOW);
    else runCommand("taskkill /IM narrator.exe /F 2>nul");
    return {true, std::string("Narrator ") + (enable ? "started" : "stopped")};
}

// ═══════════════════ System Info ═══════════════════

json SystemCommands::getBatteryInfo() {
    json info;
    SYSTEM_POWER_STATUS sps;
    if (GetSystemPowerStatus(&sps)) {
        info["ac_status"] = sps.ACLineStatus == 1 ? "plugged_in" : "on_battery";
        info["percent"] = sps.BatteryLifePercent == 255 ? -1 : (int)sps.BatteryLifePercent;
        info["charging"] = sps.BatteryFlag & 8 ? true : false;
        if (sps.BatteryLifeTime != (DWORD)-1) {
            int mins = sps.BatteryLifeTime / 60;
            info["remaining_minutes"] = mins;
        }
    }
    return info;
}

json SystemCommands::getStorageInfo() {
    json drives = json::array();
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(mask & (1 << i))) continue;
        char root[] = {'A' + (char)i, ':', '\\', 0};
        UINT type = GetDriveTypeA(root);
        if (type != DRIVE_FIXED) continue;

        ULARGE_INTEGER free_bytes, total_bytes;
        if (GetDiskFreeSpaceExA(root, &free_bytes, &total_bytes, nullptr)) {
            json d;
            d["drive"] = std::string(1, 'A' + (char)i) + ":";
            d["total_gb"] = (double)total_bytes.QuadPart / (1024.0*1024*1024);
            d["free_gb"] = (double)free_bytes.QuadPart / (1024.0*1024*1024);
            d["used_percent"] = 100.0 * (1.0 - (double)free_bytes.QuadPart / total_bytes.QuadPart);
            drives.push_back(d);
        }
    }
    return drives;
}

std::string SystemCommands::getUptime() {
    ULONGLONG ms = GetTickCount64();
    int secs = (int)(ms / 1000);
    int days = secs / 86400; secs %= 86400;
    int hours = secs / 3600; secs %= 3600;
    int mins = secs / 60;
    std::ostringstream ss;
    if (days > 0) ss << days << "d ";
    ss << hours << "h " << mins << "m";
    return ss.str();
}

json SystemCommands::getSystemSummary() {
    json summary;
    summary["battery"] = getBatteryInfo();
    summary["storage"] = getStorageInfo();
    summary["uptime"] = getUptime();
    // CPU/RAM via performance counters
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    GlobalMemoryStatusEx(&mem);
    summary["ram_usage_percent"] = (int)mem.dwMemoryLoad;
    summary["ram_total_mb"] = (int)(mem.ullTotalPhys / (1024*1024));
    summary["ram_available_mb"] = (int)(mem.ullAvailPhys / (1024*1024));
    return summary;
}

std::vector<AppInfo> SystemCommands::listRunningApps() {
    std::vector<AppInfo> apps;
    
    struct EnumData { std::vector<AppInfo>* apps; };
    EnumData data{&apps};
    
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* d = reinterpret_cast<EnumData*>(lParam);
        if (!IsWindowVisible(hwnd)) return TRUE;
        
        wchar_t title[512];
        if (GetWindowTextW(hwnd, title, 512) <= 0) return TRUE;
        
        AppInfo info;
        int sz = WideCharToMultiByte(CP_UTF8, 0, title, -1, nullptr, 0, nullptr, nullptr);
        info.window_title.resize(sz - 1);
        WideCharToMultiByte(CP_UTF8, 0, title, -1, info.window_title.data(), sz, nullptr, nullptr);
        
        GetWindowThreadProcessId(hwnd, &info.pid);
        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, info.pid);
        if (hProc) {
            wchar_t exe[MAX_PATH];
            DWORD exeSz = MAX_PATH;
            if (QueryFullProcessImageNameW(hProc, 0, exe, &exeSz)) {
                info.name = fs::path(std::wstring(exe)).filename().string();
            }
            PROCESS_MEMORY_COUNTERS pmc;
            if (GetProcessMemoryInfo(hProc, &pmc, sizeof(pmc))) {
                info.memory_mb = pmc.WorkingSetSize / (1024.0 * 1024.0);
            }
            CloseHandle(hProc);
        }
        d->apps->push_back(info);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&data));
    
    return apps;
}

// ═══════════════════ Process Management ═══════════════════

std::pair<bool, std::string> SystemCommands::killProcess(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    
    // S1 fix: reject shell metacharacters to prevent command injection
    for (char c : lower) {
        if (c == '&' || c == '|' || c == ';' || c == '>' || c == '<' ||
            c == '"' || c == '\'' || c == '`' || c == '(' || c == ')' ||
            c == '%' || c == '!' || c == '^') {
            return {false, "Invalid process name"};
        }
    }
    
    for (const auto& p : PROTECTED_PROCESSES) {
        std::string lp = p;
        std::transform(lp.begin(), lp.end(), lp.begin(), ::tolower);
        if (lower == lp) return {false, "Cannot kill protected process: " + name};
    }
    
    std::string exe = lower;
    if (exe.find(".exe") == std::string::npos) exe += ".exe";
    std::string cmd = "taskkill /IM " + exe + " /F 2>nul";
    runCommand(cmd);
    return {true, "Killed process: " + name};
}

bool SystemCommands::isAppRunning(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.find(".exe") == std::string::npos) lower += ".exe";
    
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    
    if (Process32FirstW(snap, &pe)) {
        do {
            std::wstring wname(pe.szExeFile);
            std::string pname(wname.begin(), wname.end());
            std::transform(pname.begin(), pname.end(), pname.begin(), ::tolower);
            if (pname == lower) { found = true; break; }
        } while (Process32NextW(snap, &pe));
    }
    
    CloseHandle(snap);
    return found;
}

// ═══════════════════ Power ═══════════════════

void SystemCommands::lockScreen() {
    LockWorkStation();
}

void SystemCommands::sleepComputer() {
    SetSuspendState(FALSE, FALSE, FALSE);
}

// ═══════════════════ Settings Pages ═══════════════════

void SystemCommands::openSettingsPage(const std::string& page) {
    std::string lower = page;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    
    auto it = SETTINGS_MAP.find(lower);
    std::wstring uri;
    if (it != SETTINGS_MAP.end()) uri = toWide(it->second);
    else uri = toWide("ms-settings:" + lower);
    
    ShellExecuteW(nullptr, L"open", uri.c_str(), nullptr, nullptr, SW_SHOW);
}

void SystemCommands::openTaskManager() {
    ShellExecuteW(nullptr, L"open", L"taskmgr.exe", nullptr, nullptr, SW_SHOW);
}
void SystemCommands::openDeviceManager() {
    ShellExecuteW(nullptr, L"open", L"devmgmt.msc", nullptr, nullptr, SW_SHOW);
}
void SystemCommands::openDiskCleanup() {
    ShellExecuteW(nullptr, L"open", L"cleanmgr.exe", nullptr, nullptr, SW_SHOW);
}
void SystemCommands::openControlPanel() {
    ShellExecuteW(nullptr, L"open", L"control", nullptr, nullptr, SW_SHOW);
}

// ═══════════════════ Timers & Stopwatch ═══════════════════

void SystemCommands::startTimer(int seconds, const std::string& label) {
    int clamped = std::clamp(seconds, 0, 86400);  // L9 fix: max 24 hours
    timer_threads_.emplace_back([clamped, label]() {
        std::this_thread::sleep_for(std::chrono::seconds(clamped));
        Beep(1200, 500);
        // L1 fix: use MultiByteToWideChar instead of wstring(begin, end)
        int wlen = MultiByteToWideChar(CP_UTF8, 0, label.c_str(), -1, nullptr, 0);
        std::wstring wlabel(wlen > 0 ? wlen - 1 : 0, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, label.c_str(), -1, wlabel.data(), wlen);
        std::wstring msg = L"Timer done: " + wlabel;
        MessageBoxW(nullptr, msg.c_str(), L"VISION AI Timer", MB_OK | MB_ICONINFORMATION);
    });
}

void SystemCommands::startStopwatch() {
    stopwatch_running_ = true;
    stopwatch_start_ = std::chrono::steady_clock::now();
}

std::string SystemCommands::getStopwatchTime() {
    if (!stopwatch_running_) return "Stopwatch not running";
    auto elapsed = std::chrono::steady_clock::now() - stopwatch_start_;
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
    int m = (int)(secs / 60), s = (int)(secs % 60);
    return std::to_string(m) + "m " + std::to_string(s) + "s";
}

void SystemCommands::stopStopwatch() { stopwatch_running_ = false; }

// ═══════════════════ Focus Mode ═══════════════════

void SystemCommands::startFocusMode(int minutes) {
    if (focus_mode_active_) return;
    focus_mode_active_ = true;
    // T5 fix: join previous thread before starting new one
    if (focus_thread_.joinable()) focus_thread_.join();
    focus_thread_ = std::thread([this, minutes]() {
        auto end = std::chrono::steady_clock::now() + std::chrono::minutes(minutes);
        while (focus_mode_active_ && std::chrono::steady_clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (focus_mode_active_) {
            focus_mode_active_ = false;
            Beep(800, 300); Beep(1000, 300); Beep(1200, 500);
        }
    });
    // T5 fix: no longer detached — joined in destructor
}

void SystemCommands::stopFocusMode() { focus_mode_active_ = false; }
bool SystemCommands::isFocusModeActive() const { return focus_mode_active_; }

// ═══════════════════ Health Scan ═══════════════════

json SystemCommands::systemHealthScan() {
    json report;
    report["battery"] = getBatteryInfo();
    report["storage"] = getStorageInfo();
    report["uptime"] = getUptime();
    
    MEMORYSTATUSEX mem{}; mem.dwLength = sizeof(mem);
    GlobalMemoryStatusEx(&mem);
    report["ram_usage"] = (int)mem.dwMemoryLoad;
    
    // Warnings
    json warnings = json::array();
    if (mem.dwMemoryLoad > 90) warnings.push_back("High RAM usage: " + std::to_string(mem.dwMemoryLoad) + "%");
    auto bat = getBatteryInfo();
    if (bat.contains("percent") && bat["percent"].get<int>() < 20 && bat["percent"].get<int>() >= 0)
        warnings.push_back("Low battery: " + std::to_string(bat["percent"].get<int>()) + "%");
    report["warnings"] = warnings;
    report["status"] = warnings.empty() ? "healthy" : "warnings";
    return report;
}

// ═══════════════════ Browser Integration ═══════════════════

void SystemCommands::searchInBrowser(const std::string& query, const std::string& browser) {
    std::string lower_browser = browser;
    std::transform(lower_browser.begin(), lower_browser.end(), lower_browser.begin(), ::tolower);
    
    auto it = BROWSER_EXE.find(lower_browser);
    std::string exe = (it != BROWSER_EXE.end()) ? it->second : (browser + ".exe");
    // S2 fix: URL-encode the query
    std::string encoded_query;
    for (unsigned char c : query) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded_query += c;
        } else if (c == ' ') {
            encoded_query += '+';
        } else {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", c);
            encoded_query += hex;
        }
    }
    std::string url = "https://www.google.com/search?q=" + encoded_query;
    
    // Fix: Use CreateProcessA to directly launch the specified browser
    // and avoid the OS also opening the default browser via URL protocol.
    std::string cmdline = "\"" + exe + "\" \"" + url + "\"";
    STARTUPINFOA si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, FALSE,
                       0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        // Fallback if CreateProcess fails (e.g. exe not on PATH)
        ShellExecuteA(nullptr, "open", exe.c_str(), url.c_str(), nullptr, SW_SHOW);
    }
}

void SystemCommands::openUrlInBrowser(const std::string& url, const std::string& browser) {
    std::string lower_browser = browser;
    std::transform(lower_browser.begin(), lower_browser.end(), lower_browser.begin(), ::tolower);
    
    auto it = BROWSER_EXE.find(lower_browser);
    std::string exe = (it != BROWSER_EXE.end()) ? it->second : (browser + ".exe");
    
    std::string full_url = url;
    if (full_url.find("://") == std::string::npos) full_url = "https://" + full_url;
    
    ShellExecuteA(nullptr, "open", exe.c_str(), full_url.c_str(), nullptr, SW_SHOW);
}

bool SystemCommands::isBrowser(const std::string& app_name) const {
    std::string lower = app_name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return BROWSER_EXE.count(lower) > 0;
}

} // namespace vision
