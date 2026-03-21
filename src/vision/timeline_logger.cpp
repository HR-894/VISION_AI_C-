/**
 * @file timeline_logger.cpp
 * @brief OS-Level Semantic Timeline Implementation
 */

#include "timeline_logger.h"
#include <fstream>
#include <iostream>
#include <filesystem>
#include <chrono>
#include <nlohmann/json.hpp>
#include <psapi.h>

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

using json = nlohmann::json;

namespace vision {

// Global singleton pointer for the hook callback
static TimelineLogger* g_logger_instance = nullptr;

TimelineLogger::TimelineLogger() {
    namespace fs = std::filesystem;
    // Set log file path to AppData
    char appdata[MAX_PATH];
    if (GetEnvironmentVariableA("LOCALAPPDATA", appdata, MAX_PATH) > 0) {
        fs::path dir = fs::path(appdata) / "VisionAI";
        fs::create_directories(dir);
        log_file_path_ = (dir / "timeline.jsonl").string();
    } else {
        log_file_path_ = "timeline.jsonl";
    }
}

TimelineLogger::~TimelineLogger() {
    stop();
}

void TimelineLogger::start() {
    if (running_.exchange(true)) return;
    g_logger_instance = this;

    hook_thread_ = std::thread([this]() {
        runMessageLoop();
    });
    LOG_INFO("TimelineLogger started. Tracking background context to {}", log_file_path_);
}

void TimelineLogger::stop() {
    if (!running_.exchange(false)) return;

    if (thread_id_ != 0) {
        PostThreadMessage(thread_id_, WM_QUIT, 0, 0);
    }
    if (hook_thread_.joinable()) {
        hook_thread_.join();
    }
    g_logger_instance = nullptr;
    LOG_INFO("TimelineLogger stopped.");
}

void TimelineLogger::runMessageLoop() {
    thread_id_ = GetCurrentThreadId();

    HWINEVENTHOOK hHookForeground = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    // Get initial foreground window
    if (HWND hwnd = GetForegroundWindow()) {
        logForegroundChange(hwnd);
    }

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        if (!running_.load()) break;
    }

    if (hHookForeground) UnhookWinEvent(hHookForeground);
}

void CALLBACK TimelineLogger::WinEventProc(HWINEVENTHOOK hWinEventHook, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime) {
    (void)hWinEventHook; (void)idObject; (void)idChild; (void)dwEventThread; (void)dwmsEventTime;
    
    if (event == EVENT_SYSTEM_FOREGROUND && g_logger_instance) {
        g_logger_instance->logForegroundChange(hwnd);
    }
}

void TimelineLogger::logForegroundChange(HWND hwnd) {
    if (!hwnd || !running_.load()) return;

    std::string title = getWindowTitle(hwnd);
    std::string app = getProcessName(hwnd);

    // Filter empties or simple system overlays
    if (app.empty() || title == "Task Switching" || app == "explorer.exe") return;

    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    json entry = {
        {"timestamp", now_ms},
        {"app", app},
        {"title", title}
    };

    std::lock_guard<std::mutex> lock(file_mutex_);
    std::ofstream out(log_file_path_, std::ios::app);
    if (out) {
        out << entry.dump() << "\n";
    }
}

std::string TimelineLogger::getProcessName(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return "";

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProcess) return "";

    char path[MAX_PATH] = {0};
    if (GetModuleFileNameExA(hProcess, nullptr, path, MAX_PATH)) {
        CloseHandle(hProcess);
        std::string full_path(path);
        size_t last_slash = full_path.find_last_of("\\/");
        if (last_slash != std::string::npos) {
            return full_path.substr(last_slash + 1);
        }
        return full_path;
    }
    CloseHandle(hProcess);
    return "";
}

std::string TimelineLogger::getWindowTitle(HWND hwnd) {
    char title[1024] = {0};
    if (GetWindowTextA(hwnd, title, sizeof(title))) {
        return std::string(title);
    }
    return "";
}

std::vector<TimelineEvent> TimelineLogger::searchTimeline(const std::string& query, int max_results) const {
    std::vector<TimelineEvent> results;
    
    // Simple reverse read of the JSONL file + fuzzy matching (word boundary checks)
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(file_mutex_));
    std::ifstream in(log_file_path_);
    if (!in) return results;

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) lines.push_back(line);
    }

    // Prepare query lowers for simple match
    std::string lower_query = query;
    std::transform(lower_query.begin(), lower_query.end(), lower_query.begin(), ::tolower);

    // Search backwards (most recent first)
    for (int i = (int)lines.size() - 1; i >= 0 && (int)results.size() < max_results; i--) {
        try {
            json j = json::parse(lines[i]);
            std::string app = j.value("app", "");
            std::string title = j.value("title", "");
            
            std::string combined = app + " " + title;
            std::transform(combined.begin(), combined.end(), combined.begin(), ::tolower);

            // Very basic matching — if query word overlaps entirely
            if (lower_query.empty() || combined.find(lower_query) != std::string::npos) {
                TimelineEvent ev;
                ev.timestamp = j.value("timestamp", (int64_t)0);
                ev.app_name = app;
                ev.window_title = title;
                results.push_back(ev);
            }
        } catch (...) {
            continue; // Skip malformed lines
        }
    }

    return results;
}

} // namespace vision
