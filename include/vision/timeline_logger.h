#pragma once
/**
 * @file timeline_logger.h
 * @brief OS-Level Semantic Timeline ("Recall")
 *
 * Uses Win32 Event Hooks to silently log the user's active context
 * (Window Title, App Name, Timestamp) in a background thread, writing
 * to a local JSONLines file. Provides search capabilities for the AI to
 * query past desktop activity.
 */

#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <windows.h>

namespace vision {

struct TimelineEvent {
    int64_t timestamp = 0;
    std::string app_name;
    std::string window_title;
};

class TimelineLogger {
public:
    TimelineLogger();
    ~TimelineLogger();

    // Non-copyable
    TimelineLogger(const TimelineLogger&) = delete;
    TimelineLogger& operator=(const TimelineLogger&) = delete;

    /// Starts the background Win32 message loop
    void start();

    /// Stops the background hook
    void stop();

    /// Semantic search of the JSONL timeline file (in reverse chronological order)
    /// Finds events that fuzzy-match the query
    std::vector<TimelineEvent> searchTimeline(const std::string& query, int max_results = 5) const;

private:
    std::thread hook_thread_;
    std::atomic<bool> running_{false};
    DWORD thread_id_ = 0;
    
    std::string log_file_path_;
    std::mutex file_mutex_;

    static void CALLBACK WinEventProc(HWINEVENTHOOK hWinEventHook, DWORD event,
                                      HWND hwnd, LONG idObject, LONG idChild,
                                      DWORD dwEventThread, DWORD dwmsEventTime);

    void runMessageLoop();
    void logForegroundChange(HWND hwnd);

    /// Helper to get Exe name from HWND
    static std::string getProcessName(HWND hwnd);
    /// Helper to get Window Title from HWND
    static std::string getWindowTitle(HWND hwnd);
};

} // namespace vision
