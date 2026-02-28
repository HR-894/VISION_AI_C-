#pragma once
/**
 * @file system_commands.h
 * @brief Comprehensive Windows system controls
 * 
 * Brightness (WMI), volume (Core Audio), WiFi/BT, power management,
 * process management, settings pages, timers, focus mode, health scan,
 * and browser integration.
 */

#include <string>
#include <vector>
#include <unordered_map>
#include <utility>
#include <thread>
#include <atomic>
#include <chrono>
#include <nlohmann/json.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace vision {

/// Info about a running application
struct AppInfo {
    std::string name;
    DWORD pid = 0;
    std::string window_title;
    double memory_mb = 0.0;
};

class SystemCommands {
public:
    SystemCommands();
    ~SystemCommands();

    // ── Brightness (WMI) ─────────────────────────────────────────
    std::pair<bool, std::string> setBrightness(int level);
    std::pair<bool, std::string> brightnessUp(int amount = 10);
    std::pair<bool, std::string> brightnessDown(int amount = 10);
    int getCurrentBrightness();

    // ── Network ──────────────────────────────────────────────────
    std::pair<bool, std::string> toggleWifi(bool enable);
    std::pair<bool, std::string> toggleBluetooth(bool enable);
    void toggleAirplaneMode();
    std::string getWifiName();
    std::string getIPAddress();

    // ── Display / Accessibility ──────────────────────────────────
    std::pair<bool, std::string> toggleNightLight(bool enable);
    std::pair<bool, std::string> toggleMagnifier(bool enable);
    std::pair<bool, std::string> toggleNarrator(bool enable);

    // ── System Info ──────────────────────────────────────────────
    nlohmann::json getBatteryInfo();
    nlohmann::json getStorageInfo();
    std::string getUptime();
    nlohmann::json getSystemSummary();
    std::vector<AppInfo> listRunningApps();

    // ── Process Management ───────────────────────────────────────
    std::pair<bool, std::string> killProcess(const std::string& name);
    bool isAppRunning(const std::string& name);

    // ── Power ────────────────────────────────────────────────────
    void lockScreen();
    void sleepComputer();

    // ── Settings Pages ───────────────────────────────────────────
    void openSettingsPage(const std::string& page);
    void openTaskManager();
    void openDeviceManager();
    void openDiskCleanup();
    void openControlPanel();

    // ── Timers & Stopwatch ───────────────────────────────────────
    void startTimer(int seconds, const std::string& label = "Timer");
    void startStopwatch();
    std::string getStopwatchTime();
    void stopStopwatch();

    // ── Focus Mode ───────────────────────────────────────────────
    void startFocusMode(int minutes);
    void stopFocusMode();
    bool isFocusModeActive() const;

    // ── Health Scan ──────────────────────────────────────────────
    nlohmann::json systemHealthScan();

    // ── Browser Integration ──────────────────────────────────────
    void searchInBrowser(const std::string& query, const std::string& browser);
    void openUrlInBrowser(const std::string& url, const std::string& browser);
    bool isBrowser(const std::string& app_name) const;

    // ── Static maps ──────────────────────────────────────────────
    static const std::unordered_map<std::string, std::string> BROWSER_EXE;
    static const std::unordered_map<std::string, std::string> SETTINGS_MAP;
    static const std::vector<std::string> PROTECTED_PROCESSES;

private:
    // Timer state
    std::vector<std::thread> timer_threads_;
    
    // Stopwatch state
    std::atomic<bool> stopwatch_running_{false};
    std::chrono::steady_clock::time_point stopwatch_start_;

    // Focus mode state
    std::atomic<bool> focus_mode_active_{false};
    std::thread focus_thread_;

    std::string runCommand(const std::string& cmd);
    std::wstring toWide(const std::string& str);
};

} // namespace vision
